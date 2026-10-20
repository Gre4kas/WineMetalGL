#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <tuple>
#include <vector>
#include <cstring>

#include "core/format/es_formats.h"
#include "core/format/formats.h"
#include "core/format/pixel_transfer.h"
#include "core/format/vertex_decode.h"
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

// GL_BGRA with a packed 2_10_10_10 type swaps x and z like GL_BGRA bytes
// (KHR-GL43.vertex_attrib_binding.basic-input-case5).
TEST(format_vertex_runs_decode_as_elements_do) {
    // decodeVertexFloats gives decodeVertexFloat's values, bit for bit.
    uint8_t elements[7 * 20];
    for (size_t i = 0; i < sizeof(elements); ++i) elements[i] = static_cast<uint8_t>(i * 73 + 41);
    const GLenum types[] = {GL_BYTE, GL_UNSIGNED_BYTE, GL_SHORT, GL_UNSIGNED_SHORT, GL_INT, GL_UNSIGNED_INT, GL_FLOAT};
    for (GLenum type : types)
        for (GLint size = 1; size <= 4; ++size)
            for (bool normalized : {false, true}) {
                uint8_t run[7 * 32];
                std::memset(run, 0xcd, sizeof(run));
                CHECK(decodeVertexFloats(elements, 20, 7, type, size, normalized, run, 32));
                for (int v = 0; v < 7; ++v) {
                    float expected[4];
                    decodeVertexFloat(elements + v * 20, type, size, normalized, expected);
                    CHECK(std::memcmp(run + v * 32, expected, 16) == 0);
                }
            }
    uint8_t out[16];
    CHECK(!decodeVertexFloats(elements, 4, 1, GL_HALF_FLOAT, 2, false, out, 16));
    CHECK(!decodeVertexFloats(elements, 4, 1, GL_UNSIGNED_BYTE, GL_BGRA, true, out, 16));
}

TEST(format_vertex_bgra_packed_swaps_red_and_blue) {
    const uint32_t word = 0u | (1023u << 10) | (511u << 20) | (1u << 30);
    float out[4];
    decodeVertexFloat(reinterpret_cast<const uint8_t*>(&word), GL_UNSIGNED_INT_2_10_10_10_REV, GL_BGRA, true, out);
    CHECK(out[0] > 0.49f && out[0] < 0.51f);
    CHECK(out[1] == 1.0f);
    CHECK(out[2] == 0.0f);
    CHECK(out[3] > 0.33f && out[3] < 0.34f);
}

