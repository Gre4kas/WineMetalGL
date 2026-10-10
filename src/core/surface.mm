#include "core/surface.h"

#include "core/stats.h"

#include <cstdio>
#include <cstdlib>

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

// Swap interval 0 (see surface.h).
struct Surface::Mailbox {
    static constexpr int kSlots = 3;
    enum State { Free, Pending, Ready, Presenting };
    std::mutex mutex;
    std::condition_variable changed;
    id<MTLTexture> slots[kSlots] = {};
    State state[kSlots] = {Free, Free, Free};
    uint64_t sequence[kSlots] = {};
    uint64_t nextSequence = 1;
    bool stop = false;
    id<MTLCommandQueue> queue = nil;
    std::thread thread;
};

Surface::~Surface() { stopMailbox(); }

void Surface::stopMailbox() {
    if (!mailbox_) return;
    {
        std::lock_guard<std::mutex> lock(mailbox_->mutex);
        mailbox_->stop = true;
    }
    mailbox_->changed.notify_all();
    if (mailbox_->thread.joinable()) mailbox_->thread.join();
    // Wait for presents still running on the GPU (their handlers touch the mailbox).
    std::unique_lock<std::mutex> lock(mailbox_->mutex);
    mailbox_->changed.wait(lock, [&] {
        for (Mailbox::State st : mailbox_->state)
            if (st == Mailbox::Presenting || st == Mailbox::Pending) return false;
        return true;
    });
    lock.unlock();
    mailbox_.reset();
}

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
    depthStencil_ = depthResolve_ = nil;
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

