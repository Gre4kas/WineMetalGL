#include <cstring>

#include "core/format/formats.h"
#include "core/format/pixel_transfer.h"
#include "test.h"

using namespace wmgl;

TEST(format_rgb_is_stored_with_alpha_one) {
    const FormatInfo* rgb8 = formatInfo(GL_RGB8);
    CHECK(rgb8 != nullptr);
    if (rgb8) {
        CHECK(rgb8->storage == Storage::RGBA8Unorm);
        CHECK(rgb8->storedAlphaIsOne);
        CHECK(rgb8->swizzle[3] == SwzOne);
        CHECK(rgb8->colorRenderable);
    }
}

TEST(format_depth24_uses_d32f) {
    const FormatInfo* d24s8 = formatInfo(GL_DEPTH24_STENCIL8);
    CHECK(d24s8 && d24s8->storage == Storage::Depth32Float_Stencil8 && d24s8->depthBits == 24 && d24s8->stencilBits == 8);
}

TEST(format_snorm_color_renderable) {
    // Not required by GL 3.3, but supported: Metal renders to SNORM formats.
    const FormatInfo* f = formatInfo(GL_RGBA8_SNORM);
    CHECK(f && f->colorRenderable && f->textureFilterable && !f->requiredRenderbuffer);
}

TEST(format_integer_not_filterable) {
    const FormatInfo* f = formatInfo(GL_RGBA32UI);
    CHECK(f && !f->textureFilterable && f->isInteger());
}

TEST(format_unsized_resolution) {
    CHECK_EQ(resolveInternalFormat(GL_RGBA, GL_RGBA, GL_UNSIGNED_BYTE), static_cast<GLenum>(GL_RGBA8));
    CHECK_EQ(resolveInternalFormat(GL_RGB, GL_RGB, GL_FLOAT), static_cast<GLenum>(GL_RGB32F));
    CHECK_EQ(resolveInternalFormat(GL_DEPTH_COMPONENT, GL_DEPTH_COMPONENT, GL_UNSIGNED_SHORT), static_cast<GLenum>(GL_DEPTH_COMPONENT16));
    CHECK_EQ(resolveInternalFormat(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE), static_cast<GLenum>(GL_RGBA8));
}

TEST(format_storage_sizes) {
    CHECK_EQ(storageBytes(Storage::RGBA16Float), 8u);
    CHECK_EQ(storageBytes(Storage::BC1_RGBA), 8u);
    CHECK(storageIsCompressed(Storage::BC7_RGBAUnorm));
    CHECK(!storageIsCompressed(Storage::Depth32Float));
}

// RGB9_E5 and R11F_G11F_B10F data in their packed client types is stored and
// read back bit for bit: decoding would canonicalize non-normalized RGB9_E5
// encodings (KHR-GL43.copy_image.functional_*_rgb9_e5).
TEST(format_packed_float_transfer_is_bit_exact) {
    const uint32_t texels[2] = {0xe7e20000u, 0x0000c001u};  // two encodings a re-encode would change
    PixelStoreParams ps;
    ps.alignment = 4;
    for (GLenum internal : {GL_RGB9_E5, GL_R11F_G11F_B10F}) {
        const FormatInfo* f = formatInfo(internal);
        CHECK(f != nullptr);
        if (!f) continue;
        const GLenum type = internal == GL_RGB9_E5 ? GL_UNSIGNED_INT_5_9_9_9_REV : GL_UNSIGNED_INT_10F_11F_11F_REV;
        uint32_t stored[2] = {}, back[2] = {};
        unpackPixels(texels, GL_RGB, type, 2, 1, 1, ps, *f, stored);
        packPixels(stored, *f, 2, 1, 1, GL_RGB, type, ps, false, back);
        CHECK(std::memcmp(stored, texels, sizeof(texels)) == 0);
        CHECK(std::memcmp(back, texels, sizeof(texels)) == 0);
    }
}
