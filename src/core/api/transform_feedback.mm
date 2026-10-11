// glBeginTransformFeedback / glEndTransformFeedback (GL 3.3 section 2.15).
// Capture itself happens in the draw path (DrawState::captureTransformFeedback).

#include <algorithm>

#include "core/context.h"
#include "core/draw.h"
#include "core/program.h"
#include "gl_api.gen.h"

namespace wmgl::api {

void BeginTransformFeedback(Context& ctx, GLenum primitiveMode) {
    if (primitiveMode != GL_POINTS && primitiveMode != GL_LINES && primitiveMode != GL_TRIANGLES)
        return ctx.setError(GL_INVALID_ENUM);
    TransformFeedback& xfb = *ctx.state.transformFeedback;
    if (xfb.active) return ctx.setError(GL_INVALID_OPERATION);
    // The current program, or the bound pipeline's last vertex-processing program.
    LinkedProgram* lp = ctx.draw().executable();
    if (!lp || lp->link.transformFeedback.empty()) return ctx.setError(GL_INVALID_OPERATION);
    Program* p = lp->pipeline ? lp->stageOwner[static_cast<int>(shader::Stage::Vertex)] : ctx.state.program.get();
    // Every buffer the program captures into must be bound.
    for (int b = 0; b < lp->link.xfbBufferCount; ++b)
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
    const uint64_t perPrimitive = xfb.primitiveMode == GL_POINTS ? 1 : xfb.primitiveMode == GL_LINES ? 2 : 3;
    xfb.everEnded = true;
    xfb.streamVertices = {xfb.primitivesWritten * perPrimitive, 0, 0, 0};
}

// ------------------------------------------- GL_ARB_transform_feedback2/3

namespace {

bool transformFeedback2(Context& ctx, const char* command) {
    if (ctx.version() >= 40 || ctx.hasExtension("GL_ARB_transform_feedback2")) return true;
    ctx.unimplemented(command);
    return false;
}

}  // namespace

void GenTransformFeedbacks(Context& ctx, GLsizei n, GLuint* ids) {
    if (!transformFeedback2(ctx, "glGenTransformFeedbacks")) return;
    if (n < 0) return ctx.setError(GL_INVALID_VALUE);
    if (ids) ctx.transformFeedbacks.generate(n, ids);
}

void DeleteTransformFeedbacks(Context& ctx, GLsizei n, const GLuint* ids) {
    if (!transformFeedback2(ctx, "glDeleteTransformFeedbacks")) return;
    if (n < 0) return ctx.setError(GL_INVALID_VALUE);
    if (!ids) return;
    // An active object (paused or not) cannot be deleted.
    for (GLsizei i = 0; i < n; ++i)
        if (TransformFeedback* xfb = ids[i] ? ctx.transformFeedbacks.get(ids[i]) : nullptr; xfb && xfb->active)
            return ctx.setError(GL_INVALID_OPERATION);
    for (GLsizei i = 0; i < n; ++i) {
        if (!ids[i]) continue;
        RefPtr<TransformFeedback> xfb = ctx.transformFeedbacks.remove(ids[i]);
        if (xfb && ctx.state.transformFeedback == xfb) ctx.state.transformFeedback = ctx.defaultTransformFeedback;
    }
}

GLboolean IsTransformFeedback(Context& ctx, GLuint id) {
    if (!transformFeedback2(ctx, "glIsTransformFeedback")) return GL_FALSE;
    return ctx.transformFeedbacks.exists(id) ? GL_TRUE : GL_FALSE;
}

void BindTransformFeedback(Context& ctx, GLenum target, GLuint id) {
    if (!transformFeedback2(ctx, "glBindTransformFeedback")) return;
    if (target != GL_TRANSFORM_FEEDBACK) return ctx.setError(GL_INVALID_ENUM);
    const TransformFeedback& current = *ctx.state.transformFeedback;
    if (current.active && !current.paused) return ctx.setError(GL_INVALID_OPERATION);
    if (!id) {
        ctx.state.transformFeedback = ctx.defaultTransformFeedback;
        return;
    }
    if (!ctx.transformFeedbacks.isGenerated(id)) return ctx.setError(GL_INVALID_OPERATION);
    RefPtr<TransformFeedback> xfb = ctx.transformFeedbacks.getRef(id);
    if (!xfb) {
        xfb = makeRef<TransformFeedback>(id);
        ctx.transformFeedbacks.set(id, xfb);
    }
    ctx.state.transformFeedback = xfb;
}

void PauseTransformFeedback(Context& ctx) {
    if (!transformFeedback2(ctx, "glPauseTransformFeedback")) return;
    TransformFeedback& xfb = *ctx.state.transformFeedback;
    if (!xfb.active || xfb.paused) return ctx.setError(GL_INVALID_OPERATION);
    xfb.paused = true;
}

void ResumeTransformFeedback(Context& ctx) {
    if (!transformFeedback2(ctx, "glResumeTransformFeedback")) return;
    TransformFeedback& xfb = *ctx.state.transformFeedback;
    if (!xfb.active || !xfb.paused) return ctx.setError(GL_INVALID_OPERATION);
    // The program capturing into the object must be the one in use again.
    LinkedProgram* lp = ctx.draw().executable();
    Program* p = !lp ? nullptr : lp->pipeline ? lp->stageOwner[static_cast<int>(shader::Stage::Vertex)] : ctx.state.program.get();
    if (p != xfb.program.get()) return ctx.setError(GL_INVALID_OPERATION);
    xfb.paused = false;
}

namespace {

void drawTransformFeedback(Context& ctx, GLenum mode, GLuint id, GLuint stream, GLsizei instances) {
    if (stream >= 4) return ctx.setError(GL_INVALID_VALUE);
    if (instances < 0) return ctx.setError(GL_INVALID_VALUE);
    TransformFeedback* xfb = id ? ctx.transformFeedbacks.get(id) : ctx.defaultTransformFeedback.get();
    if (!xfb) return ctx.setError(GL_INVALID_VALUE);
    if (!xfb->everEnded) return ctx.setError(GL_INVALID_OPERATION);
    const uint64_t vertices = xfb->streamVertices[stream];
    ctx.draw().drawArrays(mode, 0, static_cast<GLsizei>(std::min<uint64_t>(vertices, INT32_MAX)), instances);
}

}  // namespace

void DrawTransformFeedback(Context& ctx, GLenum mode, GLuint id) {
    if (!transformFeedback2(ctx, "glDrawTransformFeedback")) return;
    drawTransformFeedback(ctx, mode, id, 0, 1);
}

void DrawTransformFeedbackStream(Context& ctx, GLenum mode, GLuint id, GLuint stream) {
    if (ctx.version() < 40 && !ctx.hasExtension("GL_ARB_transform_feedback3")) return ctx.unimplemented("glDrawTransformFeedbackStream");
    drawTransformFeedback(ctx, mode, id, stream, 1);
}

void DrawTransformFeedbackInstanced(Context& ctx, GLenum mode, GLuint id, GLsizei instances) {
    if (ctx.version() < 42 && !ctx.hasExtension("GL_ARB_transform_feedback_instanced"))
        return ctx.unimplemented("glDrawTransformFeedbackInstanced");
    drawTransformFeedback(ctx, mode, id, 0, instances);
}

void DrawTransformFeedbackStreamInstanced(Context& ctx, GLenum mode, GLuint id, GLuint stream, GLsizei instances) {
    if (ctx.version() < 42 && !ctx.hasExtension("GL_ARB_transform_feedback_instanced"))
        return ctx.unimplemented("glDrawTransformFeedbackStreamInstanced");
    drawTransformFeedback(ctx, mode, id, stream, instances);
}

}  // namespace wmgl::api
