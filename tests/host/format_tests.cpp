#include "core/format/formats.h"
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

TEST(format_snorm_not_color_renderable) {
    const FormatInfo* f = formatInfo(GL_RGBA8_SNORM);
    CHECK(f && !f->colorRenderable && f->textureFilterable);
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
