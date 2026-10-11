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
#include "core/limits.h"
#include "core/metal/device.h"
#include "core/program.h"
#include "core/texture_storage.h"
#include "core/util/log.h"
#include "gl_api.gen.h"

namespace wmgl::api {
namespace {

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
    return transferLayout(format, type, width, height, depth, pixelStoreParams(ctx.state.pack, threeD)).bytesNeeded;
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

namespace {

// GL 4.2 image formats (GL 4.3 table 8.27): texel bits and compatibility class.
bool imageFormat(GLenum f, GLint& bits, GLenum& imageClass) {
    switch (f) {
    case GL_RGBA32F: case GL_RGBA32UI: case GL_RGBA32I: bits = 128; imageClass = GL_IMAGE_CLASS_4_X_32; return true;
    case GL_RG32F: case GL_RG32UI: case GL_RG32I: bits = 64; imageClass = GL_IMAGE_CLASS_2_X_32; return true;
    case GL_R32F: case GL_R32UI: case GL_R32I: bits = 32; imageClass = GL_IMAGE_CLASS_1_X_32; return true;
    case GL_RGBA16F: case GL_RGBA16UI: case GL_RGBA16I: case GL_RGBA16: case GL_RGBA16_SNORM:
        bits = 64; imageClass = GL_IMAGE_CLASS_4_X_16; return true;
    case GL_RG16F: case GL_RG16UI: case GL_RG16I: case GL_RG16: case GL_RG16_SNORM:
        bits = 32; imageClass = GL_IMAGE_CLASS_2_X_16; return true;
    case GL_R16F: case GL_R16UI: case GL_R16I: case GL_R16: case GL_R16_SNORM: bits = 16; imageClass = GL_IMAGE_CLASS_1_X_16; return true;
    case GL_RGBA8UI: case GL_RGBA8I: case GL_RGBA8: case GL_RGBA8_SNORM: bits = 32; imageClass = GL_IMAGE_CLASS_4_X_8; return true;
    case GL_RG8UI: case GL_RG8I: case GL_RG8: case GL_RG8_SNORM: bits = 16; imageClass = GL_IMAGE_CLASS_2_X_8; return true;
    case GL_R8UI: case GL_R8I: case GL_R8: case GL_R8_SNORM: bits = 8; imageClass = GL_IMAGE_CLASS_1_X_8; return true;
    case GL_R11F_G11F_B10F: bits = 32; imageClass = GL_IMAGE_CLASS_11_11_10; return true;
    case GL_RGB10_A2UI: case GL_RGB10_A2: bits = 32; imageClass = GL_IMAGE_CLASS_10_10_10_2; return true;
    default: return false;
    }
}

// The view compatibility class enum (GL 4.3 table 8.22), or GL_NONE.
GLenum viewClassEnum(GLenum f) {
    switch (f) {
    case GL_RGBA32F: case GL_RGBA32UI: case GL_RGBA32I: return GL_VIEW_CLASS_128_BITS;
    case GL_RGB32F: case GL_RGB32UI: case GL_RGB32I: return GL_VIEW_CLASS_96_BITS;
    case GL_RGBA16F: case GL_RG32F: case GL_RGBA16UI: case GL_RG32UI: case GL_RGBA16I: case GL_RG32I: case GL_RGBA16:
    case GL_RGBA16_SNORM: return GL_VIEW_CLASS_64_BITS;
    case GL_RGB16: case GL_RGB16_SNORM: case GL_RGB16F: case GL_RGB16UI: case GL_RGB16I: return GL_VIEW_CLASS_48_BITS;
    case GL_RG16F: case GL_R11F_G11F_B10F: case GL_R32F: case GL_RGB10_A2UI: case GL_RGBA8UI: case GL_RG16UI: case GL_R32UI:
    case GL_RGBA8I: case GL_RG16I: case GL_R32I: case GL_RGB10_A2: case GL_RGBA8: case GL_RG16: case GL_RGBA8_SNORM:
    case GL_RG16_SNORM: case GL_SRGB8_ALPHA8: case GL_RGB9_E5: return GL_VIEW_CLASS_32_BITS;
    case GL_RGB8: case GL_RGB8_SNORM: case GL_SRGB8: case GL_RGB8UI: case GL_RGB8I: return GL_VIEW_CLASS_24_BITS;
    case GL_R16F: case GL_RG8UI: case GL_R16UI: case GL_RG8I: case GL_R16I: case GL_RG8: case GL_R16: case GL_RG8_SNORM:
    case GL_R16_SNORM: return GL_VIEW_CLASS_16_BITS;
    case GL_R8UI: case GL_R8I: case GL_R8: case GL_R8_SNORM: return GL_VIEW_CLASS_8_BITS;
    case GL_COMPRESSED_RED_RGTC1: case GL_COMPRESSED_SIGNED_RED_RGTC1: return GL_VIEW_CLASS_RGTC1_RED;
    case GL_COMPRESSED_RG_RGTC2: case GL_COMPRESSED_SIGNED_RG_RGTC2: return GL_VIEW_CLASS_RGTC2_RG;
    case GL_COMPRESSED_RGBA_BPTC_UNORM: case GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM: return GL_VIEW_CLASS_BPTC_UNORM;
    case GL_COMPRESSED_RGB_BPTC_SIGNED_FLOAT: case GL_COMPRESSED_RGB_BPTC_UNSIGNED_FLOAT: return GL_VIEW_CLASS_BPTC_FLOAT;
    default: return GL_NONE;
    }
}

GLenum componentType(ComponentType t) {
    switch (t) {
    case ComponentType::SignedNormalized: return GL_SIGNED_NORMALIZED;
    case ComponentType::UnsignedNormalized: return GL_UNSIGNED_NORMALIZED;
    case ComponentType::Float: return GL_FLOAT;
    case ComponentType::Int: return GL_INT;
    case ComponentType::UnsignedInt: return GL_UNSIGNED_INT;
    default: return GL_NONE;
    }
}

// GL 4.3 section 22.3 (GL_ARB_internalformat_query2): `pname` of
// `internalformat` used with `target`. False for an unknown pname.
bool internalformatQuery(Context& ctx, GLenum target, GLenum internalformat, GLenum pname, std::vector<int64_t>& out) {
    const FormatInfo* f = formatInfo(internalformat);
    if (f && f->internalFormat != internalformat) f = nullptr;
    const bool multisample = target == GL_TEXTURE_2D_MULTISAMPLE || target == GL_TEXTURE_2D_MULTISAMPLE_ARRAY;
    const bool renderable = f && (f->colorRenderable || f->depthRenderable || f->stencilRenderable);
    bool supported = f != nullptr;
    if (supported) {
        if (target == GL_RENDERBUFFER || multisample) supported = renderable && !f->compressed;
        else if (target == GL_TEXTURE_BUFFER)  // RGB only as RGB32 (GL_ARB_texture_buffer_object_rgb32)
            supported = !f->compressed && !f->isDepthOrStencil() &&
                        (f->baseFormat != GL_RGB || internalformat == GL_RGB32F || internalformat == GL_RGB32I ||
                         internalformat == GL_RGB32UI);
        else if (f->compressed) supported = target == GL_TEXTURE_2D || target == GL_TEXTURE_2D_ARRAY || target == GL_TEXTURE_CUBE_MAP ||
                                            target == GL_TEXTURE_CUBE_MAP_ARRAY;
        else if (f->isDepthOrStencil()) supported = target != GL_TEXTURE_3D;
        if (f->compressed && supported) supported = compressedFormatSupported(ctx, f);
    }
    const bool texture = target != GL_RENDERBUFFER;
    const bool layered = target == GL_TEXTURE_1D_ARRAY || target == GL_TEXTURE_2D_ARRAY || target == GL_TEXTURE_CUBE_MAP_ARRAY ||
                         target == GL_TEXTURE_2D_MULTISAMPLE_ARRAY;
    const bool mipmaps = texture && !multisample && target != GL_TEXTURE_RECTANGLE && target != GL_TEXTURE_BUFFER;
    auto full = [](bool b) -> int64_t { return b ? GL_FULL_SUPPORT : GL_NONE; };
    auto one = [&](int64_t v) { out.assign(1, v); };
    GLint imageBits = 0;
    GLenum imageClass = GL_NONE;
    const bool image = supported && texture && !multisample && imageFormat(internalformat, imageBits, imageClass);
    switch (pname) {
    case GL_INTERNALFORMAT_SUPPORTED: one(supported ? GL_TRUE : GL_FALSE); return true;
    case GL_INTERNALFORMAT_PREFERRED: one(supported ? internalformat : GL_NONE); return true;
    case GL_INTERNALFORMAT_RED_SIZE: one(supported ? f->redBits : 0); return true;
    case GL_INTERNALFORMAT_GREEN_SIZE: one(supported ? f->greenBits : 0); return true;
    case GL_INTERNALFORMAT_BLUE_SIZE: one(supported ? f->blueBits : 0); return true;
    case GL_INTERNALFORMAT_ALPHA_SIZE: one(supported ? f->alphaBits : 0); return true;
    case GL_INTERNALFORMAT_DEPTH_SIZE: one(supported ? f->depthBits : 0); return true;
    case GL_INTERNALFORMAT_STENCIL_SIZE: one(supported ? f->stencilBits : 0); return true;
    case GL_INTERNALFORMAT_SHARED_SIZE: one(supported ? f->sharedBits : 0); return true;
    case GL_INTERNALFORMAT_RED_TYPE: one(supported && f->redBits ? componentType(f->type) : GL_NONE); return true;
    case GL_INTERNALFORMAT_GREEN_TYPE: one(supported && f->greenBits ? componentType(f->type) : GL_NONE); return true;
    case GL_INTERNALFORMAT_BLUE_TYPE: one(supported && f->blueBits ? componentType(f->type) : GL_NONE); return true;
    case GL_INTERNALFORMAT_ALPHA_TYPE: one(supported && f->alphaBits ? componentType(f->type) : GL_NONE); return true;
    case GL_INTERNALFORMAT_DEPTH_TYPE:
        one(supported && f->depthBits ? (f->storage == Storage::Depth16Unorm ? GL_UNSIGNED_NORMALIZED : GL_FLOAT) : GL_NONE);
        return true;
    case GL_INTERNALFORMAT_STENCIL_TYPE: one(supported && f->stencilBits ? GL_UNSIGNED_INT : GL_NONE); return true;
    case GL_MAX_WIDTH: one(supported ? (target == GL_TEXTURE_BUFFER ? limits::kMaxTextureBufferSize : target == GL_TEXTURE_3D ? limits::kMax3DTextureSize : limits::kMaxTextureSize) : 0); return true;
    case GL_MAX_HEIGHT:
        one(supported && target != GL_TEXTURE_1D && target != GL_TEXTURE_1D_ARRAY && target != GL_TEXTURE_BUFFER
                ? (target == GL_TEXTURE_3D ? limits::kMax3DTextureSize : limits::kMaxTextureSize)
                : 0);
        return true;
    case GL_MAX_DEPTH: one(supported && target == GL_TEXTURE_3D ? limits::kMax3DTextureSize : 0); return true;
    case GL_MAX_LAYERS: one(supported && layered ? limits::kMaxArrayLayers : 0); return true;
    case GL_MAX_COMBINED_DIMENSIONS: {
        if (!supported) return one(0), true;
        int64_t v = target == GL_TEXTURE_BUFFER ? limits::kMaxTextureBufferSize : target == GL_TEXTURE_3D ? limits::kMax3DTextureSize : limits::kMaxTextureSize;
        if (target != GL_TEXTURE_1D && target != GL_TEXTURE_1D_ARRAY && target != GL_TEXTURE_BUFFER)
            v *= target == GL_TEXTURE_3D ? limits::kMax3DTextureSize : limits::kMaxTextureSize;
        if (target == GL_TEXTURE_3D) v *= limits::kMax3DTextureSize;
        if (layered) v *= limits::kMaxArrayLayers;
        if (target == GL_TEXTURE_CUBE_MAP) v *= 6;
        one(v);
        return true;
    }
    case GL_COLOR_COMPONENTS: one(supported && !f->isDepthOrStencil() ? GL_TRUE : GL_FALSE); return true;
    case GL_DEPTH_COMPONENTS: one(supported && f->depthBits ? GL_TRUE : GL_FALSE); return true;
    case GL_STENCIL_COMPONENTS: one(supported && f->stencilBits ? GL_TRUE : GL_FALSE); return true;
    case GL_COLOR_RENDERABLE: one(supported && f->colorRenderable ? GL_TRUE : GL_FALSE); return true;
    case GL_DEPTH_RENDERABLE: one(supported && f->depthRenderable ? GL_TRUE : GL_FALSE); return true;
    case GL_STENCIL_RENDERABLE: one(supported && f->stencilRenderable ? GL_TRUE : GL_FALSE); return true;
    case GL_FRAMEBUFFER_RENDERABLE: one(full(supported && renderable && target != GL_TEXTURE_BUFFER)); return true;
    case GL_FRAMEBUFFER_RENDERABLE_LAYERED:
        one(full(supported && renderable && (layered || target == GL_TEXTURE_3D || target == GL_TEXTURE_CUBE_MAP)));
        return true;
    case GL_FRAMEBUFFER_BLEND: one(full(supported && f->colorRenderable && !f->isInteger())); return true;
    case GL_READ_PIXELS: one(full(supported && renderable)); return true;
    case GL_READ_PIXELS_FORMAT: case GL_TEXTURE_IMAGE_FORMAT: case GL_GET_TEXTURE_IMAGE_FORMAT:
        one(supported ? f->defaultTransferFormat : GL_NONE);
        return true;
    case GL_READ_PIXELS_TYPE: case GL_TEXTURE_IMAGE_TYPE: case GL_GET_TEXTURE_IMAGE_TYPE:
        one(supported ? f->defaultTransferType : GL_NONE);
        return true;
    case GL_MIPMAP: one(supported && mipmaps ? GL_TRUE : GL_FALSE); return true;
    case GL_MANUAL_GENERATE_MIPMAP: case GL_AUTO_GENERATE_MIPMAP:
        one(full(supported && mipmaps && !f->compressed && !f->isInteger() && (f->colorRenderable || f->depthBits)));
        return true;
    case GL_COLOR_ENCODING: one(supported && !f->isDepthOrStencil() ? (f->srgb ? GL_SRGB : GL_LINEAR) : GL_NONE); return true;
    case GL_SRGB_READ: one(full(supported && f->srgb)); return true;
    case GL_SRGB_WRITE: one(full(supported && f->srgb && f->colorRenderable)); return true;
    case GL_FILTER: one(full(supported && texture && f->textureFilterable)); return true;
    case GL_VERTEX_TEXTURE: case GL_TESS_CONTROL_TEXTURE: case GL_TESS_EVALUATION_TEXTURE: case GL_GEOMETRY_TEXTURE:
    case GL_FRAGMENT_TEXTURE: case GL_COMPUTE_TEXTURE:
        one(full(supported && texture));
        return true;
    case GL_TEXTURE_SHADOW: case GL_TEXTURE_GATHER_SHADOW:
        one(full(supported && texture && !multisample && target != GL_TEXTURE_3D && target != GL_TEXTURE_BUFFER && f->depthBits));
        return true;
    case GL_TEXTURE_GATHER: one(full(supported && texture && !multisample && target != GL_TEXTURE_BUFFER)); return true;
    case GL_SHADER_IMAGE_LOAD: case GL_SHADER_IMAGE_STORE: one(full(image)); return true;
    case GL_SHADER_IMAGE_ATOMIC: one(full(image && (internalformat == GL_R32I || internalformat == GL_R32UI))); return true;
    case GL_IMAGE_TEXEL_SIZE: one(image ? imageBits : 0); return true;
    case GL_IMAGE_COMPATIBILITY_CLASS: one(image ? imageClass : GL_NONE); return true;
    case GL_IMAGE_PIXEL_FORMAT: one(image ? f->defaultTransferFormat : GL_NONE); return true;
    case GL_IMAGE_PIXEL_TYPE: one(image ? f->defaultTransferType : GL_NONE); return true;
    case GL_IMAGE_FORMAT_COMPATIBILITY_TYPE: one(image ? GL_IMAGE_FORMAT_COMPATIBILITY_BY_SIZE : GL_NONE); return true;
    case GL_SIMULTANEOUS_TEXTURE_AND_DEPTH_TEST: case GL_SIMULTANEOUS_TEXTURE_AND_STENCIL_TEST:
    case GL_SIMULTANEOUS_TEXTURE_AND_DEPTH_WRITE: case GL_SIMULTANEOUS_TEXTURE_AND_STENCIL_WRITE:
        one(GL_NONE);  // a feedback loop is undefined; not offered
        return true;
    case GL_TEXTURE_COMPRESSED: one(supported && f->compressed ? GL_TRUE : GL_FALSE); return true;
    case GL_TEXTURE_COMPRESSED_BLOCK_WIDTH: case GL_TEXTURE_COMPRESSED_BLOCK_HEIGHT:
        one(supported && f->compressed ? 4 : 0);
        return true;
    case GL_TEXTURE_COMPRESSED_BLOCK_SIZE: one(supported && f->compressed ? storageBytes(f->storage) : 0); return true;
    case GL_CLEAR_BUFFER:
        one(full(supported && target == GL_TEXTURE_BUFFER));
        return true;
    case GL_TEXTURE_VIEW: one(full(supported && texture && target != GL_TEXTURE_BUFFER)); return true;
    case GL_VIEW_COMPATIBILITY_CLASS: one(supported ? viewClassEnum(internalformat) : GL_NONE); return true;
    case GL_NUM_SAMPLE_COUNTS: case GL_SAMPLES: {
        std::vector<int64_t> counts;
        const DeviceCaps& caps = Device::get().caps();
        if (supported && (target == GL_RENDERBUFFER || multisample))
            for (uint32_t c = 8; c >= 2; c /= 2)
                if (c <= caps.maxSamples && caps.sampleCountSupported[c]) counts.push_back(c);
        if (pname == GL_NUM_SAMPLE_COUNTS) one(static_cast<int64_t>(counts.size()));
        else out = counts;
        return true;
    }
    default: return false;
    }
}

bool internalformatTarget(GLenum target) {
    switch (target) {
    case GL_TEXTURE_1D: case GL_TEXTURE_1D_ARRAY: case GL_TEXTURE_2D: case GL_TEXTURE_2D_ARRAY: case GL_TEXTURE_3D:
    case GL_TEXTURE_CUBE_MAP: case GL_TEXTURE_CUBE_MAP_ARRAY: case GL_TEXTURE_RECTANGLE: case GL_TEXTURE_BUFFER:
    case GL_RENDERBUFFER: case GL_TEXTURE_2D_MULTISAMPLE: case GL_TEXTURE_2D_MULTISAMPLE_ARRAY:
        return true;
    default: return false;
    }
}

template <class T>
void getInternalformat(Context& ctx, GLenum target, GLenum internalformat, GLenum pname, GLsizei bufSize, T* params) {
    if (!internalformatTarget(target)) return ctx.setError(GL_INVALID_ENUM);
    if (bufSize < 0) return ctx.setError(GL_INVALID_VALUE);
    std::vector<int64_t> values;
    if (!internalformatQuery(ctx, target, internalformat, pname, values)) return ctx.setError(GL_INVALID_ENUM);
    for (size_t i = 0; i < values.size() && static_cast<GLsizei>(i) < bufSize; ++i)
        if (params) params[i] = static_cast<T>(values[i]);
}

}  // namespace

void GetInternalformati64v(Context& ctx, GLenum target, GLenum internalformat, GLenum pname, GLsizei bufSize, GLint64* params) {
    if (ctx.version() < 43 && !ctx.hasExtension("GL_ARB_internalformat_query2")) return ctx.unimplemented("glGetInternalformati64v");
    getInternalformat(ctx, target, internalformat, pname, bufSize, params);
}

void GetInternalformativ(Context& ctx, GLenum target, GLenum internalformat, GLenum pname, GLsizei bufSize, GLint* params) {
    // GL 4.3 (GL_ARB_internalformat_query2): every target and pname.
    if (ctx.version() >= 43 || ctx.hasExtension("GL_ARB_internalformat_query2"))
        return getInternalformat(ctx, target, internalformat, pname, bufSize, params);
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
