// Placeholder for GL_ARB_shader_image_load_store state until image units are
// implemented: the image unit count, and unbinding a unit (glBindImageTexture
// with texture 0, which the Khronos CTS issues for every unit after each case
// in GL 4.4+ contexts). Binding a texture is still unimplemented.

#include "core/context.h"
#include "gl_api.gen.h"

namespace wmgl::api {

void BindImageTexture(Context& ctx, GLuint unit, GLuint texture, GLint level, GLboolean layered, GLint layer, GLenum access,
                      GLenum format) {
    if (ctx.version() < 42 && !ctx.hasExtension("GL_ARB_shader_image_load_store")) return ctx.unimplemented("glBindImageTexture");
    if (unit >= 8 || level < 0 || layer < 0) return ctx.setError(GL_INVALID_VALUE);
    if (access != GL_READ_ONLY && access != GL_WRITE_ONLY && access != GL_READ_WRITE) return ctx.setError(GL_INVALID_ENUM);
    if (texture) ctx.unimplemented("glBindImageTexture");
}

}  // namespace wmgl::api

namespace wmgl::api {

// glMemoryBarrier until image load/store exists: nothing writes memory
// incoherently yet (Metal orders the core's passes on its tracked resources),
// so a valid barrier has nothing to do.
void MemoryBarrier(Context& ctx, GLbitfield barriers) {
    if (ctx.version() < 42 && !ctx.hasExtension("GL_ARB_shader_image_load_store")) return ctx.unimplemented("glMemoryBarrier");
    constexpr GLbitfield kKnown = 0x0000ffffu;  // GL 4.2-4.6 barrier bits
    if (barriers != GL_ALL_BARRIER_BITS && (barriers & ~kKnown)) return ctx.setError(GL_INVALID_VALUE);
}

}  // namespace wmgl::api
