#include "core/surface.h"

#include <algorithm>

#include "core/metal/command_stream.h"
#include "core/metal/device.h"
#include "core/util/log.h"

namespace wmgl {

const std::vector<wmgl_pixel_format>& pixelFormats() {
    static const std::vector<wmgl_pixel_format> formats = [] {
        std::vector<wmgl_pixel_format> list;
        struct DepthStencil { int depth, stencil; };
        const DepthStencil depthStencil[] = {{24, 8}, {24, 0}, {32, 8}, {0, 0}, {16, 0}};
        const int samples[] = {0, 2, 4};
        // Window formats first (win32u requires on-screen formats to precede the rest).
        for (int pbuffer = 0; pbuffer < 2; ++pbuffer)
            for (int srgb = 0; srgb < 2; ++srgb)
                for (int ms : samples)
                    for (const DepthStencil& ds : depthStencil)
                        for (int doubleBuffer = 1; doubleBuffer >= 0; --doubleBuffer) {
                            if (pbuffer && doubleBuffer) continue;
                            wmgl_pixel_format f = {};
                            f.draw_to_window = !pbuffer;
                            f.draw_to_pbuffer = pbuffer;
                            f.double_buffer = doubleBuffer;
                            f.color_bits = 24;
                            f.red_bits = f.green_bits = f.blue_bits = f.alpha_bits = 8;
                            f.red_shift = 16; f.green_shift = 8; f.blue_shift = 0; f.alpha_shift = 24;
                            f.depth_bits = ds.depth;
                            f.stencil_bits = ds.stencil;
                            f.sample_buffers = ms ? 1 : 0;
                            f.samples = ms;
                            f.srgb_capable = srgb;
                            f.max_pbuffer_width = f.max_pbuffer_height = 16384;
                            f.max_pbuffer_pixels = 16384 * 16384;
                            list.push_back(f);
                        }
        return list;
    }();
    return formats;
}

unsigned onscreenPixelFormatCount() {
    static const unsigned count = [] {
        unsigned n = 0;
        for (const auto& f : pixelFormats()) n += f.draw_to_window;
        return n;
    }();
    return count;
}

Surface::Surface(CAMetalLayer* layer, int format, int width, int height)
    : layer_(layer), format_(format), width_(std::max(1, width)), height_(std::max(1, height)) {
    if (format_ < 1 || format_ > static_cast<int>(pixelFormats().size())) format_ = 1;
    samples_ = std::max(1, pixelFormat().samples);
    if (layer_) {
        layer_.device = Device::get().mtl();
        layer_.pixelFormat = MTLPixelFormatBGRA8Unorm;
        layer_.framebufferOnly = YES;
        layer_.maximumDrawableCount = 3;
        layer_.displaySyncEnabled = YES;
        layer_.drawableSize = CGSizeMake(width_, height_);
    }
    allocate();
}

Surface::~Surface() = default;

void Surface::allocate() {
    id<MTLDevice> device = Device::get().mtl();
    const wmgl_pixel_format& pf = pixelFormat();
    MTLTextureDescriptor* desc = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:pf.srgb_capable ? MTLPixelFormatRGBA8Unorm_sRGB : MTLPixelFormatRGBA8Unorm
                                     width:width_
                                    height:height_
                                 mipmapped:NO];
    desc.storageMode = MTLStorageModePrivate;
    desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
    if (samples_ > 1) {
        MTLTextureDescriptor* ms = [desc copy];
        ms.textureType = MTLTextureType2DMultisample;
        ms.sampleCount = samples_;
        ms.usage = MTLTextureUsageRenderTarget | MTLTextureUsagePixelFormatView;
        color_ = [device newTextureWithDescriptor:ms];
        resolve_ = [device newTextureWithDescriptor:desc];
    } else {
        color_ = [device newTextureWithDescriptor:desc];
        resolve_ = nil;
    }
    color_.label = @"default framebuffer";
    depthStencil_ = nil;
    if (pf.depth_bits || pf.stencil_bits) {
        MTLPixelFormat dsFormat = pf.stencil_bits ? MTLPixelFormatDepth32Float_Stencil8
                                  : pf.depth_bits == 16 ? MTLPixelFormatDepth16Unorm
                                                        : MTLPixelFormatDepth32Float;
        if (!pf.depth_bits) dsFormat = MTLPixelFormatStencil8;
        MTLTextureDescriptor* ds = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:dsFormat
                                                                                       width:width_
                                                                                      height:height_
                                                                                   mipmapped:NO];
        ds.storageMode = MTLStorageModePrivate;
        ds.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        if (samples_ > 1) {
            ds.textureType = MTLTextureType2DMultisample;
            ds.sampleCount = samples_;
        }
        depthStencil_ = [device newTextureWithDescriptor:ds];
        depthStencil_.label = @"default depth/stencil";
    }
}

