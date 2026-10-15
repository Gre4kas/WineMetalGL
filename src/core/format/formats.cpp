#include "core/format/formats.h"

#include <unordered_map>

namespace wmgl {
namespace {

using CT = ComponentType;
using S = Storage;

struct Builder {
    FormatInfo f;
    Builder(GLenum internal, GLenum base, S storage, CT type) {
        f.internalFormat = internal;
        f.baseFormat = base;
        f.storage = storage;
        f.type = type;
    }
    Builder& rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
        f.redBits = r; f.greenBits = g; f.blueBits = b; f.alphaBits = a;
        return *this;
    }
    Builder& swz(Swizzle r, Swizzle g, Swizzle b, Swizzle a) {
        f.swizzle[0] = r; f.swizzle[1] = g; f.swizzle[2] = b; f.swizzle[3] = a;
        return *this;
    }
    Builder& transfer(GLenum format, GLenum type) {
        f.defaultTransferFormat = format; f.defaultTransferType = type;
        return *this;
    }
    Builder& srgb() { f.srgb = true; return *this; }
    Builder& rb() { f.requiredRenderbuffer = true; return *this; }
};

std::unordered_map<GLenum, FormatInfo> buildTable() {
    std::unordered_map<GLenum, FormatInfo> table;
    auto add = [&](Builder b) {
        FormatInfo& f = b.f;
        const GLenum base = f.baseFormat;
        const bool colorBase = base == GL_RED || base == GL_RG || base == GL_RGB || base == GL_RGBA;
        f.compressed = storageIsCompressed(f.storage);
        // SNORM formats are not required to be colour-renderable in GL 3.3, but
        // Metal renders and blends them on every Apple GPU, so they are.
        f.colorRenderable = colorBase && !f.compressed && f.storage != S::RGB9E5Float;
        f.depthRenderable = f.depthBits != 0;
        f.stencilRenderable = f.stencilBits != 0;
        f.textureFilterable = !f.isInteger();
        // RGB formats are stored with four channels; alpha reads as one.
        // (The packed float formats store no alpha at all.)
        if (base == GL_RGB && f.swizzle[3] == SwzA && !f.compressed && f.storage != S::RG11B10Float &&
            f.storage != S::RGB9E5Float) {
            f.swizzle[3] = SwzOne;
            f.storedAlphaIsOne = true;
        }
        table[f.internalFormat] = f;
    };
    auto depth = [&](GLenum internal, GLenum base, S storage, uint8_t depthBits, uint8_t stencilBits, CT type,
                     GLenum tf, GLenum tt) {
        Builder b(internal, base, storage, type);
        b.f.depthBits = depthBits;
        b.f.stencilBits = stencilBits;
        b.transfer(tf, tt);
        if (internal != GL_STENCIL_INDEX1 && internal != GL_STENCIL_INDEX4 && internal != GL_STENCIL_INDEX16) b.rb();
        add(b);
    };
    const GLenum UB = GL_UNSIGNED_BYTE, B = GL_BYTE, US = GL_UNSIGNED_SHORT, SH = GL_SHORT, UI = GL_UNSIGNED_INT,
                 I = GL_INT, HF = GL_HALF_FLOAT, F = GL_FLOAT;

    // Normalized colour formats (GL 3.3 table 3.12).
    add(Builder(GL_R8, GL_RED, S::R8Unorm, CT::UnsignedNormalized).rgba(8, 0, 0, 0).transfer(GL_RED, UB).rb());
    add(Builder(GL_R8_SNORM, GL_RED, S::R8Snorm, CT::SignedNormalized).rgba(8, 0, 0, 0).transfer(GL_RED, B));
    add(Builder(GL_R16, GL_RED, S::R16Unorm, CT::UnsignedNormalized).rgba(16, 0, 0, 0).transfer(GL_RED, US).rb());
    add(Builder(GL_R16_SNORM, GL_RED, S::R16Snorm, CT::SignedNormalized).rgba(16, 0, 0, 0).transfer(GL_RED, SH));
    add(Builder(GL_RG8, GL_RG, S::RG8Unorm, CT::UnsignedNormalized).rgba(8, 8, 0, 0).transfer(GL_RG, UB).rb());
    add(Builder(GL_RG8_SNORM, GL_RG, S::RG8Snorm, CT::SignedNormalized).rgba(8, 8, 0, 0).transfer(GL_RG, B));
    add(Builder(GL_RG16, GL_RG, S::RG16Unorm, CT::UnsignedNormalized).rgba(16, 16, 0, 0).transfer(GL_RG, US).rb());
    add(Builder(GL_RG16_SNORM, GL_RG, S::RG16Snorm, CT::SignedNormalized).rgba(16, 16, 0, 0).transfer(GL_RG, SH));
    add(Builder(GL_R3_G3_B2, GL_RGB, S::RGBA8Unorm, CT::UnsignedNormalized).rgba(3, 3, 2, 0).transfer(GL_RGB, GL_UNSIGNED_BYTE_3_3_2));
    add(Builder(GL_RGB4, GL_RGB, S::RGBA8Unorm, CT::UnsignedNormalized).rgba(4, 4, 4, 0).transfer(GL_RGB, UB));
    add(Builder(GL_RGB5, GL_RGB, S::RGBA8Unorm, CT::UnsignedNormalized).rgba(5, 5, 5, 0).transfer(GL_RGB, UB));
    add(Builder(GL_RGB8, GL_RGB, S::RGBA8Unorm, CT::UnsignedNormalized).rgba(8, 8, 8, 0).transfer(GL_RGB, UB));
    // GL 4.1 / ARB_ES2_compatibility, accepted in every version as other drivers do.
    add(Builder(GL_RGB565, GL_RGB, S::RGBA8Unorm, CT::UnsignedNormalized).rgba(5, 6, 5, 0).transfer(GL_RGB, GL_UNSIGNED_SHORT_5_6_5).rb());
    add(Builder(GL_RGB8_SNORM, GL_RGB, S::RGBA8Snorm, CT::SignedNormalized).rgba(8, 8, 8, 0).transfer(GL_RGB, B));
    add(Builder(GL_RGB10, GL_RGB, S::RGBA16Unorm, CT::UnsignedNormalized).rgba(10, 10, 10, 0).transfer(GL_RGB, US));
    add(Builder(GL_RGB12, GL_RGB, S::RGBA16Unorm, CT::UnsignedNormalized).rgba(12, 12, 12, 0).transfer(GL_RGB, US));
    add(Builder(GL_RGB16, GL_RGB, S::RGBA16Unorm, CT::UnsignedNormalized).rgba(16, 16, 16, 0).transfer(GL_RGB, US));
    add(Builder(GL_RGB16_SNORM, GL_RGB, S::RGBA16Snorm, CT::SignedNormalized).rgba(16, 16, 16, 0).transfer(GL_RGB, SH));
    add(Builder(GL_RGBA2, GL_RGBA, S::RGBA8Unorm, CT::UnsignedNormalized).rgba(2, 2, 2, 2).transfer(GL_RGBA, UB));
    add(Builder(GL_RGBA4, GL_RGBA, S::RGBA8Unorm, CT::UnsignedNormalized).rgba(4, 4, 4, 4).transfer(GL_RGBA, UB));
    add(Builder(GL_RGB5_A1, GL_RGBA, S::RGBA8Unorm, CT::UnsignedNormalized).rgba(5, 5, 5, 1).transfer(GL_RGBA, UB));
    add(Builder(GL_RGBA8, GL_RGBA, S::RGBA8Unorm, CT::UnsignedNormalized).rgba(8, 8, 8, 8).transfer(GL_RGBA, UB).rb());
    // EXT_texture_format_BGRA8888's format (OpenGL ES): RGBA8 storage, BGRA data.
    add(Builder(0x93A1 /* GL_BGRA8_EXT */, GL_RGBA, S::RGBA8Unorm, CT::UnsignedNormalized).rgba(8, 8, 8, 8).transfer(GL_BGRA, UB));
    add(Builder(GL_RGBA8_SNORM, GL_RGBA, S::RGBA8Snorm, CT::SignedNormalized).rgba(8, 8, 8, 8).transfer(GL_RGBA, B));
    add(Builder(GL_RGB10_A2, GL_RGBA, S::RGB10A2Unorm, CT::UnsignedNormalized).rgba(10, 10, 10, 2).transfer(GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV).rb());
    add(Builder(GL_RGB10_A2UI, GL_RGBA, S::RGB10A2Uint, CT::UnsignedInt).rgba(10, 10, 10, 2).transfer(GL_RGBA_INTEGER, GL_UNSIGNED_INT_2_10_10_10_REV).rb());
    add(Builder(GL_RGBA12, GL_RGBA, S::RGBA16Unorm, CT::UnsignedNormalized).rgba(12, 12, 12, 12).transfer(GL_RGBA, US));
    add(Builder(GL_RGBA16, GL_RGBA, S::RGBA16Unorm, CT::UnsignedNormalized).rgba(16, 16, 16, 16).transfer(GL_RGBA, US).rb());
    add(Builder(GL_RGBA16_SNORM, GL_RGBA, S::RGBA16Snorm, CT::SignedNormalized).rgba(16, 16, 16, 16).transfer(GL_RGBA, SH));
    add(Builder(GL_SRGB8, GL_RGB, S::RGBA8Unorm_sRGB, CT::UnsignedNormalized).rgba(8, 8, 8, 0).transfer(GL_RGB, UB).srgb());
    add(Builder(GL_SRGB8_ALPHA8, GL_RGBA, S::RGBA8Unorm_sRGB, CT::UnsignedNormalized).rgba(8, 8, 8, 8).transfer(GL_RGBA, UB).srgb().rb());

    // Floating-point formats.
    add(Builder(GL_R16F, GL_RED, S::R16Float, CT::Float).rgba(16, 0, 0, 0).transfer(GL_RED, HF).rb());
    add(Builder(GL_RG16F, GL_RG, S::RG16Float, CT::Float).rgba(16, 16, 0, 0).transfer(GL_RG, HF).rb());
    add(Builder(GL_RGB16F, GL_RGB, S::RGBA16Float, CT::Float).rgba(16, 16, 16, 0).transfer(GL_RGB, HF));
    add(Builder(GL_RGBA16F, GL_RGBA, S::RGBA16Float, CT::Float).rgba(16, 16, 16, 16).transfer(GL_RGBA, HF).rb());
    add(Builder(GL_R32F, GL_RED, S::R32Float, CT::Float).rgba(32, 0, 0, 0).transfer(GL_RED, F).rb());
    add(Builder(GL_RG32F, GL_RG, S::RG32Float, CT::Float).rgba(32, 32, 0, 0).transfer(GL_RG, F).rb());
    add(Builder(GL_RGB32F, GL_RGB, S::RGBA32Float, CT::Float).rgba(32, 32, 32, 0).transfer(GL_RGB, F));
    add(Builder(GL_RGBA32F, GL_RGBA, S::RGBA32Float, CT::Float).rgba(32, 32, 32, 32).transfer(GL_RGBA, F).rb());
    add(Builder(GL_R11F_G11F_B10F, GL_RGB, S::RG11B10Float, CT::Float).rgba(11, 11, 10, 0).transfer(GL_RGB, GL_UNSIGNED_INT_10F_11F_11F_REV).rb());
    {
        Builder b(GL_RGB9_E5, GL_RGB, S::RGB9E5Float, CT::Float);
        b.rgba(9, 9, 9, 0).transfer(GL_RGB, GL_UNSIGNED_INT_5_9_9_9_REV);
        b.f.sharedBits = 5;
        add(b);
    }

    // Integer formats.
    struct IntFmt { GLenum fmt; GLenum base; S storage; CT type; uint8_t bits; uint8_t n; GLenum xtype; };
    const IntFmt ints[] = {
        {GL_R8I, GL_RED, S::R8Sint, CT::Int, 8, 1, B},         {GL_R8UI, GL_RED, S::R8Uint, CT::UnsignedInt, 8, 1, UB},
        {GL_R16I, GL_RED, S::R16Sint, CT::Int, 16, 1, SH},     {GL_R16UI, GL_RED, S::R16Uint, CT::UnsignedInt, 16, 1, US},
        {GL_R32I, GL_RED, S::R32Sint, CT::Int, 32, 1, I},      {GL_R32UI, GL_RED, S::R32Uint, CT::UnsignedInt, 32, 1, UI},
        {GL_RG8I, GL_RG, S::RG8Sint, CT::Int, 8, 2, B},        {GL_RG8UI, GL_RG, S::RG8Uint, CT::UnsignedInt, 8, 2, UB},
        {GL_RG16I, GL_RG, S::RG16Sint, CT::Int, 16, 2, SH},    {GL_RG16UI, GL_RG, S::RG16Uint, CT::UnsignedInt, 16, 2, US},
        {GL_RG32I, GL_RG, S::RG32Sint, CT::Int, 32, 2, I},     {GL_RG32UI, GL_RG, S::RG32Uint, CT::UnsignedInt, 32, 2, UI},
        {GL_RGB8I, GL_RGB, S::RGBA8Sint, CT::Int, 8, 3, B},    {GL_RGB8UI, GL_RGB, S::RGBA8Uint, CT::UnsignedInt, 8, 3, UB},
        {GL_RGB16I, GL_RGB, S::RGBA16Sint, CT::Int, 16, 3, SH}, {GL_RGB16UI, GL_RGB, S::RGBA16Uint, CT::UnsignedInt, 16, 3, US},
        {GL_RGB32I, GL_RGB, S::RGBA32Sint, CT::Int, 32, 3, I}, {GL_RGB32UI, GL_RGB, S::RGBA32Uint, CT::UnsignedInt, 32, 3, UI},
        {GL_RGBA8I, GL_RGBA, S::RGBA8Sint, CT::Int, 8, 4, B},  {GL_RGBA8UI, GL_RGBA, S::RGBA8Uint, CT::UnsignedInt, 8, 4, UB},
        {GL_RGBA16I, GL_RGBA, S::RGBA16Sint, CT::Int, 16, 4, SH}, {GL_RGBA16UI, GL_RGBA, S::RGBA16Uint, CT::UnsignedInt, 16, 4, US},
        {GL_RGBA32I, GL_RGBA, S::RGBA32Sint, CT::Int, 32, 4, I}, {GL_RGBA32UI, GL_RGBA, S::RGBA32Uint, CT::UnsignedInt, 32, 4, UI},
    };
    for (const IntFmt& fmt : ints) {
        const GLenum xfmt = fmt.n == 1 ? GL_RED_INTEGER : fmt.n == 2 ? GL_RG_INTEGER : fmt.n == 3 ? GL_RGB_INTEGER : GL_RGBA_INTEGER;
        Builder b(fmt.fmt, fmt.base, fmt.storage, fmt.type);
        b.rgba(fmt.bits, fmt.n > 1 ? fmt.bits : 0, fmt.n > 2 ? fmt.bits : 0, fmt.n > 3 ? fmt.bits : 0).transfer(xfmt, fmt.xtype);
        if (fmt.n != 3) b.rb();
        add(b);
    }

    // Depth and stencil formats.
    depth(GL_DEPTH_COMPONENT16, GL_DEPTH_COMPONENT, S::Depth16Unorm, 16, 0, CT::UnsignedNormalized, GL_DEPTH_COMPONENT, US);
    depth(GL_DEPTH_COMPONENT24, GL_DEPTH_COMPONENT, S::Depth32Float, 24, 0, CT::UnsignedNormalized, GL_DEPTH_COMPONENT, UI);
    depth(GL_DEPTH_COMPONENT32, GL_DEPTH_COMPONENT, S::Depth32Float, 32, 0, CT::UnsignedNormalized, GL_DEPTH_COMPONENT, UI);
    depth(GL_DEPTH_COMPONENT32F, GL_DEPTH_COMPONENT, S::Depth32Float, 32, 0, CT::Float, GL_DEPTH_COMPONENT, F);
    depth(GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL, S::Depth32Float_Stencil8, 24, 8, CT::UnsignedNormalized, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8);
    depth(GL_DEPTH32F_STENCIL8, GL_DEPTH_STENCIL, S::Depth32Float_Stencil8, 32, 8, CT::Float, GL_DEPTH_STENCIL, GL_FLOAT_32_UNSIGNED_INT_24_8_REV);
    depth(GL_STENCIL_INDEX1, GL_STENCIL_INDEX, S::Stencil8, 0, 1, CT::None, GL_STENCIL_INDEX, UB);
    depth(GL_STENCIL_INDEX4, GL_STENCIL_INDEX, S::Stencil8, 0, 4, CT::None, GL_STENCIL_INDEX, UB);
    depth(GL_STENCIL_INDEX8, GL_STENCIL_INDEX, S::Stencil8, 0, 8, CT::None, GL_STENCIL_INDEX, UB);
    depth(GL_STENCIL_INDEX16, GL_STENCIL_INDEX, S::Stencil8, 0, 16, CT::None, GL_STENCIL_INDEX, US);

    // Compressed formats.
    add(Builder(GL_COMPRESSED_RED_RGTC1, GL_RED, S::BC4_RUnorm, CT::UnsignedNormalized).rgba(8, 0, 0, 0).transfer(GL_RED, UB));
    add(Builder(GL_COMPRESSED_SIGNED_RED_RGTC1, GL_RED, S::BC4_RSnorm, CT::SignedNormalized).rgba(8, 0, 0, 0).transfer(GL_RED, B));
    add(Builder(GL_COMPRESSED_RG_RGTC2, GL_RG, S::BC5_RGUnorm, CT::UnsignedNormalized).rgba(8, 8, 0, 0).transfer(GL_RG, UB));
    add(Builder(GL_COMPRESSED_SIGNED_RG_RGTC2, GL_RG, S::BC5_RGSnorm, CT::SignedNormalized).rgba(8, 8, 0, 0).transfer(GL_RG, B));
    add(Builder(GL_COMPRESSED_RGB_S3TC_DXT1_EXT, GL_RGB, S::BC1_RGBA, CT::UnsignedNormalized).rgba(5, 6, 5, 0).swz(SwzR, SwzG, SwzB, SwzOne).transfer(GL_RGB, UB));
    add(Builder(GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, GL_RGBA, S::BC1_RGBA, CT::UnsignedNormalized).rgba(5, 6, 5, 1).transfer(GL_RGBA, UB));
    add(Builder(GL_COMPRESSED_RGBA_S3TC_DXT3_EXT, GL_RGBA, S::BC2_RGBA, CT::UnsignedNormalized).rgba(5, 6, 5, 4).transfer(GL_RGBA, UB));
    add(Builder(GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, GL_RGBA, S::BC3_RGBA, CT::UnsignedNormalized).rgba(5, 6, 5, 8).transfer(GL_RGBA, UB));
    add(Builder(GL_COMPRESSED_SRGB_S3TC_DXT1_EXT, GL_RGB, S::BC1_RGBA_sRGB, CT::UnsignedNormalized).rgba(5, 6, 5, 0).swz(SwzR, SwzG, SwzB, SwzOne).transfer(GL_RGB, UB).srgb());
    add(Builder(GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT, GL_RGBA, S::BC1_RGBA_sRGB, CT::UnsignedNormalized).rgba(5, 6, 5, 1).transfer(GL_RGBA, UB).srgb());
    add(Builder(GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT3_EXT, GL_RGBA, S::BC2_RGBA_sRGB, CT::UnsignedNormalized).rgba(5, 6, 5, 4).transfer(GL_RGBA, UB).srgb());
    add(Builder(GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT, GL_RGBA, S::BC3_RGBA_sRGB, CT::UnsignedNormalized).rgba(5, 6, 5, 8).transfer(GL_RGBA, UB).srgb());
    add(Builder(GL_COMPRESSED_RGBA_BPTC_UNORM, GL_RGBA, S::BC7_RGBAUnorm, CT::UnsignedNormalized).rgba(8, 8, 8, 8).transfer(GL_RGBA, UB));
    add(Builder(GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM, GL_RGBA, S::BC7_RGBAUnorm_sRGB, CT::UnsignedNormalized).rgba(8, 8, 8, 8).transfer(GL_RGBA, UB).srgb());
    add(Builder(GL_COMPRESSED_RGB_BPTC_SIGNED_FLOAT, GL_RGB, S::BC6H_RGBFloat, CT::Float).rgba(16, 16, 16, 0).swz(SwzR, SwzG, SwzB, SwzOne).transfer(GL_RGB, F));
    add(Builder(GL_COMPRESSED_RGB_BPTC_UNSIGNED_FLOAT, GL_RGB, S::BC6H_RGBUfloat, CT::Float).rgba(16, 16, 16, 0).swz(SwzR, SwzG, SwzB, SwzOne).transfer(GL_RGB, F));
    // GL 4.3 (GL_ARB_ES3_compatibility): ETC2 and EAC, native on Apple GPUs.
    add(Builder(GL_COMPRESSED_RGB8_ETC2, GL_RGB, S::ETC2_RGB8, CT::UnsignedNormalized).rgba(8, 8, 8, 0).swz(SwzR, SwzG, SwzB, SwzOne).transfer(GL_RGB, UB));
    add(Builder(GL_COMPRESSED_SRGB8_ETC2, GL_RGB, S::ETC2_RGB8_sRGB, CT::UnsignedNormalized).rgba(8, 8, 8, 0).swz(SwzR, SwzG, SwzB, SwzOne).transfer(GL_RGB, UB).srgb());
    add(Builder(GL_COMPRESSED_RGB8_PUNCHTHROUGH_ALPHA1_ETC2, GL_RGBA, S::ETC2_RGB8A1, CT::UnsignedNormalized).rgba(8, 8, 8, 1).transfer(GL_RGBA, UB));
    add(Builder(GL_COMPRESSED_SRGB8_PUNCHTHROUGH_ALPHA1_ETC2, GL_RGBA, S::ETC2_RGB8A1_sRGB, CT::UnsignedNormalized).rgba(8, 8, 8, 1).transfer(GL_RGBA, UB).srgb());
    add(Builder(GL_COMPRESSED_RGBA8_ETC2_EAC, GL_RGBA, S::EAC_RGBA8, CT::UnsignedNormalized).rgba(8, 8, 8, 8).transfer(GL_RGBA, UB));
    add(Builder(GL_COMPRESSED_SRGB8_ALPHA8_ETC2_EAC, GL_RGBA, S::EAC_RGBA8_sRGB, CT::UnsignedNormalized).rgba(8, 8, 8, 8).transfer(GL_RGBA, UB).srgb());
    add(Builder(GL_COMPRESSED_R11_EAC, GL_RED, S::EAC_R11Unorm, CT::UnsignedNormalized).rgba(11, 0, 0, 0).transfer(GL_RED, UB));
    add(Builder(GL_COMPRESSED_SIGNED_R11_EAC, GL_RED, S::EAC_R11Snorm, CT::SignedNormalized).rgba(11, 0, 0, 0).transfer(GL_RED, B));
    add(Builder(GL_COMPRESSED_RG11_EAC, GL_RG, S::EAC_RG11Unorm, CT::UnsignedNormalized).rgba(11, 11, 0, 0).transfer(GL_RG, UB));
    add(Builder(GL_COMPRESSED_SIGNED_RG11_EAC, GL_RG, S::EAC_RG11Snorm, CT::SignedNormalized).rgba(11, 11, 0, 0).transfer(GL_RG, B));
    // OES_compressed_ETC1_RGB8_texture (OpenGL ES): ETC1 blocks are ETC2 RGB8 blocks.
    add(Builder(0x8D64 /* GL_ETC1_RGB8_OES */, GL_RGB, S::ETC2_RGB8, CT::UnsignedNormalized).rgba(8, 8, 8, 0).swz(SwzR, SwzG, SwzB, SwzOne).transfer(GL_RGB, UB));
    // KHR_texture_compression_astc_ldr: GL_COMPRESSED_RGBA_ASTC_<w>x<h>_KHR
    // (0x93B0...) and GL_COMPRESSED_SRGB8_ALPHA8_ASTC_<w>x<h>_KHR (0x93D0...).
    add(Builder(0x93B0, GL_RGBA, S::ASTC_4x4, CT::UnsignedNormalized).rgba(8, 8, 8, 8).transfer(GL_RGBA, UB));
    add(Builder(0x93D0, GL_RGBA, S::ASTC_4x4_sRGB, CT::UnsignedNormalized).rgba(8, 8, 8, 8).transfer(GL_RGBA, UB).srgb());
    add(Builder(0x93B1, GL_RGBA, S::ASTC_5x4, CT::UnsignedNormalized).rgba(8, 8, 8, 8).transfer(GL_RGBA, UB));
    add(Builder(0x93D1, GL_RGBA, S::ASTC_5x4_sRGB, CT::UnsignedNormalized).rgba(8, 8, 8, 8).transfer(GL_RGBA, UB).srgb());
    add(Builder(0x93B2, GL_RGBA, S::ASTC_5x5, CT::UnsignedNormalized).rgba(8, 8, 8, 8).transfer(GL_RGBA, UB));
    add(Builder(0x93D2, GL_RGBA, S::ASTC_5x5_sRGB, CT::UnsignedNormalized).rgba(8, 8, 8, 8).transfer(GL_RGBA, UB).srgb());
    add(Builder(0x93B3, GL_RGBA, S::ASTC_6x5, CT::UnsignedNormalized).rgba(8, 8, 8, 8).transfer(GL_RGBA, UB));
    add(Builder(0x93D3, GL_RGBA, S::ASTC_6x5_sRGB, CT::UnsignedNormalized).rgba(8, 8, 8, 8).transfer(GL_RGBA, UB).srgb());
    add(Builder(0x93B4, GL_RGBA, S::ASTC_6x6, CT::UnsignedNormalized).rgba(8, 8, 8, 8).transfer(GL_RGBA, UB));
    add(Builder(0x93D4, GL_RGBA, S::ASTC_6x6_sRGB, CT::UnsignedNormalized).rgba(8, 8, 8, 8).transfer(GL_RGBA, UB).srgb());
    add(Builder(0x93B5, GL_RGBA, S::ASTC_8x5, CT::UnsignedNormalized).rgba(8, 8, 8, 8).transfer(GL_RGBA, UB));
    add(Builder(0x93D5, GL_RGBA, S::ASTC_8x5_sRGB, CT::UnsignedNormalized).rgba(8, 8, 8, 8).transfer(GL_RGBA, UB).srgb());
    add(Builder(0x93B6, GL_RGBA, S::ASTC_8x6, CT::UnsignedNormalized).rgba(8, 8, 8, 8).transfer(GL_RGBA, UB));
    add(Builder(0x93D6, GL_RGBA, S::ASTC_8x6_sRGB, CT::UnsignedNormalized).rgba(8, 8, 8, 8).transfer(GL_RGBA, UB).srgb());
    add(Builder(0x93B7, GL_RGBA, S::ASTC_8x8, CT::UnsignedNormalized).rgba(8, 8, 8, 8).transfer(GL_RGBA, UB));
    add(Builder(0x93D7, GL_RGBA, S::ASTC_8x8_sRGB, CT::UnsignedNormalized).rgba(8, 8, 8, 8).transfer(GL_RGBA, UB).srgb());
    add(Builder(0x93B8, GL_RGBA, S::ASTC_10x5, CT::UnsignedNormalized).rgba(8, 8, 8, 8).transfer(GL_RGBA, UB));
    add(Builder(0x93D8, GL_RGBA, S::ASTC_10x5_sRGB, CT::UnsignedNormalized).rgba(8, 8, 8, 8).transfer(GL_RGBA, UB).srgb());
    add(Builder(0x93B9, GL_RGBA, S::ASTC_10x6, CT::UnsignedNormalized).rgba(8, 8, 8, 8).transfer(GL_RGBA, UB));
    add(Builder(0x93D9, GL_RGBA, S::ASTC_10x6_sRGB, CT::UnsignedNormalized).rgba(8, 8, 8, 8).transfer(GL_RGBA, UB).srgb());
    add(Builder(0x93BA, GL_RGBA, S::ASTC_10x8, CT::UnsignedNormalized).rgba(8, 8, 8, 8).transfer(GL_RGBA, UB));
    add(Builder(0x93DA, GL_RGBA, S::ASTC_10x8_sRGB, CT::UnsignedNormalized).rgba(8, 8, 8, 8).transfer(GL_RGBA, UB).srgb());
    add(Builder(0x93BB, GL_RGBA, S::ASTC_10x10, CT::UnsignedNormalized).rgba(8, 8, 8, 8).transfer(GL_RGBA, UB));
    add(Builder(0x93DB, GL_RGBA, S::ASTC_10x10_sRGB, CT::UnsignedNormalized).rgba(8, 8, 8, 8).transfer(GL_RGBA, UB).srgb());
    add(Builder(0x93BC, GL_RGBA, S::ASTC_12x10, CT::UnsignedNormalized).rgba(8, 8, 8, 8).transfer(GL_RGBA, UB));
    add(Builder(0x93DC, GL_RGBA, S::ASTC_12x10_sRGB, CT::UnsignedNormalized).rgba(8, 8, 8, 8).transfer(GL_RGBA, UB).srgb());
    add(Builder(0x93BD, GL_RGBA, S::ASTC_12x12, CT::UnsignedNormalized).rgba(8, 8, 8, 8).transfer(GL_RGBA, UB));
    add(Builder(0x93DD, GL_RGBA, S::ASTC_12x12_sRGB, CT::UnsignedNormalized).rgba(8, 8, 8, 8).transfer(GL_RGBA, UB).srgb());

    // Compatibility-profile luminance/alpha/intensity formats (sized).
    auto legacy = [&](GLenum internal, GLenum base, S storage, CT type, uint8_t lBits, uint8_t aBits, uint8_t iBits,
                      Swizzle r, Swizzle g, Swizzle b, Swizzle a, GLenum xfmt, GLenum xtype) {
        Builder bld(internal, base, storage, type);
        bld.f.luminanceBits = lBits;
        bld.f.alphaBits = aBits;
        bld.f.intensityBits = iBits;
        bld.swz(r, g, b, a).transfer(xfmt, xtype);
        add(bld);
    };
    legacy(GL_ALPHA8, GL_ALPHA, S::R8Unorm, CT::UnsignedNormalized, 0, 8, 0, SwzZero, SwzZero, SwzZero, SwzR, GL_ALPHA, UB);
    legacy(GL_ALPHA16, GL_ALPHA, S::R16Unorm, CT::UnsignedNormalized, 0, 16, 0, SwzZero, SwzZero, SwzZero, SwzR, GL_ALPHA, US);
    legacy(GL_LUMINANCE8, GL_LUMINANCE, S::R8Unorm, CT::UnsignedNormalized, 8, 0, 0, SwzR, SwzR, SwzR, SwzOne, GL_LUMINANCE, UB);
    legacy(GL_LUMINANCE16, GL_LUMINANCE, S::R16Unorm, CT::UnsignedNormalized, 16, 0, 0, SwzR, SwzR, SwzR, SwzOne, GL_LUMINANCE, US);
    legacy(GL_LUMINANCE8_ALPHA8, GL_LUMINANCE_ALPHA, S::RG8Unorm, CT::UnsignedNormalized, 8, 8, 0, SwzR, SwzR, SwzR, SwzG, GL_LUMINANCE_ALPHA, UB);
    legacy(GL_LUMINANCE16_ALPHA16, GL_LUMINANCE_ALPHA, S::RG16Unorm, CT::UnsignedNormalized, 16, 16, 0, SwzR, SwzR, SwzR, SwzG, GL_LUMINANCE_ALPHA, US);
    legacy(GL_INTENSITY8, GL_INTENSITY, S::R8Unorm, CT::UnsignedNormalized, 0, 0, 8, SwzR, SwzR, SwzR, SwzR, GL_LUMINANCE, UB);
    legacy(GL_INTENSITY16, GL_INTENSITY, S::R16Unorm, CT::UnsignedNormalized, 0, 0, 16, SwzR, SwzR, SwzR, SwzR, GL_LUMINANCE, US);
    legacy(GL_ALPHA32F_ARB, GL_ALPHA, S::R32Float, CT::Float, 0, 32, 0, SwzZero, SwzZero, SwzZero, SwzR, GL_ALPHA, F);
    legacy(GL_LUMINANCE32F_ARB, GL_LUMINANCE, S::R32Float, CT::Float, 32, 0, 0, SwzR, SwzR, SwzR, SwzOne, GL_LUMINANCE, F);
    legacy(GL_LUMINANCE_ALPHA32F_ARB, GL_LUMINANCE_ALPHA, S::RG32Float, CT::Float, 32, 32, 0, SwzR, SwzR, SwzR, SwzG, GL_LUMINANCE_ALPHA, F);
    legacy(GL_ALPHA16F_ARB, GL_ALPHA, S::R16Float, CT::Float, 0, 16, 0, SwzZero, SwzZero, SwzZero, SwzR, GL_ALPHA, HF);
    legacy(GL_LUMINANCE16F_ARB, GL_LUMINANCE, S::R16Float, CT::Float, 16, 0, 0, SwzR, SwzR, SwzR, SwzOne, GL_LUMINANCE, HF);
    legacy(GL_LUMINANCE_ALPHA16F_ARB, GL_LUMINANCE_ALPHA, S::RG16Float, CT::Float, 16, 16, 0, SwzR, SwzR, SwzR, SwzG, GL_LUMINANCE_ALPHA, HF);
    // Compressed luminance (EXT_texture_compression_latc, and ATI's 3Dc, the
    // same blocks as LATC2: luminance in the first 64 bits, alpha in the
    // second): RGTC blocks read with luminance swizzles. Frictional's HPL3
    // (SOMA) stores normal maps as 3Dc and reads them as luminance alpha
    // (green and alpha).
    legacy(GL_COMPRESSED_LUMINANCE_LATC1_EXT, GL_LUMINANCE, S::BC4_RUnorm, CT::UnsignedNormalized, 8, 0, 0, SwzR, SwzR, SwzR, SwzOne, GL_LUMINANCE, UB);
    legacy(GL_COMPRESSED_SIGNED_LUMINANCE_LATC1_EXT, GL_LUMINANCE, S::BC4_RSnorm, CT::SignedNormalized, 8, 0, 0, SwzR, SwzR, SwzR, SwzOne, GL_LUMINANCE, B);
    legacy(GL_COMPRESSED_LUMINANCE_ALPHA_LATC2_EXT, GL_LUMINANCE_ALPHA, S::BC5_RGUnorm, CT::UnsignedNormalized, 8, 8, 0, SwzR, SwzR, SwzR, SwzG, GL_LUMINANCE_ALPHA, UB);
    legacy(GL_COMPRESSED_SIGNED_LUMINANCE_ALPHA_LATC2_EXT, GL_LUMINANCE_ALPHA, S::BC5_RGSnorm, CT::SignedNormalized, 8, 8, 0, SwzR, SwzR, SwzR, SwzG, GL_LUMINANCE_ALPHA, B);
    legacy(GL_COMPRESSED_LUMINANCE_ALPHA_3DC_ATI, GL_LUMINANCE_ALPHA, S::BC5_RGUnorm, CT::UnsignedNormalized, 8, 8, 0, SwzR, SwzR, SwzR, SwzG, GL_LUMINANCE_ALPHA, UB);
    return table;
}

const std::unordered_map<GLenum, FormatInfo>& table() {
    static const auto* formats = new std::unordered_map<GLenum, FormatInfo>(buildTable());
    return *formats;
}

}  // namespace

