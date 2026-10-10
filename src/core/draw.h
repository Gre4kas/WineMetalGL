// The draw path: turns GL state into Metal render passes, pipelines and draws.
#pragma once

#import <Metal/Metal.h>

#include <cstdint>

#include "core/gl/types.h"
#include "core/metal/command_stream.h"
#include "core/metal/pipeline_cache.h"
#include "core/metal/transient.h"

namespace wmgl {

class Context;
class Framebuffer;
struct LinkedProgram;

struct TargetFormats {
    MTLPixelFormat color[8] = {};
    MTLPixelFormat depth = MTLPixelFormatInvalid, stencil = MTLPixelFormatInvalid;
    bool alphaIsOne[8] = {};   // RGB formats stored with an alpha channel
    bool integer[8] = {};
    bool srgb[8] = {};
};

class DrawState {
public:
    explicit DrawState(Context& ctx);

    // Describes the current draw framebuffer. Returns false (and sets
    // GL_INVALID_FRAMEBUFFER_OPERATION) if it is incomplete.
    bool drawTarget(RenderTargetDesc& target, TargetFormats& formats, bool reportErrors = true);
    // Read framebuffer colour/depth/stencil texture for glReadPixels and blits.
    id<MTLTexture> readColorTexture(uint32_t& level, uint32_t& slice, GLenum& internalFormat, int& width, int& height);

    void drawArrays(GLenum mode, GLint first, GLsizei count, GLsizei instances);
    void drawElements(GLenum mode, GLsizei count, GLenum type, const void* indices, GLsizei instances, GLint baseVertex);
    void clear(GLbitfield mask);

    TransientAllocator& transient() { return transient_; }

private:
    bool validateDraw(GLenum mode);
    id<MTLRenderCommandEncoder> prepare(GLenum mode, MTLPrimitiveType& primitive);
    // Encodes one draw. indexData (CPU, uint32) or indexBuffer select indexed drawing.
    // minVertex/vertexSpan is the vertex range attribute conversion must cover.
    bool issue(GLenum mode, GLint first, GLsizei count, GLsizei instances, const uint32_t* indexData,
               id<MTLBuffer> indexBuffer, NSUInteger indexOffset, MTLIndexType indexType, GLint baseVertex,
               GLint minVertex, GLsizei vertexSpan);
    void clearWithDraw(const RenderTargetDesc& target, const TargetFormats& formats, const PendingClear& clear);
    bool bindVertexInputs(id<MTLRenderCommandEncoder> encoder, PipelineKey& key, LinkedProgram& program,
                          GLint firstVertex, GLsizei vertexCount, GLsizei instances);
    void bindUniforms(id<MTLRenderCommandEncoder> encoder, LinkedProgram& program);
    void bindTextures(id<MTLRenderCommandEncoder> encoder, LinkedProgram& program);
    void applyDynamicState(id<MTLRenderCommandEncoder> encoder, const RenderTargetDesc& target);

    Context& ctx_;
    TransientAllocator transient_;
    uint64_t lastPassGeneration_ = 0;
    PipelineKey lastKey_{};
    MTLScissorRect lastScissor_{};
};

}  // namespace wmgl