TEST(format_transfer_fast_paths) {
    PixelStoreParams ps;  // alignment 4
    // RGB bytes into RGB8 (stored RGBA8): rows padded to 4 bytes, alpha opaque.
    const FormatInfo* rgb8 = formatInfo(GL_RGB8);
    CHECK(rgb8 != nullptr);
    if (rgb8) {
        const uint8_t rgb[2][4 * 3] = {{1, 2, 3, 4, 5, 6, 7, 8, 9, 0, 0, 0}, {10, 11, 12, 13, 14, 15, 16, 17, 18, 0, 0, 0}};
        uint8_t stored[2 * 3 * 4] = {};
        unpackPixels(rgb, GL_RGB, GL_UNSIGNED_BYTE, 3, 2, 1, ps, *rgb8, stored);
        const uint8_t want[] = {1, 2, 3, 255, 4, 5, 6, 255, 7, 8, 9, 255, 10, 11, 12, 255, 13, 14, 15, 255, 16, 17, 18, 255};
        CHECK(std::memcmp(stored, want, sizeof(want)) == 0);
    }
    // BGRA into RGBA8, as bytes and as UNSIGNED_INT_8_8_8_8_REV; and back.
    const FormatInfo* rgba8 = formatInfo(GL_RGBA8);
    CHECK(rgba8 != nullptr);
    if (rgba8) {
        const uint8_t bgra[8] = {10, 20, 30, 40, 50, 60, 70, 80};
        uint8_t fromBytes[8] = {}, fromRev[8] = {};
        unpackPixels(bgra, GL_BGRA, GL_UNSIGNED_BYTE, 2, 1, 1, ps, *rgba8, fromBytes);
        unpackPixels(bgra, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, 2, 1, 1, ps, *rgba8, fromRev);
        const uint8_t want[8] = {30, 20, 10, 40, 70, 60, 50, 80};
        CHECK(std::memcmp(fromBytes, want, 8) == 0);
        CHECK(std::memcmp(fromRev, want, 8) == 0);
        uint8_t back[8] = {}, backRev[8] = {}, asRgba[8] = {};
        packPixels(want, *rgba8, 2, 1, 1, GL_BGRA, GL_UNSIGNED_BYTE, ps, true, back);
        packPixels(want, *rgba8, 2, 1, 1, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, ps, true, backRev);
        packPixels(want, *rgba8, 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, ps, true, asRgba);
        CHECK(std::memcmp(back, bgra, 8) == 0);
        CHECK(std::memcmp(backRev, bgra, 8) == 0);
        CHECK(std::memcmp(asRgba, want, 8) == 0);
    }
    // Half floats into RGBA16F are copied whole, and byte-swapped when asked.
    const FormatInfo* rgba16f = formatInfo(GL_RGBA16F);
    CHECK(rgba16f != nullptr);
    if (rgba16f) {
        const uint16_t halfs[4] = {0x3c00, 0xc000, 0x7e01, 0x0001};  // 1, -2, a NaN payload, a denormal
        uint16_t stored[4] = {}, back[4] = {};
        unpackPixels(halfs, GL_RGBA, GL_HALF_FLOAT, 1, 1, 1, ps, *rgba16f, stored);
        CHECK(std::memcmp(stored, halfs, 8) == 0);
        packPixels(stored, *rgba16f, 1, 1, 1, GL_RGBA, GL_HALF_FLOAT, ps, false, back);
        CHECK(std::memcmp(back, halfs, 8) == 0);
        PixelStoreParams swapped = ps;
        swapped.swapBytes = true;
        uint16_t fromSwapped[4] = {};
        unpackPixels(halfs, GL_RGBA, GL_HALF_FLOAT, 1, 1, 1, swapped, *rgba16f, fromSwapped);
        CHECK(fromSwapped[0] == 0x003c && fromSwapped[1] == 0x00c0);
    }
    // Luminance-alpha reads back the bytes it stored.
    const FormatInfo* la = formatInfo(GL_LUMINANCE8_ALPHA8);
    if (la) {
        const uint8_t texels[4] = {200, 100, 50, 25};
        uint8_t stored[4] = {}, back[4] = {};
        unpackPixels(texels, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, 2, 1, 1, ps, *la, stored);
        packPixels(stored, *la, 2, 1, 1, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, ps, true, back);
        CHECK(std::memcmp(back, texels, 4) == 0);
    }
}

TEST(bench_pixel_transfer) {
    // WMGL_BENCH: unpack and pack of a 2048x2048 image in common upload formats.
    if (!std::getenv("WMGL_BENCH")) return;
    constexpr int kSize = 2048;
    std::vector<uint8_t> client(static_cast<size_t>(kSize) * kSize * 8, 7), stored(static_cast<size_t>(kSize) * kSize * 8);
    PixelStoreParams ps;
    struct Case { const char* name; GLenum internal, format, type; bool pack; };
    const Case cases[] = {{"RGB8 <- RGB ubyte", GL_RGB8, GL_RGB, GL_UNSIGNED_BYTE, false},
                          {"RGBA8 <- BGRA ubyte", GL_RGBA8, GL_BGRA, GL_UNSIGNED_BYTE, false},
                          {"RGBA8 <- BGRA 8888_REV", GL_RGBA8, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, false},
                          {"RGBA16F <- RGBA half", GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, false},
                          {"RGBA8 -> RGBA ubyte", GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, true},
                          {"RGBA8 -> BGRA ubyte", GL_RGBA8, GL_BGRA, GL_UNSIGNED_BYTE, true}};
    for (const Case& c : cases) {
        const FormatInfo* f = formatInfo(c.internal);
        if (!f) continue;
        const auto start = std::chrono::steady_clock::now();
        if (c.pack) packPixels(stored.data(), *f, kSize, kSize, 1, c.format, c.type, ps, true, client.data());
        else unpackPixels(client.data(), c.format, c.type, kSize, kSize, 1, ps, *f, stored.data());
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        std::fprintf(stderr, "  bench: %-26s %7.2f ms\n", c.name, ms);
    }
}

