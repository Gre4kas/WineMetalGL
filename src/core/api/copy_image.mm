// GL_ARB_copy_image (GL 4.3 section 18.3.3): raw texel copies between
// textures and renderbuffers of compatible formats, as Metal blits. Formats
// with the same Metal pixel format copy texture to texture; other compatible
// pairs (same texel size, or an uncompressed texel the size of a compressed
// block) copy their bytes through a staging buffer.

#include <algorithm>

#include "core/context.h"
#include "core/format/formats.h"
#include "core/metal/device.h"
#include "core/texture_storage.h"
#include "gl_api.gen.h"

namespace wmgl::api {
namespace {

struct Image {
    id<MTLTexture> mtl = nil;
    const FormatInfo* format = nullptr;
    GLsizei width = 0, height = 0, depth = 1;  // depth: layers, faces or 3D depth
    bool layered3D = false;                    // z is a 3D depth (else a slice)
    uint32_t samples = 1;
};

// The image of `name`/`target` at `level`, with its Metal storage.
bool resolveImage(Context& ctx, GLuint name, GLenum target, GLint level, Image& out) {
    if (target == GL_RENDERBUFFER) {
        Renderbuffer* rb = ctx.renderbuffer(name);
        if (!rb) return ctx.setError(GL_INVALID_VALUE), false;
        if (level != 0 || !rb->format || !rb->mtl) return ctx.setError(GL_INVALID_VALUE), false;
        out = {rb->mtl, rb->format, rb->width, rb->height, 1, false, static_cast<uint32_t>(std::max(rb->samples, 1))};
        return true;
    }
    switch (target) {
    case GL_TEXTURE_1D: case GL_TEXTURE_2D: case GL_TEXTURE_3D: case GL_TEXTURE_1D_ARRAY: case GL_TEXTURE_2D_ARRAY:
    case GL_TEXTURE_RECTANGLE: case GL_TEXTURE_CUBE_MAP: case GL_TEXTURE_CUBE_MAP_ARRAY: case GL_TEXTURE_2D_MULTISAMPLE:
    case GL_TEXTURE_2D_MULTISAMPLE_ARRAY:
        break;
    default: return ctx.setError(GL_INVALID_ENUM), false;
    }
    Texture* tex = ctx.texture(name);
    if (!name || !tex) return ctx.setError(GL_INVALID_VALUE), false;
    if (tex->target != target) return ctx.setError(GL_INVALID_ENUM), false;
    if (level < 0 || level >= Texture::kMaxLevels) return ctx.setError(GL_INVALID_VALUE), false;
    const TextureImage& image = tex->image(0, level);
    if (!image.defined()) return ctx.setError(GL_INVALID_VALUE), false;
    // Cube maps must be cube complete to be copied from or to (all faces).
    if (target == GL_TEXTURE_CUBE_MAP)
        for (int f = 1; f < 6; ++f)
            if (!tex->image(f, level).defined()) return ctx.setError(GL_INVALID_OPERATION), false;
    if (!ensureTextureStorage(ctx, *tex) || !tex->mtl) return ctx.setError(GL_INVALID_OPERATION), false;
    out.mtl = tex->mtl;
    out.format = image.format;
    out.width = image.width;
    out.height = target == GL_TEXTURE_1D_ARRAY ? 1 : std::max(image.height, 1);
    out.depth = target == GL_TEXTURE_CUBE_MAP ? 6 : target == GL_TEXTURE_1D_ARRAY ? std::max(image.height, 1)
                                                                                    : std::max(image.depth, 1);
    out.layered3D = target == GL_TEXTURE_3D;
    out.samples = static_cast<uint32_t>(std::max(image.samples, 1));
    return true;
}

// Block (or texel) footprint and bytes of a format.
void blockOf(const FormatInfo& f, uint32_t& w, uint32_t& h, uint32_t& bytes) {
    if (f.compressed) {
        w = h = 4;
        bytes = storageBytes(f.storage);
    } else {
        w = h = 1;
        bytes = storageBytes(f.storage);
    }
}

}  // namespace

void CopyImageSubData(Context& ctx, GLuint srcName, GLenum srcTarget, GLint srcLevel, GLint srcX, GLint srcY, GLint srcZ,
                      GLuint dstName, GLenum dstTarget, GLint dstLevel, GLint dstX, GLint dstY, GLint dstZ, GLsizei srcWidth,
                      GLsizei srcHeight, GLsizei srcDepth) {
    Image src, dst;
    if (!resolveImage(ctx, srcName, srcTarget, srcLevel, src) || !resolveImage(ctx, dstName, dstTarget, dstLevel, dst)) return;
    if (srcWidth < 0 || srcHeight < 0 || srcDepth < 0) return ctx.setError(GL_INVALID_VALUE);
    if (src.samples != dst.samples) return ctx.setError(GL_INVALID_OPERATION);
    uint32_t sbw, sbh, sbytes, dbw, dbh, dbytes;
    blockOf(*src.format, sbw, sbh, sbytes);
    blockOf(*dst.format, dbw, dbh, dbytes);
    // Compatible: same block size in bytes (texel <-> texel, block <-> block, texel <-> block).
    if (sbytes != dbytes || src.format->isDepthOrStencil() != dst.format->isDepthOrStencil() ||
        (src.format->isDepthOrStencil() && src.format != dst.format))
        return ctx.setError(GL_INVALID_OPERATION);
    // The destination region covers the same blocks (a compressed source block is one destination texel).
    const GLsizei dstWidth = static_cast<GLsizei>(srcWidth / static_cast<GLsizei>(sbw) * static_cast<GLsizei>(dbw));
    const GLsizei dstHeight = static_cast<GLsizei>(srcHeight / static_cast<GLsizei>(sbh) * static_cast<GLsizei>(dbh));
    auto inside = [](const Image& im, GLint x, GLint y, GLint z, GLsizei w, GLsizei h, GLsizei d) {
        return x >= 0 && y >= 0 && z >= 0 && x + w <= im.width && y + h <= im.height && z + d <= im.depth;
    };
    if (!inside(src, srcX, srcY, srcZ, srcWidth, srcHeight, srcDepth) ||
        !inside(dst, dstX, dstY, dstZ, dstWidth, dstHeight, srcDepth))
        return ctx.setError(GL_INVALID_VALUE);
    // Compressed regions are whole blocks (or reach the image edge).
    auto aligned = [](const Image& im, GLint x, GLint y, GLsizei w, GLsizei h, uint32_t bw, uint32_t bh) {
        if (bw == 1) return true;
        return x % static_cast<GLint>(bw) == 0 && y % static_cast<GLint>(bh) == 0 &&
               (w % static_cast<GLsizei>(bw) == 0 || x + w == im.width) && (h % static_cast<GLsizei>(bh) == 0 || y + h == im.height);
    };
    if (!aligned(src, srcX, srcY, srcWidth, srcHeight, sbw, sbh) || !aligned(dst, dstX, dstY, dstWidth, dstHeight, dbw, dbh))
        return ctx.setError(GL_INVALID_VALUE);
    if (!srcWidth || !srcHeight || !srcDepth) return;
    if (src.samples > 1) {
        // Metal blits do not copy multisample textures.
        ctx.setError(GL_INVALID_OPERATION);
        return;
    }
    id<MTLBlitCommandEncoder> blit = ctx.stream().blit();
    const bool direct = src.mtl.pixelFormat == dst.mtl.pixelFormat;
    const uint32_t blocksW = (static_cast<uint32_t>(srcWidth) + sbw - 1) / sbw;
    const uint32_t blocksH = (static_cast<uint32_t>(srcHeight) + sbh - 1) / sbh;
    const NSUInteger bytesPerRow = static_cast<NSUInteger>(blocksW) * sbytes;
    const NSUInteger bytesPerImage = bytesPerRow * blocksH;
    id<MTLBuffer> staging = nil;
    if (!direct)
        staging = [Device::get().mtl() newBufferWithLength:std::max<NSUInteger>(bytesPerImage, 16)
                                                   options:MTLResourceStorageModePrivate];
    for (GLsizei layer = 0; layer < srcDepth; ++layer) {
        const NSUInteger sSlice = src.layered3D ? 0 : static_cast<NSUInteger>(srcZ + layer);
        const NSUInteger dSlice = dst.layered3D ? 0 : static_cast<NSUInteger>(dstZ + layer);
        const MTLOrigin sOrigin = {static_cast<NSUInteger>(srcX), static_cast<NSUInteger>(srcY),
                                   src.layered3D ? static_cast<NSUInteger>(srcZ + layer) : 0};
        const MTLOrigin dOrigin = {static_cast<NSUInteger>(dstX), static_cast<NSUInteger>(dstY),
                                   dst.layered3D ? static_cast<NSUInteger>(dstZ + layer) : 0};
        const MTLSize sSize = {static_cast<NSUInteger>(srcWidth), static_cast<NSUInteger>(srcHeight), 1};
        if (direct) {
            [blit copyFromTexture:src.mtl sourceSlice:sSlice sourceLevel:static_cast<NSUInteger>(srcLevel) sourceOrigin:sOrigin
                       sourceSize:sSize toTexture:dst.mtl destinationSlice:dSlice destinationLevel:static_cast<NSUInteger>(dstLevel)
                destinationOrigin:dOrigin];
            continue;
        }
        [blit copyFromTexture:src.mtl sourceSlice:sSlice sourceLevel:static_cast<NSUInteger>(srcLevel) sourceOrigin:sOrigin
                   sourceSize:sSize toBuffer:staging destinationOffset:0 destinationBytesPerRow:bytesPerRow
        destinationBytesPerImage:bytesPerImage];
        const MTLSize dSize = {static_cast<NSUInteger>(dstWidth ? dstWidth : static_cast<GLsizei>(blocksW * dbw)),
                               static_cast<NSUInteger>(dstHeight ? dstHeight : static_cast<GLsizei>(blocksH * dbh)), 1};
        [blit copyFromBuffer:staging sourceOffset:0 sourceBytesPerRow:bytesPerRow sourceBytesPerImage:bytesPerImage
                  sourceSize:dSize toTexture:dst.mtl destinationSlice:dSlice destinationLevel:static_cast<NSUInteger>(dstLevel)
           destinationOrigin:dOrigin];
    }
}

}  // namespace wmgl::api
