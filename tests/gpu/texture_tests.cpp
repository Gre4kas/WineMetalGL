// Texture specification, sampling and read-back.
#include <cmath>
#include <cstring>

#include "gpu.h"

namespace {

const char* kQuadVs = R"(#version 330 core
in vec2 position;
out vec2 uv;
void main() { uv = position * 0.5 + 0.5; gl_Position = vec4(position, 0.0, 1.0); })";

// Draws `fs` over the whole framebuffer and returns the centre pixel.
std::vector<uint8_t> drawAndRead(const char* fs, int x = 32, int y = 32) {
    GLuint p = gpu::program(kQuadVs, fs);
    GLuint vao = gpu::quad();
    glUseProgram(p);
    glUniform1i(glGetUniformLocation(p, "tex"), 0);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    auto pixel = gpu::readRGBA8(x, y, 1, 1);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
    return pixel;
}

GLuint texture2D(GLenum internalFormat, int w, int h, GLenum format, GLenum type, const void* data) {
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(internalFormat), w, h, 0, format, type, data);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    return tex;
}

}  // namespace

GPU_TEST(gpu_texture_rgba8_sample) {
    const uint8_t texels[4 * 4] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255};
    GLuint tex = texture2D(GL_RGBA8, 2, 2, GL_RGBA, GL_UNSIGNED_BYTE, texels);
    const char* fs = R"(#version 330 core
in vec2 uv; uniform sampler2D tex; out vec4 o;
void main() { o = texture(tex, uv); })";
    // Texel (0, 0) is the bottom-left quarter.
    const uint8_t red[4] = {255, 0, 0, 255}, white[4] = {255, 255, 255, 255};
    CHECK(gpu::near(drawAndRead(fs, 8, 8).data(), red));
    CHECK(gpu::near(drawAndRead(fs, 56, 56).data(), white));
    glDeleteTextures(1, &tex);
    CHECK_GL();
}

GPU_TEST(gpu_texture_formats_roundtrip) {
    struct Case { GLenum internal, format, type; int bytes; };
    const Case cases[] = {
        {GL_R8, GL_RED, GL_UNSIGNED_BYTE, 1},       {GL_RG8, GL_RG, GL_UNSIGNED_BYTE, 2},
        {GL_RGB8, GL_RGB, GL_UNSIGNED_BYTE, 3},     {GL_RGBA16F, GL_RGBA, GL_FLOAT, 16},
        {GL_R32F, GL_RED, GL_FLOAT, 4},             {GL_RGBA8UI, GL_RGBA_INTEGER, GL_UNSIGNED_BYTE, 4},
        {GL_R32I, GL_RED_INTEGER, GL_INT, 4},       {GL_RGB10_A2, GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV, 4},
        {GL_SRGB8_ALPHA8, GL_RGBA, GL_UNSIGNED_BYTE, 4}, {GL_DEPTH_COMPONENT32F, GL_DEPTH_COMPONENT, GL_FLOAT, 4},
        {GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, 4},
    };
    for (const Case& c : cases) {
        const int w = 5, h = 3;
        std::vector<uint8_t> in(static_cast<size_t>(w) * h * c.bytes), out(in.size(), 0xcd);
        for (size_t i = 0; i < in.size(); ++i) in[i] = static_cast<uint8_t>((i * 37 + 11) & 0xff);
        if (c.type == GL_FLOAT) {
            auto* f = reinterpret_cast<float*>(in.data());
            for (size_t i = 0; i < in.size() / 4; ++i) f[i] = c.internal == GL_DEPTH_COMPONENT32F ? (i % 7) / 7.0f : i * 0.25f;
        }
        if (c.internal == GL_DEPTH24_STENCIL8) {
            // Depth is 24-bit; keep values exactly representable through the float storage.
            auto* v = reinterpret_cast<uint32_t*>(in.data());
            for (size_t i = 0; i < in.size() / 4; ++i) v[i] = (static_cast<uint32_t>(i * 4096) << 8) | (i & 0xff);
        }
        GLuint tex = texture2D(c.internal, w, h, c.format, c.type, in.data());
        CHECK_GL();
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glGetTexImage(GL_TEXTURE_2D, 0, c.format, c.type, out.data());
        CHECK_GL();
        const bool equal = in == out;
        if (!equal) std::fprintf(stderr, "  round trip mismatch for internal format 0x%x\n", c.internal);
        CHECK(equal);
        glDeleteTextures(1, &tex);
    }
}

