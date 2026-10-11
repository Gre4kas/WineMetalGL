// Tessellation state (GL 4.0 section 10.1.15, GL_ARB_tessellation_shader).

#include "core/context.h"
#include "core/limits.h"
#include "gl_api.gen.h"

namespace wmgl::api {

void PatchParameteri(Context& ctx, GLenum pname, GLint value) {
    if (!ctx.tessellationShaders()) {
        ctx.unimplemented("glPatchParameteri");
        return;
    }
    if (pname != GL_PATCH_VERTICES) {
        ctx.setError(GL_INVALID_ENUM);
        return;
    }
    if (value <= 0 || value > limits::kMaxPatchVertices) {
        ctx.setError(GL_INVALID_VALUE);
        return;
    }
    ctx.state.patchVertices = value;
}

void PatchParameterfv(Context& ctx, GLenum pname, const GLfloat* values) {
    if (!ctx.tessellationShaders()) {
        ctx.unimplemented("glPatchParameterfv");
        return;
    }
    switch (pname) {
    case GL_PATCH_DEFAULT_OUTER_LEVEL:
        for (int i = 0; i < 4; ++i) ctx.state.patchDefaultOuterLevel[i] = values[i];
        return;
    case GL_PATCH_DEFAULT_INNER_LEVEL:
        for (int i = 0; i < 2; ++i) ctx.state.patchDefaultInnerLevel[i] = values[i];
        return;
    default:
        ctx.setError(GL_INVALID_ENUM);
        return;
    }
}

}  // namespace wmgl::api
