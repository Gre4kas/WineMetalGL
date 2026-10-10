// glBlitFramebuffer (GL 3.3 section 4.3.2).
//
// Same-size, same-format colour and depth copies use the blit encoder.
// Everything else renders a full-target triangle whose fragments map their
// window position into the source rectangle (utility shaders wmgl_blit_*).
// Stencil has no shader export path: it is computed on the CPU and copied,
// or written bit by bit with stencil-replace draws into multisampled targets.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/context.h"
#include "core/draw.h"
#include "core/metal/device.h"
#include "core/util/log.h"
#include "gl_api.gen.h"

namespace wmgl::api {
namespace {

struct BlitParams {
    float dst[4];
    float src[4];
    float srcSize[4];
};

struct Rect {
    int x0, y0, x1, y1;  // half-open, x0 <= x1
    bool empty() const { return x1 <= x0 || y1 <= y0; }
};

Rect normalized(GLint x0, GLint y0, GLint x1, GLint y1) {
    return {std::min(x0, x1), std::min(y0, y1), std::max(x0, x1), std::max(y0, y1)};
}

Rect intersect(Rect a, Rect b) {
    return {std::max(a.x0, b.x0), std::max(a.y0, b.y0), std::min(a.x1, b.x1), std::min(a.y1, b.y1)};
}

id<MTLRenderPipelineState> blitPipeline(NSString* fragment, MTLPixelFormat color, MTLPixelFormat depth,
                                        MTLPixelFormat stencil, uint32_t samples) {
    static std::mutex mutex;
    static std::unordered_map<std::string, id<MTLRenderPipelineState>> cache;
    const std::string key = std::string(fragment.UTF8String) + "/" + std::to_string(color) + "/" + std::to_string(depth) +
                            "/" + std::to_string(stencil) + "/" + std::to_string(samples);
    std::lock_guard<std::mutex> lock(mutex);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    id<MTLLibrary> library = Device::get().utilityLibrary();
    MTLRenderPipelineDescriptor* desc = [MTLRenderPipelineDescriptor new];
    desc.vertexFunction = [library newFunctionWithName:@"wmgl_clear_vs"];
    desc.fragmentFunction = [library newFunctionWithName:fragment];
    desc.colorAttachments[0].pixelFormat = color;
    desc.depthAttachmentPixelFormat = depth;
    desc.stencilAttachmentPixelFormat = stencil;
    desc.rasterSampleCount = samples;
    NSError* error = nil;
    id<MTLRenderPipelineState> pipeline = [Device::get().mtl() newRenderPipelineStateWithDescriptor:desc error:&error];
    if (!pipeline) WMGL_ERR("blit pipeline %s: %s", fragment.UTF8String, error.localizedDescription.UTF8String);
    cache[key] = pipeline;
    return pipeline;
}

// A single-level 2D (or 2D multisample) texture holding the source slice.
id<MTLTexture> sourceView(Context& ctx, const RenderTargetDesc::Target& t, uint32_t width, uint32_t height) {
    id<MTLTexture> texture = t.texture;
    const bool ms = texture.sampleCount > 1;
    if (texture.textureType == MTLTextureType3D) {
        // A 3D slice cannot be viewed as 2D: copy it out.
        MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:texture.pixelFormat
                                                                                         width:width
                                                                                        height:height
                                                                                     mipmapped:NO];
        desc.storageMode = MTLStorageModePrivate;
        desc.usage = MTLTextureUsageShaderRead;
        id<MTLTexture> copy = [Device::get().mtl() newTextureWithDescriptor:desc];
        [ctx.stream().blit() copyFromTexture:texture sourceSlice:0 sourceLevel:t.level
                                sourceOrigin:MTLOriginMake(0, 0, t.slice) sourceSize:MTLSizeMake(width, height, 1)
                                   toTexture:copy destinationSlice:0 destinationLevel:0
                           destinationOrigin:MTLOriginMake(0, 0, 0)];
        return copy;
    }
    if ((texture.textureType == MTLTextureType2D || texture.textureType == MTLTextureType2DMultisample) && t.level == 0 &&
        texture.mipmapLevelCount == 1)
        return texture;
    return [texture newTextureViewWithPixelFormat:texture.pixelFormat
                                      textureType:ms ? MTLTextureType2DMultisample : MTLTextureType2D
                                           levels:NSMakeRange(t.level, 1)
                                           slices:NSMakeRange(t.slice, 1)];
}

bool sameDirection(GLint a0, GLint a1, GLint b0, GLint b1) { return (a1 - a0) == (b1 - b0); }

// Destination pixels the blit may write: the destination rectangle, clipped
// to the target and (when enabled) the scissor box.
Rect writableRect(Context& ctx, const Rect& dst, uint32_t width, uint32_t height) {
    Rect r = intersect(dst, {0, 0, static_cast<int>(width), static_cast<int>(height)});
    const ContextState& st = ctx.state;
    if (st.scissorTest)
        r = intersect(r, {st.scissor[0], st.scissor[1], st.scissor[0] + std::max(0, st.scissor[2]),
                          st.scissor[1] + std::max(0, st.scissor[3])});
    return r;
}

// Copy with the blit encoder when the blit is a 1:1 copy between identical formats.
bool tryCopy(Context& ctx, const RenderTargetDesc::Target& src, uint32_t srcW, uint32_t srcH,
             const RenderTargetDesc::Target& dst, uint32_t dstW, uint32_t dstH, const GLint s[4], const GLint d[4]) {
    if (src.texture.pixelFormat != dst.texture.pixelFormat || src.texture.sampleCount != dst.texture.sampleCount) return false;
    if (!sameDirection(s[0], s[2], d[0], d[2]) || !sameDirection(s[1], s[3], d[1], d[3])) return false;
    if (src.texture == dst.texture && src.level == dst.level && src.slice == dst.slice) return false;
    // Clip the destination, then shift the source by the same amount.
    const Rect dstRect = normalized(d[0], d[1], d[2], d[3]);
    Rect clip = writableRect(ctx, dstRect, dstW, dstH);
    const int dx = std::min(s[0], s[2]) - dstRect.x0, dy = std::min(s[1], s[3]) - dstRect.y0;
    clip = intersect(clip, {-dx, -dy, static_cast<int>(srcW) - dx, static_cast<int>(srcH) - dy});
    if (clip.empty()) return true;
    const bool src3D = src.texture.textureType == MTLTextureType3D, dst3D = dst.texture.textureType == MTLTextureType3D;
    [ctx.stream().blit() copyFromTexture:src.texture sourceSlice:src3D ? 0 : src.slice sourceLevel:src.level
                            sourceOrigin:MTLOriginMake(clip.x0 + dx, clip.y0 + dy, src3D ? src.slice : 0)
                              sourceSize:MTLSizeMake(clip.x1 - clip.x0, clip.y1 - clip.y0, 1)
                               toTexture:dst.texture destinationSlice:dst3D ? 0 : dst.slice destinationLevel:dst.level
                       destinationOrigin:MTLOriginMake(clip.x0, clip.y0, dst3D ? dst.slice : 0)];
    return true;
}

void drawBlit(Context& ctx, const RenderTargetDesc& target, id<MTLRenderPipelineState> pipeline, id<MTLTexture> source,
              const BlitParams& params, const Rect& scissor, bool depth, bool linear) {
    id<MTLRenderCommandEncoder> encoder = ctx.stream().render(target);
    [encoder setRenderPipelineState:pipeline];
    DepthStencilKey ds;
    std::memset(&ds, 0, sizeof(ds));
    ds.depthCompare = static_cast<uint8_t>(MTLCompareFunctionAlways);
    ds.depthWrite = depth;
    [encoder setDepthStencilState:ctx.share().pipelines().depthStencil(ds)];
    MTLViewport viewport = {0, 0, static_cast<double>(target.width), static_cast<double>(target.height), 0, 1};
    [encoder setViewport:viewport];
    [encoder setScissorRect:{static_cast<NSUInteger>(scissor.x0), static_cast<NSUInteger>(scissor.y0),
                             static_cast<NSUInteger>(scissor.x1 - scissor.x0), static_cast<NSUInteger>(scissor.y1 - scissor.y0)}];
    [encoder setCullMode:MTLCullModeNone];
    [encoder setTriangleFillMode:MTLTriangleFillModeFill];
    [encoder setDepthClipMode:MTLDepthClipModeClamp];
    const float zero[4] = {};
    [encoder setVertexBytes:zero length:sizeof(zero) atIndex:0];
    [encoder setFragmentTexture:source atIndex:0];
    SamplerKey key;
    std::memset(&key, 0, sizeof(key));
    key.minFilter = key.magFilter = linear ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    key.mipFilter = MTLSamplerMipFilterNotMipmapped;
    key.sAddress = key.tAddress = key.rAddress = MTLSamplerAddressModeClampToEdge;
    key.compare = MTLCompareFunctionNever;
    key.maxAnisotropy = 1;
    key.normalized = 1;
    key.lodMax = 1000.0f;
    [encoder setFragmentSamplerState:ctx.share().pipelines().sampler(key) atIndex:0];
    [encoder setFragmentBytes:&params length:sizeof(params) atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    ctx.stream().noteDraw();
}

// Resolves a multisampled depth/stencil level to a single-sample texture
// (sample 0 for both aspects, as GL allows any sample for stencil).
RenderTargetDesc::Target resolveDepthStencil(Context& ctx, const RenderTargetDesc::Target& src, uint32_t width,
                                             uint32_t height) {
    MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:src.texture.pixelFormat
                                                                                     width:width
                                                                                    height:height
                                                                                 mipmapped:NO];
    desc.storageMode = MTLStorageModePrivate;
    desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    id<MTLTexture> resolved = [Device::get().mtl() newTextureWithDescriptor:desc];
    ctx.stream().resolvePendingClear();
    ctx.stream().endEncoder();
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    const bool combined = src.texture.pixelFormat == MTLPixelFormatDepth32Float_Stencil8;
    if (combined || src.texture.pixelFormat != MTLPixelFormatStencil8) {
        pass.depthAttachment.texture = src.texture;
        pass.depthAttachment.resolveTexture = resolved;
        pass.depthAttachment.loadAction = MTLLoadActionLoad;
        pass.depthAttachment.storeAction = MTLStoreActionStoreAndMultisampleResolve;
        pass.depthAttachment.depthResolveFilter = MTLMultisampleDepthResolveFilterSample0;
    }
    if (combined || src.texture.pixelFormat == MTLPixelFormatStencil8) {
        pass.stencilAttachment.texture = src.texture;
        pass.stencilAttachment.resolveTexture = resolved;
        pass.stencilAttachment.loadAction = MTLLoadActionLoad;
        pass.stencilAttachment.storeAction = MTLStoreActionStoreAndMultisampleResolve;
        pass.stencilAttachment.stencilResolveFilter = MTLMultisampleStencilResolveFilterSample0;
    }
    [[ctx.stream().commandBuffer() renderCommandEncoderWithDescriptor:pass] endEncoding];
    RenderTargetDesc::Target out;
    out.texture = resolved;
    return out;
}

// Writes per-pixel stencil `values` (0x100: keep) over `clip` of a
// multisampled target: a zeroing pass, then one stencil-replace pass per bit.
void writeStencilMultisample(Context& ctx, const RenderTargetDesc::Target& dst, MTLPixelFormat depthFormat,
                             uint32_t samples, uint32_t dstW, uint32_t dstH, const Rect& clip,
                             const std::vector<uint16_t>& values) {
    const int w = clip.x1 - clip.x0, h = clip.y1 - clip.y0;
    MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR16Uint
                                                                                    width:static_cast<NSUInteger>(w)
                                                                                   height:static_cast<NSUInteger>(h)
                                                                                mipmapped:NO];
    desc.storageMode = MTLStorageModeShared;
    desc.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> texture = [Device::get().mtl() newTextureWithDescriptor:desc];
    [texture replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:values.data()
               bytesPerRow:static_cast<NSUInteger>(w) * 2];
    RenderTargetDesc target;
    target.stencil = dst;
    const bool combined = dst.texture.pixelFormat == MTLPixelFormatDepth32Float_Stencil8;
    if (combined) target.depth = dst;
    target.width = dstW;
    target.height = dstH;
    target.samples = samples;
    id<MTLRenderPipelineState> pipeline = blitPipeline(@"wmgl_blit_stencil_bit_fs", MTLPixelFormatInvalid,
                                                       combined ? depthFormat : MTLPixelFormatInvalid,
                                                       dst.texture.pixelFormat, samples);
    if (!pipeline) return;
    id<MTLRenderCommandEncoder> encoder = ctx.stream().render(target);
    [encoder setRenderPipelineState:pipeline];
    [encoder setViewport:{0, 0, static_cast<double>(dstW), static_cast<double>(dstH), 0, 1}];
    [encoder setScissorRect:{static_cast<NSUInteger>(clip.x0), static_cast<NSUInteger>(clip.y0), static_cast<NSUInteger>(w),
                             static_cast<NSUInteger>(h)}];
    [encoder setCullMode:MTLCullModeNone];
    [encoder setTriangleFillMode:MTLTriangleFillModeFill];
    [encoder setDepthClipMode:MTLDepthClipModeClamp];
    const float zero[4] = {};
    [encoder setVertexBytes:zero length:sizeof(zero) atIndex:0];
    [encoder setFragmentTexture:texture atIndex:0];
    for (int pass = 0; pass <= 8; ++pass) {
        DepthStencilKey ds;
        std::memset(&ds, 0, sizeof(ds));
        ds.depthCompare = static_cast<uint8_t>(MTLCompareFunctionAlways);
        ds.stencilEnabled = 1;
        for (DepthStencilKey::Face* f : {&ds.front, &ds.back}) {
            f->compare = static_cast<uint8_t>(MTLCompareFunctionAlways);
            f->fail = f->depthFail = f->pass = static_cast<uint8_t>(MTLStencilOperationReplace);
            f->readMask = 0xff;
            f->writeMask = pass == 0 ? 0xffu : 1u << (pass - 1);
        }
        [encoder setDepthStencilState:ctx.share().pipelines().depthStencil(ds)];
        [encoder setStencilReferenceValue:pass == 0 ? 0u : 0xffu];
        const uint32_t p[4] = {static_cast<uint32_t>(clip.x0), static_cast<uint32_t>(clip.y0),
                               pass == 0 ? 0u : 1u << (pass - 1), 0};
        [encoder setFragmentBytes:p length:sizeof(p) atIndex:0];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    }
    ctx.stream().noteDraw();
}

