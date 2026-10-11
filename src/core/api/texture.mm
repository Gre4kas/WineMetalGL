// Texture and sampler objects: binding, image specification, copies,
// read-back and parameters (GL 3.3 sections 3.8, 6.1.3 and 6.1.4).

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <type_traits>
#include <vector>

#include "core/context.h"
#include "core/draw.h"
#include "core/format/pixel_transfer.h"
#include "core/limits.h"
#include "core/metal/device.h"
#include "core/metal/pixel_format.h"
#include "core/texture_storage.h"
#include "core/util/log.h"
#include "gl_api.gen.h"

namespace wmgl {

int textureTargetIndex(GLenum target) {
    switch (target) {
    case GL_TEXTURE_1D: return kTex1D;
    case GL_TEXTURE_2D: return kTex2D;
    case GL_TEXTURE_3D: return kTex3D;
    case GL_TEXTURE_1D_ARRAY: return kTex1DArray;
    case GL_TEXTURE_2D_ARRAY: return kTex2DArray;
    case GL_TEXTURE_RECTANGLE: return kTexRectangle;
    case GL_TEXTURE_CUBE_MAP: return kTexCubeMap;
    case GL_TEXTURE_BUFFER: return kTexBuffer;
    case GL_TEXTURE_2D_MULTISAMPLE: return kTex2DMultisample;
    case GL_TEXTURE_2D_MULTISAMPLE_ARRAY: return kTex2DMultisampleArray;
    case GL_TEXTURE_CUBE_MAP_ARRAY: return kTexCubeMapArray;
    default: return -1;
    }
}

GLenum textureTargetEnum(int index) {
    static const GLenum kTargets[kTexTargetCount] = {
        GL_TEXTURE_1D, GL_TEXTURE_2D, GL_TEXTURE_3D, GL_TEXTURE_1D_ARRAY, GL_TEXTURE_2D_ARRAY, GL_TEXTURE_RECTANGLE,
        GL_TEXTURE_CUBE_MAP, GL_TEXTURE_BUFFER, GL_TEXTURE_2D_MULTISAMPLE, GL_TEXTURE_2D_MULTISAMPLE_ARRAY,
        GL_TEXTURE_CUBE_MAP_ARRAY};
    return index >= 0 && index < kTexTargetCount ? kTargets[index] : 0;
}

}  // namespace wmgl