const FormatInfo* formatInfo(GLenum internalFormat) {
    auto it = table().find(internalFormat);
    return it == table().end() ? nullptr : &it->second;
}

GLenum resolveInternalFormat(GLenum internalFormat, GLenum format, GLenum type) {
    switch (internalFormat) {
    case 1: case GL_LUMINANCE: return GL_LUMINANCE8;
    case 2: case GL_LUMINANCE_ALPHA: return GL_LUMINANCE8_ALPHA8;
    case 3: internalFormat = GL_RGB; break;
    case 4: internalFormat = GL_RGBA; break;
    case GL_ALPHA: return type == GL_FLOAT ? GL_ALPHA32F_ARB : GL_ALPHA8;
    case GL_INTENSITY: return GL_INTENSITY8;
    case GL_DEPTH_COMPONENT: return type == GL_FLOAT ? GL_DEPTH_COMPONENT32F : type == GL_UNSIGNED_SHORT ? GL_DEPTH_COMPONENT16 : GL_DEPTH_COMPONENT24;
    case GL_DEPTH_STENCIL: return type == GL_FLOAT_32_UNSIGNED_INT_24_8_REV ? GL_DEPTH32F_STENCIL8 : GL_DEPTH24_STENCIL8;
    case GL_STENCIL_INDEX: return GL_STENCIL_INDEX8;
    case GL_COMPRESSED_RED: return GL_R8;
    case GL_COMPRESSED_RG: return GL_RG8;
    case GL_COMPRESSED_RGB: return GL_RGB8;
    case GL_COMPRESSED_RGBA: return GL_RGBA8;
    case GL_COMPRESSED_SRGB: return GL_SRGB8;
    case GL_COMPRESSED_SRGB_ALPHA: return GL_SRGB8_ALPHA8;
    case GL_SRGB: return GL_SRGB8;
    case GL_SRGB_ALPHA: return GL_SRGB8_ALPHA8;
    default: break;
    }
    if (internalFormat != GL_RED && internalFormat != GL_RG && internalFormat != GL_RGB && internalFormat != GL_RGBA)
        return internalFormat;
    const int n = internalFormat == GL_RED ? 1 : internalFormat == GL_RG ? 2 : internalFormat == GL_RGB ? 3 : 4;
    static const GLenum unorm8[] = {GL_R8, GL_RG8, GL_RGB8, GL_RGBA8};
    static const GLenum unorm16[] = {GL_R16, GL_RG16, GL_RGB16, GL_RGBA16};
    static const GLenum snorm8[] = {GL_R8_SNORM, GL_RG8_SNORM, GL_RGB8_SNORM, GL_RGBA8_SNORM};
    static const GLenum snorm16[] = {GL_R16_SNORM, GL_RG16_SNORM, GL_RGB16_SNORM, GL_RGBA16_SNORM};
    static const GLenum f16[] = {GL_R16F, GL_RG16F, GL_RGB16F, GL_RGBA16F};
    static const GLenum f32[] = {GL_R32F, GL_RG32F, GL_RGB32F, GL_RGBA32F};
    (void)format;
    switch (type) {
    case GL_UNSIGNED_SHORT: return unorm16[n - 1];
    case GL_BYTE: return snorm8[n - 1];
    case GL_SHORT: return snorm16[n - 1];
    case GL_HALF_FLOAT: return f16[n - 1];
    case GL_FLOAT: return f32[n - 1];
    case GL_UNSIGNED_INT_2_10_10_10_REV: return n == 4 ? GL_RGB10_A2 : unorm8[n - 1];
    case GL_UNSIGNED_INT_10F_11F_11F_REV: return n == 3 ? GL_R11F_G11F_B10F : f16[n - 1];
    case GL_UNSIGNED_INT_5_9_9_9_REV: return n == 3 ? GL_RGB9_E5 : f16[n - 1];
    default: return unorm8[n - 1];
    }
}

