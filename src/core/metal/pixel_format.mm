#include "core/metal/pixel_format.h"

namespace wmgl {

MTLPixelFormat mtlPixelFormat(Storage storage) {
    switch (storage) {
    case Storage::R8Unorm: return MTLPixelFormatR8Unorm;
    case Storage::R8Snorm: return MTLPixelFormatR8Snorm;
    case Storage::R8Uint: return MTLPixelFormatR8Uint;
    case Storage::R8Sint: return MTLPixelFormatR8Sint;
    case Storage::R16Unorm: return MTLPixelFormatR16Unorm;
    case Storage::R16Snorm: return MTLPixelFormatR16Snorm;
    case Storage::R16Uint: return MTLPixelFormatR16Uint;
    case Storage::R16Sint: return MTLPixelFormatR16Sint;
    case Storage::R16Float: return MTLPixelFormatR16Float;
    case Storage::RG8Unorm: return MTLPixelFormatRG8Unorm;
    case Storage::RG8Snorm: return MTLPixelFormatRG8Snorm;
    case Storage::RG8Uint: return MTLPixelFormatRG8Uint;
    case Storage::RG8Sint: return MTLPixelFormatRG8Sint;
    case Storage::R32Uint: return MTLPixelFormatR32Uint;
    case Storage::R32Sint: return MTLPixelFormatR32Sint;
    case Storage::R32Float: return MTLPixelFormatR32Float;
    case Storage::RG16Unorm: return MTLPixelFormatRG16Unorm;
    case Storage::RG16Snorm: return MTLPixelFormatRG16Snorm;
    case Storage::RG16Uint: return MTLPixelFormatRG16Uint;
    case Storage::RG16Sint: return MTLPixelFormatRG16Sint;
    case Storage::RG16Float: return MTLPixelFormatRG16Float;
    case Storage::RGBA8Unorm: return MTLPixelFormatRGBA8Unorm;
    case Storage::RGBA8Unorm_sRGB: return MTLPixelFormatRGBA8Unorm_sRGB;
    case Storage::RGBA8Snorm: return MTLPixelFormatRGBA8Snorm;
    case Storage::RGBA8Uint: return MTLPixelFormatRGBA8Uint;
    case Storage::RGBA8Sint: return MTLPixelFormatRGBA8Sint;
    case Storage::BGRA8Unorm: return MTLPixelFormatBGRA8Unorm;
    case Storage::RGB10A2Unorm: return MTLPixelFormatRGB10A2Unorm;
    case Storage::RGB10A2Uint: return MTLPixelFormatRGB10A2Uint;
    case Storage::RG11B10Float: return MTLPixelFormatRG11B10Float;
    case Storage::RGB9E5Float: return MTLPixelFormatRGB9E5Float;
    case Storage::RG32Uint: return MTLPixelFormatRG32Uint;
    case Storage::RG32Sint: return MTLPixelFormatRG32Sint;
    case Storage::RG32Float: return MTLPixelFormatRG32Float;
    case Storage::RGBA16Unorm: return MTLPixelFormatRGBA16Unorm;
    case Storage::RGBA16Snorm: return MTLPixelFormatRGBA16Snorm;
    case Storage::RGBA16Uint: return MTLPixelFormatRGBA16Uint;
    case Storage::RGBA16Sint: return MTLPixelFormatRGBA16Sint;
    case Storage::RGBA16Float: return MTLPixelFormatRGBA16Float;
    case Storage::RGBA32Uint: return MTLPixelFormatRGBA32Uint;
    case Storage::RGBA32Sint: return MTLPixelFormatRGBA32Sint;
    case Storage::RGBA32Float: return MTLPixelFormatRGBA32Float;
    case Storage::BC1_RGBA: return MTLPixelFormatBC1_RGBA;
    case Storage::BC1_RGBA_sRGB: return MTLPixelFormatBC1_RGBA_sRGB;
    case Storage::BC2_RGBA: return MTLPixelFormatBC2_RGBA;
    case Storage::BC2_RGBA_sRGB: return MTLPixelFormatBC2_RGBA_sRGB;
    case Storage::BC3_RGBA: return MTLPixelFormatBC3_RGBA;
    case Storage::BC3_RGBA_sRGB: return MTLPixelFormatBC3_RGBA_sRGB;
    case Storage::BC4_RUnorm: return MTLPixelFormatBC4_RUnorm;
    case Storage::BC4_RSnorm: return MTLPixelFormatBC4_RSnorm;
    case Storage::BC5_RGUnorm: return MTLPixelFormatBC5_RGUnorm;
    case Storage::BC5_RGSnorm: return MTLPixelFormatBC5_RGSnorm;
    case Storage::BC6H_RGBFloat: return MTLPixelFormatBC6H_RGBFloat;
    case Storage::BC6H_RGBUfloat: return MTLPixelFormatBC6H_RGBUfloat;
    case Storage::BC7_RGBAUnorm: return MTLPixelFormatBC7_RGBAUnorm;
    case Storage::BC7_RGBAUnorm_sRGB: return MTLPixelFormatBC7_RGBAUnorm_sRGB;
    case Storage::ETC2_RGB8: return MTLPixelFormatETC2_RGB8;
    case Storage::ETC2_RGB8_sRGB: return MTLPixelFormatETC2_RGB8_sRGB;
    case Storage::ETC2_RGB8A1: return MTLPixelFormatETC2_RGB8A1;
    case Storage::ETC2_RGB8A1_sRGB: return MTLPixelFormatETC2_RGB8A1_sRGB;
    case Storage::EAC_RGBA8: return MTLPixelFormatEAC_RGBA8;
    case Storage::EAC_RGBA8_sRGB: return MTLPixelFormatEAC_RGBA8_sRGB;
    case Storage::EAC_R11Unorm: return MTLPixelFormatEAC_R11Unorm;
    case Storage::EAC_R11Snorm: return MTLPixelFormatEAC_R11Snorm;
    case Storage::EAC_RG11Unorm: return MTLPixelFormatEAC_RG11Unorm;
    case Storage::EAC_RG11Snorm: return MTLPixelFormatEAC_RG11Snorm;
    case Storage::ASTC_4x4: return MTLPixelFormatASTC_4x4_LDR;
    case Storage::ASTC_4x4_sRGB: return MTLPixelFormatASTC_4x4_sRGB;
    case Storage::ASTC_5x4: return MTLPixelFormatASTC_5x4_LDR;
    case Storage::ASTC_5x4_sRGB: return MTLPixelFormatASTC_5x4_sRGB;
    case Storage::ASTC_5x5: return MTLPixelFormatASTC_5x5_LDR;
    case Storage::ASTC_5x5_sRGB: return MTLPixelFormatASTC_5x5_sRGB;
    case Storage::ASTC_6x5: return MTLPixelFormatASTC_6x5_LDR;
    case Storage::ASTC_6x5_sRGB: return MTLPixelFormatASTC_6x5_sRGB;
    case Storage::ASTC_6x6: return MTLPixelFormatASTC_6x6_LDR;
    case Storage::ASTC_6x6_sRGB: return MTLPixelFormatASTC_6x6_sRGB;
    case Storage::ASTC_8x5: return MTLPixelFormatASTC_8x5_LDR;
    case Storage::ASTC_8x5_sRGB: return MTLPixelFormatASTC_8x5_sRGB;
    case Storage::ASTC_8x6: return MTLPixelFormatASTC_8x6_LDR;
    case Storage::ASTC_8x6_sRGB: return MTLPixelFormatASTC_8x6_sRGB;
    case Storage::ASTC_8x8: return MTLPixelFormatASTC_8x8_LDR;
    case Storage::ASTC_8x8_sRGB: return MTLPixelFormatASTC_8x8_sRGB;
    case Storage::ASTC_10x5: return MTLPixelFormatASTC_10x5_LDR;
    case Storage::ASTC_10x5_sRGB: return MTLPixelFormatASTC_10x5_sRGB;
    case Storage::ASTC_10x6: return MTLPixelFormatASTC_10x6_LDR;
    case Storage::ASTC_10x6_sRGB: return MTLPixelFormatASTC_10x6_sRGB;
    case Storage::ASTC_10x8: return MTLPixelFormatASTC_10x8_LDR;
    case Storage::ASTC_10x8_sRGB: return MTLPixelFormatASTC_10x8_sRGB;
    case Storage::ASTC_10x10: return MTLPixelFormatASTC_10x10_LDR;
    case Storage::ASTC_10x10_sRGB: return MTLPixelFormatASTC_10x10_sRGB;
    case Storage::ASTC_12x10: return MTLPixelFormatASTC_12x10_LDR;
    case Storage::ASTC_12x10_sRGB: return MTLPixelFormatASTC_12x10_sRGB;
    case Storage::ASTC_12x12: return MTLPixelFormatASTC_12x12_LDR;
    case Storage::ASTC_12x12_sRGB: return MTLPixelFormatASTC_12x12_sRGB;
    case Storage::Depth16Unorm: return MTLPixelFormatDepth16Unorm;
    case Storage::Depth32Float: return MTLPixelFormatDepth32Float;
    case Storage::Stencil8: return MTLPixelFormatStencil8;
    case Storage::Depth32Float_Stencil8: return MTLPixelFormatDepth32Float_Stencil8;
    default: return MTLPixelFormatInvalid;
    }
}

static MTLTextureSwizzle swizzleFor(Swizzle s) {
    switch (s) {
    case SwzR: return MTLTextureSwizzleRed;
    case SwzG: return MTLTextureSwizzleGreen;
    case SwzB: return MTLTextureSwizzleBlue;
    case SwzA: return MTLTextureSwizzleAlpha;
    case SwzZero: return MTLTextureSwizzleZero;
    default: return MTLTextureSwizzleOne;
    }
}

MTLTextureSwizzleChannels composeSwizzle(const FormatInfo& format, const GLenum gl[4]) {
    Swizzle out[4];
    for (int i = 0; i < 4; ++i) {
        switch (gl[i]) {
        case GL_RED: out[i] = format.swizzle[0]; break;
        case GL_GREEN: out[i] = format.swizzle[1]; break;
        case GL_BLUE: out[i] = format.swizzle[2]; break;
        case GL_ALPHA: out[i] = format.swizzle[3]; break;
        case GL_ZERO: out[i] = SwzZero; break;
        default: out[i] = SwzOne; break;
        }
    }
    return MTLTextureSwizzleChannelsMake(swizzleFor(out[0]), swizzleFor(out[1]), swizzleFor(out[2]), swizzleFor(out[3]));
}

}  // namespace wmgl