// OpenGL ES glCopyTexImage: unsized formats take sources of at most 8 bits
// per component (table 8.22), and OES_required_internalformat's sized
// luminance/alpha formats copy like their unsized ones.
TEST(format_es_copy_tex_image_rules) {
    GLenum error = GL_NO_ERROR;
    const FormatInfo& rgba8 = *formatInfo(GL_RGBA8);
    const FormatInfo& rgb10a2 = *formatInfo(GL_RGB10_A2);
    CHECK(es::copyTexImageFormat(rgba8, GL_RGB, error) == GL_RGB8);
    CHECK(es::copyTexImageFormat(rgb10a2, GL_RGB, error) == 0);
    CHECK(error == GL_INVALID_OPERATION);
    CHECK(es::copyTexImageFormat(rgb10a2, GL_LUMINANCE, error) == 0);
    CHECK(es::copyTexImageFormat(rgb10a2, GL_RGB10_A2, error) == GL_RGB10_A2);
    CHECK(es::copyTexImageFormat(rgba8, GL_ALPHA8, error) == GL_ALPHA8);
    CHECK(es::copyTexImageFormat(rgba8, GL_LUMINANCE8_ALPHA8, error) == GL_LUMINANCE8_ALPHA8);
    CHECK(es::copyTexImageFormat(rgba8, 0x8043 /* LUMINANCE4_ALPHA4_OES */, error) == GL_LUMINANCE8_ALPHA8);
    CHECK(es::copyTexImageFormat(*formatInfo(GL_RGB8), GL_ALPHA8, error) == 0);
    GLenum type = GL_UNSIGNED_BYTE;
    CHECK(es::texImageFormat(0x8043, GL_LUMINANCE_ALPHA, type, error) == GL_LUMINANCE8_ALPHA8);
    type = GL_UNSIGNED_BYTE;
    CHECK(es::texImageFormat(0x8043, GL_LUMINANCE, type, error) == 0);
    CHECK(error == GL_INVALID_OPERATION);
}

// unpackPixels' table conversions of 16-bit packed groups and floats into
// half floats give the generic conversion's texels, bit for bit: the same
// groups byte-swapped with GL_UNPACK_SWAP_BYTES take the generic path.
TEST(format_unpack_fast_paths_match_generic) {
    PixelStoreParams ps, swapped;
    ps.alignment = swapped.alignment = 1;
    swapped.swapBytes = true;
    std::vector<uint16_t> groups(65536), swappedGroups(65536);
    for (uint32_t v = 0; v < 65536; ++v) {
        groups[v] = static_cast<uint16_t>(v);
        swappedGroups[v] = static_cast<uint16_t>((v >> 8) | (v << 8));
    }
    const struct { GLenum internal, format, type; } packed[] = {
        {GL_RGB565, GL_RGB, GL_UNSIGNED_SHORT_5_6_5},           {GL_RGB8, GL_RGB, GL_UNSIGNED_SHORT_5_6_5_REV},
        {GL_RGBA4, GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4},         {GL_RGBA8, GL_BGRA, GL_UNSIGNED_SHORT_4_4_4_4_REV},
        {GL_RGB5_A1, GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1},       {GL_RGB8, GL_BGRA, GL_UNSIGNED_SHORT_1_5_5_5_REV},
        {GL_SRGB8_ALPHA8, GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4},
    };
    for (const auto& p : packed) {
        const FormatInfo* f = formatInfo(p.internal);
        CHECK(f != nullptr);
        if (!f) continue;
        std::vector<uint32_t> fast(65536), generic(65536);
        unpackPixels(groups.data(), p.format, p.type, 256, 256, 1, ps, *f, fast.data());
        unpackPixels(swappedGroups.data(), p.format, p.type, 256, 256, 1, swapped, *f, generic.data());
        CHECK(fast == generic);
    }
    const FormatInfo* rgba16f = formatInfo(GL_RGBA16F);
    const FormatInfo* rgb16f = formatInfo(GL_RGB16F);
    CHECK(rgba16f && rgb16f);
    if (rgba16f && rgb16f) {
        const float floats[12] = {1.0f, -2.5f, 65504.0f, 1e-7f, 70000.0f, -0.0f, 0.333f, 1e-5f, 3.0f, 4.0f, 5.0f, 6.0f};
        uint32_t swappedFloats[12];
        for (int i = 0; i < 12; ++i) {
            uint32_t u;
            std::memcpy(&u, &floats[i], 4);
            swappedFloats[i] = __builtin_bswap32(u);
        }
        for (const auto& [info, format, count] : {std::tuple{rgba16f, GL_RGBA, 3}, std::tuple{rgb16f, GL_RGB, 4}}) {
            uint16_t fast[16] = {}, generic[16] = {};
            unpackPixels(floats, format, GL_FLOAT, count, 1, 1, ps, *info, fast);
            unpackPixels(swappedFloats, format, GL_FLOAT, count, 1, 1, swapped, *info, generic);
            CHECK(std::memcmp(fast, generic, sizeof(fast)) == 0);
        }
    }
}

