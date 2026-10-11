// Image units (GL 4.2 section 8.26, GL_ARB_shader_image_load_store). The
// draw path binds a view of each image uniform's unit (DrawState::imageView).

#include "core/context.h"
#include "core/limits.h"
#include "gl_api.gen.h"

namespace wmgl::api {
namespace {

// The formats an image unit takes (GL 4.2 table 3.21).
bool isImageFormat(GLenum format) {
    switch (format) {
    case GL_RGBA32F: case GL_RGBA16F: case GL_RG32F: case GL_RG16F: case GL_R11F_G11F_B10F: case GL_R32F: case GL_R16F:
    case GL_RGBA32UI: case GL_RGBA16UI: case GL_RGB10_A2UI: case GL_RGBA8UI: case GL_RG32UI: case GL_RG16UI: case GL_RG8UI:
    case GL_R32UI: case GL_R16UI: case GL_R8UI: case GL_RGBA32I: case GL_RGBA16I: case GL_RGBA8I: case GL_RG32I:
    case GL_RG16I: case GL_RG8I: case GL_R32I: case GL_R16I: case GL_R8I: case GL_RGBA16: case GL_RGB10_A2:
    case GL_RGBA8: case GL_RG16: case GL_RG8: case GL_R16: case GL_R8: case GL_RGBA16_SNORM: case GL_RGBA8_SNORM:
    case GL_RG16_SNORM: case GL_RG8_SNORM: case GL_R16_SNORM: case GL_R8_SNORM:
        return true;
    default:
        return false;
    }
}

}  // namespace

void BindImageTexture(Context& ctx, GLuint unit, GLuint texture, GLint level, GLboolean layered, GLint layer, GLenum access,
                      GLenum format) {
    if (!ctx.imageLoadStore()) return ctx.unimplemented("glBindImageTexture");
    if (unit >= static_cast<GLuint>(ContextState::kMaxImageUnits)) return ctx.setError(GL_INVALID_VALUE);
    RefPtr<Texture> tex;
    if (texture) {
        ShareLock lock(ctx);
        Texture* t = ctx.share().textures.get(texture);
        if (!t) return ctx.setError(GL_INVALID_VALUE);
        tex = RefPtr<Texture>(t);
    }
    if (level < 0 || layer < 0) return ctx.setError(GL_INVALID_VALUE);
    if (access != GL_READ_ONLY && access != GL_WRITE_ONLY && access != GL_READ_WRITE) return ctx.setError(GL_INVALID_ENUM);
    if (!isImageFormat(format)) return ctx.setError(GL_INVALID_VALUE);
    ImageUnit& u = ctx.state.imageUnits[unit];
    u.texture = tex;
    u.level = level;
    u.layered = layered != GL_FALSE;
    u.layer = layer;
    u.access = access;
    u.format = format;
    // Storage gains shader write usage before the next draw that reads it.
    if (tex && !tex->shaderWrite) {
        tex->shaderWrite = true;
        tex->storageDirty = true;
    }
    ctx.markDirty(kDirtyTextures);
}

}  // namespace wmgl::api