// Stencil blit through the CPU (nearest filtering only, as GL requires).
void blitStencil(Context& ctx, const RenderTargetDesc::Target& source, uint32_t srcW, uint32_t srcH,
                 const RenderTargetDesc::Target& dst, MTLPixelFormat dstDepthFormat, uint32_t dstSamples, uint32_t dstW,
                 uint32_t dstH, const GLint s[4], const GLint d[4]) {
    const RenderTargetDesc::Target src =
        source.texture.sampleCount > 1 ? resolveDepthStencil(ctx, source, srcW, srcH) : source;
    const Rect clip = writableRect(ctx, normalized(d[0], d[1], d[2], d[3]), dstW, dstH);
    if (clip.empty()) return;
    const bool multisample = dst.texture.sampleCount > 1;
    auto option = [](id<MTLTexture> t) {
        return t.pixelFormat == MTLPixelFormatDepth32Float_Stencil8 ? MTLBlitOptionStencilFromDepthStencil : MTLBlitOptionNone;
    };
    id<MTLDevice> device = Device::get().mtl();
    id<MTLBuffer> in = [device newBufferWithLength:std::max<size_t>(1, static_cast<size_t>(srcW) * srcH)
                                           options:MTLResourceStorageModeShared];
    const bool src3D = src.texture.textureType == MTLTextureType3D;
    [ctx.stream().blit() copyFromTexture:src.texture sourceSlice:src3D ? 0 : src.slice sourceLevel:src.level
                            sourceOrigin:MTLOriginMake(0, 0, src3D ? src.slice : 0) sourceSize:MTLSizeMake(srcW, srcH, 1)
                                toBuffer:in destinationOffset:0 destinationBytesPerRow:srcW
                 destinationBytesPerImage:static_cast<size_t>(srcW) * srcH options:option(src.texture)];
    const int w = clip.x1 - clip.x0, h = clip.y1 - clip.y0;
    id<MTLBuffer> out = [device newBufferWithLength:static_cast<size_t>(w) * h options:MTLResourceStorageModeShared];
    const bool dst3D = dst.texture.textureType == MTLTextureType3D;
    // Keep the destination's existing values where the source is outside its bounds.
    if (!multisample)
        [ctx.stream().blit() copyFromTexture:dst.texture sourceSlice:dst3D ? 0 : dst.slice sourceLevel:dst.level
                                sourceOrigin:MTLOriginMake(clip.x0, clip.y0, dst3D ? dst.slice : 0) sourceSize:MTLSizeMake(w, h, 1)
                                    toBuffer:out destinationOffset:0 destinationBytesPerRow:static_cast<NSUInteger>(w)
                     destinationBytesPerImage:static_cast<size_t>(w) * h options:option(dst.texture)];
    ctx.stream().finish();
    const uint8_t* from = static_cast<const uint8_t*>(in.contents);
    uint8_t* to = static_cast<uint8_t*>(out.contents);
    std::vector<uint16_t> values(multisample ? static_cast<size_t>(w) * h : 0, 0x100);
    const double sx = static_cast<double>(s[2] - s[0]) / (d[2] - d[0]), sy = static_cast<double>(s[3] - s[1]) / (d[3] - d[1]);
    for (int y = clip.y0; y < clip.y1; ++y) {
        for (int x = clip.x0; x < clip.x1; ++x) {
            const int px = static_cast<int>(std::floor(s[0] + (x + 0.5 - d[0]) * sx));
            const int py = static_cast<int>(std::floor(s[1] + (y + 0.5 - d[1]) * sy));
            if (px < 0 || py < 0 || px >= static_cast<int>(srcW) || py >= static_cast<int>(srcH)) continue;
            const size_t at = static_cast<size_t>(y - clip.y0) * w + (x - clip.x0);
            const uint8_t v = from[static_cast<size_t>(py) * srcW + px];
            if (multisample) values[at] = v;
            else to[at] = v;
        }
    }
    if (multisample) {
        writeStencilMultisample(ctx, dst, dstDepthFormat, dstSamples, dstW, dstH, clip, values);
        return;
    }
    [ctx.stream().blit() copyFromBuffer:out sourceOffset:0 sourceBytesPerRow:static_cast<NSUInteger>(w)
                    sourceBytesPerImage:static_cast<size_t>(w) * h sourceSize:MTLSizeMake(w, h, 1) toTexture:dst.texture
                       destinationSlice:dst3D ? 0 : dst.slice destinationLevel:dst.level
                      destinationOrigin:MTLOriginMake(clip.x0, clip.y0, dst3D ? dst.slice : 0) options:option(dst.texture)];
}

