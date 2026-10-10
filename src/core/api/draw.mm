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

}  // namespace wmgl::api
