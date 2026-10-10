// The draw path: turns GL state into Metal render passes, pipelines and draws.
#pragma once

#import <Metal/Metal.h>

#include <cstdint>
#include <vector>

#include "core/format/formats.h"
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
    // Read framebuffer source for GL_COLOR, GL_DEPTH or GL_STENCIL. For the
    // default framebuffer, colour is the resolved back buffer while `samples`
    // reports the surface's sample count. False if the buffer does not exist.
    bool readTarget(GLenum buffer, RenderTargetDesc::Target& out, const FormatInfo*& info, uint32_t& width,
                    uint32_t& height, uint32_t& samples);

    void drawArrays(GLenum mode, GLint first, GLsizei count, GLsizei instances);
    void drawElements(GLenum mode, GLsizei count, GLenum type, const void* indices, GLsizei instances, GLint baseVertex);
    void clear(GLbitfield mask);
    // glClearBuffer*: buffer is GL_COLOR (drawbuffer = draw buffer index),
    // GL_DEPTH, GL_STENCIL or GL_DEPTH_STENCIL. Colour values are converted
    // to the attachment's type.
    void clearBuffer(GLenum buffer, GLint drawbuffer, const double color[4], double depth, GLint stencil);

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
    // `gather`, when given, lists the vertex fetched for each vertex of a
    // non-indexed draw (transform feedback capture): per-vertex attributes
    // are gathered on the CPU in that order.
    bool bindVertexInputs(id<MTLRenderCommandEncoder> encoder, PipelineKey& key, LinkedProgram& program,
                          GLint firstVertex, GLsizei vertexCount, GLsizei instances,
                          const std::vector<uint32_t>* gather = nullptr);
    // Writes the transform feedback output of a draw: `vertices` are the
    // vertex numbers (gl_VertexID) in draw order.
    void captureTransformFeedback(GLenum mode, const std::vector<uint32_t>& vertices, GLsizei instances);
    void bindCaptureDefaults(id<MTLRenderCommandEncoder> encoder, LinkedProgram& program);
    void waitForCpuVertexReads(LinkedProgram& program, bool gather);
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