void Surface::resize(int width, int height) {
    width = std::max(1, width);
    height = std::max(1, height);
    std::lock_guard<std::mutex> lock(mutex_);
    if (width == width_ && height == height_) return;
    width_ = width;
    height_ = height;
    if (layer_) layer_.drawableSize = CGSizeMake(width_, height_);
    allocate();
}

void Surface::setSwapInterval(int interval) {
    interval_ = interval;
    if (layer_) layer_.displaySyncEnabled = interval != 0;
}

id<MTLTexture> Surface::linearColor() {
    if (color_.pixelFormat != MTLPixelFormatRGBA8Unorm_sRGB) return color_;
    if (!linearColor_ || linearColor_.parentTexture != color_)
        linearColor_ = [color_ newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm];
    return linearColor_;
}

void Surface::describe(RenderTargetDesc& target) const {
    target = RenderTargetDesc();
    target.color[0].texture = color_;
    if (depthStencil_) {
        if (hasDepth()) target.depth.texture = depthStencil_;
        if (hasStencil()) target.stencil.texture = depthStencil_;
    }
    target.width = width_;
    target.height = height_;
    target.samples = samples_;
}

void Surface::resolveForRead(CommandStream& stream) {
    if (!resolve_) return;
    // Resolve with an empty pass whose store action resolves into resolve_.
    stream.resolvePendingClear();
    stream.endEncoder();
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = color_;
    pass.colorAttachments[0].resolveTexture = resolve_;
    pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
    pass.colorAttachments[0].storeAction = MTLStoreActionStoreAndMultisampleResolve;
    [[stream.commandBuffer() renderCommandEncoderWithDescriptor:pass] endEncoding];
}

bool Surface::swap(CommandStream& stream) {
    if (!layer_) {
        stream.flush();
        return true;
    }
    resolveForRead(stream);
    stream.resolvePendingClear();
    stream.endEncoder();
    id<CAMetalDrawable> drawable = [layer_ nextDrawable];
    if (!drawable) {
        WMGL_WARN("no drawable available");
        stream.flush();
        return false;
    }
    if (!presentPipeline_) {
        id<MTLLibrary> library = Device::get().utilityLibrary();
        MTLRenderPipelineDescriptor* desc = [MTLRenderPipelineDescriptor new];
        desc.vertexFunction = [library newFunctionWithName:@"wmgl_fullscreen_vs"];
        desc.fragmentFunction = [library newFunctionWithName:@"wmgl_copy_fs"];
        desc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        NSError* error = nil;
        presentPipeline_ = [Device::get().mtl() newRenderPipelineStateWithDescriptor:desc error:&error];
        if (!presentPipeline_) WMGL_ERR("present pipeline: %s", error.localizedDescription.UTF8String);
        MTLSamplerDescriptor* s = [MTLSamplerDescriptor new];
        s.minFilter = s.magFilter = MTLSamplerMinMagFilterNearest;
        presentSampler_ = [Device::get().mtl() newSamplerStateWithDescriptor:s];
    }
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = drawable.texture;
    pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> encoder = [stream.commandBuffer() renderCommandEncoderWithDescriptor:pass];
    [encoder setRenderPipelineState:presentPipeline_];
    // The back buffer may be sRGB-encoded; present the stored bytes unchanged.
    id<MTLTexture> source = resolvedColor();
    if (source.pixelFormat == MTLPixelFormatRGBA8Unorm_sRGB)
        source = [source newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm];
    // No flip in the shader: the drawable's top row samples the back buffer's
    // last row, which is GL's top row (GL row 0 = Metal row 0 = window bottom).
    const float params[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    [encoder setVertexBytes:params length:sizeof(params) atIndex:0];
    [encoder setFragmentTexture:source atIndex:0];
    [encoder setFragmentSamplerState:presentSampler_ atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];
    stream.present(drawable);
    stream.flush();
    return true;
}

}  // namespace wmgl