bool isValidTextureInternalFormat(GLenum internalFormat, bool compatibility, bool stencilTextures) {
    switch (internalFormat) {
    case GL_RED: case GL_RG: case GL_RGB: case GL_RGBA: case GL_DEPTH_COMPONENT: case GL_DEPTH_STENCIL:
    case GL_COMPRESSED_RED: case GL_COMPRESSED_RG: case GL_COMPRESSED_RGB: case GL_COMPRESSED_RGBA:
    case GL_COMPRESSED_SRGB: case GL_COMPRESSED_SRGB_ALPHA: case GL_SRGB: case GL_SRGB_ALPHA:
        return true;
    case 1: case 2: case 3: case 4: case GL_ALPHA: case GL_LUMINANCE: case GL_LUMINANCE_ALPHA: case GL_INTENSITY:
        return compatibility;
    case GL_STENCIL_INDEX: case GL_STENCIL_INDEX8:
        return stencilTextures;  // GL 4.4, GL_ARB_texture_stencil8
    case GL_STENCIL_INDEX1: case GL_STENCIL_INDEX4: case GL_STENCIL_INDEX16:
        return false;
    default: break;
    }
    const FormatInfo* info = formatInfo(internalFormat);
    if (!info) return false;
    const GLenum base = info->baseFormat;
    if (!compatibility && (base == GL_ALPHA || base == GL_LUMINANCE || base == GL_LUMINANCE_ALPHA || base == GL_INTENSITY))
        return false;
    return true;
}

