// Extensions games query on 3.3 contexts (W23):
//  - GL_KHR_robustness / GL_ARB_robustness: bounded queries (glReadnPixels,
//    glGetnUniform*, glGetnTexImage, ...) and the reset status. Metal does
//    not report device resets to the application, so the status is always
//    GL_NO_ERROR.
//  - GL_ARB_internalformat_query: supported sample counts per format.
//  - GL_ARB_invalidate_subdata: invalidation hints, validated and ignored.
//  - GL_ARB_copy_image: glCopyImageSubData through Metal blits.

#include <algorithm>
#include <cstring>

#include "core/compat/compat_api.h"
#include "core/compat/fixed_state.h"
#include "core/context.h"
#include "core/format/pixel_transfer.h"
#include "core/metal/device.h"
#include "core/program.h"
#include "core/texture_storage.h"
#include "core/util/log.h"
#include "gl_api.gen.h"

namespace wmgl::api {
namespace {

// IMAGE_HEIGHT and SKIP_IMAGES apply to three-dimensional images only.
PixelStoreParams packParams(const PixelStore& s, bool threeD) {
    PixelStoreParams ps;
    ps.alignment = s.alignment;
    ps.rowLength = s.rowLength;
    ps.imageHeight = threeD ? s.imageHeight : 0;
    ps.skipPixels = s.skipPixels;
    ps.skipRows = s.skipRows;
    ps.skipImages = threeD ? s.skipImages : 0;
    ps.swapBytes = s.swapBytes;
    return ps;
}

// glReadnPixels and friends: a client buffer too small for the result is
// INVALID_OPERATION and nothing is written (KHR_robustness section 4.3.2).
bool fits(Context& ctx, size_t needed, GLsizei bufSize) {
    if (bufSize < 0) {
        ctx.setError(GL_INVALID_VALUE);
        return false;
    }
    if (ctx.state.pixelPackBuffer) return true;  // the buffer object bounds apply instead
    if (needed > static_cast<size_t>(bufSize)) {
        ctx.setError(GL_INVALID_OPERATION);
        return false;
    }
    return true;
}

size_t imageBytes(Context& ctx, GLenum format, GLenum type, GLint width, GLint height, GLint depth, bool threeD) {
    if (width <= 0 || height <= 0 || depth <= 0) return 0;
    return transferLayout(format, type, width, height, depth, packParams(ctx.state.pack, threeD)).bytesNeeded;
}

}  // namespace

// ------------------------------------------------------------ robustness

GLenum GetGraphicsResetStatus(Context& ctx) {
    (void)ctx;
    return GL_NO_ERROR;
}

void ReadnPixels(Context& ctx, GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type,
                 GLsizei bufSize, void* data) {
    if (width < 0 || height < 0) return ctx.setError(GL_INVALID_VALUE);
    if (!fits(ctx, imageBytes(ctx, format, type, width, height, 1, false), bufSize)) return;
    ReadPixels(ctx, x, y, width, height, format, type, data);
}

void GetnUniformfv(Context& ctx, GLuint program, GLint location, GLsizei bufSize, GLfloat* params) {
    const int n = uniformComponents(ctx, program, location);
    if (n < 0) return GetUniformfv(ctx, program, location, params);  // reports the error
    if (static_cast<size_t>(n) * sizeof(GLfloat) > static_cast<size_t>(std::max(bufSize, 0)))
        return ctx.setError(GL_INVALID_OPERATION);
    GetUniformfv(ctx, program, location, params);
}

void GetnUniformiv(Context& ctx, GLuint program, GLint location, GLsizei bufSize, GLint* params) {
    const int n = uniformComponents(ctx, program, location);
    if (n < 0) return GetUniformiv(ctx, program, location, params);
    if (static_cast<size_t>(n) * sizeof(GLint) > static_cast<size_t>(std::max(bufSize, 0)))
        return ctx.setError(GL_INVALID_OPERATION);
    GetUniformiv(ctx, program, location, params);
}

void GetnUniformuiv(Context& ctx, GLuint program, GLint location, GLsizei bufSize, GLuint* params) {
    const int n = uniformComponents(ctx, program, location);
    if (n < 0) return GetUniformuiv(ctx, program, location, params);
    if (static_cast<size_t>(n) * sizeof(GLuint) > static_cast<size_t>(std::max(bufSize, 0)))
        return ctx.setError(GL_INVALID_OPERATION);
    GetUniformuiv(ctx, program, location, params);
}

void GetnUniformdv(Context& ctx, GLuint program, GLint location, GLsizei bufSize, GLdouble* params) {
    const int n = uniformComponents(ctx, program, location);
    if (n < 0) return GetUniformdv(ctx, program, location, params);
    if (static_cast<size_t>(n) * sizeof(GLdouble) > static_cast<size_t>(std::max(bufSize, 0)))
        return ctx.setError(GL_INVALID_OPERATION);
    GetUniformdv(ctx, program, location, params);
}

void GetnTexImage(Context& ctx, GLenum target, GLint level, GLenum format, GLenum type, GLsizei bufSize, void* pixels) {
    GLint w = 0, h = 0, d = 0;
    const GLenum before = ctx.takeError();
    GetTexLevelParameteriv(ctx, target, level, GL_TEXTURE_WIDTH, &w);
    GetTexLevelParameteriv(ctx, target, level, GL_TEXTURE_HEIGHT, &h);
    GetTexLevelParameteriv(ctx, target, level, GL_TEXTURE_DEPTH, &d);
    const GLenum error = ctx.takeError();
    if (before) ctx.setError(before);
    if (error) return GetTexImage(ctx, target, level, format, type, pixels);  // reports the error
    if (!fits(ctx, imageBytes(ctx, format, type, w, h, d,
                              target == GL_TEXTURE_3D || target == GL_TEXTURE_2D_ARRAY ||
                                  target == GL_TEXTURE_CUBE_MAP_ARRAY),
             bufSize)) return;
    GetTexImage(ctx, target, level, format, type, pixels);
}

void GetnCompressedTexImage(Context& ctx, GLenum target, GLint lod, GLsizei bufSize, void* pixels) {
    GLint size = 0;
    const GLenum before = ctx.takeError();
    GetTexLevelParameteriv(ctx, target, lod, GL_TEXTURE_COMPRESSED_IMAGE_SIZE, &size);
    const GLenum error = ctx.takeError();
    if (before) ctx.setError(before);
    if (error) return GetCompressedTexImage(ctx, target, lod, pixels);
    if (!fits(ctx, static_cast<size_t>(std::max(size, 0)), bufSize)) return;
    GetCompressedTexImage(ctx, target, lod, pixels);
}

// Compatibility-profile queries. Evaluators, pixel maps beyond their
// defaults and the imaging subset are not implemented (GL_ARB_imaging is not
// advertised), so these report what the unextended state holds.
void GetnMapdv(Context& ctx, GLenum, GLenum, GLsizei, GLdouble*) { ctx.setError(GL_INVALID_ENUM); }
void GetnMapfv(Context& ctx, GLenum, GLenum, GLsizei, GLfloat*) { ctx.setError(GL_INVALID_ENUM); }
void GetnMapiv(Context& ctx, GLenum, GLenum, GLsizei, GLint*) { ctx.setError(GL_INVALID_ENUM); }

namespace {

bool validPixelMap(GLenum map) { return map >= GL_PIXEL_MAP_I_TO_I && map <= GL_PIXEL_MAP_A_TO_A; }

// Every pixel map holds its initial single entry, 0.
template <class T>
void getPixelMap(Context& ctx, GLenum map, GLsizei bufSize, T* values) {
    if (!ctx.isCompat()) return ctx.setError(GL_INVALID_OPERATION);
    if (!validPixelMap(map)) return ctx.setError(GL_INVALID_ENUM);
    if (Buffer* pbo = ctx.state.pixelPackBuffer.get()) {
        // `values` is an offset into the pixel pack buffer.
        const uintptr_t offset = reinterpret_cast<uintptr_t>(values);
        if (pbo->mappedNonPersistent() || !pbo->mtl || offset > static_cast<uintptr_t>(pbo->size) ||
            static_cast<uintptr_t>(pbo->size) - offset < sizeof(T))
            return ctx.setError(GL_INVALID_OPERATION);
        if (pbo->lastUseSerial) ctx.stream().waitFor(pbo->lastUseSerial);
        const T zero = 0;
        std::memcpy(pbo->contents() + offset, &zero, sizeof(T));
        ++pbo->version;
        return;
    }
    if (bufSize < static_cast<GLsizei>(sizeof(T))) return ctx.setError(GL_INVALID_OPERATION);
    if (values) values[0] = 0;
}

}  // namespace

void GetPixelMapfv(Context& ctx, GLenum map, GLfloat* values) { getPixelMap(ctx, map, sizeof(GLfloat), values); }
void GetPixelMapuiv(Context& ctx, GLenum map, GLuint* values) { getPixelMap(ctx, map, sizeof(GLuint), values); }
void GetPixelMapusv(Context& ctx, GLenum map, GLushort* values) { getPixelMap(ctx, map, sizeof(GLushort), values); }
void GetnPixelMapfv(Context& ctx, GLenum map, GLsizei bufSize, GLfloat* values) { getPixelMap(ctx, map, bufSize, values); }
void GetnPixelMapuiv(Context& ctx, GLenum map, GLsizei bufSize, GLuint* values) { getPixelMap(ctx, map, bufSize, values); }
void GetnPixelMapusv(Context& ctx, GLenum map, GLsizei bufSize, GLushort* values) { getPixelMap(ctx, map, bufSize, values); }

void GetnPolygonStipple(Context& ctx, GLsizei bufSize, GLubyte* pattern) {
    // The pack state decides how many bytes the 32x32 pattern spans.
    if (!ctx.state.pixelPackBuffer && (bufSize < 0 || static_cast<size_t>(bufSize) < bitmapBytes(ctx.state.pack, 32, 32)))
        return ctx.setError(GL_INVALID_OPERATION);
    GetPolygonStipple(ctx, pattern);
}

void GetnColorTable(Context& ctx, GLenum, GLenum, GLenum, GLsizei, void*) { ctx.setError(GL_INVALID_ENUM); }
void GetnConvolutionFilter(Context& ctx, GLenum, GLenum, GLenum, GLsizei, void*) { ctx.setError(GL_INVALID_ENUM); }
void GetnSeparableFilter(Context& ctx, GLenum, GLenum, GLenum, GLsizei, void*, GLsizei, void*, void*) {
    ctx.setError(GL_INVALID_ENUM);
}
void GetnHistogram(Context& ctx, GLenum, GLboolean, GLenum, GLenum, GLsizei, void*) { ctx.setError(GL_INVALID_ENUM); }
void GetnMinmax(Context& ctx, GLenum, GLboolean, GLenum, GLenum, GLsizei, void*) { ctx.setError(GL_INVALID_ENUM); }

// GL_ARB_robustness names of the same commands.
GLenum GetGraphicsResetStatusARB(Context& ctx) { return GetGraphicsResetStatus(ctx); }
void GetnTexImageARB(Context& ctx, GLenum target, GLint level, GLenum format, GLenum type, GLsizei bufSize, void* img) {
    GetnTexImage(ctx, target, level, format, type, bufSize, img);
}
void GetnCompressedTexImageARB(Context& ctx, GLenum target, GLint lod, GLsizei bufSize, void* img) {
    GetnCompressedTexImage(ctx, target, lod, bufSize, img);
}
void GetnUniformfvARB(Context& ctx, GLuint program, GLint location, GLsizei bufSize, GLfloat* params) {
    GetnUniformfv(ctx, program, location, bufSize, params);
}
void GetnUniformivARB(Context& ctx, GLuint program, GLint location, GLsizei bufSize, GLint* params) {
    GetnUniformiv(ctx, program, location, bufSize, params);
}
void GetnUniformuivARB(Context& ctx, GLuint program, GLint location, GLsizei bufSize, GLuint* params) {
    GetnUniformuiv(ctx, program, location, bufSize, params);
}
void GetnUniformdvARB(Context& ctx, GLuint program, GLint location, GLsizei bufSize, GLdouble* params) {
    GetnUniformdv(ctx, program, location, bufSize, params);
}
void GetnMapdvARB(Context& ctx, GLenum target, GLenum query, GLsizei bufSize, GLdouble* v) { GetnMapdv(ctx, target, query, bufSize, v); }
void GetnMapfvARB(Context& ctx, GLenum target, GLenum query, GLsizei bufSize, GLfloat* v) { GetnMapfv(ctx, target, query, bufSize, v); }
void GetnMapivARB(Context& ctx, GLenum target, GLenum query, GLsizei bufSize, GLint* v) { GetnMapiv(ctx, target, query, bufSize, v); }
void GetnPixelMapfvARB(Context& ctx, GLenum map, GLsizei bufSize, GLfloat* values) { GetnPixelMapfv(ctx, map, bufSize, values); }
void GetnPixelMapuivARB(Context& ctx, GLenum map, GLsizei bufSize, GLuint* values) { GetnPixelMapuiv(ctx, map, bufSize, values); }
void GetnPixelMapusvARB(Context& ctx, GLenum map, GLsizei bufSize, GLushort* values) { GetnPixelMapusv(ctx, map, bufSize, values); }
void GetnPolygonStippleARB(Context& ctx, GLsizei bufSize, GLubyte* pattern) { GetnPolygonStipple(ctx, bufSize, pattern); }
void GetnColorTableARB(Context& ctx, GLenum target, GLenum format, GLenum type, GLsizei bufSize, void* table) {
    GetnColorTable(ctx, target, format, type, bufSize, table);
}
void GetnConvolutionFilterARB(Context& ctx, GLenum target, GLenum format, GLenum type, GLsizei bufSize, void* image) {
    GetnConvolutionFilter(ctx, target, format, type, bufSize, image);
}
void GetnSeparableFilterARB(Context& ctx, GLenum target, GLenum format, GLenum type, GLsizei rowBufSize, void* row,
                            GLsizei columnBufSize, void* column, void* span) {
    GetnSeparableFilter(ctx, target, format, type, rowBufSize, row, columnBufSize, column, span);
}
void GetnHistogramARB(Context& ctx, GLenum target, GLboolean reset, GLenum format, GLenum type, GLsizei bufSize, void* values) {
    GetnHistogram(ctx, target, reset, format, type, bufSize, values);
}
void GetnMinmaxARB(Context& ctx, GLenum target, GLboolean reset, GLenum format, GLenum type, GLsizei bufSize, void* values) {
    GetnMinmax(ctx, target, reset, format, type, bufSize, values);
}

// ------------------------------------------------------------ internal format query

void GetInternalformativ(Context& ctx, GLenum target, GLenum internalformat, GLenum pname, GLsizei bufSize, GLint* params) {
    if (target != GL_RENDERBUFFER && target != GL_TEXTURE_2D_MULTISAMPLE && target != GL_TEXTURE_2D_MULTISAMPLE_ARRAY)
        return ctx.setError(GL_INVALID_ENUM);
    if (pname != GL_NUM_SAMPLE_COUNTS && pname != GL_SAMPLES) return ctx.setError(GL_INVALID_ENUM);
    const FormatInfo* info = formatInfo(internalformat);
    if (!info || !(info->colorRenderable || info->depthRenderable || info->stencilRenderable))
        return ctx.setError(GL_INVALID_ENUM);
    if (bufSize < 0) return ctx.setError(GL_INVALID_VALUE);
    // Supported counts, highest first (GL_MAX_INTEGER_SAMPLES limits integer formats).
    std::vector<GLint> counts;
    const uint32_t max = Device::get().caps().maxSamples;  // = GL_MAX_INTEGER_SAMPLES too
    const DeviceCaps& caps = Device::get().caps();
    for (uint32_t s = 8; s >= 2; s /= 2)
        if (s <= max && caps.sampleCountSupported[s]) counts.push_back(static_cast<GLint>(s));
    if (pname == GL_NUM_SAMPLE_COUNTS) {
        if (bufSize >= 1 && params) params[0] = static_cast<GLint>(counts.size());
        return;
    }
    for (size_t i = 0; i < counts.size() && static_cast<GLsizei>(i) < bufSize; ++i)
        if (params) params[i] = counts[i];
}

// ------------------------------------------------------------ invalidation hints

namespace {

bool validAttachment(Context& ctx, GLenum target, GLenum attachment) {
    const bool defaultFramebuffer = target == GL_READ_FRAMEBUFFER ? !ctx.state.readFramebuffer : !ctx.state.drawFramebuffer;
    if (defaultFramebuffer) return attachment == GL_COLOR || attachment == GL_DEPTH || attachment == GL_STENCIL;
    if (attachment == GL_DEPTH_ATTACHMENT || attachment == GL_STENCIL_ATTACHMENT || attachment == GL_DEPTH_STENCIL_ATTACHMENT)
        return true;
    return attachment >= GL_COLOR_ATTACHMENT0 && attachment < GL_COLOR_ATTACHMENT0 + Framebuffer::kMaxColor;
}

void invalidateFramebuffer(Context& ctx, GLenum target, GLsizei count, const GLenum* attachments) {
    if (target != GL_FRAMEBUFFER && target != GL_DRAW_FRAMEBUFFER && target != GL_READ_FRAMEBUFFER)
        return ctx.setError(GL_INVALID_ENUM);
    if (count < 0) return ctx.setError(GL_INVALID_VALUE);
    const bool defaultFramebuffer = target == GL_READ_FRAMEBUFFER ? !ctx.state.readFramebuffer : !ctx.state.drawFramebuffer;
    for (GLsizei i = 0; i < count; ++i) {
        const GLenum a = attachments[i];
        if (!validAttachment(ctx, target, a)) {
            // COLOR_ATTACHMENTi beyond MAX_COLOR_ATTACHMENTS of a framebuffer object is INVALID_OPERATION.
            const bool colorBeyond = !defaultFramebuffer && a >= GL_COLOR_ATTACHMENT0 && a <= GL_COLOR_ATTACHMENT31;
            return ctx.setError(colorBeyond ? GL_INVALID_OPERATION : GL_INVALID_ENUM);
        }
    }
}

Texture* invalidatedTexture(Context& ctx, GLuint name, GLint level) {
    Texture* tex = ctx.texture(name);
    if (!name || !tex || level < 0 || level >= Texture::kMaxLevels) {
        ctx.setError(GL_INVALID_VALUE);
        return nullptr;
    }
    if ((tex->target == GL_TEXTURE_RECTANGLE || tex->target == GL_TEXTURE_BUFFER || tex->target == GL_TEXTURE_2D_MULTISAMPLE ||
         tex->target == GL_TEXTURE_2D_MULTISAMPLE_ARRAY) && level != 0) {
        ctx.setError(GL_INVALID_VALUE);
        return nullptr;
    }
    return tex;
}

}  // namespace

void InvalidateFramebuffer(Context& ctx, GLenum target, GLsizei numAttachments, const GLenum* attachments) {
    invalidateFramebuffer(ctx, target, numAttachments, attachments);
}

void InvalidateSubFramebuffer(Context& ctx, GLenum target, GLsizei numAttachments, const GLenum* attachments, GLint x,
                              GLint y, GLsizei width, GLsizei height) {
    if (width < 0 || height < 0) return ctx.setError(GL_INVALID_VALUE);
    (void)x;
    (void)y;
    invalidateFramebuffer(ctx, target, numAttachments, attachments);
}

void InvalidateTexImage(Context& ctx, GLuint texture, GLint level) { invalidatedTexture(ctx, texture, level); }

void InvalidateTexSubImage(Context& ctx, GLuint texture, GLint level, GLint xoffset, GLint yoffset, GLint zoffset,
                           GLsizei width, GLsizei height, GLsizei depth) {
    Texture* tex = invalidatedTexture(ctx, texture, level);
    if (!tex) return;
    if (width < 0 || height < 0 || depth < 0) return ctx.setError(GL_INVALID_VALUE);
    const TextureImage& image = tex->image(0, level);
    if (xoffset < 0 || yoffset < 0 || zoffset < 0 || xoffset + width > image.width ||
        yoffset + height > std::max(image.height, 1) || zoffset + depth > std::max(image.depth, tex->faceCount()))
        return ctx.setError(GL_INVALID_VALUE);
}

void InvalidateBufferData(Context& ctx, GLuint buffer) {
    Buffer* b = ctx.buffer(buffer);
    if (!b) return ctx.setError(GL_INVALID_VALUE);
    if (b->mappedNonPersistent()) ctx.setError(GL_INVALID_OPERATION);
}

void InvalidateBufferSubData(Context& ctx, GLuint buffer, GLintptr offset, GLsizeiptr length) {
    Buffer* b = ctx.buffer(buffer);
    if (!b || offset < 0 || length < 0 || offset + length > b->size) return ctx.setError(GL_INVALID_VALUE);
    if (b->mappedNonPersistent()) ctx.setError(GL_INVALID_OPERATION);
}

}  // namespace wmgl::api