void Surface::resize(int width, int height, CommandStream* stream) {
    width = std::max(1, width);
    height = std::max(1, height);
    std::lock_guard<std::mutex> lock(mutex_);
    if (width == width_ && height == height_) return;
    id<MTLTexture> oldColor = color_;
    const int oldWidth = width_, oldHeight = height_;
    width_ = width;
    height_ = height;
    if (layer_) layer_.drawableSize = CGSizeMake(width_, height_);
    allocate();
    // Keep the overlapping contents: Wine reports a resize right before the
    // swap that presents the frame drawn at the old size.
    if (!oldColor || oldColor.sampleCount != color_.sampleCount) return;
    const MTLSize size = MTLSizeMake(std::min(oldWidth, width_), std::min(oldHeight, height_), 1);
    auto copy = [&](id<MTLBlitCommandEncoder> blit) {
        [blit copyFromTexture:oldColor sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:size
                    toTexture:color_ destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
    };
    if (stream) {
        copy(stream->blit());
        return;
    }
    static Queue* queue = new Queue();
    CommandStream own(*queue);
    copy(own.blit());
    own.flush();
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

id<MTLTexture> Surface::resolvedDepthStencil(CommandStream& stream) {
    if (!depthStencil_ || samples_ <= 1) return depthStencil_;
    if (!depthResolve_) {
        MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:depthStencil_.pixelFormat
                                                                                         width:width_
                                                                                        height:height_
                                                                                     mipmapped:NO];
        desc.storageMode = MTLStorageModePrivate;
        desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        depthResolve_ = [Device::get().mtl() newTextureWithDescriptor:desc];
        depthResolve_.label = @"default depth/stencil (resolved)";
    }
    stream.resolvePendingClear();
    stream.endEncoder();
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    if (hasDepth()) {
        pass.depthAttachment.texture = depthStencil_;
        pass.depthAttachment.resolveTexture = depthResolve_;
        pass.depthAttachment.loadAction = MTLLoadActionLoad;
        pass.depthAttachment.storeAction = MTLStoreActionStoreAndMultisampleResolve;
        pass.depthAttachment.depthResolveFilter = MTLMultisampleDepthResolveFilterSample0;
    }
    if (hasStencil()) {
        pass.stencilAttachment.texture = depthStencil_;
        pass.stencilAttachment.resolveTexture = depthResolve_;
        pass.stencilAttachment.loadAction = MTLLoadActionLoad;
        pass.stencilAttachment.storeAction = MTLStoreActionStoreAndMultisampleResolve;
        pass.stencilAttachment.stencilResolveFilter = MTLMultisampleStencilResolveFilterSample0;
    }
    [[stream.commandBuffer() renderCommandEncoderWithDescriptor:pass] endEncoding];
    return depthResolve_;
}

namespace {

// WINEMETALGL_DUMP_FRAMES=<dir> writes every swapped back buffer to
// <dir>/frame-<n>.ppm (debugging aid; GL row 0 is the bottom of the image).
void dumpFrame(CommandStream& stream, id<MTLTexture> color, int width, int height) {
    static const char* dir = std::getenv("WINEMETALGL_DUMP_FRAMES");
    if (!dir || !color || color.sampleCount > 1) return;
    static int frame = 0;
    const NSUInteger rowBytes = static_cast<NSUInteger>(width) * 4;
    id<MTLBuffer> staging = [Device::get().mtl() newBufferWithLength:rowBytes * static_cast<NSUInteger>(height)
                                                             options:MTLResourceStorageModeShared];
    [stream.blit() copyFromTexture:color sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                        sourceSize:MTLSizeMake(width, height, 1) toBuffer:staging destinationOffset:0
            destinationBytesPerRow:rowBytes destinationBytesPerImage:rowBytes * static_cast<NSUInteger>(height)];
    stream.finish();
    char path[1024];
    std::snprintf(path, sizeof(path), "%s/frame-%04d.ppm", dir, frame++);
    if (FILE* f = std::fopen(path, "wb")) {
        std::fprintf(f, "P6\n%d %d\n255\n", width, height);
        const uint8_t* p = static_cast<const uint8_t*>(staging.contents);
        for (int y = height - 1; y >= 0; --y)
            for (int x = 0; x < width; ++x) std::fwrite(p + (static_cast<size_t>(y) * width + x) * 4, 1, 3, f);
        std::fclose(f);
    }
}

}  // namespace

void Surface::ensurePresentPipeline() {
    std::lock_guard<std::mutex> lock(pipelineMutex_);
    if (presentPipeline_) return;
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

void Surface::encodePresent(id<MTLCommandBuffer> commandBuffer, id<MTLTexture> source, id<CAMetalDrawable> drawable) {
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = drawable.texture;
    pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> encoder = [commandBuffer renderCommandEncoderWithDescriptor:pass];
    [encoder setRenderPipelineState:presentPipeline_];
    // The back buffer may be sRGB-encoded; present the stored bytes unchanged.
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
}

bool Surface::swap(CommandStream& stream) {
    stats::Timer presentTimer(&stats::Frame::presentNanos);
    resolveForRead(stream);
    dumpFrame(stream, resolvedColor(), width_, height_);
    if (!layer_) {
        stream.flush();
        return true;
    }
    resolveForRead(stream);
    stream.resolvePendingClear();
    stream.endEncoder();
    ensurePresentPipeline();
    if (interval_ == 0) return swapMailbox(stream);
    stopMailbox();
    id<CAMetalDrawable> drawable = nil;
    {
        stats::Timer drawableTimer(&stats::Frame::drawableNanos);
        drawable = [layer_ nextDrawable];
    }
    if (!drawable) {
        WMGL_WARN("no drawable available");
        stream.flush();
        return false;
    }
    encodePresent(stream.commandBuffer(), resolvedColor(), drawable);
    stream.present(drawable);
    stream.flush();
    return true;
}

bool Surface::swapMailbox(CommandStream& stream) {
    id<MTLTexture> source = resolvedColor();
    if (mailbox_ && (mailbox_->slots[0].width != source.width || mailbox_->slots[0].height != source.height ||
                     mailbox_->slots[0].pixelFormat != source.pixelFormat))
        stopMailbox();
    if (!mailbox_) {
        mailbox_ = std::make_unique<Mailbox>();
        Mailbox* box = mailbox_.get();
        MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:source.pixelFormat
                                                                                         width:source.width
                                                                                        height:source.height
                                                                                     mipmapped:NO];
        desc.storageMode = MTLStorageModePrivate;
        desc.usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
        for (int i = 0; i < Mailbox::kSlots; ++i) box->slots[i] = [Device::get().mtl() newTextureWithDescriptor:desc];
        box->queue = [Device::get().mtl() newCommandQueue];
        CAMetalLayer* layer = layer_;
        box->thread = std::thread([this, box, layer] {
            for (;;) {
                int slot = -1;
                {
                    std::unique_lock<std::mutex> lock(box->mutex);
                    box->changed.wait(lock, [&] {
                        if (box->stop) return true;
                        for (int i = 0; i < Mailbox::kSlots; ++i)
                            if (box->state[i] == Mailbox::Ready) return true;
                        return false;
                    });
                    if (box->stop) return;
                    // The newest finished frame; older finished ones are dropped.
                    for (int i = 0; i < Mailbox::kSlots; ++i)
                        if (box->state[i] == Mailbox::Ready && (slot < 0 || box->sequence[i] > box->sequence[slot])) slot = i;
                    for (int i = 0; i < Mailbox::kSlots; ++i)
                        if (i != slot && box->state[i] == Mailbox::Ready) box->state[i] = Mailbox::Free;
                    box->state[slot] = Mailbox::Presenting;
                }
                box->changed.notify_all();
                @autoreleasepool {
                    id<CAMetalDrawable> drawable = [layer nextDrawable];
                    id<MTLCommandBuffer> commandBuffer = [box->queue commandBuffer];
                    if (drawable) {
                        encodePresent(commandBuffer, box->slots[slot], drawable);
                        [commandBuffer presentDrawable:drawable];
                    }
                    [commandBuffer addCompletedHandler:^(id<MTLCommandBuffer>) {
                        {
                            std::lock_guard<std::mutex> lock(box->mutex);
                            box->state[slot] = Mailbox::Free;
                        }
                        box->changed.notify_all();
                    }];
                    [commandBuffer commit];
                }
            }
        });
    }
    Mailbox* box = mailbox_.get();
    int slot = -1;
    {
        // A free slot, else the finished frame the presenter has not taken
        // yet (it is dropped). Only a GPU two frames behind makes this wait.
        stats::Timer drawableTimer(&stats::Frame::drawableNanos);
        std::unique_lock<std::mutex> lock(box->mutex);
        box->changed.wait(lock, [&] {
            for (int i = 0; i < Mailbox::kSlots; ++i)
                if (box->state[i] == Mailbox::Free || box->state[i] == Mailbox::Ready) return true;
            return false;
        });
        for (int i = 0; i < Mailbox::kSlots && slot < 0; ++i)
            if (box->state[i] == Mailbox::Free) slot = i;
        for (int i = 0; i < Mailbox::kSlots && slot < 0; ++i)
            if (box->state[i] == Mailbox::Ready) slot = i;
        box->state[slot] = Mailbox::Pending;
        box->sequence[slot] = box->nextSequence++;
    }
    id<MTLBlitCommandEncoder> blit = stream.blit();
    [blit copyFromTexture:source sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake(source.width, source.height, 1) toTexture:box->slots[slot] destinationSlice:0
         destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
    stream.endEncoder();
    [stream.commandBuffer() addCompletedHandler:^(id<MTLCommandBuffer>) {
        {
            std::lock_guard<std::mutex> lock(box->mutex);
            box->state[slot] = Mailbox::Ready;
        }
        box->changed.notify_all();
    }];
    stream.flush();
    return true;
}

}  // namespace wmgl
