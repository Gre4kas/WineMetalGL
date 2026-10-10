// OpenGL internal formats and how WineMetalGL stores them.
//
// Pure C++ (no Metal headers) so the pixel-transfer engine and its host tests
// can use it. src/core/metal/pixel_format.mm maps Storage to MTLPixelFormat.
#pragma once

#include <cstdint>

#include "core/gl/types.h"

namespace wmgl {

// Storage formats WineMetalGL uses on Metal. Names follow MTLPixelFormat.
enum class Storage : uint8_t {
    Invalid,
    R8Unorm, R8Snorm, R8Uint, R8Sint,
    R16Unorm, R16Snorm, R16Uint, R16Sint, R16Float,
    RG8Unorm, RG8Snorm, RG8Uint, RG8Sint,
    R32Uint, R32Sint, R32Float,
    RG16Unorm, RG16Snorm, RG16Uint, RG16Sint, RG16Float,
    RGBA8Unorm, RGBA8Unorm_sRGB, RGBA8Snorm, RGBA8Uint, RGBA8Sint,
    BGRA8Unorm,
    RGB10A2Unorm, RGB10A2Uint, RG11B10Float, RGB9E5Float,
    RG32Uint, RG32Sint, RG32Float,
    RGBA16Unorm, RGBA16Snorm, RGBA16Uint, RGBA16Sint, RGBA16Float,
    RGBA32Uint, RGBA32Sint, RGBA32Float,
    BC1_RGBA, BC1_RGBA_sRGB, BC2_RGBA, BC2_RGBA_sRGB, BC3_RGBA, BC3_RGBA_sRGB,
    BC4_RUnorm, BC4_RSnorm, BC5_RGUnorm, BC5_RGSnorm,
    BC6H_RGBFloat, BC6H_RGBUfloat, BC7_RGBAUnorm, BC7_RGBAUnorm_sRGB,
    Depth16Unorm, Depth32Float, Stencil8, Depth32Float_Stencil8,
    Count
};

// Component data type as GL reports it (TEXTURE_RED_TYPE etc.).
enum class ComponentType : uint8_t { None, UnsignedNormalized, SignedNormalized, Float, Int, UnsignedInt };

// Where each GL channel (R, G, B, A) is read from in the stored texel.
enum Swizzle : uint8_t { SwzR = 0, SwzG = 1, SwzB = 2, SwzA = 3, SwzZero = 4, SwzOne = 5 };

struct FormatInfo {
    GLenum internalFormat = 0;       // the sized internal format
    GLenum baseFormat = 0;           // GL_RED, GL_RG, GL_RGB, GL_RGBA, GL_DEPTH_COMPONENT, ...
    Storage storage = Storage::Invalid;
    ComponentType type = ComponentType::None;
    uint8_t redBits = 0, greenBits = 0, blueBits = 0, alphaBits = 0;
    uint8_t depthBits = 0, stencilBits = 0, sharedBits = 0;
    uint8_t luminanceBits = 0, intensityBits = 0;
    Swizzle swizzle[4] = {SwzR, SwzG, SwzB, SwzA};  // emulation swizzle
    bool srgb = false;
    bool compressed = false;
    bool colorRenderable = false;    // GL 3.3 "color-renderable"
    bool depthRenderable = false;
    bool stencilRenderable = false;
    bool textureFilterable = true;
    bool requiredRenderbuffer = false;  // one of the GL 3.3 required renderbuffer formats
    bool storedAlphaIsOne = false;   // RGB stored as RGBA: alpha is not part of the format
    GLenum defaultTransferFormat = 0, defaultTransferType = 0;  // used for unsized-format resolution

    bool isDepth() const { return depthBits != 0; }
    bool isStencil() const { return stencilBits != 0; }
    bool isDepthOrStencil() const { return depthBits || stencilBits; }
    bool isInteger() const { return type == ComponentType::Int || type == ComponentType::UnsignedInt; }
    bool isSignedInteger() const { return type == ComponentType::Int; }
    bool isFloat() const { return type == ComponentType::Float; }
};

// Format info for a sized internal format, or nullptr if unknown.
const FormatInfo* formatInfo(GLenum internalFormat);

// Resolves an unsized internal format (glTexImage with GL_RGBA etc.) using the
// transfer format/type, as GL's "effective internal format" rules describe.
GLenum resolveInternalFormat(GLenum internalFormat, GLenum format, GLenum type);

// True when the internal format is accepted by glTexImage* in this profile.
bool isValidTextureInternalFormat(GLenum internalFormat, bool compatibility);

// Bytes per texel for uncompressed storage, or per 4x4 block for BC formats.
uint32_t storageBytes(Storage storage);
bool storageIsCompressed(Storage storage);
const char* storageName(Storage storage);

}  // namespace wmgl