// unpackPixels' byte shuffles into RGBA8.
TEST(format_unpack_byte_shuffles) {
    PixelStoreParams ps;
    ps.alignment = 1;
    const FormatInfo* rgba8 = formatInfo(GL_RGBA8);
    const FormatInfo* rgb8 = formatInfo(GL_RGB8);
    CHECK(rgba8 && rgb8);
    if (!rgba8 || !rgb8) return;
    const uint8_t bytes[8] = {10, 20, 30, 40, 50, 60, 70, 80};
    const struct { const FormatInfo* info; GLenum format, type; int width; uint8_t want[8]; } cases[] = {
        {rgba8, GL_BGR, GL_UNSIGNED_BYTE, 2, {30, 20, 10, 255, 60, 50, 40, 255}},
        {rgba8, GL_RGB, GL_UNSIGNED_BYTE, 2, {10, 20, 30, 255, 40, 50, 60, 255}},
        {rgb8, GL_RGBA, GL_UNSIGNED_BYTE, 2, {10, 20, 30, 255, 50, 60, 70, 255}},
        {rgb8, GL_BGRA, GL_UNSIGNED_BYTE, 2, {30, 20, 10, 255, 70, 60, 50, 255}},
        {rgba8, GL_LUMINANCE, GL_UNSIGNED_BYTE, 2, {10, 10, 10, 255, 20, 20, 20, 255}},
        {rgba8, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, 2, {10, 10, 10, 20, 30, 30, 30, 40}},
        {rgba8, GL_RGBA, GL_UNSIGNED_INT_8_8_8_8, 2, {40, 30, 20, 10, 80, 70, 60, 50}},
        {rgba8, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8, 2, {20, 30, 40, 10, 60, 70, 80, 50}},
    };
    for (const auto& c : cases) {
        uint8_t stored[8] = {};
        unpackPixels(bytes, c.format, c.type, c.width, 1, 1, ps, *c.info, stored);
        CHECK(std::memcmp(stored, c.want, 8) == 0);
        if (c.type == GL_UNSIGNED_INT_8_8_8_8) {
            // The generic path (byte-swapped words) agrees.
            PixelStoreParams swapped = ps;
            swapped.swapBytes = true;
            const uint8_t reversed[8] = {40, 30, 20, 10, 80, 70, 60, 50};
            uint8_t generic[8] = {};
            unpackPixels(reversed, c.format, c.type, c.width, 1, 1, swapped, *c.info, generic);
            CHECK(std::memcmp(generic, c.want, 8) == 0);
        }
    }
}

// RGB bytes into RGBA8, four texels a step and the tail one at a time: rows
// of several widths, each followed by bytes the conversion must not read
// into the texels.
TEST(format_unpack_rgb_rows) {
    PixelStoreParams ps;
    ps.alignment = 1;
    const FormatInfo* rgb8 = formatInfo(GL_RGB8);
    CHECK(rgb8 != nullptr);
    if (!rgb8) return;
    for (int width : {1, 5, 6, 7, 13, 64}) {
        std::vector<uint8_t> rgb(static_cast<size_t>(width) * 3 * 2 + 16, 0xee);
        for (size_t i = 0; i < static_cast<size_t>(width) * 3 * 2; ++i) rgb[i] = static_cast<uint8_t>(i * 7 + 1);
        std::vector<uint8_t> stored(static_cast<size_t>(width) * 4 * 2, 0), want(stored.size());
        for (size_t t = 0; t < static_cast<size_t>(width) * 2; ++t) {
            std::memcpy(&want[t * 4], &rgb[t * 3], 3);
            want[t * 4 + 3] = 0xff;
        }
        unpackPixels(rgb.data(), GL_RGB, GL_UNSIGNED_BYTE, width, 2, 1, ps, *rgb8, stored.data());
        CHECK(stored == want);
    }
}
