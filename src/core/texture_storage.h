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

// GL texture completeness (GL 3.3 section 3.8.14) for sampling with `params`.
bool textureComplete(Context& ctx, Texture& tex, const SamplerParams* params);

// View for sampling: levels [base, max], composed swizzle, linear view of
// stencil/depth as appropriate.
id<MTLTexture> samplingView(Context& ctx, Texture& tex);

id<MTLSamplerState> samplerState(Context& ctx, Texture& tex, const SamplerParams& params, bool shadow);
id<MTLSamplerState> samplerState(Context& ctx, const SamplerParams& params, bool shadow);

// 1x1 texture returning (0, 0, 0, 1) for incomplete textures of a sampler type.
id<MTLTexture> dummyTexture(GLenum samplerType);

// Number of mip levels in a full chain for these dimensions.
GLsizei fullMipCount(GLsizei width, GLsizei height, GLsizei depth);
MTLTextureType mtlTextureType(GLenum target, GLsizei samples);

// Uploads tightly packed texels in `storage` layout to a region of tex.mtl.
void uploadTexels(Context& ctx, Texture& tex, int face, int level, int x, int y, int z, int width, int height,
                  int depth, const void* data, size_t bytesPerRow, size_t bytesPerImage);

}  // namespace wmgl
