// glBeginTransformFeedback / glEndTransformFeedback (GL 3.3 section 2.15).
// Capture itself happens in the draw path (DrawState::captureTransformFeedback).

#include "core/context.h"
#include "core/program.h"
#include "gl_api.gen.h"

namespace wmgl::api {

void BeginTransformFeedback(Context& ctx, GLenum primitiveMode) {
    if (primitiveMode != GL_POINTS && primitiveMode != GL_LINES && primitiveMode != GL_TRIANGLES)
        return ctx.setError(GL_INVALID_ENUM);
    TransformFeedback& xfb = *ctx.state.transformFeedback;
    if (xfb.active) return ctx.setError(GL_INVALID_OPERATION);
    Program* p = ctx.state.program.get();
    if (!p || !p->linked || p->linked->link.transformFeedback.empty()) return ctx.setError(GL_INVALID_OPERATION);
    // Every buffer the program captures into must be bound.
    for (int b = 0; b < p->linked->link.xfbBufferCount; ++b)
        if (!xfb.buffers[b].buffer) return ctx.setError(GL_INVALID_OPERATION);
    xfb.active = true;
    xfb.paused = false;
    xfb.primitiveMode = primitiveMode;
    xfb.program = p;
    xfb.written = {};
    xfb.primitivesWritten = 0;
}

void EndTransformFeedback(Context& ctx) {
    TransformFeedback& xfb = *ctx.state.transformFeedback;
    if (!xfb.active) return ctx.setError(GL_INVALID_OPERATION);
    xfb.active = false;
    xfb.paused = false;
    xfb.program = nullptr;
}

}  // namespace wmgl::api