uint32_t storageBytes(Storage storage) {
    switch (storage) {
    case S::R8Unorm: case S::R8Snorm: case S::R8Uint: case S::R8Sint: case S::Stencil8: return 1;
    case S::R16Unorm: case S::R16Snorm: case S::R16Uint: case S::R16Sint: case S::R16Float:
    case S::RG8Unorm: case S::RG8Snorm: case S::RG8Uint: case S::RG8Sint: case S::Depth16Unorm: return 2;
    case S::R32Uint: case S::R32Sint: case S::R32Float: case S::RG16Unorm: case S::RG16Snorm: case S::RG16Uint:
    case S::RG16Sint: case S::RG16Float: case S::RGBA8Unorm: case S::RGBA8Unorm_sRGB: case S::RGBA8Snorm:
    case S::RGBA8Uint: case S::RGBA8Sint: case S::BGRA8Unorm: case S::RGB10A2Unorm: case S::RGB10A2Uint:
    case S::RG11B10Float: case S::RGB9E5Float: case S::Depth32Float: return 4;
    case S::RG32Uint: case S::RG32Sint: case S::RG32Float: case S::RGBA16Unorm: case S::RGBA16Snorm:
    case S::RGBA16Uint: case S::RGBA16Sint: case S::RGBA16Float: case S::Depth32Float_Stencil8: return 8;
    case S::RGBA32Uint: case S::RGBA32Sint: case S::RGBA32Float: return 16;
    case S::BC1_RGBA: case S::BC1_RGBA_sRGB: case S::BC4_RUnorm: case S::BC4_RSnorm: return 8;
    case S::ETC2_RGB8: case S::ETC2_RGB8_sRGB: case S::ETC2_RGB8A1: case S::ETC2_RGB8A1_sRGB: case S::EAC_R11Unorm:
    case S::EAC_R11Snorm: return 8;
    case S::EAC_RGBA8: case S::EAC_RGBA8_sRGB: case S::EAC_RG11Unorm: case S::EAC_RG11Snorm: return 16;
    case S::BC2_RGBA: case S::BC2_RGBA_sRGB: case S::BC3_RGBA: case S::BC3_RGBA_sRGB: case S::BC5_RGUnorm:
    case S::BC5_RGSnorm: case S::BC6H_RGBFloat: case S::BC6H_RGBUfloat: case S::BC7_RGBAUnorm:
    case S::BC7_RGBAUnorm_sRGB: return 16;
    default: return storageIsAstc(storage) ? 16 : 0;
    }
}

