// Storage -> MTLPixelFormat, and Metal texture helpers.
#pragma once

#import <Metal/Metal.h>

#include "core/format/formats.h"

namespace wmgl {

MTLPixelFormat mtlPixelFormat(Storage storage);
// Swizzle channels for sampling a texture of this format with GL swizzle `gl`
// (GL_RED.. GL_ONE, applied after the format's own emulation swizzle).
MTLTextureSwizzleChannels composeSwizzle(const FormatInfo& format, const GLenum gl[4]);

}  // namespace wmgl