GPU_TEST(gpu_texture_rgb_alpha_is_one) {
    const uint8_t texel[3] = {10, 20, 30};
    GLuint tex = texture2D(GL_RGB8, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, texel);
    const char* fs = R"(#version 330 core
in vec2 uv; uniform sampler2D tex; out vec4 o;
void main() { o = texture(tex, uv); })";
    const uint8_t want[4] = {10, 20, 30, 255};
    CHECK(gpu::near(drawAndRead(fs).data(), want));
    glDeleteTextures(1, &tex);
    CHECK_GL();
}

GPU_TEST(gpu_texture_integer_sampler) {
    const uint32_t texel = 4000000000u;
    GLuint tex = texture2D(GL_R32UI, 1, 1, GL_RED_INTEGER, GL_UNSIGNED_INT, &texel);
    const char* fs = R"(#version 330 core
in vec2 uv; uniform usampler2D tex; out vec4 o;
void main() { o = texture(tex, uv).r == 4000000000u ? vec4(0, 1, 0, 1) : vec4(1, 0, 0, 1); })";
    const uint8_t green[4] = {0, 255, 0, 255};
    CHECK(gpu::near(drawAndRead(fs).data(), green));
    glDeleteTextures(1, &tex);
    CHECK_GL();
}

GPU_TEST(gpu_texture_swizzle_and_sampler_object) {
    const uint8_t texel[4 * 4] = {255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255};
    GLuint tex = texture2D(GL_RGBA8, 2, 2, GL_RGBA, GL_UNSIGNED_BYTE, texel);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_ZERO);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, GL_RED);
    // The texture alone is incomplete (mipmapped min filter, level 1 missing) but
    // the sampler object overrides the filter.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    GLuint sampler = 0;
    glGenSamplers(1, &sampler);
    glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glBindSampler(0, sampler);
    const char* fs = R"(#version 330 core
in vec2 uv; uniform sampler2D tex; out vec4 o;
void main() { o = texture(tex, uv); })";
    const uint8_t want[4] = {0, 255, 0, 255};
    CHECK(gpu::near(drawAndRead(fs).data(), want));
    glBindSampler(0, 0);
    // Without the sampler the texture is incomplete and samples (0, 0, 0, 1).
    const uint8_t black[4] = {0, 0, 0, 255};
    CHECK(gpu::near(drawAndRead(fs).data(), black));
    glDeleteSamplers(1, &sampler);
    glDeleteTextures(1, &tex);
    CHECK_GL();
}

GPU_TEST(gpu_texture_generate_mipmap) {
    std::vector<uint8_t> texels(4 * 4 * 4);
    for (int i = 0; i < 16; ++i) {
        const bool white = (i % 2) == ((i / 4) % 2);
        for (int c = 0; c < 3; ++c) texels[i * 4 + c] = white ? 255 : 0;
        texels[i * 4 + 3] = 255;
    }
    GLuint tex = texture2D(GL_RGBA8, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, texels.data());
    glGenerateMipmap(GL_TEXTURE_2D);
    GLint w = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 2, GL_TEXTURE_WIDTH, &w);
    CHECK_EQ(w, 1);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
    const char* fs = R"(#version 330 core
in vec2 uv; uniform sampler2D tex; out vec4 o;
void main() { o = textureLod(tex, uv, 2.0); })";
    const auto pixel = drawAndRead(fs);
    CHECK(pixel[0] > 100 && pixel[0] < 155);
    glDeleteTextures(1, &tex);
    CHECK_GL();
}

