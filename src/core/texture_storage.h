// Metal storage for GL textures: allocation, completeness, sampling views.
#pragma once

#import <Metal/Metal.h>

#include "core/objects.h"

namespace wmgl {

class Context;

// Allocates (or reallocates) tex.mtl to match its defined images. Storage
// covers the full mip chain derived from level 0 so later levels fit.
// Returns false if the texture has no base image.
bool ensureTextureStorage(Context& ctx, Texture& tex);
// The size of `level` in tex.mtl.
void storageLevelSize(const Texture& tex, int level, GLsizei& w, GLsizei& h, GLsizei& d);
// True if image (face, level) has storage in tex.mtl that matches its size
// (a level defined off the mip chain has none).
bool imageHasStorage(Context& ctx, Texture& tex, int face, int level);

// GL texture completeness (GL 3.3 section 3.8.14) for sampling with `params`.
bool textureComplete(Context& ctx, Texture& tex, const SamplerParams* params);

// View for sampling: levels [base, max], composed swizzle, linear view of
// stencil/depth as appropriate.
id<MTLTexture> samplingView(Context& ctx, Texture& tex);

id<MTLSamplerState> samplerState(Context& ctx, Texture& tex, const SamplerParams& params, bool shadow);
id<MTLSamplerState> samplerState(Context& ctx, const SamplerParams& params, bool shadow);

// 1x1 texture returning (0, 0, 0, 1) for incomplete textures of a sampler type.
id<MTLTexture> dummyTexture(GLenum samplerType);
// GL 4.2 image units: the view an image uniform of GL type `imageType` reads
// for `unit` (format, level, layer or all layers), or nil when the unit
// cannot give one (no texture, incompatible format or shape).
struct ImageUnit;
// `format` is the image uniform's format qualifier, which decides how the
// texels are interpreted (the unit's format when it has none).
//
// Some bindings get a shadow texture instead of a view, which the draw
// copies the texels into before it and back after it
// (DrawState::prepareImageShadows):
//  - one slice of a 3D texture bound to a 2D image (Metal has no 2D view of
//    a 3D texture's slice);
//  - an r32i or r32ui image of a texture of another 32-bit format (Metal
//    texture atomics need an R32 texture made for them).
// `shadow`, when given, describes the shadow imageView returns, if any.
struct ImageShadow {
    id<MTLTexture> texture;  // the texture the image unit names
    id<MTLTexture> shadow;   // nil: the binding is a view, not a shadow
    NSUInteger level = 0, firstSlice = 0, slices = 1, firstPlane = 0, planes = 1;
    NSUInteger width = 0, height = 0, texelBytes = 4;
};
id<MTLTexture> imageView(Context& ctx, const ImageUnit& unit, GLenum imageType, GLenum format,
                         ImageShadow* shadow = nullptr);
// Copies a shadow's texels from its texture (toShadow) or back.
void copyImageShadow(Context& ctx, const ImageShadow& shadow, bool toShadow);
// A small writable texture for image uniforms without a usable unit.
id<MTLTexture> dummyImage(GLenum imageType);

// Number of mip levels in a full chain for these dimensions.
GLsizei fullMipCount(GLsizei width, GLsizei height, GLsizei depth);
MTLTextureType mtlTextureType(GLenum target, GLsizei samples);

// Uploads tightly packed texels in `storage` layout to a region of tex.mtl.
void uploadTexels(Context& ctx, Texture& tex, int face, int level, int x, int y, int z, int width, int height,
                  int depth, const void* data, size_t bytesPerRow, size_t bytesPerImage);

namespace api {
// glGenerateMipmap of a depth or depth/stencil texture (api/blit.mm).
void generateDepthMipmaps(Context& ctx, Texture& tex, int base, int last);
// Whether a compressed format is exposed in this context (api/texture.mm).
bool compressedFormatSupported(Context& ctx, const FormatInfo* info);
}  // namespace api

}  // namespace wmgl
