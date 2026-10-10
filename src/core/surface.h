// Window and pbuffer surfaces: the default framebuffer.
//
// The default framebuffer is an offscreen back buffer in GL row order (row 0
// is the bottom). Swapping draws it, flipped, into the CAMetalLayer's
// drawable. Reads of GL_FRONT/GL_BACK and swap-copy semantics therefore
// never depend on the drawable.
#pragma once

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <mutex>
#include <vector>

#include "core/util/ref.h"
#include "winemetalgl/driver.h"

namespace wmgl {

class CommandStream;
struct RenderTargetDesc;

// The pixel formats WineMetalGL offers (1-based index = WGL pixel format).
const std::vector<wmgl_pixel_format>& pixelFormats();
unsigned onscreenPixelFormatCount();

class Surface : public RefCounted {
public:
    Surface(CAMetalLayer* layer, int format, int width, int height);
    ~Surface() override;

    bool isWindow() const { return layer_ != nil; }
    int format() const { return format_; }
    const wmgl_pixel_format& pixelFormat() const { return pixelFormats()[format_ - 1]; }
    int width() const { return width_; }
    int height() const { return height_; }
    uint32_t samples() const { return samples_; }

    // Resizes the buffers (contents become undefined, as GL allows).
    void resize(int width, int height);
    void setSwapInterval(int interval);

    id<MTLTexture> color() const { return color_; }          // what draws render into (MSAA if samples > 1)
    id<MTLTexture> resolvedColor() const { return resolve_ ? resolve_ : color_; }
    id<MTLTexture> depthStencil() const { return depthStencil_; }
    bool hasDepth() const { return pixelFormat().depth_bits > 0; }
    bool hasStencil() const { return pixelFormat().stencil_bits > 0; }
    bool srgbCapable() const { return pixelFormat().srgb_capable; }
    void describe(RenderTargetDesc& target) const;

    // Resolves (if multisampled) and presents the back buffer. The stream's
    // command buffer is committed.
    bool swap(CommandStream& stream);
    // Resolves multisampled colour into the single-sample texture for reads.
    void resolveForRead(CommandStream& stream);

private:
    void allocate();

    std::mutex mutex_;
    CAMetalLayer* layer_ = nil;
    int format_ = 1;
    int width_ = 0, height_ = 0;
    uint32_t samples_ = 1;
    int interval_ = 1;
    id<MTLTexture> color_ = nil, resolve_ = nil, depthStencil_ = nil;
    id<MTLRenderPipelineState> presentPipeline_ = nil;
    id<MTLSamplerState> presentSampler_ = nil;
};

}  // namespace wmgl