GPU_TEST(gpu_texture_array_and_3d) {
    std::vector<uint8_t> layers(2 * 2 * 3 * 4);
    for (int layer = 0; layer < 3; ++layer)
        for (int i = 0; i < 4; ++i) {
            uint8_t* t = &layers[(layer * 4 + i) * 4];
            t[0] = static_cast<uint8_t>(layer * 100);
            t[1] = 7;
            t[2] = 9;
            t[3] = 255;
        }
    GLuint tex[2];
    glGenTextures(2, tex);
    glBindTexture(GL_TEXTURE_2D_ARRAY, tex[0]);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, 2, 2, 3, 0, GL_RGBA, GL_UNSIGNED_BYTE, layers.data());
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    const char* fs = R"(#version 330 core
in vec2 uv; uniform sampler2DArray tex; out vec4 o;
void main() { o = texture(tex, vec3(uv, 2.0)); })";
    auto pixel = drawAndRead(fs);
    CHECK_EQ(static_cast<int>(pixel[0]), 200);
    glBindTexture(GL_TEXTURE_3D, tex[1]);
    glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA8, 2, 2, 3, 0, GL_RGBA, GL_UNSIGNED_BYTE, layers.data());
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    std::vector<uint8_t> out(layers.size());
    glGetTexImage(GL_TEXTURE_3D, 0, GL_RGBA, GL_UNSIGNED_BYTE, out.data());
    CHECK(out == layers);
    const char* fs3 = R"(#version 330 core
in vec2 uv; uniform sampler3D tex; out vec4 o;
void main() { o = texture(tex, vec3(uv, 0.5)); })";
    pixel = drawAndRead(fs3);
    CHECK_EQ(static_cast<int>(pixel[0]), 100);
    glDeleteTextures(2, tex);
    CHECK_GL();
}

GPU_TEST(gpu_texture_buffer) {
    const float values[4] = {0.0f, 0.25f, 0.5f, 1.0f};
    GLuint buffer = 0, tex = 0;
    glGenBuffers(1, &buffer);
    glBindBuffer(GL_TEXTURE_BUFFER, buffer);
    glBufferData(GL_TEXTURE_BUFFER, sizeof(values), values, GL_STATIC_DRAW);
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_BUFFER, tex);
    glTexBuffer(GL_TEXTURE_BUFFER, GL_R32F, buffer);
    const char* fs = R"(#version 330 core
in vec2 uv; uniform samplerBuffer tex; out vec4 o;
void main() { o = vec4(texelFetch(tex, 3).r, texelFetch(tex, 1).r, 0, 1); })";
    const uint8_t want[4] = {255, 64, 0, 255};
    CHECK(gpu::near(drawAndRead(fs).data(), want));
    glDeleteTextures(1, &tex);
    glDeleteBuffers(1, &buffer);
    CHECK_GL();
}

GPU_TEST(gpu_texture_rgtc_roundtrip) {
    // One BC4 block: endpoints 255 and 0, all indices 0 -> red 255.
    const uint8_t block[8] = {255, 0, 0, 0, 0, 0, 0, 0};
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RED_RGTC1, 4, 4, 0, sizeof(block), block);
    CHECK_GL();
    GLint compressed = 0, size = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_COMPRESSED, &compressed);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_COMPRESSED_IMAGE_SIZE, &size);
    CHECK_EQ(compressed, 1);
    CHECK_EQ(size, 8);
    uint8_t out[8] = {};
    glGetCompressedTexImage(GL_TEXTURE_2D, 0, out);
    CHECK(std::memcmp(out, block, 8) == 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    const char* fs = R"(#version 330 core
in vec2 uv; uniform sampler2D tex; out vec4 o;
void main() { o = texture(tex, uv); })";
    const uint8_t want[4] = {255, 0, 0, 255};
    CHECK(gpu::near(drawAndRead(fs).data(), want));
    glDeleteTextures(1, &tex);
    CHECK_GL();
}

GPU_TEST(gpu_texture_errors) {
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, -1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_VALUE));
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_SHORT_5_6_5, nullptr);
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_OPERATION));
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8UI, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_OPERATION));
    glBindTexture(GL_TEXTURE_3D, tex);
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_OPERATION));
    glBindTexture(GL_TEXTURE_2D, 12345);
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_OPERATION));
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_OPERATION));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_ENUM));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, -1);
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_VALUE));
    // Proxy: too large is not an error, it zeroes the proxy image.
    glTexImage2D(GL_PROXY_TEXTURE_2D, 0, GL_RGBA8, 1 << 20, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    CHECK_GL();
    GLint w = -1;
    glGetTexLevelParameteriv(GL_PROXY_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
    CHECK_EQ(w, 0);
    CHECK(glIsTexture(tex));
    glDeleteTextures(1, &tex);
    CHECK(!glIsTexture(tex));
    CHECK_GL();
}