namespace wmgl::api {
namespace {

using namespace wmgl::limits;

// ------------------------------------------------------------ targets

bool targetSupported(Context& ctx, GLenum target) {
    if (target == GL_TEXTURE_CUBE_MAP_ARRAY) return ctx.version() >= 40;
    return textureTargetIndex(target) >= 0;
}

Texture* boundTexture(Context& ctx, GLenum target) {
    const int index = textureTargetIndex(target);
    if (index < 0) return nullptr;
    return ctx.state.textureUnits[ctx.state.activeTexture].bound[index].get();
}

bool isCubeFace(GLenum target) {
    return target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z;
}

GLenum proxyBase(GLenum target) {
    switch (target) {
    case GL_PROXY_TEXTURE_1D: return GL_TEXTURE_1D;
    case GL_PROXY_TEXTURE_2D: return GL_TEXTURE_2D;
    case GL_PROXY_TEXTURE_3D: return GL_TEXTURE_3D;
    case GL_PROXY_TEXTURE_1D_ARRAY: return GL_TEXTURE_1D_ARRAY;
    case GL_PROXY_TEXTURE_2D_ARRAY: return GL_TEXTURE_2D_ARRAY;
    case GL_PROXY_TEXTURE_RECTANGLE: return GL_TEXTURE_RECTANGLE;
    case GL_PROXY_TEXTURE_CUBE_MAP: return GL_TEXTURE_CUBE_MAP;
    case GL_PROXY_TEXTURE_2D_MULTISAMPLE: return GL_TEXTURE_2D_MULTISAMPLE;
    case GL_PROXY_TEXTURE_2D_MULTISAMPLE_ARRAY: return GL_TEXTURE_2D_MULTISAMPLE_ARRAY;
    default: return 0;
    }
}

// The texture image an image call addresses.
struct ImageTarget {
    Texture* tex = nullptr;
    GLenum target = 0;   // texture target (cube faces resolve to GL_TEXTURE_CUBE_MAP)
    int face = 0;
    bool proxy = false;
};

// dims: 1, 2 or 3 (TexImage1D/2D/3D); generates GL_INVALID_ENUM for targets
// that the call does not accept.
bool imageTarget(Context& ctx, GLenum target, int dims, bool allowProxy, ImageTarget& out) {
    GLenum base = target;
    if (allowProxy && proxyBase(target)) {
        base = proxyBase(target);
        out.proxy = true;
    }
    if (isCubeFace(base)) {
        out.face = static_cast<int>(base - GL_TEXTURE_CUBE_MAP_POSITIVE_X);
        base = GL_TEXTURE_CUBE_MAP;
        if (dims != 2) return ctx.setError(GL_INVALID_ENUM), false;
    } else if (base == GL_TEXTURE_CUBE_MAP && !out.proxy) {
        return ctx.setError(GL_INVALID_ENUM), false;
    }
    bool ok = false;
    switch (base) {
    case GL_TEXTURE_1D: ok = dims == 1; break;
    case GL_TEXTURE_2D: case GL_TEXTURE_1D_ARRAY: case GL_TEXTURE_RECTANGLE: case GL_TEXTURE_CUBE_MAP: ok = dims == 2; break;
    case GL_TEXTURE_3D: case GL_TEXTURE_2D_ARRAY: ok = dims == 3; break;
    case GL_TEXTURE_CUBE_MAP_ARRAY: ok = dims == 3 && ctx.version() >= 40; break;
    default: break;
    }
    if (!ok) return ctx.setError(GL_INVALID_ENUM), false;
    out.target = base;
    out.tex = out.proxy ? ctx.proxyTextures[textureTargetIndex(base)].get() : boundTexture(ctx, base);
    return out.tex != nullptr;
}

GLint maxSizeFor(GLenum target) {
    switch (target) {
    case GL_TEXTURE_3D: return kMax3DTextureSize;
    default: return kMaxTextureSize;
    }
}

int maxLevelFor(GLenum target) {
    if (target == GL_TEXTURE_RECTANGLE || target == GL_TEXTURE_2D_MULTISAMPLE ||
        target == GL_TEXTURE_2D_MULTISAMPLE_ARRAY || target == GL_TEXTURE_BUFFER)
        return 0;
    int levels = 0;
    for (GLint size = maxSizeFor(target); size > 1; size >>= 1) ++levels;
    return std::min(levels, Texture::kMaxLevels - 1);
}

// ------------------------------------------------------------ transfers

bool isTypeEnum(GLenum type) {
    switch (type) {
    case GL_UNSIGNED_BYTE: case GL_BYTE: case GL_UNSIGNED_SHORT: case GL_SHORT: case GL_UNSIGNED_INT: case GL_INT:
    case GL_HALF_FLOAT: case GL_FLOAT: case GL_UNSIGNED_BYTE_3_3_2: case GL_UNSIGNED_BYTE_2_3_3_REV:
    case GL_UNSIGNED_SHORT_5_6_5: case GL_UNSIGNED_SHORT_5_6_5_REV: case GL_UNSIGNED_SHORT_4_4_4_4:
    case GL_UNSIGNED_SHORT_4_4_4_4_REV: case GL_UNSIGNED_SHORT_5_5_5_1: case GL_UNSIGNED_SHORT_1_5_5_5_REV:
    case GL_UNSIGNED_INT_8_8_8_8: case GL_UNSIGNED_INT_8_8_8_8_REV: case GL_UNSIGNED_INT_10_10_10_2:
    case GL_UNSIGNED_INT_2_10_10_10_REV: case GL_UNSIGNED_INT_24_8: case GL_UNSIGNED_INT_10F_11F_11F_REV:
    case GL_UNSIGNED_INT_5_9_9_9_REV: case GL_FLOAT_32_UNSIGNED_INT_24_8_REV:
        return true;
    default: return false;
    }
}

bool isFormatEnum(Context& ctx, GLenum format) {
    return validTransferFormatType(format, GL_UNSIGNED_BYTE, ctx.isCompat()) ||
           validTransferFormatType(format, GL_FLOAT, ctx.isCompat()) ||
           validTransferFormatType(format, GL_UNSIGNED_INT_24_8, ctx.isCompat()) ||
           validTransferFormatType(format, GL_FLOAT_32_UNSIGNED_INT_24_8_REV, ctx.isCompat()) ||
           validTransferFormatType(format, GL_INT, ctx.isCompat());
}

// Validates a client (format, type); sets the GL error and returns false if invalid.
bool checkFormatType(Context& ctx, GLenum format, GLenum type) {
    if (validTransferFormatType(format, type, ctx.isCompat())) return true;
    if (!isTypeEnum(type) || !isFormatEnum(ctx, format)) ctx.setError(GL_INVALID_ENUM);
    else ctx.setError(GL_INVALID_OPERATION);
    return false;
}

bool threeDTarget(GLenum target) {
    return target == GL_TEXTURE_3D || target == GL_TEXTURE_2D_ARRAY || target == GL_TEXTURE_CUBE_MAP_ARRAY;
}

size_t typeSize(GLenum type) {
    switch (type) {
    case GL_UNSIGNED_BYTE: case GL_BYTE: case GL_UNSIGNED_BYTE_3_3_2: case GL_UNSIGNED_BYTE_2_3_3_REV: return 1;
    case GL_UNSIGNED_SHORT: case GL_SHORT: case GL_HALF_FLOAT: case GL_UNSIGNED_SHORT_5_6_5: case GL_UNSIGNED_SHORT_5_6_5_REV:
    case GL_UNSIGNED_SHORT_4_4_4_4: case GL_UNSIGNED_SHORT_4_4_4_4_REV: case GL_UNSIGNED_SHORT_5_5_5_1:
    case GL_UNSIGNED_SHORT_1_5_5_5_REV:
        return 2;
    default: return 4;
    }
}

// Resolves the source pointer of an unpack operation: a client pointer, or
// an offset into the bound PIXEL_UNPACK_BUFFER. Returns false on error.
bool unpackSource(Context& ctx, const void* pixels, size_t bytesNeeded, size_t alignment, const uint8_t*& out) {
    out = static_cast<const uint8_t*>(pixels);
    Buffer* pbo = ctx.state.pixelUnpackBuffer.get();
    if (!pbo) return true;
    const uintptr_t offset = reinterpret_cast<uintptr_t>(pixels);
    if (pbo->mappedNonPersistent()) return ctx.setError(GL_INVALID_OPERATION), false;
    if (offset + bytesNeeded > static_cast<size_t>(pbo->size)) return ctx.setError(GL_INVALID_OPERATION), false;
    if (alignment > 1 && offset % alignment) return ctx.setError(GL_INVALID_OPERATION), false;
    if (!pbo->mtl) {
        out = nullptr;
        return true;
    }
    ctx.stream().waitFor(pbo->lastUseSerial);
    out = pbo->contents() + offset;
    return true;
}

// Resolves the destination pointer of a pack operation (GetTexImage).
bool packDestination(Context& ctx, void* pixels, size_t bytesNeeded, size_t alignment, uint8_t*& out) {
    out = static_cast<uint8_t*>(pixels);
    Buffer* pbo = ctx.state.pixelPackBuffer.get();
    if (!pbo) return true;
    const uintptr_t offset = reinterpret_cast<uintptr_t>(pixels);
    if (pbo->mappedNonPersistent()) return ctx.setError(GL_INVALID_OPERATION), false;
    if (offset + bytesNeeded > static_cast<size_t>(pbo->size)) return ctx.setError(GL_INVALID_OPERATION), false;
    if (alignment > 1 && offset % alignment) return ctx.setError(GL_INVALID_OPERATION), false;
    if (!pbo->mtl) {
        out = nullptr;
        return true;
    }
    ctx.stream().waitFor(pbo->lastUseSerial);
    out = pbo->contents() + offset;
    ++pbo->version;
    return true;
}

// Metal level dimensions of the texture storage.
// Maps a GL image region to the (x, y, z, w, h, d) uploadTexels expects:
// the rows of a 1D array image are its layers.
void metalRegion(const Texture& tex, int& x, int& y, int& z, int& w, int& h, int& d) {
    if (tex.target == GL_TEXTURE_1D_ARRAY) {
        z = y;
        d = h;
        y = 0;
        h = 1;
    }
}

void imageChanged(Context& ctx, Texture& tex) {
    ++tex.version;
    ctx.markDirty(kDirtyTextures | kDirtyFramebuffer);
}

// Records the shape of image (face, level).
void defineImage(Context& ctx, Texture& tex, int face, int level, GLenum requested, const FormatInfo* info, GLsizei w,
                 GLsizei h, GLsizei d, GLsizei samples = 0, bool fixedLocations = true) {
    TextureImage& image = tex.image(face, level);
    const bool reshaped = image.format != info || image.width != w || image.height != h || image.depth != d ||
                          image.samples != samples;
    image.internalFormat = info ? info->internalFormat : 0;
    image.requestedFormat = requested;
    image.format = info;
    image.width = w;
    image.height = h;
    image.depth = d;
    image.samples = samples;
    image.fixedSampleLocations = fixedLocations;
    if (reshaped) tex.storageDirty = true;
    imageChanged(ctx, tex);
}

void clearImage(TextureImage& image) { image = TextureImage(); }

// Converts client pixels and uploads them into a region of an image.
void uploadRegion(Context& ctx, Texture& tex, int face, int level, int x, int y, int z, int w, int h, int d,
                  GLenum format, GLenum type, const uint8_t* src) {
    if (!src || w <= 0 || h <= 0 || d <= 0) return;
    const TextureImage& image = tex.image(face, level);
    if (!image.format) return;
    if (!imageHasStorage(ctx, tex, face, level)) {
        logOnce(LogLevel::Warn, "inconsistent-level", "texture %u level %d does not fit its mip chain; contents dropped",
                tex.name, level);
        return;
    }
    const size_t texel = storageBytes(image.format->storage);
    std::vector<uint8_t> texels(static_cast<size_t>(w) * h * d * texel);
    unpackPixels(src, format, type, w, h, d, pixelStoreParams(ctx.state.unpack, threeDTarget(tex.target)), *image.format,
                 texels.data());
    metalRegion(tex, x, y, z, w, h, d);
    uploadTexels(ctx, tex, face, level, x, y, z, w, h, d, texels.data(), static_cast<size_t>(w) * texel,
                 static_cast<size_t>(w) * h * texel);
    imageChanged(ctx, tex);
}

size_t compressedSize(const FormatInfo& info, GLsizei w, GLsizei h, GLsizei d) {
    const size_t blocksX = (static_cast<size_t>(w) + 3) / 4, blocksY = (static_cast<size_t>(h) + 3) / 4;
    return blocksX * blocksY * static_cast<size_t>(std::max(1, d)) * storageBytes(info.storage);
}

// GL_ARB_compressed_texture_pixel_storage: with the block parameters set,
// ROW_LENGTH, IMAGE_HEIGHT and the SKIP values select blocks of a larger
// image in client memory (or a pixel buffer).
struct CompressedLayout {
    bool active = false;
    size_t blockBytes = 0, blocksX = 0, blocksY = 0, depth = 1;
    size_t start = 0, rowStride = 0, imageStride = 0;
    size_t bytesNeeded() const {
        if (!active) return 0;
        return blocksX && blocksY && depth ? start + (depth - 1) * imageStride + (blocksY - 1) * rowStride + blocksX * blockBytes : 0;
    }
};

CompressedLayout compressedLayout(const PixelStore& ps, const FormatInfo& info, int dims, GLsizei w, GLsizei h, GLsizei d) {
    CompressedLayout l;
    const size_t bytes = storageBytes(info.storage);
    // The parameters apply when set for every dimension of the call
    // (and only if they describe this format's 4x4 blocks).
    l.active = ps.blockSize > 0 && ps.blockWidth > 0 && (dims < 2 || ps.blockHeight > 0) && (dims < 3 || ps.blockDepth > 0) &&
               static_cast<size_t>(ps.blockSize) == bytes && ps.blockWidth == 4 && (dims < 2 || ps.blockHeight == 4) &&
               (dims < 3 || ps.blockDepth == 1);
    if (!l.active) return l;
    l.blockBytes = bytes;
    l.blocksX = (static_cast<size_t>(w) + 3) / 4;
    l.blocksY = (static_cast<size_t>(h) + 3) / 4;
    l.depth = static_cast<size_t>(std::max(1, d));
    const size_t rowBlocks = ps.rowLength > 0 ? (static_cast<size_t>(ps.rowLength) + 3) / 4 : l.blocksX;
    const size_t imageRows = ps.imageHeight > 0 ? (static_cast<size_t>(ps.imageHeight) + 3) / 4 : l.blocksY;
    l.rowStride = rowBlocks * bytes;
    l.imageStride = imageRows * l.rowStride;
    l.start = static_cast<size_t>(ps.skipPixels / 4) * bytes + static_cast<size_t>(ps.skipRows / 4) * l.rowStride +
              (dims == 3 ? static_cast<size_t>(ps.skipImages) * l.imageStride : 0);
    return l;
}

// Copies the selected blocks of `src` into consecutive blocks.
std::vector<uint8_t> gatherBlocks(const CompressedLayout& l, const uint8_t* src) {
    std::vector<uint8_t> out(l.blocksX * l.blocksY * l.depth * l.blockBytes);
    uint8_t* to = out.data();
    for (size_t z = 0; z < l.depth; ++z)
        for (size_t y = 0; y < l.blocksY; ++y, to += l.blocksX * l.blockBytes)
            std::memcpy(to, src + l.start + z * l.imageStride + y * l.rowStride, l.blocksX * l.blockBytes);
    return out;
}

bool compressedFormatSupported(Context& ctx, const FormatInfo* info) {
    if (!info || !info->compressed || !Device::get().caps().bcTextures) return false;
    switch (info->storage) {
    case Storage::BC1_RGBA: case Storage::BC1_RGBA_sRGB: case Storage::BC2_RGBA: case Storage::BC2_RGBA_sRGB:
    case Storage::BC3_RGBA: case Storage::BC3_RGBA_sRGB:
        return ctx.hasExtension("GL_EXT_texture_compression_s3tc");
    case Storage::BC4_RUnorm: case Storage::BC4_RSnorm: case Storage::BC5_RGUnorm: case Storage::BC5_RGSnorm:
        return true;  // RGTC is core in GL 3.0
    default:
        return ctx.hasExtension("GL_ARB_texture_compression_bptc");
    }
}

void uploadCompressed(Context& ctx, Texture& tex, int face, int level, int x, int y, int z, int w, int h, int d,
                      const uint8_t* src) {
    if (!src || w <= 0 || h <= 0 || d <= 0) return;
    const TextureImage& image = tex.image(face, level);
    if (!imageHasStorage(ctx, tex, face, level)) return;
    const size_t rowBytes = ((static_cast<size_t>(w) + 3) / 4) * storageBytes(image.format->storage);
    const size_t imageBytes = rowBytes * ((static_cast<size_t>(h) + 3) / 4);
    uploadTexels(ctx, tex, face, level, x, y, z, w, h, d, src, rowBytes, imageBytes);
    imageChanged(ctx, tex);
}

// Converts client pixels to RGTC blocks and uploads them (TexImage/TexSubImage
// with an RGTC internal format and uncompressed data).
void uploadRgtcRegion(Context& ctx, Texture& tex, int face, int level, int x, int y, int z, int w, int h, int d,
                      GLenum format, GLenum type, const uint8_t* src) {
    if (!src || w <= 0 || h <= 0 || d <= 0) return;
    const TextureImage& image = tex.image(face, level);
    const FormatInfo* rgba = formatInfo(GL_RGBA32F);
    std::vector<float> texels(static_cast<size_t>(w) * h * d * 4);
    unpackPixels(src, format, type, w, h, d, pixelStoreParams(ctx.state.unpack, threeDTarget(tex.target)), *rgba, texels.data());
    std::vector<uint8_t> blocks(compressedSize(*image.format, w, h, d));
    encodeRgtc(image.format->storage, texels.data(), w, h, d, blocks.data());
    metalRegion(tex, x, y, z, w, h, d);
    uploadCompressed(ctx, tex, face, level, x, y, z, w, h, d, blocks.data());
}

// Image size validation shared by TexImage and CopyTexImage. Returns false
// (setting INVALID_VALUE unless `proxy`) when the size is not allowed.
bool sizeAllowed(Context& ctx, const ImageTarget& t, GLint level, GLsizei w, GLsizei h, GLsizei d, GLint border,
                 bool& tooLarge) {
    tooLarge = false;
    if (level < 0 || level > maxLevelFor(t.target)) return ctx.setError(GL_INVALID_VALUE), false;
    if (w < 0 || h < 0 || d < 0 || border != 0) return ctx.setError(GL_INVALID_VALUE), false;
    const GLint maxSize = maxSizeFor(t.target) >> level;
    const bool oneDArray = t.target == GL_TEXTURE_1D_ARRAY;
    const bool array = t.target == GL_TEXTURE_2D_ARRAY || t.target == GL_TEXTURE_CUBE_MAP_ARRAY;
    if (w > maxSize || (!oneDArray && h > maxSize) || (oneDArray && h > kMaxArrayLayers) ||
        (t.target == GL_TEXTURE_3D && d > maxSize) || (array && d > kMaxArrayLayers))
        tooLarge = true;
    if (t.target == GL_TEXTURE_CUBE_MAP && w != h) return ctx.setError(GL_INVALID_VALUE), false;
    if (t.target == GL_TEXTURE_CUBE_MAP_ARRAY && (w != h || d % 6)) return ctx.setError(GL_INVALID_VALUE), false;
    if (tooLarge && !t.proxy) return ctx.setError(GL_INVALID_VALUE), false;
    return true;
}

// TexImage/TexSubImage source formats for depth images: DEPTH_COMPONENT or
// DEPTH_STENCIL data (the depth part is used for depth-only images);
// STENCIL_INDEX uploads need stencil texturing (GL 4.4).
bool textureTransferCompatible(GLenum format, GLenum type, const FormatInfo& info) {
    if (info.isDepthOrStencil()) return format == GL_DEPTH_COMPONENT || format == GL_DEPTH_STENCIL;
    return transferCompatible(format, type, info);
}

// Whether an internal format may be used with a target (depth formats only
// on 1D/2D-like targets; compressed formats only on 2D-like targets).
bool formatAllowedOnTarget(const FormatInfo& info, GLenum target) {
    if (info.isDepthOrStencil())
        return target != GL_TEXTURE_3D;
    if (info.compressed)
        return target == GL_TEXTURE_2D || target == GL_TEXTURE_2D_ARRAY || target == GL_TEXTURE_CUBE_MAP ||
               target == GL_TEXTURE_CUBE_MAP_ARRAY;
    return true;
}

void texImage(Context& ctx, int dims, GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height,
              GLsizei depth, GLint border, GLenum format, GLenum type, const void* pixels) {
    ImageTarget t;
    if (!imageTarget(ctx, target, dims, true, t)) return;
    if (!checkFormatType(ctx, format, type)) return;
    const GLenum requested = static_cast<GLenum>(internalformat);
    if (!isValidTextureInternalFormat(requested, ctx.isCompat())) return ctx.setError(GL_INVALID_VALUE);
    bool tooLarge;
    if (!sizeAllowed(ctx, t, level, width, height, depth, border, tooLarge)) return;
    const FormatInfo* info = formatInfo(resolveInternalFormat(requested, format, type));
    if (!info) return ctx.setError(GL_INVALID_VALUE);
    if (!formatAllowedOnTarget(*info, t.target)) return ctx.setError(GL_INVALID_OPERATION);
    if (info->compressed && !compressedFormatSupported(ctx, info)) return ctx.setError(GL_INVALID_ENUM);
    if (!textureTransferCompatible(format, type, *info)) return ctx.setError(GL_INVALID_OPERATION);
    if (!t.proxy && t.tex->immutable) return ctx.setError(GL_INVALID_OPERATION);
    if (t.proxy) {
        TextureImage& image = t.tex->image(t.face, level);
        if (tooLarge || mtlPixelFormat(info->storage) == MTLPixelFormatInvalid) return clearImage(image);
        image.internalFormat = info->internalFormat;
        image.requestedFormat = requested;
        image.format = info;
        image.width = width;
        image.height = height;
        image.depth = depth;
        return;
    }
    const PixelStoreParams ps = pixelStoreParams(ctx.state.unpack, dims == 3);
    const TransferLayout layout = transferLayout(format, type, width, height, depth, ps);
    const uint8_t* src = nullptr;
    if (!unpackSource(ctx, pixels, width && height && depth ? layout.bytesNeeded : 0, typeSize(type), src)) return;
    defineImage(ctx, *t.tex, t.face, level, requested, info, width, height, depth);
    if (info->compressed) {
        if (isRgtcStorage(info->storage)) {
            if (imageHasStorage(ctx, *t.tex, t.face, level))
                uploadRgtcRegion(ctx, *t.tex, t.face, level, 0, 0, 0, width, height, depth, format, type, src);
        } else if (src) {
            // S3TC/BPTC encoders are not implemented; the image keeps undefined contents.
            logOnce(LogLevel::Warn, "compress-on-upload", "uncompressed upload to a compressed internal format ignored");
        }
        return;
    }
    uploadRegion(ctx, *t.tex, t.face, level, 0, 0, 0, width, height, depth, format, type, src);
}

void texSubImage(Context& ctx, int dims, GLenum target, GLint level, GLint x, GLint y, GLint z, GLsizei w, GLsizei h,
                 GLsizei d, GLenum format, GLenum type, const void* pixels) {
    ImageTarget t;
    if (!imageTarget(ctx, target, dims, false, t)) return;
    if (!checkFormatType(ctx, format, type)) return;
    if (level < 0 || level > maxLevelFor(t.target)) return ctx.setError(GL_INVALID_VALUE);
    if (w < 0 || h < 0 || d < 0) return ctx.setError(GL_INVALID_VALUE);
    const TextureImage& image = t.tex->image(t.face, level);
    if (!image.defined()) return ctx.setError(GL_INVALID_OPERATION);
    if (x < 0 || y < 0 || z < 0 || w > image.width - x || h > image.height - y || d > image.depth - z)
        return ctx.setError(GL_INVALID_VALUE);
    const bool rgtc = isRgtcStorage(image.format->storage);
    if (image.format->compressed && !rgtc) return ctx.setError(GL_INVALID_OPERATION);
    // RGTC regions must be block aligned, except where they reach the image edge (GL 3.3 section 3.8.4).
    if (rgtc && ((x % 4) || (y % 4) || ((w % 4) && x + w != image.width) || ((h % 4) && y + h != image.height)))
        return ctx.setError(GL_INVALID_OPERATION);
    if (!textureTransferCompatible(format, type, *image.format)) return ctx.setError(GL_INVALID_OPERATION);
    const TransferLayout layout = transferLayout(format, type, w, h, d, pixelStoreParams(ctx.state.unpack, dims == 3));
    const uint8_t* src = nullptr;
    if (!unpackSource(ctx, pixels, w && h && d ? layout.bytesNeeded : 0, typeSize(type), src)) return;
    if (rgtc) {
        if (imageHasStorage(ctx, *t.tex, t.face, level)) uploadRgtcRegion(ctx, *t.tex, t.face, level, x, y, z, w, h, d, format, type, src);
        return;
    }
    uploadRegion(ctx, *t.tex, t.face, level, x, y, z, w, h, d, format, type, src);
}

void compressedTexImage(Context& ctx, int dims, GLenum target, GLint level, GLenum internalformat, GLsizei width,
                        GLsizei height, GLsizei depth, GLint border, GLsizei imageSize, const void* data) {
    ImageTarget t;
    if (!imageTarget(ctx, target, dims, true, t)) return;
    const FormatInfo* info = formatInfo(internalformat);
    if (!info || !info->compressed || info->internalFormat != internalformat || !compressedFormatSupported(ctx, info))
        return ctx.setError(GL_INVALID_ENUM);
    if (!formatAllowedOnTarget(*info, t.target)) return ctx.setError(GL_INVALID_OPERATION);
    bool tooLarge;
    if (!sizeAllowed(ctx, t, level, width, height, depth, border, tooLarge)) return;
    if (!t.proxy && t.tex->immutable) return ctx.setError(GL_INVALID_OPERATION);
    if (t.proxy) {
        TextureImage& image = t.tex->image(t.face, level);
        if (tooLarge) return clearImage(image);
        image.internalFormat = image.requestedFormat = internalformat;
        image.format = info;
        image.width = width;
        image.height = height;
        image.depth = depth;
        return;
    }
    if (imageSize < 0 || static_cast<size_t>(imageSize) != compressedSize(*info, width, height, depth))
        return ctx.setError(GL_INVALID_VALUE);
    const CompressedLayout layout = compressedLayout(ctx.state.unpack, *info, dims, width, height, depth);
    const uint8_t* src = nullptr;
    if (!unpackSource(ctx, data, layout.active ? layout.bytesNeeded() : static_cast<size_t>(imageSize), 1, src)) return;
    std::vector<uint8_t> gathered;
    if (layout.active && src) {
        gathered = gatherBlocks(layout, src);
        src = gathered.data();
    }
    defineImage(ctx, *t.tex, t.face, level, internalformat, info, width, height, depth);
    int x = 0, y = 0, z = 0, w = width, h = height, d = depth;
    metalRegion(*t.tex, x, y, z, w, h, d);
    uploadCompressed(ctx, *t.tex, t.face, level, x, y, z, w, h, d, src);
}

void compressedTexSubImage(Context& ctx, int dims, GLenum target, GLint level, GLint x, GLint y, GLint z, GLsizei w,
                           GLsizei h, GLsizei d, GLenum format, GLsizei imageSize, const void* data) {
    ImageTarget t;
    if (!imageTarget(ctx, target, dims, false, t)) return;
    if (level < 0 || level > maxLevelFor(t.target)) return ctx.setError(GL_INVALID_VALUE);
    if (w < 0 || h < 0 || d < 0) return ctx.setError(GL_INVALID_VALUE);
    const FormatInfo* info = formatInfo(format);
    if (!info || !info->compressed || info->internalFormat != format) return ctx.setError(GL_INVALID_ENUM);
    const TextureImage& image = t.tex->image(t.face, level);
    if (!image.defined() || image.format != info) return ctx.setError(GL_INVALID_OPERATION);
    if (x < 0 || y < 0 || z < 0 || w > image.width - x || h > image.height - y || d > image.depth - z)
        return ctx.setError(GL_INVALID_VALUE);
    // Regions must be block aligned, except where they reach the image edge.
    if ((x % 4) || (y % 4) || ((w % 4) && x + w != image.width) || ((h % 4) && y + h != image.height))
        return ctx.setError(GL_INVALID_OPERATION);
    if (imageSize < 0 || static_cast<size_t>(imageSize) != compressedSize(*info, w, h, d))
        return ctx.setError(GL_INVALID_VALUE);
    const CompressedLayout layout = compressedLayout(ctx.state.unpack, *info, dims, w, h, d);
    const uint8_t* src = nullptr;
    if (!unpackSource(ctx, data, layout.active ? layout.bytesNeeded() : static_cast<size_t>(imageSize), 1, src)) return;
    std::vector<uint8_t> gathered;
    if (layout.active && src) {
        gathered = gatherBlocks(layout, src);
        src = gathered.data();
    }
    metalRegion(*t.tex, x, y, z, w, h, d);
    uploadCompressed(ctx, *t.tex, t.face, level, x, y, z, w, h, d, src);
}

// ------------------------------------------------------------ copies

// The read framebuffer's colour or depth source for copies.
struct CopySource {
    id<MTLTexture> texture = nil;
    uint32_t level = 0, slice = 0;
    const FormatInfo* info = nullptr;
    int width = 0, height = 0;
};

// `stencil`: the destination has stencil, so the read framebuffer needs
// both depth and stencil (GL 3.3 section 3.8.4).
bool copySource(Context& ctx, bool depth, CopySource& out, bool stencil = false) {
    Framebuffer* fb = ctx.state.readFramebuffer.get();
    if (CheckFramebufferStatus(ctx, GL_READ_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        return ctx.setError(GL_INVALID_FRAMEBUFFER_OPERATION), false;
    if (!depth) {
        if (fb ? fb->readBuffer == GL_NONE : ctx.state.defaultReadBuffer == GL_NONE)
            return ctx.setError(GL_INVALID_OPERATION), false;
        GLenum internalFormat = 0;
        out.texture = ctx.draw().readColorTexture(out.level, out.slice, internalFormat, out.width, out.height);
        out.info = formatInfo(internalFormat);
    } else if (fb) {
        const Attachment& a = fb->depth;
        if (!a.attached() || (stencil && !fb->stencil.attached())) return ctx.setError(GL_INVALID_OPERATION), false;
        if (a.type == GL_TEXTURE) {
            Texture& tex = *a.texture;
            const int face = a.cubeFace ? static_cast<int>(a.cubeFace - GL_TEXTURE_CUBE_MAP_POSITIVE_X) : 0;
            if (!imageHasStorage(ctx, tex, face, a.level)) return ctx.setError(GL_INVALID_OPERATION), false;
            const TextureImage& image = tex.image(a.cubeFace ? a.cubeFace - GL_TEXTURE_CUBE_MAP_POSITIVE_X : 0, a.level);
            out.texture = tex.mtl;
            out.level = static_cast<uint32_t>(a.level);
            out.slice = a.cubeFace ? a.cubeFace - GL_TEXTURE_CUBE_MAP_POSITIVE_X : static_cast<uint32_t>(a.layer);
            out.info = image.format;
            out.width = image.width;
            out.height = image.height;
        } else {
            out.texture = a.renderbuffer->mtl;
            out.info = a.renderbuffer->format;
            out.width = a.renderbuffer->width;
            out.height = a.renderbuffer->height;
        }
    } else {
        Surface* surface = ctx.readSurface();
        if (!surface || !surface->hasDepth() || (stencil && !surface->hasStencil()))
            return ctx.setError(GL_INVALID_OPERATION), false;
        out.texture = surface->resolvedDepthStencil(ctx.stream());
        out.info = formatInfo(surface->hasStencil()                  ? GL_DEPTH24_STENCIL8
                              : surface->pixelFormat().depth_bits == 16 ? GL_DEPTH_COMPONENT16
                                                                        : GL_DEPTH_COMPONENT24);
        out.width = surface->width();
        out.height = surface->height();
    }
    if (!out.texture || !out.info) return ctx.setError(GL_INVALID_OPERATION), false;
    if (out.texture.sampleCount > 1) return ctx.setError(GL_INVALID_OPERATION), false;
    return true;
}

// Reads a region of a texture into tightly packed texels of `storage`
// (depth or stencil aspect selected by `option`).
std::vector<uint8_t> readRegion(Context& ctx, id<MTLTexture> texture, uint32_t level, uint32_t slice, int x, int y,
                                int z, int w, int h, int d, size_t texelBytes, MTLBlitOption option) {
    const size_t rowBytes = static_cast<size_t>(w) * texelBytes, imageBytes = rowBytes * static_cast<size_t>(h);
    id<MTLBuffer> staging = [Device::get().mtl() newBufferWithLength:std::max<size_t>(1, imageBytes * d)
                                                             options:MTLResourceStorageModeShared];
    id<MTLBlitCommandEncoder> blit = ctx.stream().blit();
    if (texture.textureType == MTLTextureType3D) {
        [blit copyFromTexture:texture sourceSlice:0 sourceLevel:level sourceOrigin:MTLOriginMake(x, y, z)
                   sourceSize:MTLSizeMake(w, h, d) toBuffer:staging destinationOffset:0 destinationBytesPerRow:rowBytes
      destinationBytesPerImage:imageBytes options:option];
    } else {
        for (int i = 0; i < d; ++i)
            [blit copyFromTexture:texture sourceSlice:slice + static_cast<uint32_t>(i) sourceLevel:level
                     sourceOrigin:MTLOriginMake(x, y, 0) sourceSize:MTLSizeMake(w, h, 1) toBuffer:staging
                destinationOffset:imageBytes * static_cast<size_t>(i) destinationBytesPerRow:rowBytes
          destinationBytesPerImage:imageBytes options:option];
    }
    ctx.stream().finish();  // callers go on using the texture's state: keep the share-group lock
    const uint8_t* p = static_cast<const uint8_t*>(staging.contents);
    return std::vector<uint8_t>(p, p + imageBytes * static_cast<size_t>(d));
}

// Reads texels of a texture image in its storage layout, splitting combined
// depth/stencil into the 8-byte storage texel.
std::vector<uint8_t> readImageTexels(Context& ctx, id<MTLTexture> texture, Storage storage, uint32_t level,
                                     uint32_t slice, int x, int y, int z, int w, int h, int d) {
    if (storage == Storage::Depth32Float_Stencil8) {
        std::vector<uint8_t> depth = readRegion(ctx, texture, level, slice, x, y, z, w, h, d, 4,
                                                MTLBlitOptionDepthFromDepthStencil);
        std::vector<uint8_t> stencil = readRegion(ctx, texture, level, slice, x, y, z, w, h, d, 1,
                                                  MTLBlitOptionStencilFromDepthStencil);
        const size_t count = static_cast<size_t>(w) * h * d;
        std::vector<uint8_t> out(count * 8, 0);
        for (size_t i = 0; i < count; ++i) {
            std::memcpy(&out[i * 8], &depth[i * 4], 4);
            out[i * 8 + 4] = stencil[i];
        }
        return out;
    }
    return readRegion(ctx, texture, level, slice, x, y, z, w, h, d, storageBytes(storage), MTLBlitOptionNone);
}

// Copies a read-framebuffer region into image (face, level) at (dx, dy, dz).
void copyRegion(Context& ctx, Texture& tex, int face, int level, int dx, int dy, int dz, int x, int y, int w, int h,
                const CopySource& src) {
    const TextureImage& image = tex.image(face, level);
    // Pixels outside the read framebuffer are undefined; skip them.
    const int x0 = std::max(0, x), y0 = std::max(0, y);
    const int x1 = std::min(src.width, x + w), y1 = std::min(src.height, y + h);
    if (x1 <= x0 || y1 <= y0 || !image.format) return;
    dx += x0 - x;
    dy += y0 - y;
    const int cw = x1 - x0, ch = y1 - y0;
    if (!imageHasStorage(ctx, tex, face, level)) return;
    int mx = dx, my = dy, mz = dz, mw = cw, mh = ch, md = 1;
    metalRegion(tex, mx, my, mz, mw, mh, md);
    const NSUInteger destSlice = static_cast<NSUInteger>(tex.target == GL_TEXTURE_CUBE_MAP ? face
                                                         : tex.target == GL_TEXTURE_3D  ? 0
                                                                                        : mz);
    const NSUInteger destZ = tex.target == GL_TEXTURE_3D ? static_cast<NSUInteger>(mz) : 0;
    // A 3D source layer is a depth plane, not a slice.
    const bool src3D = src.texture.textureType == MTLTextureType3D;
    const uint32_t srcSlice = src3D ? 0 : src.slice, srcZ = src3D ? src.slice : 0;
    // A raw copy keeps the stored channels: exact unless the destination
    // maps them (alpha and luminance-alpha formats, RGB's stored alpha).
    const FormatInfo& dstInfo = *image.format;
    const bool raw = dstInfo.baseFormat != GL_ALPHA && dstInfo.baseFormat != GL_LUMINANCE_ALPHA &&
                     (!dstInfo.storedAlphaIsOne || src.info->storedAlphaIsOne);
    if (raw && src.texture.pixelFormat == tex.mtl.pixelFormat && tex.target != GL_TEXTURE_1D_ARRAY) {
        id<MTLBlitCommandEncoder> blit = ctx.stream().blit();
        [blit copyFromTexture:src.texture sourceSlice:srcSlice sourceLevel:src.level sourceOrigin:MTLOriginMake(x0, y0, srcZ)
                   sourceSize:MTLSizeMake(cw, ch, 1) toTexture:tex.mtl destinationSlice:destSlice
             destinationLevel:static_cast<NSUInteger>(level) destinationOrigin:MTLOriginMake(mx, my, destZ)];
        imageChanged(ctx, tex);
        return;
    }
    // Format conversion through the canonical texel.
    std::vector<uint8_t> texels = readImageTexels(ctx, src.texture, src.info->storage, src.level, srcSlice, x0, y0,
                                                  static_cast<int>(srcZ), cw, ch, 1);
    const size_t inBytes = storageBytes(src.info->storage), outBytes = storageBytes(image.format->storage);
    std::vector<uint8_t> converted(static_cast<size_t>(cw) * ch * outBytes);
    for (size_t i = 0; i < static_cast<size_t>(cw) * ch; ++i) {
        Texel texel;
        decodeStorage(src.info->storage, &texels[i * inBytes], texel);
        // Source channels missing from the read buffer format read as (0, 0, 0, 1).
        const uint8_t bits[4] = {src.info->redBits, src.info->greenBits, src.info->blueBits, src.info->alphaBits};
        if (!src.info->isDepthOrStencil()) {
            for (int c = 0; c < 4; ++c)
                if (!bits[c]) texel.c[c] = c == 3 ? 1.0 : 0.0;
            mapRgbaToStored(dstInfo, texel);
        }
        encodeStorage(image.format->storage, texel, &converted[i * outBytes]);
    }
    if (tex.target == GL_TEXTURE_1D_ARRAY) {
        // Each source row is one layer.
        for (int row = 0; row < ch; ++row)
            uploadTexels(ctx, tex, face, level, dx, 0, dy + row, cw, 1, 1, &converted[static_cast<size_t>(row) * cw * outBytes],
                         static_cast<size_t>(cw) * outBytes, static_cast<size_t>(cw) * outBytes);
    } else {
        uploadTexels(ctx, tex, face, level, mx, my, mz, mw, mh, md, converted.data(), static_cast<size_t>(cw) * outBytes,
                     static_cast<size_t>(cw) * ch * outBytes);
    }
    imageChanged(ctx, tex);
}

bool copyFormatsCompatible(const FormatInfo& src, const FormatInfo& dst) {
    if (dst.isDepthOrStencil() != src.isDepthOrStencil()) return false;
    if (dst.isInteger() != src.isInteger()) return false;
    if (dst.isInteger() && dst.isSignedInteger() != src.isSignedInteger()) return false;
    return true;
}

void copyTexImage(Context& ctx, int dims, GLenum target, GLint level, GLenum internalformat, GLint x, GLint y,
                  GLsizei width, GLsizei height, GLint border) {
    ImageTarget t;
    if (!imageTarget(ctx, target, dims, false, t)) return;
    if (!isValidTextureInternalFormat(internalformat, ctx.isCompat())) return ctx.setError(GL_INVALID_VALUE);
    bool tooLarge;
    if (!sizeAllowed(ctx, t, level, width, dims == 1 ? 1 : height, 1, border, tooLarge)) return;
    const FormatInfo* probe = formatInfo(resolveInternalFormat(internalformat, 0, 0));
    if (!probe) return ctx.setError(GL_INVALID_VALUE);
    if (probe->compressed && !compressedFormatSupported(ctx, probe)) return ctx.setError(GL_INVALID_ENUM);
    if (!formatAllowedOnTarget(*probe, t.target)) return ctx.setError(GL_INVALID_OPERATION);
    if (t.tex->immutable) return ctx.setError(GL_INVALID_OPERATION);
    CopySource src;
    if (!copySource(ctx, probe->isDepthOrStencil(), src, probe->stencilBits > 0)) return;
    // Unsized formats take their component sizes from the read buffer.
    const FormatInfo* info = probe;
    if (formatInfo(internalformat) == nullptr) {
        const FormatInfo* matched = formatInfo(resolveInternalFormat(internalformat, src.info->defaultTransferFormat,
                                                                     src.info->defaultTransferType));
        if (matched && matched->baseFormat == probe->baseFormat) info = matched;
    }
    if (!copyFormatsCompatible(*src.info, *info)) return ctx.setError(GL_INVALID_OPERATION);
    const GLsizei h = dims == 1 ? 1 : height;
    defineImage(ctx, *t.tex, t.face, level, internalformat, info, width, h, 1);
    if (info->compressed) return;
    copyRegion(ctx, *t.tex, t.face, level, 0, 0, 0, x, y, width, h, src);
}

void copyTexSubImage(Context& ctx, int dims, GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset,
                     GLint x, GLint y, GLsizei width, GLsizei height) {
    ImageTarget t;
    if (!imageTarget(ctx, target, dims, false, t)) return;
    if (level < 0 || level > maxLevelFor(t.target)) return ctx.setError(GL_INVALID_VALUE);
    if (width < 0 || height < 0) return ctx.setError(GL_INVALID_VALUE);
    const TextureImage& image = t.tex->image(t.face, level);
    if (!image.defined()) return ctx.setError(GL_INVALID_OPERATION);
    if (xoffset < 0 || yoffset < 0 || zoffset < 0 || width > image.width - xoffset || height > image.height - yoffset ||
        zoffset >= image.depth)
        return ctx.setError(GL_INVALID_VALUE);
    if (image.format->compressed) return ctx.setError(GL_INVALID_OPERATION);
    CopySource src;
    if (!copySource(ctx, image.format->isDepthOrStencil(), src)) return;
    if (!copyFormatsCompatible(*src.info, *image.format)) return ctx.setError(GL_INVALID_OPERATION);
    copyRegion(ctx, *t.tex, t.face, level, xoffset, yoffset, zoffset, x, y, width, height, src);
}

// ------------------------------------------------------------ parameters

struct ParamValues {
    double f[4] = {};
    int64_t i[4] = {};
    enum Kind { Int, Float, PureInt, PureUint } kind = Int;
    int count = 1;
};

ParamValues paramInts(const GLint* v, int count, ParamValues::Kind kind = ParamValues::Int) {
    ParamValues p;
    p.kind = kind;
    p.count = count;
    for (int i = 0; i < count; ++i) {
        p.i[i] = v[i];
        p.f[i] = v[i];
    }
    return p;
}

ParamValues paramUints(const GLuint* v, int count) {
    ParamValues p;
    p.kind = ParamValues::PureUint;
    p.count = count;
    for (int i = 0; i < count; ++i) {
        p.i[i] = v[i];
        p.f[i] = v[i];
    }
    return p;
}

ParamValues paramFloats(const GLfloat* v, int count) {
    ParamValues p;
    p.kind = ParamValues::Float;
    p.count = count;
    for (int i = 0; i < count; ++i) {
        p.f[i] = v[i];
        p.i[i] = static_cast<int64_t>(std::lround(v[i]));
    }
    return p;
}

bool isVectorParam(GLenum pname) { return pname == GL_TEXTURE_BORDER_COLOR || pname == GL_TEXTURE_SWIZZLE_RGBA; }

// GL_CLAMP is compatibility-profile only.
bool validWrap(GLenum v, bool compat) {
    return v == GL_CLAMP_TO_EDGE || v == GL_REPEAT || v == GL_MIRRORED_REPEAT || v == GL_CLAMP_TO_BORDER ||
           (compat && v == GL_CLAMP);
}

bool validCompareFunc(GLenum v) { return v >= GL_NEVER && v <= GL_ALWAYS; }

// Enum-valued parameters must be given as integral values.
bool enumValue(const ParamValues& v, GLenum& out) {
    if (v.kind == ParamValues::Float && v.f[0] != std::floor(v.f[0])) return false;
    out = static_cast<GLenum>(v.i[0]);
    return true;
}

// Applies a sampler-state parameter. Returns GL_NO_ERROR, an error, or
// GL_INVALID_ENUM for names that are not sampler state.
GLenum setSamplerParam(SamplerParams& p, GLenum pname, const ParamValues& v, GLenum target, bool compat) {
    const bool rect = target == GL_TEXTURE_RECTANGLE;
    GLenum e = 0;
    switch (pname) {
    case GL_TEXTURE_MIN_FILTER:
        if (!enumValue(v, e)) return GL_INVALID_ENUM;
        if (e != GL_NEAREST && e != GL_LINEAR && e != GL_NEAREST_MIPMAP_NEAREST && e != GL_LINEAR_MIPMAP_NEAREST &&
            e != GL_NEAREST_MIPMAP_LINEAR && e != GL_LINEAR_MIPMAP_LINEAR)
            return GL_INVALID_ENUM;
        if (rect && e != GL_NEAREST && e != GL_LINEAR) return GL_INVALID_ENUM;
        p.minFilter = e;
        return GL_NO_ERROR;
    case GL_TEXTURE_MAG_FILTER:
        if (!enumValue(v, e) || (e != GL_NEAREST && e != GL_LINEAR)) return GL_INVALID_ENUM;
        p.magFilter = e;
        return GL_NO_ERROR;
    case GL_TEXTURE_WRAP_S: case GL_TEXTURE_WRAP_T: case GL_TEXTURE_WRAP_R:
        if (!enumValue(v, e) || !validWrap(e, compat)) return GL_INVALID_ENUM;
        if (rect && e != GL_CLAMP_TO_EDGE && e != GL_CLAMP_TO_BORDER && e != GL_CLAMP) return GL_INVALID_ENUM;
        (pname == GL_TEXTURE_WRAP_S ? p.wrapS : pname == GL_TEXTURE_WRAP_T ? p.wrapT : p.wrapR) = e;
        return GL_NO_ERROR;
    case GL_TEXTURE_MIN_LOD: p.minLod = static_cast<GLfloat>(v.f[0]); return GL_NO_ERROR;
    case GL_TEXTURE_MAX_LOD: p.maxLod = static_cast<GLfloat>(v.f[0]); return GL_NO_ERROR;
    case GL_TEXTURE_LOD_BIAS: p.lodBias = static_cast<GLfloat>(v.f[0]); return GL_NO_ERROR;
    case GL_TEXTURE_COMPARE_MODE:
        if (!enumValue(v, e) || (e != GL_NONE && e != GL_COMPARE_REF_TO_TEXTURE)) return GL_INVALID_ENUM;
        p.compareMode = e;
        return GL_NO_ERROR;
    case GL_TEXTURE_COMPARE_FUNC:
        if (!enumValue(v, e) || !validCompareFunc(e)) return GL_INVALID_ENUM;
        p.compareFunc = e;
        return GL_NO_ERROR;
    case GL_TEXTURE_MAX_ANISOTROPY:
        if (v.f[0] < 1.0) return GL_INVALID_VALUE;
        p.maxAnisotropy = static_cast<GLfloat>(std::min<double>(v.f[0], kMaxAnisotropy));
        return GL_NO_ERROR;
    case GL_TEXTURE_BORDER_COLOR:
        for (int i = 0; i < 4; ++i) {
            switch (v.kind) {
            case ParamValues::Float: p.borderColor.f[i] = static_cast<GLfloat>(v.f[i]); break;
            // glTexParameteriv: signed normalized conversion.
            case ParamValues::Int: p.borderColor.f[i] = static_cast<GLfloat>((2.0 * v.i[i] + 1.0) / 4294967295.0); break;
            case ParamValues::PureInt: p.borderColor.i[i] = static_cast<GLint>(v.i[i]); break;
            case ParamValues::PureUint: p.borderColor.u[i] = static_cast<GLuint>(v.i[i]); break;
            }
        }
        p.borderIsInteger = v.kind == ParamValues::PureInt || v.kind == ParamValues::PureUint;
        return GL_NO_ERROR;
    default: return GL_INVALID_ENUM;
    }
}

bool isSamplerParam(GLenum pname) {
    switch (pname) {
    case GL_TEXTURE_MIN_FILTER: case GL_TEXTURE_MAG_FILTER: case GL_TEXTURE_WRAP_S: case GL_TEXTURE_WRAP_T:
    case GL_TEXTURE_WRAP_R: case GL_TEXTURE_MIN_LOD: case GL_TEXTURE_MAX_LOD: case GL_TEXTURE_LOD_BIAS:
    case GL_TEXTURE_COMPARE_MODE: case GL_TEXTURE_COMPARE_FUNC: case GL_TEXTURE_MAX_ANISOTROPY:
    case GL_TEXTURE_BORDER_COLOR:
        return true;
    default: return false;
    }
}

bool validSwizzle(int64_t v) {
    return v == GL_RED || v == GL_GREEN || v == GL_BLUE || v == GL_ALPHA || v == GL_ZERO || v == GL_ONE;
}

void texParameter(Context& ctx, GLenum target, GLenum pname, const ParamValues& v, bool vectorCall) {
    if (!targetSupported(ctx, target) || target == GL_TEXTURE_BUFFER) return ctx.setError(GL_INVALID_ENUM);
    if (!vectorCall && isVectorParam(pname)) return ctx.setError(GL_INVALID_ENUM);
    Texture* tex = boundTexture(ctx, target);
    if (!tex) return ctx.setError(GL_INVALID_OPERATION);
    const bool multisample = target == GL_TEXTURE_2D_MULTISAMPLE || target == GL_TEXTURE_2D_MULTISAMPLE_ARRAY;
    if (isSamplerParam(pname)) {
        if (multisample) return ctx.setError(GL_INVALID_ENUM);
        if (pname == GL_TEXTURE_MAX_ANISOTROPY && !ctx.hasExtension("GL_EXT_texture_filter_anisotropic") &&
            ctx.version() < 46)
            return ctx.setError(GL_INVALID_ENUM);
        const GLenum error = setSamplerParam(tex->sampler, pname, v, target, ctx.isCompat());
        if (error) return ctx.setError(error);
        ctx.markDirty(kDirtyTextures);
        return;
    }
    switch (pname) {
    case GL_TEXTURE_BASE_LEVEL:
    case GL_TEXTURE_MAX_LEVEL: {
        if (v.kind == ParamValues::Float && v.f[0] != std::floor(v.f[0])) return ctx.setError(GL_INVALID_VALUE);
        if (v.i[0] < 0) return ctx.setError(GL_INVALID_VALUE);
        const GLint value = static_cast<GLint>(std::min<int64_t>(v.i[0], std::numeric_limits<GLint>::max()));
        if (pname == GL_TEXTURE_BASE_LEVEL) {
            if ((multisample || target == GL_TEXTURE_RECTANGLE) && value != 0) return ctx.setError(GL_INVALID_OPERATION);
            tex->baseLevel = value;
        } else {
            tex->maxLevel = value;
        }
        tex->views.clear();
        imageChanged(ctx, *tex);
        return;
    }
    case GL_TEXTURE_SWIZZLE_R: case GL_TEXTURE_SWIZZLE_G: case GL_TEXTURE_SWIZZLE_B: case GL_TEXTURE_SWIZZLE_A:
        if (!validSwizzle(v.i[0]) || (v.kind == ParamValues::Float && v.f[0] != std::floor(v.f[0])))
            return ctx.setError(GL_INVALID_ENUM);
        tex->swizzle[pname - GL_TEXTURE_SWIZZLE_R] = static_cast<GLenum>(v.i[0]);
        ctx.markDirty(kDirtyTextures);
        return;
    case GL_TEXTURE_SWIZZLE_RGBA:
        for (int i = 0; i < 4; ++i)
            if (!validSwizzle(v.i[i])) return ctx.setError(GL_INVALID_ENUM);
        for (int i = 0; i < 4; ++i) tex->swizzle[i] = static_cast<GLenum>(v.i[i]);
        ctx.markDirty(kDirtyTextures);
        return;
    case GL_DEPTH_STENCIL_TEXTURE_MODE:
        if (ctx.version() < 43) return ctx.setError(GL_INVALID_ENUM);
        if (v.i[0] != GL_DEPTH_COMPONENT && v.i[0] != GL_STENCIL_INDEX) return ctx.setError(GL_INVALID_ENUM);
        tex->depthStencilMode = static_cast<GLenum>(v.i[0]);
        ctx.markDirty(kDirtyTextures);
        return;
    default: return ctx.setError(GL_INVALID_ENUM);
    }
}

// Reads a sampler-state parameter into `out` (4 entries). Returns false for unknown names.
bool getSamplerParam(const SamplerParams& p, GLenum pname, ParamValues& out) {
    out.count = 1;
    auto setEnum = [&](GLenum e) { out.i[0] = e; out.f[0] = e; out.kind = ParamValues::Int; };
    auto setFloat = [&](double f) { out.f[0] = f; out.i[0] = static_cast<int64_t>(std::lround(f)); out.kind = ParamValues::Float; };
    switch (pname) {
    case GL_TEXTURE_MIN_FILTER: setEnum(p.minFilter); return true;
    case GL_TEXTURE_MAG_FILTER: setEnum(p.magFilter); return true;
    case GL_TEXTURE_WRAP_S: setEnum(p.wrapS); return true;
    case GL_TEXTURE_WRAP_T: setEnum(p.wrapT); return true;
    case GL_TEXTURE_WRAP_R: setEnum(p.wrapR); return true;
    case GL_TEXTURE_MIN_LOD: setFloat(p.minLod); return true;
    case GL_TEXTURE_MAX_LOD: setFloat(p.maxLod); return true;
    case GL_TEXTURE_LOD_BIAS: setFloat(p.lodBias); return true;
    case GL_TEXTURE_COMPARE_MODE: setEnum(p.compareMode); return true;
    case GL_TEXTURE_COMPARE_FUNC: setEnum(p.compareFunc); return true;
    case GL_TEXTURE_MAX_ANISOTROPY: setFloat(p.maxAnisotropy); return true;
    case GL_TEXTURE_BORDER_COLOR:
        out.count = 4;
        if (p.borderIsInteger) {
            out.kind = ParamValues::PureInt;
            for (int i = 0; i < 4; ++i) {
                out.i[i] = p.borderColor.i[i];
                out.f[i] = p.borderColor.i[i];
            }
        } else {
            out.kind = ParamValues::Float;
            for (int i = 0; i < 4; ++i) {
                out.f[i] = p.borderColor.f[i];
                out.i[i] = static_cast<int64_t>(p.borderColor.f[i]);
            }
        }
        return true;
    default: return false;
    }
}

// Converts a float to an integer as glGet*iv does for normalized values.
GLint floatToNormalizedInt(double f) {
    const double v = std::clamp(f, -1.0, 1.0) * 2147483647.0;
    return static_cast<GLint>(std::lround(v));
}

template <class T>
void writeParam(const ParamValues& v, GLenum pname, T* params, ParamValues::Kind call) {
    for (int i = 0; i < v.count; ++i) {
        if constexpr (std::is_same_v<T, GLfloat>) {
            params[i] = static_cast<GLfloat>(v.f[i]);
        } else if (call == ParamValues::Int && pname == GL_TEXTURE_BORDER_COLOR && v.kind == ParamValues::Float) {
            params[i] = static_cast<T>(floatToNormalizedInt(v.f[i]));
        } else if (call == ParamValues::PureUint) {
            params[i] = static_cast<T>(static_cast<uint32_t>(v.i[i]));
        } else {
            params[i] = static_cast<T>(v.kind == ParamValues::Float ? std::lround(v.f[i]) : v.i[i]);
        }
    }
}

template <class T>
void getTexParameter(Context& ctx, GLenum target, GLenum pname, T* params, ParamValues::Kind call) {
    if (!targetSupported(ctx, target) || target == GL_TEXTURE_BUFFER) return ctx.setError(GL_INVALID_ENUM);
    Texture* tex = boundTexture(ctx, target);
    if (!tex) return ctx.setError(GL_INVALID_OPERATION);
    const bool multisample = target == GL_TEXTURE_2D_MULTISAMPLE || target == GL_TEXTURE_2D_MULTISAMPLE_ARRAY;
    ParamValues v;
    if (isSamplerParam(pname)) {
        if (multisample) return ctx.setError(GL_INVALID_ENUM);
        if (pname == GL_TEXTURE_MAX_ANISOTROPY && !ctx.hasExtension("GL_EXT_texture_filter_anisotropic") &&
            ctx.version() < 46)
            return ctx.setError(GL_INVALID_ENUM);
        getSamplerParam(tex->sampler, pname, v);
        return writeParam(v, pname, params, call);
    }
    v.kind = ParamValues::Int;
    switch (pname) {
    case GL_TEXTURE_BASE_LEVEL: v.i[0] = tex->baseLevel; break;
    case GL_TEXTURE_MAX_LEVEL: v.i[0] = tex->maxLevel; break;
    case GL_TEXTURE_SWIZZLE_R: case GL_TEXTURE_SWIZZLE_G: case GL_TEXTURE_SWIZZLE_B: case GL_TEXTURE_SWIZZLE_A:
        v.i[0] = tex->swizzle[pname - GL_TEXTURE_SWIZZLE_R];
        break;
    case GL_TEXTURE_SWIZZLE_RGBA:
        v.count = 4;
        for (int i = 0; i < 4; ++i) v.i[i] = tex->swizzle[i];
        break;
    case GL_TEXTURE_IMMUTABLE_FORMAT:
        if (ctx.version() < 42 && !ctx.hasExtension("GL_ARB_texture_storage")) return ctx.setError(GL_INVALID_ENUM);
        v.i[0] = tex->immutable;
        break;
    case GL_IMAGE_FORMAT_COMPATIBILITY_TYPE:
        // GL 4.2: image views of a texture match its format by texel size.
        if (!ctx.imageLoadStore()) return ctx.setError(GL_INVALID_ENUM);
        v.i[0] = GL_IMAGE_FORMAT_COMPATIBILITY_BY_SIZE;
        break;
    case GL_TEXTURE_IMMUTABLE_LEVELS:
        if (ctx.version() < 43) return ctx.setError(GL_INVALID_ENUM);
        v.i[0] = tex->immutableLevels;
        break;
    case GL_DEPTH_STENCIL_TEXTURE_MODE:
        if (ctx.version() < 43) return ctx.setError(GL_INVALID_ENUM);
        v.i[0] = tex->depthStencilMode;
        break;
    default: return ctx.setError(GL_INVALID_ENUM);
    }
    for (int i = 0; i < v.count; ++i) v.f[i] = static_cast<double>(v.i[i]);
    writeParam(v, pname, params, call);
}

// ------------------------------------------------------------ level queries

template <class T>
void getTexLevelParameter(Context& ctx, GLenum target, GLint level, GLenum pname, T* params) {
    GLenum base = proxyBase(target) ? proxyBase(target) : target;
    int face = 0;
    if (isCubeFace(base)) {
        face = static_cast<int>(base - GL_TEXTURE_CUBE_MAP_POSITIVE_X);
        base = GL_TEXTURE_CUBE_MAP;
    } else if (target == GL_TEXTURE_CUBE_MAP) {
        return ctx.setError(GL_INVALID_ENUM);
    }
    if (!targetSupported(ctx, base)) return ctx.setError(GL_INVALID_ENUM);
    if (level < 0 || level > maxLevelFor(base)) return ctx.setError(GL_INVALID_VALUE);
    Texture* tex = proxyBase(target) ? ctx.proxyTextures[textureTargetIndex(base)].get() : boundTexture(ctx, base);
    if (!tex) return ctx.setError(GL_INVALID_OPERATION);
    // A buffer texture's single image is its buffer, viewed with the buffer format.
    const bool bufferTexture = base == GL_TEXTURE_BUFFER;
    const TextureImage& image = tex->image(face, level);
    const FormatInfo* info = bufferTexture ? (tex->buffer ? formatInfo(tex->bufferFormat) : nullptr) : image.format;
    GLsizei width = image.width, height = image.height, depth = image.depth;
    GLenum internalFormat = image.defined() ? image.requestedFormat : GL_RGBA;
    if (bufferTexture) {
        const int64_t bytes = !tex->buffer ? 0 : tex->bufferSize < 0 ? tex->buffer->size - tex->bufferOffset : tex->bufferSize;
        width = info ? static_cast<GLsizei>(std::min<int64_t>(bytes / storageBytes(info->storage), kMaxTextureBufferSize)) : 0;
        height = depth = info ? 1 : 0;
        internalFormat = tex->bufferFormat ? tex->bufferFormat : GL_R8;
    }
    auto componentType = [&](uint8_t bits) -> GLenum {
        if (!info || !bits) return GL_NONE;
        switch (info->type) {
        case ComponentType::UnsignedNormalized: return GL_UNSIGNED_NORMALIZED;
        case ComponentType::SignedNormalized: return GL_SIGNED_NORMALIZED;
        case ComponentType::Float: return GL_FLOAT;
        case ComponentType::Int: return GL_INT;
        case ComponentType::UnsignedInt: return GL_UNSIGNED_INT;
        default: return GL_NONE;
        }
    };
    int64_t v = 0;
    switch (pname) {
    case GL_TEXTURE_WIDTH: v = width; break;
    case GL_TEXTURE_HEIGHT: v = height; break;
    case GL_TEXTURE_DEPTH: v = depth; break;
    case GL_TEXTURE_INTERNAL_FORMAT: v = internalFormat; break;
    case GL_TEXTURE_RED_SIZE: v = info ? info->redBits : 0; break;
    case GL_TEXTURE_GREEN_SIZE: v = info ? info->greenBits : 0; break;
    case GL_TEXTURE_BLUE_SIZE: v = info ? info->blueBits : 0; break;
    case GL_TEXTURE_ALPHA_SIZE: v = info ? info->alphaBits : 0; break;
    case GL_TEXTURE_DEPTH_SIZE: v = info ? info->depthBits : 0; break;
    case GL_TEXTURE_STENCIL_SIZE: v = info ? info->stencilBits : 0; break;
    case GL_TEXTURE_SHARED_SIZE: v = info ? info->sharedBits : 0; break;
    case GL_TEXTURE_RED_TYPE: v = componentType(info ? info->redBits : 0); break;
    case GL_TEXTURE_GREEN_TYPE: v = componentType(info ? info->greenBits : 0); break;
    case GL_TEXTURE_BLUE_TYPE: v = componentType(info ? info->blueBits : 0); break;
    case GL_TEXTURE_ALPHA_TYPE: v = componentType(info ? info->alphaBits : 0); break;
    case GL_TEXTURE_DEPTH_TYPE: v = componentType(info ? info->depthBits : 0); break;
    case GL_TEXTURE_COMPRESSED: v = info && info->compressed; break;
    case GL_TEXTURE_COMPRESSED_IMAGE_SIZE:
        if (!info || !info->compressed) return ctx.setError(GL_INVALID_OPERATION);
        v = static_cast<int64_t>(compressedSize(*info, width, height, depth));
        break;
    case GL_TEXTURE_SAMPLES: v = image.samples; break;
    case GL_TEXTURE_FIXED_SAMPLE_LOCATIONS: v = image.fixedSampleLocations; break;
    case GL_TEXTURE_BUFFER_DATA_STORE_BINDING: v = bufferTexture && tex->buffer ? tex->buffer->name : 0; break;
    case GL_TEXTURE_BUFFER_OFFSET:
    case GL_TEXTURE_BUFFER_SIZE:
        if (ctx.version() < 43) return ctx.setError(GL_INVALID_ENUM);
        v = !bufferTexture || !tex->buffer ? 0
            : pname == GL_TEXTURE_BUFFER_OFFSET ? tex->bufferOffset
            : tex->bufferSize < 0 ? tex->buffer->size : tex->bufferSize;
        break;
    case GL_TEXTURE_LUMINANCE_SIZE:
    case GL_TEXTURE_INTENSITY_SIZE:
        if (!ctx.isCompat()) return ctx.setError(GL_INVALID_ENUM);
        v = info ? (pname == GL_TEXTURE_LUMINANCE_SIZE ? info->luminanceBits : info->intensityBits) : 0;
        break;
    case GL_TEXTURE_LUMINANCE_TYPE:
    case GL_TEXTURE_INTENSITY_TYPE:
        if (!ctx.isCompat()) return ctx.setError(GL_INVALID_ENUM);
        v = componentType(info ? (pname == GL_TEXTURE_LUMINANCE_TYPE ? info->luminanceBits : info->intensityBits) : 0);
        break;
    default: return ctx.setError(GL_INVALID_ENUM);
    }
    *params = static_cast<T>(v);
}

void texImageMultisample(Context& ctx, GLenum target, bool proxy, GLsizei samples, GLenum internalformat,
                         GLsizei width, GLsizei height, GLsizei depth, GLboolean fixedsamplelocations) {
    const FormatInfo* info = formatInfo(resolveInternalFormat(internalformat, 0, 0));
    if (!info || info->compressed || (!info->colorRenderable && !info->isDepthOrStencil()))
        return ctx.setError(GL_INVALID_ENUM);
    if (samples <= 0 || width < 0 || height < 0 || depth < 0) return ctx.setError(GL_INVALID_VALUE);
    const DeviceCaps& caps = Device::get().caps();
    bool tooLarge = width > kMaxTextureSize || height > kMaxTextureSize || depth > kMaxArrayLayers;
    if (samples > static_cast<GLsizei>(caps.maxSamples)) {
        if (!proxy) return ctx.setError(GL_INVALID_OPERATION);
        tooLarge = true;
    }
    if (tooLarge && !proxy) return ctx.setError(GL_INVALID_VALUE);
    Texture* tex = proxy ? ctx.proxyTextures[textureTargetIndex(target)].get() : boundTexture(ctx, target);
    if (!tex) return ctx.setError(GL_INVALID_OPERATION);
    if (tex->immutable) return ctx.setError(GL_INVALID_OPERATION);
    // Round up to a sample count Metal supports.
    GLsizei actual = 1;
    while (actual < samples && actual < 64) actual *= 2;  // (larger requests are out of range anyway)
    while (actual < 8 && !caps.sampleCountSupported[actual]) actual *= 2;
    actual = std::min<GLsizei>(actual, static_cast<GLsizei>(caps.maxSamples));
    if (proxy) {
        TextureImage& image = tex->image(0, 0);
        if (tooLarge) return clearImage(image);
        image.internalFormat = info->internalFormat;
        image.requestedFormat = internalformat;
        image.format = info;
        image.width = width;
        image.height = height;
        image.depth = depth;
        image.samples = actual;
        image.fixedSampleLocations = fixedsamplelocations != GL_FALSE;
        return;
    }
    defineImage(ctx, *tex, 0, 0, internalformat, info, width, height, depth, actual, fixedsamplelocations != GL_FALSE);
    ensureTextureStorage(ctx, *tex);
}

// glTexStorage* (GL_ARB_texture_storage): defines every level at once and
// makes the texture's format and size immutable.
void texStorage(Context& ctx, int dims, GLenum target, GLsizei levels, GLenum internalformat, GLsizei width,
                GLsizei height, GLsizei depth) {
    GLenum base = proxyBase(target) ? proxyBase(target) : target;
    const bool proxy = base != target;
    bool ok = false;
    switch (base) {
    case GL_TEXTURE_1D: ok = dims == 1; break;
    case GL_TEXTURE_2D: case GL_TEXTURE_1D_ARRAY: case GL_TEXTURE_RECTANGLE: case GL_TEXTURE_CUBE_MAP: ok = dims == 2; break;
    case GL_TEXTURE_3D: case GL_TEXTURE_2D_ARRAY: ok = dims == 3; break;
    case GL_TEXTURE_CUBE_MAP_ARRAY: ok = dims == 3 && ctx.version() >= 40; break;
    default: break;
    }
    if (!ok) return ctx.setError(GL_INVALID_ENUM);
    const FormatInfo* info = formatInfo(internalformat);
    if (!info || info->internalFormat != internalformat) return ctx.setError(GL_INVALID_ENUM);
    if (info->compressed && !compressedFormatSupported(ctx, info)) return ctx.setError(GL_INVALID_ENUM);
    if (levels < 1 || width < 1 || height < 1 || depth < 1) return ctx.setError(GL_INVALID_VALUE);
    Texture* tex = proxy ? ctx.proxyTextures[textureTargetIndex(base)].get() : boundTexture(ctx, base);
    if (!tex || (!proxy && tex->name == 0)) return ctx.setError(GL_INVALID_OPERATION);
    if (!proxy && tex->immutable) return ctx.setError(GL_INVALID_OPERATION);
    if (!formatAllowedOnTarget(*info, base)) return ctx.setError(GL_INVALID_OPERATION);
    const bool oneDArray = base == GL_TEXTURE_1D_ARRAY;
    const bool arrays = base == GL_TEXTURE_2D_ARRAY || base == GL_TEXTURE_CUBE_MAP_ARRAY;
    const GLsizei chain = fullMipCount(width, oneDArray || base == GL_TEXTURE_1D ? 1 : height, base == GL_TEXTURE_3D ? depth : 1);
    if (levels > chain || (base == GL_TEXTURE_RECTANGLE && levels != 1)) return ctx.setError(GL_INVALID_OPERATION);
    if ((base == GL_TEXTURE_CUBE_MAP || base == GL_TEXTURE_CUBE_MAP_ARRAY) && width != height)
        return ctx.setError(GL_INVALID_VALUE);
    const GLint maxSize = maxSizeFor(base);
    const bool tooLarge = width > maxSize || (!oneDArray && height > maxSize) || (oneDArray && height > kMaxArrayLayers) ||
                          (base == GL_TEXTURE_3D && depth > maxSize) || (arrays && depth > kMaxArrayLayers);
    if (tooLarge && !proxy) return ctx.setError(GL_INVALID_VALUE);
    GLsizei w = width, h = height, d = depth;
    for (int level = 0; level < Texture::kMaxLevels; ++level) {
        for (int face = 0; face < tex->faceCount(); ++face) {
            TextureImage& image = tex->image(face, level);
            if (level >= levels || (proxy && tooLarge)) {
                clearImage(image);
                continue;
            }
            if (proxy) {
                image.internalFormat = image.requestedFormat = internalformat;
                image.format = info;
                image.width = w;
                image.height = h;
                image.depth = d;
            } else {
                defineImage(ctx, *tex, face, level, internalformat, info, w, h, d);
            }
        }
        w = std::max(1, w / 2);
        if (!oneDArray && base != GL_TEXTURE_1D) h = std::max(1, h / 2);
        if (base == GL_TEXTURE_3D) d = std::max(1, d / 2);
    }
    if (proxy) return;
    tex->immutable = true;
    tex->immutableLevels = levels;
    tex->storageDirty = true;
    ensureTextureStorage(ctx, *tex);
}

void unbindTextureEverywhere(Context& ctx, Texture* tex) {
    for (TextureUnit& unit : ctx.state.textureUnits)
        for (int i = 0; i < kTexTargetCount; ++i)
            if (unit.bound[i].get() == tex) unit.bound[i] = ctx.defaultTextures[i];
    // A deleted texture's image units are unbound (texture 0).
    for (ImageUnit& unit : ctx.state.imageUnits)
        if (unit.texture.get() == tex) unit.texture.reset();
    for (Framebuffer* fb : {ctx.state.drawFramebuffer.get(), ctx.state.readFramebuffer.get()}) {
        if (!fb) continue;
        bool changed = false;
        auto detach = [&](Attachment& a) {
            if (a.type == GL_TEXTURE && a.texture.get() == tex) {
                a = Attachment();
                changed = true;
            }
        };
        for (Attachment& a : fb->color) detach(a);
        detach(fb->depth);
        detach(fb->stencil);
        if (changed) ++fb->version;
    }
    ctx.markDirty(kDirtyTextures | kDirtyFramebuffer);
}

Sampler* samplerObject(Context& ctx, GLuint name) {
    ShareLock lock(ctx);
    return ctx.share().samplers.get(name);
}

}  // namespace

// ------------------------------------------------------------ objects

void ActiveTexture(Context& ctx, GLenum texture) {
    if (texture < GL_TEXTURE0 || texture >= GL_TEXTURE0 + static_cast<GLenum>(kMaxCombinedTextureUnits))
        return ctx.setError(GL_INVALID_ENUM);
    ctx.state.activeTexture = texture - GL_TEXTURE0;
}

void GenTextures(Context& ctx, GLsizei n, GLuint* textures) {
    if (n < 0) return ctx.setError(GL_INVALID_VALUE);
    ShareLock lock(ctx);
    ctx.share().textures.generate(n, textures);
}

void DeleteTextures(Context& ctx, GLsizei n, const GLuint* textures) {
    if (n < 0) return ctx.setError(GL_INVALID_VALUE);
    for (GLsizei i = 0; i < n; ++i) {
        if (!textures[i]) continue;
        RefPtr<Texture> tex;
        {
            ShareLock lock(ctx);
            tex = ctx.share().textures.remove(textures[i]);
        }
        if (tex) unbindTextureEverywhere(ctx, tex.get());
    }
}

GLboolean IsTexture(Context& ctx, GLuint texture) {
    ShareLock lock(ctx);
    return ctx.share().textures.exists(texture) ? GL_TRUE : GL_FALSE;
}

void BindTexture(Context& ctx, GLenum target, GLuint name) {
    if (!targetSupported(ctx, target)) return ctx.setError(GL_INVALID_ENUM);
    const int index = textureTargetIndex(target);
    RefPtr<Texture> tex;
    if (name == 0) {
        tex = ctx.defaultTextures[index];
    } else {
        ShareLock lock(ctx);
        tex = ctx.share().textures.getRef(name);
        if (!tex) {
            if (!ctx.share().textures.isGenerated(name)) {
                if (ctx.isCore()) return ctx.setError(GL_INVALID_OPERATION);
                ctx.share().textures.reserve(name);
            }
            tex = makeRef<Texture>(name, target);
            // Rectangle textures default to linear filtering without mipmaps and edge clamping.
            if (target == GL_TEXTURE_RECTANGLE) {
                tex->sampler.minFilter = GL_LINEAR;
                tex->sampler.wrapS = tex->sampler.wrapT = tex->sampler.wrapR = GL_CLAMP_TO_EDGE;
            }
            ctx.share().textures.set(name, tex);
        } else if (tex->target != target) {
            return ctx.setError(GL_INVALID_OPERATION);
        }
    }
    ctx.state.textureUnits[ctx.state.activeTexture].bound[index] = tex;
    ctx.markDirty(kDirtyTextures);
}

// ------------------------------------------------------------ images

void TexImage1D(Context& ctx, GLenum target, GLint level, GLint internalformat, GLsizei width, GLint border,
                GLenum format, GLenum type, const void* pixels) {
    texImage(ctx, 1, target, level, internalformat, width, 1, 1, border, format, type, pixels);
}

void TexImage2D(Context& ctx, GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height,
                GLint border, GLenum format, GLenum type, const void* pixels) {
    texImage(ctx, 2, target, level, internalformat, width, height, 1, border, format, type, pixels);
}

void TexImage3D(Context& ctx, GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height,
                GLsizei depth, GLint border, GLenum format, GLenum type, const void* pixels) {
    texImage(ctx, 3, target, level, internalformat, width, height, depth, border, format, type, pixels);
}

void TexSubImage1D(Context& ctx, GLenum target, GLint level, GLint xoffset, GLsizei width, GLenum format, GLenum type,
                   const void* pixels) {
    texSubImage(ctx, 1, target, level, xoffset, 0, 0, width, 1, 1, format, type, pixels);
}

void TexSubImage2D(Context& ctx, GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width,
                   GLsizei height, GLenum format, GLenum type, const void* pixels) {
    texSubImage(ctx, 2, target, level, xoffset, yoffset, 0, width, height, 1, format, type, pixels);
}

void TexSubImage3D(Context& ctx, GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset,
                   GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLenum type, const void* pixels) {
    texSubImage(ctx, 3, target, level, xoffset, yoffset, zoffset, width, height, depth, format, type, pixels);
}

void CompressedTexImage1D(Context& ctx, GLenum target, GLint level, GLenum internalformat, GLsizei width, GLint border,
                          GLsizei imageSize, const void* data) {
    // No GL 3.3 compressed format supports 1D textures.
    (void)target; (void)level; (void)width; (void)border; (void)imageSize; (void)data;
    if (target != GL_TEXTURE_1D && target != GL_PROXY_TEXTURE_1D) return ctx.setError(GL_INVALID_ENUM);
    const FormatInfo* info = formatInfo(internalformat);
    ctx.setError(info && info->compressed ? GL_INVALID_OPERATION : GL_INVALID_ENUM);
}

void CompressedTexImage2D(Context& ctx, GLenum target, GLint level, GLenum internalformat, GLsizei width,
                          GLsizei height, GLint border, GLsizei imageSize, const void* data) {
    compressedTexImage(ctx, 2, target, level, internalformat, width, height, 1, border, imageSize, data);
}

void CompressedTexImage3D(Context& ctx, GLenum target, GLint level, GLenum internalformat, GLsizei width,
                          GLsizei height, GLsizei depth, GLint border, GLsizei imageSize, const void* data) {
    compressedTexImage(ctx, 3, target, level, internalformat, width, height, depth, border, imageSize, data);
}

void CompressedTexSubImage1D(Context& ctx, GLenum target, GLint level, GLint xoffset, GLsizei width, GLenum format,
                             GLsizei imageSize, const void* data) {
    (void)level; (void)xoffset; (void)width; (void)imageSize; (void)data;
    if (target != GL_TEXTURE_1D) return ctx.setError(GL_INVALID_ENUM);
    const FormatInfo* info = formatInfo(format);
    ctx.setError(info && info->compressed ? GL_INVALID_OPERATION : GL_INVALID_ENUM);
}

void CompressedTexSubImage2D(Context& ctx, GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width,
                             GLsizei height, GLenum format, GLsizei imageSize, const void* data) {
    compressedTexSubImage(ctx, 2, target, level, xoffset, yoffset, 0, width, height, 1, format, imageSize, data);
}

void CompressedTexSubImage3D(Context& ctx, GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset,
                             GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLsizei imageSize,
                             const void* data) {
    compressedTexSubImage(ctx, 3, target, level, xoffset, yoffset, zoffset, width, height, depth, format, imageSize,
                          data);
}

void CopyTexImage1D(Context& ctx, GLenum target, GLint level, GLenum internalformat, GLint x, GLint y, GLsizei width,
                    GLint border) {
    copyTexImage(ctx, 1, target, level, internalformat, x, y, width, 1, border);
}

void CopyTexImage2D(Context& ctx, GLenum target, GLint level, GLenum internalformat, GLint x, GLint y, GLsizei width,
                    GLsizei height, GLint border) {
    copyTexImage(ctx, 2, target, level, internalformat, x, y, width, height, border);
}

void CopyTexSubImage1D(Context& ctx, GLenum target, GLint level, GLint xoffset, GLint x, GLint y, GLsizei width) {
    copyTexSubImage(ctx, 1, target, level, xoffset, 0, 0, x, y, width, 1);
}

void CopyTexSubImage2D(Context& ctx, GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y,
                       GLsizei width, GLsizei height) {
    copyTexSubImage(ctx, 2, target, level, xoffset, yoffset, 0, x, y, width, height);
}

void CopyTexSubImage3D(Context& ctx, GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLint x,
                       GLint y, GLsizei width, GLsizei height) {
    copyTexSubImage(ctx, 3, target, level, xoffset, yoffset, zoffset, x, y, width, height);
}

void TexImage2DMultisample(Context& ctx, GLenum target, GLsizei samples, GLenum internalformat, GLsizei width,
                           GLsizei height, GLboolean fixedsamplelocations) {
    if (target != GL_TEXTURE_2D_MULTISAMPLE && target != GL_PROXY_TEXTURE_2D_MULTISAMPLE)
        return ctx.setError(GL_INVALID_ENUM);
    texImageMultisample(ctx, GL_TEXTURE_2D_MULTISAMPLE, target == GL_PROXY_TEXTURE_2D_MULTISAMPLE, samples,
                        internalformat, width, height, 1, fixedsamplelocations);
}

void TexImage3DMultisample(Context& ctx, GLenum target, GLsizei samples, GLenum internalformat, GLsizei width,
                           GLsizei height, GLsizei depth, GLboolean fixedsamplelocations) {
    if (target != GL_TEXTURE_2D_MULTISAMPLE_ARRAY && target != GL_PROXY_TEXTURE_2D_MULTISAMPLE_ARRAY)
        return ctx.setError(GL_INVALID_ENUM);
    texImageMultisample(ctx, GL_TEXTURE_2D_MULTISAMPLE_ARRAY, target == GL_PROXY_TEXTURE_2D_MULTISAMPLE_ARRAY, samples,
                        internalformat, width, height, depth, fixedsamplelocations);
}

void TexBuffer(Context& ctx, GLenum target, GLenum internalformat, GLuint buffer) {
    if (target != GL_TEXTURE_BUFFER) return ctx.setError(GL_INVALID_ENUM);
    switch (internalformat) {
    case GL_R8: case GL_R16: case GL_R16F: case GL_R32F: case GL_R8I: case GL_R16I: case GL_R32I: case GL_R8UI:
    case GL_R16UI: case GL_R32UI: case GL_RG8: case GL_RG16: case GL_RG16F: case GL_RG32F: case GL_RG8I: case GL_RG16I:
    case GL_RG32I: case GL_RG8UI: case GL_RG16UI: case GL_RG32UI: case GL_RGBA8: case GL_RGBA16: case GL_RGBA16F:
    case GL_RGBA32F: case GL_RGBA8I: case GL_RGBA16I: case GL_RGBA32I: case GL_RGBA8UI: case GL_RGBA16UI:
    case GL_RGBA32UI:
        break;
    default: return ctx.setError(GL_INVALID_ENUM);
    }
    Texture* tex = boundTexture(ctx, target);
    if (!tex) return ctx.setError(GL_INVALID_OPERATION);
    RefPtr<Buffer> object;
    if (buffer) {
        ShareLock lock(ctx);
        object = ctx.share().buffers.getRef(buffer);
        if (!object) return ctx.setError(GL_INVALID_OPERATION);
    }
    tex->buffer = object;
    tex->bufferFormat = internalformat;
    tex->bufferOffset = 0;
    tex->bufferSize = -1;
    tex->views.clear();
    imageChanged(ctx, *tex);
}

void GenerateMipmap(Context& ctx, GLenum target) {
    if (target != GL_TEXTURE_1D && target != GL_TEXTURE_2D && target != GL_TEXTURE_3D &&
        target != GL_TEXTURE_1D_ARRAY && target != GL_TEXTURE_2D_ARRAY && target != GL_TEXTURE_CUBE_MAP &&
        !(target == GL_TEXTURE_CUBE_MAP_ARRAY && ctx.version() >= 40))
        return ctx.setError(GL_INVALID_ENUM);
    Texture* tex = boundTexture(ctx, target);
    if (!tex) return ctx.setError(GL_INVALID_OPERATION);
    // An immutable texture's levels are fixed by TexStorage: the base level is
    // clamped to them and generation stops at the last one.
    const int levelEnd = tex->immutable ? std::max<int>(1, tex->immutableLevels) : Texture::kMaxLevels;
    const int base = tex->immutable ? std::min(tex->baseLevel, levelEnd - 1) : tex->baseLevel;
    if (base >= Texture::kMaxLevels) return;
    const TextureImage& baseImage = tex->image(0, base);
    if (!baseImage.defined()) return;
    if (target == GL_TEXTURE_CUBE_MAP) {
        for (int face = 0; face < 6; ++face) {
            const TextureImage& image = tex->image(face, base);
            if (!image.defined() || image.width != baseImage.width || image.height != baseImage.height ||
                image.internalFormat != baseImage.internalFormat || image.width != image.height)
                return ctx.setError(GL_INVALID_OPERATION);
        }
    }
    const FormatInfo* info = baseImage.format;
    if (info->compressed || info->isInteger() || info->isDepthOrStencil()) return ctx.setError(GL_INVALID_OPERATION);
    // Define every level from base + 1 to the end of the chain (or maxLevel).
    const bool oneD = target == GL_TEXTURE_1D || target == GL_TEXTURE_1D_ARRAY;
    GLsizei w = baseImage.width, h = baseImage.height, d = baseImage.depth;
    int last = base;
    for (int level = base + 1; level < levelEnd && level <= tex->maxLevel; ++level) {
        const bool done = w == 1 && (oneD || h == 1) && (target != GL_TEXTURE_3D || d == 1);
        if (done) break;
        w = std::max(1, w / 2);
        if (!oneD) h = std::max(1, h / 2);
        if (target == GL_TEXTURE_3D) d = std::max(1, d / 2);
        if (!tex->immutable)
            for (int face = 0; face < tex->faceCount(); ++face)
                defineImage(ctx, *tex, face, level, baseImage.requestedFormat, info, w, h, d);
        last = level;
    }
    if (last == base || !ensureTextureStorage(ctx, *tex)) return;
    if (!imageHasStorage(ctx, *tex, 0, base) || !imageHasStorage(ctx, *tex, 0, last)) return;
    // Metal generates every level of a texture from its first: use a view starting at the base level.
    const NSUInteger slices = tex->mtl.textureType == MTLTextureTypeCube        ? 6
                              : tex->mtl.textureType == MTLTextureTypeCubeArray ? tex->mtl.arrayLength * 6
                                                                                : tex->mtl.arrayLength;
    id<MTLTexture> view = [tex->mtl newTextureViewWithPixelFormat:tex->mtl.pixelFormat
                                                      textureType:tex->mtl.textureType
                                                           levels:NSMakeRange(static_cast<NSUInteger>(base),
                                                                              static_cast<NSUInteger>(last - base + 1))
                                                           slices:NSMakeRange(0, std::max<NSUInteger>(1, slices))];
    [ctx.stream().blit() generateMipmapsForTexture:view ? view : tex->mtl];
    imageChanged(ctx, *tex);
}

void TexStorage1D(Context& ctx, GLenum target, GLsizei levels, GLenum internalformat, GLsizei width) {
    texStorage(ctx, 1, target, levels, internalformat, width, 1, 1);
}

void TexStorage2D(Context& ctx, GLenum target, GLsizei levels, GLenum internalformat, GLsizei width, GLsizei height) {
    texStorage(ctx, 2, target, levels, internalformat, width, height, 1);
}

void TexStorage3D(Context& ctx, GLenum target, GLsizei levels, GLenum internalformat, GLsizei width, GLsizei height,
                  GLsizei depth) {
    texStorage(ctx, 3, target, levels, internalformat, width, height, depth);
}

void TexStorage2DMultisample(Context& ctx, GLenum target, GLsizei samples, GLenum internalformat, GLsizei width,
                             GLsizei height, GLboolean fixedsamplelocations) {
    if (target != GL_TEXTURE_2D_MULTISAMPLE && target != GL_PROXY_TEXTURE_2D_MULTISAMPLE)
        return ctx.setError(GL_INVALID_ENUM);
    const FormatInfo* info = formatInfo(internalformat);
    if (!info || info->internalFormat != internalformat) return ctx.setError(GL_INVALID_ENUM);
    if (width < 1 || height < 1) return ctx.setError(GL_INVALID_VALUE);
    texImageMultisample(ctx, GL_TEXTURE_2D_MULTISAMPLE, target == GL_PROXY_TEXTURE_2D_MULTISAMPLE, samples,
                        internalformat, width, height, 1, fixedsamplelocations);
    if (Texture* tex = boundTexture(ctx, GL_TEXTURE_2D_MULTISAMPLE); tex && target == GL_TEXTURE_2D_MULTISAMPLE && tex->image(0, 0).defined()) {
        tex->immutable = true;
        tex->immutableLevels = 1;
    }
}

void TexStorage3DMultisample(Context& ctx, GLenum target, GLsizei samples, GLenum internalformat, GLsizei width,
                             GLsizei height, GLsizei depth, GLboolean fixedsamplelocations) {
    if (target != GL_TEXTURE_2D_MULTISAMPLE_ARRAY && target != GL_PROXY_TEXTURE_2D_MULTISAMPLE_ARRAY)
        return ctx.setError(GL_INVALID_ENUM);
    const FormatInfo* info = formatInfo(internalformat);
    if (!info || info->internalFormat != internalformat) return ctx.setError(GL_INVALID_ENUM);
    if (width < 1 || height < 1 || depth < 1) return ctx.setError(GL_INVALID_VALUE);
    texImageMultisample(ctx, GL_TEXTURE_2D_MULTISAMPLE_ARRAY, target == GL_PROXY_TEXTURE_2D_MULTISAMPLE_ARRAY, samples,
                        internalformat, width, height, depth, fixedsamplelocations);
    if (Texture* tex = boundTexture(ctx, GL_TEXTURE_2D_MULTISAMPLE_ARRAY);
        tex && target == GL_TEXTURE_2D_MULTISAMPLE_ARRAY && tex->image(0, 0).defined()) {
        tex->immutable = true;
        tex->immutableLevels = 1;
    }
}

// ------------------------------------------------------------ read-back

void GetTexImage(Context& ctx, GLenum target, GLint level, GLenum format, GLenum type, void* pixels) {
    ImageTarget t;
    const int dims = target == GL_TEXTURE_1D ? 1
                     : (target == GL_TEXTURE_3D || target == GL_TEXTURE_2D_ARRAY || target == GL_TEXTURE_CUBE_MAP_ARRAY) ? 3
                                                                                                                       : 2;
    if (!imageTarget(ctx, target, dims, false, t)) return;
    if (level < 0 || level > maxLevelFor(t.target)) return ctx.setError(GL_INVALID_VALUE);
    if (!checkFormatType(ctx, format, type)) return;
    // Reading stencil indices from a texture needs stencil texturing (GL 4.4).
    if (format == GL_STENCIL_INDEX && ctx.version() < 44) return ctx.setError(GL_INVALID_ENUM);
    const TextureImage& image = t.tex->image(t.face, level);
    if (!image.defined()) return;
    const FormatInfo& info = *image.format;
    const bool depthRequest = format == GL_DEPTH_COMPONENT, stencilRequest = format == GL_STENCIL_INDEX,
               depthStencilRequest = format == GL_DEPTH_STENCIL;
    if ((depthRequest && !info.depthBits) || (depthStencilRequest && !(info.depthBits && info.stencilBits)) ||
        (stencilRequest && !info.stencilBits) || (!depthRequest && !stencilRequest && !depthStencilRequest && info.isDepthOrStencil()))
        return ctx.setError(GL_INVALID_OPERATION);
    if (!info.isDepthOrStencil() && !transferCompatible(format, type, info)) return ctx.setError(GL_INVALID_OPERATION);
    if (info.compressed && !isRgtcStorage(info.storage)) {
        logOnce(LogLevel::Warn, "get-compressed-uncompressed", "GetTexImage of an S3TC/BPTC image is not supported");
        return;
    }
    const PixelStoreParams ps = pixelStoreParams(ctx.state.pack, dims == 3);
    const TransferLayout layout = transferLayout(format, type, image.width, image.height, image.depth, ps);
    uint8_t* dst = nullptr;
    if (!packDestination(ctx, pixels, layout.bytesNeeded, typeSize(type), dst)) return;
    if (!dst || !imageHasStorage(ctx, *t.tex, t.face, level)) return;
    int x = 0, y = 0, z = 0, w = image.width, h = image.height, d = image.depth;
    metalRegion(*t.tex, x, y, z, w, h, d);
    const uint32_t slice = static_cast<uint32_t>(t.tex->target == GL_TEXTURE_CUBE_MAP ? t.face : 0);
    if (isRgtcStorage(info.storage)) {
        // Read the blocks and decode them to float texels.
        const size_t rowBytes = ((static_cast<size_t>(image.width) + 3) / 4) * storageBytes(info.storage);
        const size_t imageBytes = rowBytes * ((static_cast<size_t>(image.height) + 3) / 4);
        id<MTLBuffer> staging = [Device::get().mtl() newBufferWithLength:std::max<size_t>(1, imageBytes * image.depth)
                                                                 options:MTLResourceStorageModeShared];
        id<MTLBlitCommandEncoder> blit = ctx.stream().blit();
        for (int layer = 0; layer < image.depth; ++layer)
            [blit copyFromTexture:t.tex->mtl
                      sourceSlice:static_cast<NSUInteger>(t.tex->target == GL_TEXTURE_CUBE_MAP ? t.face : layer)
                      sourceLevel:static_cast<NSUInteger>(level) sourceOrigin:MTLOriginMake(0, 0, 0)
                       sourceSize:MTLSizeMake(image.width, image.height, 1) toBuffer:staging
                destinationOffset:imageBytes * static_cast<size_t>(layer) destinationBytesPerRow:rowBytes
          destinationBytesPerImage:imageBytes];
        ctx.stream().finish();
        std::vector<float> rgba(static_cast<size_t>(image.width) * image.height * image.depth * 4);
        decodeRgtc(info.storage, static_cast<const uint8_t*>(staging.contents), image.width, image.height, image.depth,
                   rgba.data());
        packPixels(rgba.data(), *formatInfo(GL_RGBA32F), image.width, image.height, image.depth, format, type, ps, false, dst);
        return;
    }
    std::vector<uint8_t> texels =
        readImageTexels(ctx, t.tex->mtl, info.storage, static_cast<uint32_t>(level), slice, x, y, z, w, h, d);
    packPixels(texels.data(), info, image.width, image.height, image.depth, format, type, ps, false, dst);
}

void GetCompressedTexImage(Context& ctx, GLenum target, GLint level, void* img) {
    ImageTarget t;
    const int dims = target == GL_TEXTURE_1D ? 1
                     : (target == GL_TEXTURE_3D || target == GL_TEXTURE_2D_ARRAY || target == GL_TEXTURE_CUBE_MAP_ARRAY) ? 3
                                                                                                                       : 2;
    if (!imageTarget(ctx, target, dims, false, t)) return;
    if (level < 0 || level > maxLevelFor(t.target)) return ctx.setError(GL_INVALID_VALUE);
    const TextureImage& image = t.tex->image(t.face, level);
    if (!image.defined() || !image.format->compressed) return ctx.setError(GL_INVALID_OPERATION);
    const size_t size = compressedSize(*image.format, image.width, image.height, image.depth);
    const CompressedLayout layout = compressedLayout(ctx.state.pack, *image.format, dims, image.width, image.height, image.depth);
    uint8_t* dst = nullptr;
    if (!packDestination(ctx, img, layout.active ? layout.bytesNeeded() : size, 1, dst)) return;
    if (!dst || !imageHasStorage(ctx, *t.tex, t.face, level)) return;
    const size_t blockBytes = storageBytes(image.format->storage);
    const size_t rowBytes = ((static_cast<size_t>(image.width) + 3) / 4) * blockBytes;
    const size_t imageBytes = rowBytes * ((static_cast<size_t>(image.height) + 3) / 4);
    id<MTLBuffer> staging = [Device::get().mtl() newBufferWithLength:std::max<size_t>(1, size)
                                                             options:MTLResourceStorageModeShared];
    id<MTLBlitCommandEncoder> blit = ctx.stream().blit();
    const int layers = image.depth;
    for (int layer = 0; layer < layers; ++layer) {
        const NSUInteger slice = static_cast<NSUInteger>(t.tex->target == GL_TEXTURE_CUBE_MAP ? t.face : layer);
        [blit copyFromTexture:t.tex->mtl sourceSlice:slice sourceLevel:static_cast<NSUInteger>(level)
                 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(image.width, image.height, 1)
                     toBuffer:staging destinationOffset:imageBytes * static_cast<size_t>(layer)
       destinationBytesPerRow:rowBytes destinationBytesPerImage:imageBytes];
    }
    ctx.stream().finish();
    if (!layout.active) {
        std::memcpy(dst, staging.contents, size);
        return;
    }
    // Scatter the blocks into the layout PACK_COMPRESSED_BLOCK_* describes.
    const uint8_t* from = static_cast<const uint8_t*>(staging.contents);
    for (size_t z = 0; z < layout.depth; ++z)
        for (size_t y = 0; y < layout.blocksY; ++y, from += layout.blocksX * layout.blockBytes)
            std::memcpy(dst + layout.start + z * layout.imageStride + y * layout.rowStride, from, layout.blocksX * layout.blockBytes);
}

void GetTexLevelParameteriv(Context& ctx, GLenum target, GLint level, GLenum pname, GLint* params) {
    getTexLevelParameter(ctx, target, level, pname, params);
}

void GetTexLevelParameterfv(Context& ctx, GLenum target, GLint level, GLenum pname, GLfloat* params) {
    getTexLevelParameter(ctx, target, level, pname, params);
}

// ------------------------------------------------------------ parameters

void TexParameteri(Context& ctx, GLenum target, GLenum pname, GLint param) {
    texParameter(ctx, target, pname, paramInts(&param, 1), false);
}

void TexParameteriv(Context& ctx, GLenum target, GLenum pname, const GLint* params) {
    texParameter(ctx, target, pname, paramInts(params, isVectorParam(pname) ? 4 : 1), true);
}

void TexParameterf(Context& ctx, GLenum target, GLenum pname, GLfloat param) {
    texParameter(ctx, target, pname, paramFloats(&param, 1), false);
}

void TexParameterfv(Context& ctx, GLenum target, GLenum pname, const GLfloat* params) {
    texParameter(ctx, target, pname, paramFloats(params, isVectorParam(pname) ? 4 : 1), true);
}

void TexParameterIiv(Context& ctx, GLenum target, GLenum pname, const GLint* params) {
    texParameter(ctx, target, pname, paramInts(params, isVectorParam(pname) ? 4 : 1, ParamValues::PureInt), true);
}

void TexParameterIuiv(Context& ctx, GLenum target, GLenum pname, const GLuint* params) {
    texParameter(ctx, target, pname, paramUints(params, isVectorParam(pname) ? 4 : 1), true);
}

void GetTexParameteriv(Context& ctx, GLenum target, GLenum pname, GLint* params) {
    getTexParameter(ctx, target, pname, params, ParamValues::Int);
}

void GetTexParameterfv(Context& ctx, GLenum target, GLenum pname, GLfloat* params) {
    getTexParameter(ctx, target, pname, params, ParamValues::Float);
}

void GetTexParameterIiv(Context& ctx, GLenum target, GLenum pname, GLint* params) {
    getTexParameter(ctx, target, pname, params, ParamValues::PureInt);
}

void GetTexParameterIuiv(Context& ctx, GLenum target, GLenum pname, GLuint* params) {
    getTexParameter(ctx, target, pname, params, ParamValues::PureUint);
}

// ------------------------------------------------------------ samplers

void GenSamplers(Context& ctx, GLsizei count, GLuint* samplers) {
    if (count < 0) return ctx.setError(GL_INVALID_VALUE);
    ShareLock lock(ctx);
    ctx.share().samplers.generate(count, samplers);
    for (GLsizei i = 0; i < count; ++i) ctx.share().samplers.set(samplers[i], makeRef<Sampler>(samplers[i]));
}

void DeleteSamplers(Context& ctx, GLsizei count, const GLuint* samplers) {
    if (count < 0) return ctx.setError(GL_INVALID_VALUE);
    for (GLsizei i = 0; i < count; ++i) {
        RefPtr<Sampler> sampler;
        {
            ShareLock lock(ctx);
            sampler = ctx.share().samplers.remove(samplers[i]);
        }
        if (!sampler) continue;
        for (TextureUnit& unit : ctx.state.textureUnits)
            if (unit.sampler.get() == sampler.get()) unit.sampler = nullptr;
        ctx.markDirty(kDirtyTextures);
    }
}

GLboolean IsSampler(Context& ctx, GLuint sampler) {
    ShareLock lock(ctx);
    return ctx.share().samplers.exists(sampler) ? GL_TRUE : GL_FALSE;
}

void BindSampler(Context& ctx, GLuint unit, GLuint sampler) {
    if (unit >= static_cast<GLuint>(kMaxCombinedTextureUnits)) return ctx.setError(GL_INVALID_VALUE);
    RefPtr<Sampler> object;
    if (sampler) {
        ShareLock lock(ctx);
        object = ctx.share().samplers.getRef(sampler);
        if (!object) return ctx.setError(GL_INVALID_OPERATION);
    }
    ctx.state.textureUnits[unit].sampler = object;
    ctx.markDirty(kDirtyTextures);
}

namespace {

void samplerParameter(Context& ctx, GLuint sampler, GLenum pname, const ParamValues& v, bool vectorCall) {
    Sampler* object = samplerObject(ctx, sampler);
    if (!object) return ctx.setError(GL_INVALID_OPERATION);
    if (!isSamplerParam(pname) || (!vectorCall && isVectorParam(pname))) return ctx.setError(GL_INVALID_ENUM);
    if (pname == GL_TEXTURE_MAX_ANISOTROPY && !ctx.hasExtension("GL_EXT_texture_filter_anisotropic") && ctx.version() < 46)
        return ctx.setError(GL_INVALID_ENUM);
    const GLenum error = setSamplerParam(object->params, pname, v, 0, ctx.isCompat());
    if (error) return ctx.setError(error);
    ctx.markDirty(kDirtyTextures);
}

template <class T>
void getSamplerParameter(Context& ctx, GLuint sampler, GLenum pname, T* params, ParamValues::Kind call) {
    Sampler* object = samplerObject(ctx, sampler);
    if (!object) return ctx.setError(GL_INVALID_OPERATION);
    if (pname == GL_TEXTURE_MAX_ANISOTROPY && !ctx.hasExtension("GL_EXT_texture_filter_anisotropic") && ctx.version() < 46)
        return ctx.setError(GL_INVALID_ENUM);
    ParamValues v;
    if (!getSamplerParam(object->params, pname, v)) return ctx.setError(GL_INVALID_ENUM);
    writeParam(v, pname, params, call);
}

}  // namespace

void SamplerParameteri(Context& ctx, GLuint sampler, GLenum pname, GLint param) {
    samplerParameter(ctx, sampler, pname, paramInts(&param, 1), false);
}

void SamplerParameteriv(Context& ctx, GLuint sampler, GLenum pname, const GLint* param) {
    samplerParameter(ctx, sampler, pname, paramInts(param, isVectorParam(pname) ? 4 : 1), true);
}

void SamplerParameterf(Context& ctx, GLuint sampler, GLenum pname, GLfloat param) {
    samplerParameter(ctx, sampler, pname, paramFloats(&param, 1), false);
}

void SamplerParameterfv(Context& ctx, GLuint sampler, GLenum pname, const GLfloat* param) {
    samplerParameter(ctx, sampler, pname, paramFloats(param, isVectorParam(pname) ? 4 : 1), true);
}

void SamplerParameterIiv(Context& ctx, GLuint sampler, GLenum pname, const GLint* param) {
    samplerParameter(ctx, sampler, pname, paramInts(param, isVectorParam(pname) ? 4 : 1, ParamValues::PureInt), true);
}

void SamplerParameterIuiv(Context& ctx, GLuint sampler, GLenum pname, const GLuint* param) {
    samplerParameter(ctx, sampler, pname, paramUints(param, isVectorParam(pname) ? 4 : 1), true);
}

void GetSamplerParameteriv(Context& ctx, GLuint sampler, GLenum pname, GLint* params) {
    getSamplerParameter(ctx, sampler, pname, params, ParamValues::Int);
}

void GetSamplerParameterfv(Context& ctx, GLuint sampler, GLenum pname, GLfloat* params) {
    getSamplerParameter(ctx, sampler, pname, params, ParamValues::Float);
}

void GetSamplerParameterIiv(Context& ctx, GLuint sampler, GLenum pname, GLint* params) {
    getSamplerParameter(ctx, sampler, pname, params, ParamValues::PureInt);
}

void GetSamplerParameterIuiv(Context& ctx, GLuint sampler, GLenum pname, GLuint* params) {
    getSamplerParameter(ctx, sampler, pname, params, ParamValues::PureUint);
}

}  // namespace wmgl::api
