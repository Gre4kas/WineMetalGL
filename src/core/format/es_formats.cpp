#include "core/format/es_formats.h"

#include <iterator>

namespace wmgl::es {
namespace {

constexpr GLenum kBgra8 = 0x93A1;  // GL_BGRA8_EXT

struct Row {
    GLenum format, type, internal;
};

// OpenGL ES 3.2 table 8.2 (sized internal formats), with EXT_texture_norm16.
const Row kSized[] = {
    {GL_RGBA, GL_UNSIGNED_BYTE, GL_RGBA8},
    {GL_RGBA, GL_UNSIGNED_BYTE, GL_RGB5_A1},
    {GL_RGBA, GL_UNSIGNED_BYTE, GL_RGBA4},
    {GL_RGBA, GL_UNSIGNED_BYTE, GL_SRGB8_ALPHA8},
    {GL_RGBA, GL_BYTE, GL_RGBA8_SNORM},
    {GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4, GL_RGBA4},
    {GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1, GL_RGB5_A1},
    {GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV, GL_RGB10_A2},
    {GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV, GL_RGB5_A1},
    {GL_RGBA, GL_HALF_FLOAT, GL_RGBA16F},
    {GL_RGBA, GL_FLOAT, GL_RGBA32F},
    {GL_RGBA, GL_FLOAT, GL_RGBA16F},
    {GL_RGBA, GL_UNSIGNED_SHORT, GL_RGBA16},
    {GL_RGBA, GL_SHORT, GL_RGBA16_SNORM},
    {GL_RGBA_INTEGER, GL_UNSIGNED_BYTE, GL_RGBA8UI},
    {GL_RGBA_INTEGER, GL_BYTE, GL_RGBA8I},
    {GL_RGBA_INTEGER, GL_UNSIGNED_SHORT, GL_RGBA16UI},
    {GL_RGBA_INTEGER, GL_SHORT, GL_RGBA16I},
    {GL_RGBA_INTEGER, GL_UNSIGNED_INT, GL_RGBA32UI},
    {GL_RGBA_INTEGER, GL_INT, GL_RGBA32I},
    {GL_RGBA_INTEGER, GL_UNSIGNED_INT_2_10_10_10_REV, GL_RGB10_A2UI},
    {GL_RGB, GL_UNSIGNED_BYTE, GL_RGB8},
    {GL_RGB, GL_UNSIGNED_BYTE, GL_RGB565},
    {GL_RGB, GL_UNSIGNED_BYTE, GL_SRGB8},
    {GL_RGB, GL_BYTE, GL_RGB8_SNORM},
    {GL_RGB, GL_UNSIGNED_SHORT_5_6_5, GL_RGB565},
    {GL_RGB, GL_UNSIGNED_INT_10F_11F_11F_REV, GL_R11F_G11F_B10F},
    {GL_RGB, GL_UNSIGNED_INT_5_9_9_9_REV, GL_RGB9_E5},
    {GL_RGB, GL_HALF_FLOAT, GL_RGB16F},
    {GL_RGB, GL_HALF_FLOAT, GL_R11F_G11F_B10F},
    {GL_RGB, GL_HALF_FLOAT, GL_RGB9_E5},
    {GL_RGB, GL_FLOAT, GL_RGB32F},
    {GL_RGB, GL_FLOAT, GL_RGB16F},
    {GL_RGB, GL_FLOAT, GL_R11F_G11F_B10F},
    {GL_RGB, GL_FLOAT, GL_RGB9_E5},
    {GL_RGB, GL_UNSIGNED_SHORT, GL_RGB16},
    {GL_RGB, GL_SHORT, GL_RGB16_SNORM},
    {GL_RGB_INTEGER, GL_UNSIGNED_BYTE, GL_RGB8UI},
    {GL_RGB_INTEGER, GL_BYTE, GL_RGB8I},
    {GL_RGB_INTEGER, GL_UNSIGNED_SHORT, GL_RGB16UI},
    {GL_RGB_INTEGER, GL_SHORT, GL_RGB16I},
    {GL_RGB_INTEGER, GL_UNSIGNED_INT, GL_RGB32UI},
    {GL_RGB_INTEGER, GL_INT, GL_RGB32I},
    {GL_RG, GL_UNSIGNED_BYTE, GL_RG8},
    {GL_RG, GL_BYTE, GL_RG8_SNORM},
    {GL_RG, GL_HALF_FLOAT, GL_RG16F},
    {GL_RG, GL_FLOAT, GL_RG32F},
    {GL_RG, GL_FLOAT, GL_RG16F},
    {GL_RG, GL_UNSIGNED_SHORT, GL_RG16},
    {GL_RG, GL_SHORT, GL_RG16_SNORM},
    {GL_RG_INTEGER, GL_UNSIGNED_BYTE, GL_RG8UI},
    {GL_RG_INTEGER, GL_BYTE, GL_RG8I},
    {GL_RG_INTEGER, GL_UNSIGNED_SHORT, GL_RG16UI},
    {GL_RG_INTEGER, GL_SHORT, GL_RG16I},
    {GL_RG_INTEGER, GL_UNSIGNED_INT, GL_RG32UI},
    {GL_RG_INTEGER, GL_INT, GL_RG32I},
    {GL_RED, GL_UNSIGNED_BYTE, GL_R8},
    {GL_RED, GL_BYTE, GL_R8_SNORM},
    {GL_RED, GL_HALF_FLOAT, GL_R16F},
    {GL_RED, GL_FLOAT, GL_R32F},
    {GL_RED, GL_FLOAT, GL_R16F},
    {GL_RED, GL_UNSIGNED_SHORT, GL_R16},
    {GL_RED, GL_SHORT, GL_R16_SNORM},
    {GL_RED_INTEGER, GL_UNSIGNED_BYTE, GL_R8UI},
    {GL_RED_INTEGER, GL_BYTE, GL_R8I},
    {GL_RED_INTEGER, GL_UNSIGNED_SHORT, GL_R16UI},
    {GL_RED_INTEGER, GL_SHORT, GL_R16I},
    {GL_RED_INTEGER, GL_UNSIGNED_INT, GL_R32UI},
    {GL_RED_INTEGER, GL_INT, GL_R32I},
    {GL_DEPTH_COMPONENT, GL_UNSIGNED_SHORT, GL_DEPTH_COMPONENT16},
    {GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, GL_DEPTH_COMPONENT24},
    {GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, GL_DEPTH_COMPONENT16},
    {GL_DEPTH_COMPONENT, GL_FLOAT, GL_DEPTH_COMPONENT32F},
    {GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, GL_DEPTH_COMPONENT32},  // OES_depth32, OES_required_internalformat
    {GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, GL_DEPTH24_STENCIL8},
    {GL_DEPTH_STENCIL, GL_FLOAT_32_UNSIGNED_INT_24_8_REV, GL_DEPTH32F_STENCIL8},
    {GL_STENCIL_INDEX, GL_UNSIGNED_BYTE, GL_STENCIL_INDEX8},  // OpenGL ES 3.2, OES_texture_stencil8
    // OES_required_internalformat with EXT_texture_type_2_10_10_10_REV
    {GL_RGB, GL_UNSIGNED_INT_2_10_10_10_REV, GL_RGB8},
    {GL_RGB, GL_UNSIGNED_INT_2_10_10_10_REV, GL_RGB565},
};

// Table 8.3 (unsized internal formats: internalformat == format) and the
// extensions' unsized formats, with the sized format each makes.
const Row kUnsized[] = {
    {GL_RGBA, GL_UNSIGNED_BYTE, GL_RGBA8},
    {GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4, GL_RGBA4},
    {GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1, GL_RGB5_A1},
    {GL_RGB, GL_UNSIGNED_BYTE, GL_RGB8},
    {GL_RGB, GL_UNSIGNED_SHORT_5_6_5, GL_RGB565},
    {GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, GL_LUMINANCE8_ALPHA8},
    {GL_LUMINANCE, GL_UNSIGNED_BYTE, GL_LUMINANCE8},
    {GL_ALPHA, GL_UNSIGNED_BYTE, GL_ALPHA8},
    // OES_texture_float
    {GL_RGBA, GL_FLOAT, GL_RGBA32F},
    {GL_RGB, GL_FLOAT, GL_RGB32F},
    {GL_LUMINANCE_ALPHA, GL_FLOAT, GL_LUMINANCE_ALPHA32F_ARB},
    {GL_LUMINANCE, GL_FLOAT, GL_LUMINANCE32F_ARB},
    {GL_ALPHA, GL_FLOAT, GL_ALPHA32F_ARB},
    // OES_texture_half_float (its HALF_FLOAT_OES, not OpenGL ES 3's HALF_FLOAT)
    {GL_RGBA, kHalfFloatOes, GL_RGBA16F},
    {GL_RGB, kHalfFloatOes, GL_RGB16F},
    {GL_LUMINANCE_ALPHA, kHalfFloatOes, GL_LUMINANCE_ALPHA16F_ARB},
    {GL_LUMINANCE, kHalfFloatOes, GL_LUMINANCE16F_ARB},
    {GL_ALPHA, kHalfFloatOes, GL_ALPHA16F_ARB},
    // EXT_texture_rg (with the float extensions)
    {GL_RED, GL_UNSIGNED_BYTE, GL_R8},
    {GL_RG, GL_UNSIGNED_BYTE, GL_RG8},
    {GL_RED, GL_FLOAT, GL_R32F},
    {GL_RG, GL_FLOAT, GL_RG32F},
    {GL_RED, kHalfFloatOes, GL_R16F},
    {GL_RG, kHalfFloatOes, GL_RG16F},
    // EXT_sRGB
    {GL_SRGB, GL_UNSIGNED_BYTE, GL_SRGB8},
    {GL_SRGB_ALPHA, GL_UNSIGNED_BYTE, GL_SRGB8_ALPHA8},
    // OES_depth_texture, OES_packed_depth_stencil
    {GL_DEPTH_COMPONENT, GL_UNSIGNED_SHORT, GL_DEPTH_COMPONENT16},
    {GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, GL_DEPTH_COMPONENT24},
    {GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, GL_DEPTH24_STENCIL8},
    // EXT_texture_format_BGRA8888
    {kBgra, GL_UNSIGNED_BYTE, kBgra8},
    // EXT_texture_type_2_10_10_10_REV (its RGB images are not color-renderable)
    {GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV, GL_RGB10_A2},
    {GL_RGB, GL_UNSIGNED_INT_2_10_10_10_REV, GL_RGB10},
};

// Sized formats glTexImage takes without a table 8.2 row of their own:
// the luminance and alpha formats of EXT_texture_storage, for which the
// unsized table's format and type apply.
bool sizedLegacy(GLenum internal) {
    switch (internal) {
    case GL_RGB10: case GL_ALPHA8: case GL_LUMINANCE8: case GL_LUMINANCE8_ALPHA8: case GL_ALPHA32F_ARB: case GL_LUMINANCE32F_ARB:
    case GL_LUMINANCE_ALPHA32F_ARB: case GL_ALPHA16F_ARB: case GL_LUMINANCE16F_ARB: case GL_LUMINANCE_ALPHA16F_ARB:
    case kBgra8:
        return true;
    default: return false;
    }
}

bool unsizedFormat(GLenum internal) {
    switch (internal) {
    case GL_RGBA: case GL_RGB: case GL_LUMINANCE_ALPHA: case GL_LUMINANCE: case GL_ALPHA: case GL_RED: case GL_RG:
    case GL_SRGB: case GL_SRGB_ALPHA: case GL_DEPTH_COMPONENT: case GL_DEPTH_STENCIL: case kBgra:
        return true;
    default: return false;
    }
}

bool sizedFormat(GLenum internal) {
    for (const Row& r : kSized)
        if (r.internal == internal) return true;
    return sizedLegacy(internal);
}

GLenum normalType(GLenum type) { return type == kHalfFloatOes ? GL_HALF_FLOAT : type; }

}  // namespace

bool isFormat(GLenum format) {
    switch (format) {
    case GL_RGBA: case GL_RGB: case GL_RG: case GL_RED: case GL_RGBA_INTEGER: case GL_RGB_INTEGER: case GL_RG_INTEGER:
    case GL_RED_INTEGER: case GL_LUMINANCE_ALPHA: case GL_LUMINANCE: case GL_ALPHA: case GL_DEPTH_COMPONENT:
    case GL_DEPTH_STENCIL: case GL_STENCIL_INDEX: case GL_SRGB: case GL_SRGB_ALPHA: case kBgra:
        return true;
    default: return false;
    }
}

bool isType(GLenum type) {
    switch (type) {
    case GL_UNSIGNED_BYTE: case GL_BYTE: case GL_UNSIGNED_SHORT: case GL_SHORT: case GL_UNSIGNED_INT: case GL_INT:
    case GL_HALF_FLOAT: case kHalfFloatOes: case GL_FLOAT: case GL_UNSIGNED_SHORT_5_6_5: case GL_UNSIGNED_SHORT_4_4_4_4:
    case GL_UNSIGNED_SHORT_5_5_5_1: case GL_UNSIGNED_INT_2_10_10_10_REV: case GL_UNSIGNED_INT_10F_11F_11F_REV:
    case GL_UNSIGNED_INT_5_9_9_9_REV: case GL_UNSIGNED_INT_24_8: case GL_FLOAT_32_UNSIGNED_INT_24_8_REV:
        return true;
    default: return false;
    }
}

GLenum texImageFormat(GLenum internalformat, GLenum format, GLenum& type, GLenum& error) {
    error = GL_NO_ERROR;
    if (!isFormat(format) || !isType(type)) {
        error = GL_INVALID_ENUM;
        return 0;
    }
    const GLenum given = type;
    type = normalType(type);
    if (unsizedFormat(internalformat)) {
        if (internalformat == format)
            for (const Row& r : kUnsized)
                if (r.format == format && r.type == given) return r.internal;
        error = GL_INVALID_OPERATION;
        return 0;
    }
    if (!sizedFormat(internalformat)) {
        error = GL_INVALID_VALUE;
        return 0;
    }
    for (const Row& r : kSized)
        if (r.internal == internalformat && r.format == format && r.type == given) return internalformat;
    // The luminance/alpha sized formats take their unsized format's data.
    for (const Row& r : kUnsized)
        if (r.internal == internalformat && r.format == format && r.type == given) return internalformat;
    error = GL_INVALID_OPERATION;
    return 0;
}

bool texSubImageCompatible(GLenum internalformat, GLenum format, GLenum& type, GLenum& error) {
    error = GL_NO_ERROR;
    if (!isFormat(format) || !isType(type)) {
        error = GL_INVALID_ENUM;
        return false;
    }
    const GLenum given = type;
    type = normalType(type);
    for (const Row& r : kSized)
        if (r.internal == internalformat && r.format == format && r.type == given) return true;
    for (const Row& r : kUnsized)
        if (r.internal == internalformat && r.format == format && r.type == given) return true;
    error = GL_INVALID_OPERATION;
    return false;
}

bool textureStorageFormat(GLenum internalformat) { return sizedFormat(internalformat); }

bool colorRenderable(GLenum f) {
    switch (f) {
    case GL_R8: case GL_RG8: case GL_RGB8: case GL_RGB565: case GL_RGBA4: case GL_RGB5_A1: case GL_RGBA8:
    case GL_RGB10_A2: case GL_RGB10_A2UI: case GL_SRGB8_ALPHA8: case GL_R8I: case GL_R8UI: case GL_R16I:
    case GL_R16UI: case GL_R32I: case GL_R32UI: case GL_RG8I: case GL_RG8UI: case GL_RG16I: case GL_RG16UI:
    case GL_RG32I: case GL_RG32UI: case GL_RGBA8I: case GL_RGBA8UI: case GL_RGBA16I: case GL_RGBA16UI:
    case GL_RGBA32I: case GL_RGBA32UI: case kBgra8:
    // EXT_color_buffer_float (and EXT_color_buffer_half_float's RGB16F)
    case GL_R16F: case GL_RG16F: case GL_RGBA16F: case GL_R32F: case GL_RG32F: case GL_RGBA32F:
    case GL_R11F_G11F_B10F: case GL_RGB16F:
    // EXT_texture_norm16, EXT_render_snorm
    case GL_R16: case GL_RG16: case GL_RGBA16: case GL_R8_SNORM: case GL_RG8_SNORM: case GL_RGBA8_SNORM:
    case GL_R16_SNORM: case GL_RG16_SNORM: case GL_RGBA16_SNORM:
        return true;
    default: return false;
    }
}

bool depthOrStencilRenderable(GLenum f) {
    switch (f) {
    case GL_DEPTH_COMPONENT16: case GL_DEPTH_COMPONENT24: case GL_DEPTH_COMPONENT32: case GL_DEPTH_COMPONENT32F:
    case GL_DEPTH24_STENCIL8: case GL_DEPTH32F_STENCIL8: case GL_STENCIL_INDEX8:
        return true;
    default: return false;
    }
}

bool renderbufferFormat(GLenum f) {
    // Every color-renderable format but the texture-only RGB16F, and the
    // depth and stencil formats (DEPTH_COMPONENT32: OES_depth32).
    return (colorRenderable(f) && f != GL_RGB16F) || depthOrStencilRenderable(f);
}

bool readPixelsCompatible(GLenum internalformat, GLenum format, GLenum type) {
    switch (internalformat) {
    case GL_R8I: case GL_R16I: case GL_R32I: case GL_RG8I: case GL_RG16I: case GL_RG32I: case GL_RGBA8I:
    case GL_RGBA16I: case GL_RGBA32I:
        return format == GL_RGBA_INTEGER && type == GL_INT;
    case GL_R8UI: case GL_R16UI: case GL_R32UI: case GL_RG8UI: case GL_RG16UI: case GL_RG32UI: case GL_RGBA8UI:
    case GL_RGBA16UI: case GL_RGBA32UI: case GL_RGB10_A2UI:
        return format == GL_RGBA_INTEGER && type == GL_UNSIGNED_INT;
    case GL_R16F: case GL_RG16F: case GL_RGBA16F: case GL_R32F: case GL_RG32F: case GL_RGBA32F:
    case GL_R11F_G11F_B10F: case GL_RGB16F:
        return format == GL_RGBA && type == GL_FLOAT;
    case GL_RGB10_A2:
        return format == GL_RGBA && (type == GL_UNSIGNED_BYTE || type == GL_UNSIGNED_INT_2_10_10_10_REV);
    case GL_R16: case GL_RG16: case GL_RGBA16:
        return format == GL_RGBA && (type == GL_UNSIGNED_BYTE || type == GL_UNSIGNED_SHORT);
    case GL_R8_SNORM: case GL_RG8_SNORM: case GL_RGBA8_SNORM:
        return format == GL_RGBA && type == GL_BYTE;
    case GL_R16_SNORM: case GL_RG16_SNORM: case GL_RGBA16_SNORM:
        return format == GL_RGBA && (type == GL_BYTE || type == GL_SHORT);
    default:  // normalized fixed point
        return format == GL_RGBA && type == GL_UNSIGNED_BYTE;
    }
}

namespace {

// The base formats a read buffer's components can make (table 8.21: a
// destination's components must all be in the source).
bool baseCompatible(GLenum source, GLenum dest) {
    auto has = [](GLenum base, char c) {
        switch (base) {
        case GL_RED: return c == 'r';
        case GL_RG: return c == 'r' || c == 'g';
        case GL_RGB: return c != 'a';
        case GL_RGBA: return true;
        default: return false;
        }
    };
    switch (dest) {
    case GL_ALPHA: return has(source, 'a');
    case GL_LUMINANCE: case GL_RED: return has(source, 'r');
    case GL_LUMINANCE_ALPHA: return has(source, 'r') && has(source, 'a');
    case GL_RG: return has(source, 'g');
    case GL_RGB: return has(source, 'b');
    case GL_RGBA: return has(source, 'a') && has(source, 'b');
    default: return false;
    }
}

bool sameComponentType(const FormatInfo& a, const FormatInfo& b) { return a.type == b.type && a.srgb == b.srgb; }

}  // namespace

GLenum copyTexImageFormat(const FormatInfo& source, GLenum internalformat, GLenum& error) {
    error = GL_INVALID_OPERATION;
    switch (internalformat) {
    case GL_RGBA: case GL_RGB: case GL_LUMINANCE_ALPHA: case GL_LUMINANCE: case GL_ALPHA: {
        // Unsized: from normalized fixed-point (linear) sources, the sized
        // format of the source's component sizes (table 8.22).
        if (source.type != ComponentType::UnsignedNormalized || source.srgb || !baseCompatible(source.baseFormat, internalformat))
            return 0;
        const int r = source.redBits, a = source.alphaBits;
        switch (internalformat) {
        case GL_LUMINANCE: return GL_LUMINANCE8;
        case GL_ALPHA: return GL_ALPHA8;
        case GL_LUMINANCE_ALPHA: return a == 1 || a >= 4 ? GL_LUMINANCE8_ALPHA8 : 0;
        case GL_RGB: return r <= 5 ? GL_RGB565 : GL_RGB8;
        default:  // GL_RGBA
            return r <= 4 && a <= 4 ? GL_RGBA4 : r == 5 && a == 1 ? GL_RGB5_A1 : r <= 8 && a >= 2 && a <= 8 ? GL_RGBA8 : 0;
        }
    }
    default: break;
    }
    const FormatInfo* dest = formatInfo(internalformat);
    if (!dest || dest->internalFormat != internalformat || dest->compressed || dest->isDepthOrStencil() ||
        dest->luminanceBits || dest->intensityBits || (dest->baseFormat == GL_ALPHA) || !sizedFormat(internalformat)) {
        error = GL_INVALID_VALUE;
        return 0;
    }
    // Sized: the same component types, and each of the destination's
    // components exactly as large as the source's.
    if (!sameComponentType(source, *dest) || !baseCompatible(source.baseFormat, dest->baseFormat) || dest->sharedBits ||
        source.sharedBits)
        return 0;
    if ((dest->redBits && dest->redBits != source.redBits) || (dest->greenBits && dest->greenBits != source.greenBits) ||
        (dest->blueBits && dest->blueBits != source.blueBits) || (dest->alphaBits && dest->alphaBits != source.alphaBits))
        return 0;
    return internalformat;
}

bool copyTexSubImageCompatible(const FormatInfo& source, const FormatInfo& dest) {
    if (dest.compressed || dest.isDepthOrStencil()) return false;
    const GLenum base = dest.luminanceBits && dest.alphaBits ? GL_LUMINANCE_ALPHA
                        : dest.luminanceBits                 ? GL_LUMINANCE
                                                             : dest.baseFormat;
    if (!baseCompatible(source.baseFormat, base)) return false;
    // Fixed-point destinations take fixed-point (and, with
    // EXT_color_buffer_float, nothing else) sources.
    const bool fixed = [](const FormatInfo& f) {
        return f.type == ComponentType::UnsignedNormalized || f.type == ComponentType::SignedNormalized;
    }(dest);
    if (fixed) return source.type == ComponentType::UnsignedNormalized && source.srgb == dest.srgb;
    return sameComponentType(source, dest);
}

}  // namespace wmgl::es