bool storageIsCompressed(Storage storage) {
    return storage >= S::BC1_RGBA && storage <= S::ASTC_12x12_sRGB;
}

bool storageIsAstc(Storage storage) { return storage >= S::ASTC_4x4 && storage <= S::ASTC_12x12_sRGB; }

void storageBlockSize(Storage storage, int& width, int& height) {
    if (storageIsAstc(storage)) {
        static const int kSizes[][2] = {{4, 4}, {5, 4}, {5, 5}, {6, 5},  {6, 6},   {8, 5},   {8, 6},
                                        {8, 8}, {10, 5}, {10, 6}, {10, 8}, {10, 10}, {12, 10}, {12, 12}};
        const int i = (static_cast<int>(storage) - static_cast<int>(S::ASTC_4x4)) / 2;
        width = kSizes[i][0];
        height = kSizes[i][1];
        return;
    }
    width = height = storageIsCompressed(storage) ? 4 : 1;
}

size_t storageBlocksAcross(Storage storage, size_t texels, bool vertical) {
    int w, h;
    storageBlockSize(storage, w, h);
    const size_t block = static_cast<size_t>(vertical ? h : w);
    return (texels + block - 1) / block;
}

const char* storageName(Storage storage) {
    static const char* const names[] = {
        "Invalid", "R8Unorm", "R8Snorm", "R8Uint", "R8Sint", "R16Unorm", "R16Snorm", "R16Uint", "R16Sint", "R16Float",
        "RG8Unorm", "RG8Snorm", "RG8Uint", "RG8Sint", "R32Uint", "R32Sint", "R32Float", "RG16Unorm", "RG16Snorm",
        "RG16Uint", "RG16Sint", "RG16Float", "RGBA8Unorm", "RGBA8Unorm_sRGB", "RGBA8Snorm", "RGBA8Uint", "RGBA8Sint",
        "BGRA8Unorm", "RGB10A2Unorm", "RGB10A2Uint", "RG11B10Float", "RGB9E5Float", "RG32Uint", "RG32Sint", "RG32Float",
        "RGBA16Unorm", "RGBA16Snorm", "RGBA16Uint", "RGBA16Sint", "RGBA16Float", "RGBA32Uint", "RGBA32Sint",
        "RGBA32Float", "BC1_RGBA", "BC1_RGBA_sRGB", "BC2_RGBA", "BC2_RGBA_sRGB", "BC3_RGBA", "BC3_RGBA_sRGB",
        "BC4_RUnorm", "BC4_RSnorm", "BC5_RGUnorm", "BC5_RGSnorm", "BC6H_RGBFloat", "BC6H_RGBUfloat", "BC7_RGBAUnorm",
        "BC7_RGBAUnorm_sRGB", "ETC2_RGB8", "ETC2_RGB8_sRGB", "ETC2_RGB8A1", "ETC2_RGB8A1_sRGB", "EAC_RGBA8",
        "EAC_RGBA8_sRGB", "EAC_R11Unorm", "EAC_R11Snorm", "EAC_RG11Unorm", "EAC_RG11Snorm",
        "ASTC_4x4", "ASTC_4x4_sRGB", "ASTC_5x4", "ASTC_5x4_sRGB", "ASTC_5x5", "ASTC_5x5_sRGB", "ASTC_6x5", "ASTC_6x5_sRGB", "ASTC_6x6", "ASTC_6x6_sRGB", "ASTC_8x5", "ASTC_8x5_sRGB", "ASTC_8x6", "ASTC_8x6_sRGB", "ASTC_8x8", "ASTC_8x8_sRGB", "ASTC_10x5", "ASTC_10x5_sRGB", "ASTC_10x6", "ASTC_10x6_sRGB", "ASTC_10x8", "ASTC_10x8_sRGB", "ASTC_10x10", "ASTC_10x10_sRGB", "ASTC_12x10", "ASTC_12x10_sRGB", "ASTC_12x12", "ASTC_12x12_sRGB", "Depth16Unorm", "Depth32Float", "Stencil8", "Depth32Float_Stencil8"};
    static_assert(sizeof(names) / sizeof(names[0]) == static_cast<size_t>(S::Count), "storage names");
    return names[static_cast<size_t>(storage)];
}

}  // namespace wmgl
