// Drawing commands (GL 3.3 section 2.8.3).

#include "core/context.h"
#include "core/draw.h"
#include "gl_api.gen.h"

namespace wmgl::api {

void DrawArrays(Context& ctx, GLenum mode, GLint first, GLsizei count) { ctx.draw().drawArrays(mode, first, count, 1); }

void DrawArraysInstanced(Context& ctx, GLenum mode, GLint first, GLsizei count, GLsizei instancecount) {
    ctx.draw().drawArrays(mode, first, count, instancecount);
}

void DrawElements(Context& ctx, GLenum mode, GLsizei count, GLenum type, const void* indices) {
    ctx.draw().drawElements(mode, count, type, indices, 1, 0);
}

void DrawElementsInstanced(Context& ctx, GLenum mode, GLsizei count, GLenum type, const void* indices, GLsizei instancecount) {
    ctx.draw().drawElements(mode, count, type, indices, instancecount, 0);
}

void DrawElementsBaseVertex(Context& ctx, GLenum mode, GLsizei count, GLenum type, const void* indices, GLint basevertex) {
    ctx.draw().drawElements(mode, count, type, indices, 1, basevertex);
}

void DrawElementsInstancedBaseVertex(Context& ctx, GLenum mode, GLsizei count, GLenum type, const void* indices,
                                     GLsizei instancecount, GLint basevertex) {
    ctx.draw().drawElements(mode, count, type, indices, instancecount, basevertex);
}

void DrawRangeElements(Context& ctx, GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const void* indices) {
    if (end < start) return ctx.setError(GL_INVALID_VALUE);
    ctx.draw().drawElements(mode, count, type, indices, 1, 0);
}

void DrawRangeElementsBaseVertex(Context& ctx, GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type,
                                 const void* indices, GLint basevertex) {
    if (end < start) return ctx.setError(GL_INVALID_VALUE);
    ctx.draw().drawElements(mode, count, type, indices, 1, basevertex);
}

void MultiDrawArrays(Context& ctx, GLenum mode, const GLint* first, const GLsizei* count, GLsizei drawcount) {
    if (drawcount < 0) return ctx.setError(GL_INVALID_VALUE);
    for (GLsizei i = 0; i < drawcount; ++i) ctx.draw().drawArrays(mode, first[i], count[i], 1);
}

void MultiDrawElements(Context& ctx, GLenum mode, const GLsizei* count, GLenum type, const void* const* indices,
                       GLsizei drawcount) {
    if (drawcount < 0) return ctx.setError(GL_INVALID_VALUE);
    for (GLsizei i = 0; i < drawcount; ++i) ctx.draw().drawElements(mode, count[i], type, indices[i], 1, 0);
}

void MultiDrawElementsBaseVertex(Context& ctx, GLenum mode, const GLsizei* count, GLenum type, const void* const* indices,
                                 GLsizei drawcount, const GLint* basevertex) {
    if (drawcount < 0) return ctx.setError(GL_INVALID_VALUE);
    for (GLsizei i = 0; i < drawcount; ++i) ctx.draw().drawElements(mode, count[i], type, indices[i], 1, basevertex[i]);
}

void Clear(Context& ctx, GLbitfield mask) { ctx.draw().clear(mask); }

namespace {

bool checkDrawBuffer(Context& ctx, GLenum buffer, GLint drawbuffer) {
    const bool color = buffer == GL_COLOR;
    if (drawbuffer < 0 || (color && drawbuffer >= ContextState::kMaxDrawBuffers) || (!color && drawbuffer != 0)) {
        ctx.setError(GL_INVALID_VALUE);
        return false;
    }
    return true;
}

}  // namespace

void ClearBufferiv(Context& ctx, GLenum buffer, GLint drawbuffer, const GLint* value) {
    if (buffer != GL_COLOR && buffer != GL_STENCIL) return ctx.setError(GL_INVALID_ENUM);
    if (!checkDrawBuffer(ctx, buffer, drawbuffer)) return;
    if (buffer == GL_STENCIL) return ctx.draw().clearBuffer(GL_STENCIL, 0, nullptr, 0.0, value[0]);
    const double color[4] = {static_cast<double>(value[0]), static_cast<double>(value[1]), static_cast<double>(value[2]),
                             static_cast<double>(value[3])};
    ctx.draw().clearBuffer(GL_COLOR, drawbuffer, color, 0.0, 0);
}

void ClearBufferuiv(Context& ctx, GLenum buffer, GLint drawbuffer, const GLuint* value) {
    if (buffer != GL_COLOR) return ctx.setError(GL_INVALID_ENUM);
    if (!checkDrawBuffer(ctx, buffer, drawbuffer)) return;
    const double color[4] = {static_cast<double>(value[0]), static_cast<double>(value[1]), static_cast<double>(value[2]),
                             static_cast<double>(value[3])};
    ctx.draw().clearBuffer(GL_COLOR, drawbuffer, color, 0.0, 0);
}

void ClearBufferfv(Context& ctx, GLenum buffer, GLint drawbuffer, const GLfloat* value) {
    if (buffer != GL_COLOR && buffer != GL_DEPTH) return ctx.setError(GL_INVALID_ENUM);
    if (!checkDrawBuffer(ctx, buffer, drawbuffer)) return;
    if (buffer == GL_DEPTH) return ctx.draw().clearBuffer(GL_DEPTH, 0, nullptr, value[0], 0);
    const double color[4] = {value[0], value[1], value[2], value[3]};
    ctx.draw().clearBuffer(GL_COLOR, drawbuffer, color, 0.0, 0);
}

void ClearBufferfi(Context& ctx, GLenum buffer, GLint drawbuffer, GLfloat depth, GLint stencil) {
    if (buffer != GL_DEPTH_STENCIL) return ctx.setError(GL_INVALID_ENUM);
    if (!checkDrawBuffer(ctx, buffer, drawbuffer)) return;
    ctx.draw().clearBuffer(GL_DEPTH_STENCIL, 0, nullptr, depth, stencil);
}

}  // namespace wmgl::api
