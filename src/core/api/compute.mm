// glDispatchCompute / glDispatchComputeIndirect (GL_ARB_compute_shader, GL 4.3 section 19).

#include "core/context.h"
#include "core/draw.h"
#include "core/limits.h"
#include "core/program.h"
#include "gl_api.gen.h"

namespace wmgl::api {
namespace {

// The current program's executable, if it has a compute shader.
LinkedProgram* computeProgram(Context& ctx) {
    Program* p = ctx.state.program.get();
    LinkedProgram* lp = p ? p->linked.get() : nullptr;
    return lp && lp->hasStage(shader::Stage::Compute) ? lp : nullptr;
}

}  // namespace

void DispatchCompute(Context& ctx, GLuint x, GLuint y, GLuint z) {
    if (!ctx.computeShaders()) return ctx.unimplemented("glDispatchCompute");
    if (!computeProgram(ctx)) return ctx.setError(GL_INVALID_OPERATION);
    const GLuint max = static_cast<GLuint>(limits::kMaxComputeWorkGroupCount);
    if (x > max || y > max || z > max) return ctx.setError(GL_INVALID_VALUE);
    if (!x || !y || !z) return;
    ctx.draw().dispatch(x, y, z, nullptr, 0);
}

void DispatchComputeIndirect(Context& ctx, GLintptr indirect) {
    if (!ctx.computeShaders()) return ctx.unimplemented("glDispatchComputeIndirect");
    if (indirect < 0 || indirect % 4) return ctx.setError(GL_INVALID_VALUE);
    if (!computeProgram(ctx)) return ctx.setError(GL_INVALID_OPERATION);
    Buffer* buffer = ctx.state.dispatchIndirectBuffer.get();
    if (!buffer || !buffer->mtl || buffer->mapped || indirect + 12 > buffer->size) return ctx.setError(GL_INVALID_OPERATION);
    // Group counts above the limit are undefined behaviour; Metal ignores them safely.
    ctx.draw().dispatch(0, 0, 0, buffer, indirect);
}

}  // namespace wmgl::api