NSString* colorShader(const FormatInfo* info, bool ms) {
    if (info && info->isSignedInteger()) return ms ? @"wmgl_blit_int_ms_fs" : @"wmgl_blit_int_fs";
    if (info && info->isInteger()) return ms ? @"wmgl_blit_uint_ms_fs" : @"wmgl_blit_uint_fs";
    return ms ? @"wmgl_blit_float_ms_fs" : @"wmgl_blit_float_fs";
}

}  // namespace

void BlitFramebuffer(Context& ctx, GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0,
                     GLint dstX1, GLint dstY1, GLbitfield mask, GLenum filter) {
    if (mask & ~(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT)) return ctx.setError(GL_INVALID_VALUE);
    if (filter != GL_NEAREST && filter != GL_LINEAR) return ctx.setError(GL_INVALID_ENUM);
    if ((mask & (GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT)) && filter == GL_LINEAR)
        return ctx.setError(GL_INVALID_OPERATION);
    if (CheckFramebufferStatus(ctx, GL_READ_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE ||
        CheckFramebufferStatus(ctx, GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        return ctx.setError(GL_INVALID_FRAMEBUFFER_OPERATION);
    DrawState& draw = ctx.draw();
    RenderTargetDesc dst;
    TargetFormats dstFormats;
    if (!draw.drawTarget(dst, dstFormats)) return;

    // Sources; buffers missing from either framebuffer are silently skipped.
    RenderTargetDesc::Target srcColor, srcDepth, srcStencil;
    const FormatInfo *colorInfo = nullptr, *depthInfo = nullptr, *stencilInfo = nullptr;
    uint32_t srcW = 0, srcH = 0, srcSamples = 1, w, h, s;
    bool doColor = (mask & GL_COLOR_BUFFER_BIT) && draw.readTarget(GL_COLOR, srcColor, colorInfo, srcW, srcH, srcSamples);
    bool doDepth = (mask & GL_DEPTH_BUFFER_BIT) && dstFormats.depthAttached && draw.readTarget(GL_DEPTH, srcDepth, depthInfo, w, h, s);
    if (doDepth && !doColor) { srcW = w; srcH = h; srcSamples = s; }
    bool doStencil = (mask & GL_STENCIL_BUFFER_BIT) && dstFormats.stencilAttached &&
                     draw.readTarget(GL_STENCIL, srcStencil, stencilInfo, w, h, s);
    if (doStencil && !doColor && !doDepth) { srcW = w; srcH = h; srcSamples = s; }
    if (doColor) {
        bool anyDst = false;
        for (int i = 0; i < 8; ++i) anyDst |= dst.color[i].texture != nil;
        doColor = anyDst;
    }

    // Validation (GL 3.3 section 4.3.2).
    const bool multisampled = srcSamples > 1 || dst.samples > 1;
    if (multisampled && (std::abs(srcX1 - srcX0) != std::abs(dstX1 - dstX0) || std::abs(srcY1 - srcY0) != std::abs(dstY1 - dstY0)))
        return ctx.setError(GL_INVALID_OPERATION);
    if (srcSamples > 1 && dst.samples > 1 && srcSamples != dst.samples) return ctx.setError(GL_INVALID_OPERATION);
    if (doColor) {
        for (int i = 0; i < 8; ++i) {
            if (!dst.color[i].texture) continue;
            const bool srcInt = colorInfo && colorInfo->isInteger();
            if (srcInt != dstFormats.integer[i]) return ctx.setError(GL_INVALID_OPERATION);
            if (srcInt && filter == GL_LINEAR) return ctx.setError(GL_INVALID_OPERATION);
        }
    }
    if (doDepth && srcDepth.texture.pixelFormat != dstFormats.depth) return ctx.setError(GL_INVALID_OPERATION);
    if (doStencil && srcStencil.texture.pixelFormat != dstFormats.stencil) return ctx.setError(GL_INVALID_OPERATION);
    if (!doColor && !doDepth && !doStencil) return;

    const GLint srcRect[4] = {srcX0, srcY0, srcX1, srcY1};
    const GLint dstRect[4] = {dstX0, dstY0, dstX1, dstY1};
    if (srcX0 == srcX1 || srcY0 == srcY1 || dstX0 == dstX1 || dstY0 == dstY1) return;
    const Rect scissor = writableRect(ctx, normalized(dstX0, dstY0, dstX1, dstY1), dst.width, dst.height);
    if (scissor.empty()) return;
    BlitParams params = {{static_cast<float>(dstX0), static_cast<float>(dstY0), static_cast<float>(dstX1), static_cast<float>(dstY1)},
                         {static_cast<float>(srcX0), static_cast<float>(srcY0), static_cast<float>(srcX1), static_cast<float>(srcY1)},
                         {static_cast<float>(srcW), static_cast<float>(srcH), filter == GL_LINEAR ? 1.0f : 0.0f, 0}};

    if (doColor) {
        id<MTLTexture> view = nil;
        for (int i = 0; i < 8; ++i) {
            if (!dst.color[i].texture) continue;
            if (tryCopy(ctx, srcColor, srcW, srcH, dst.color[i], dst.width, dst.height, srcRect, dstRect)) continue;
            if (!view) view = sourceView(ctx, srcColor, srcW, srcH);
            RenderTargetDesc target;
            target.color[0] = dst.color[i];
            target.width = dst.width;
            target.height = dst.height;
            target.samples = dst.samples;
            id<MTLRenderPipelineState> pipeline =
                blitPipeline(colorShader(colorInfo, view.sampleCount > 1), dstFormats.color[i], MTLPixelFormatInvalid,
                             MTLPixelFormatInvalid, dst.samples);
            if (pipeline) drawBlit(ctx, target, pipeline, view, params, scissor, false, filter == GL_LINEAR);
        }
    }
    if (doDepth) {
        // Copying a combined depth/stencil texture would also copy stencil.
        const bool combined = dstFormats.depth == MTLPixelFormatDepth32Float_Stencil8;
        if (combined || !tryCopy(ctx, srcDepth, srcW, srcH, dst.depth, dst.width, dst.height, srcRect, dstRect)) {
            id<MTLTexture> view = sourceView(ctx, srcDepth, srcW, srcH);
            RenderTargetDesc target;
            target.depth = dst.depth;
            if (combined) target.stencil = dst.depth;  // Metal binds combined formats at both points
            target.width = dst.width;
            target.height = dst.height;
            target.samples = dst.samples;
            id<MTLRenderPipelineState> pipeline = blitPipeline(view.sampleCount > 1 ? @"wmgl_blit_depth_ms_fs" : @"wmgl_blit_depth_fs",
                                                               MTLPixelFormatInvalid, dstFormats.depth,
                                                               combined ? dstFormats.depth : MTLPixelFormatInvalid, dst.samples);
            if (pipeline) drawBlit(ctx, target, pipeline, view, params, scissor, true, false);
        }
    }
    if (doStencil)
        blitStencil(ctx, srcStencil, srcW, srcH, dst.stencil, dstFormats.depth, dst.samples, dst.width, dst.height, srcRect,
                    dstRect);
    // Draws after the blit start a fresh pass with their own state.
    ctx.stream().endEncoder();
    ctx.markDirty(kDirtyAll);
}

}  // namespace wmgl::api
