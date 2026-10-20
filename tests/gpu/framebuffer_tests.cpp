// Framebuffer objects, blits, buffer clears and copies.
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "gpu.h"

namespace {

GLuint colorTexture(GLenum internalFormat, int w, int h, GLenum format, GLenum type) {
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(internalFormat), w, h, 0, format, type, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    return tex;
}

GLuint framebuffer(GLuint tex) {
    GLuint fb = 0;
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    return fb;
}

}  // namespace

GPU_TEST(gpu_fbo_clear_and_read) {
    GLuint tex = colorTexture(GL_RGBA8, 16, 16, GL_RGBA, GL_UNSIGNED_BYTE);
    GLuint fb = framebuffer(tex);
    CHECK_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE));
    glClearColor(0, 0, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    const uint8_t blue[4] = {0, 0, 255, 255};
    CHECK(gpu::near(gpu::readRGBA8(3, 3, 1, 1).data(), blue));
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    // The texture now holds the cleared colour.
    std::vector<uint8_t> texels(16 * 16 * 4);
    glBindTexture(GL_TEXTURE_2D, tex);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels.data());
    CHECK(gpu::near(&texels[100 * 4], blue));
    glDeleteFramebuffers(1, &fb);
    glDeleteTextures(1, &tex);
    CHECK_GL();
}

GPU_TEST(gpu_clear_buffer_integer) {
    GLuint tex = colorTexture(GL_RGBA32I, 8, 8, GL_RGBA_INTEGER, GL_INT);
    GLuint fb = framebuffer(tex);
    const GLint value[4] = {-5, 7, 1 << 20, -(1 << 30)};
    glClearBufferiv(GL_COLOR, 0, value);
    CHECK_GL();
    GLint out[4] = {};
    glReadPixels(2, 2, 1, 1, GL_RGBA_INTEGER, GL_INT, out);
    CHECK(out[0] == value[0] && out[1] == value[1] && out[2] == value[2] && out[3] == value[3]);
    // A scissored clear goes through the draw path.
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 0, 4, 4);
    const GLint other[4] = {1, 2, 3, 4};
    glClearBufferiv(GL_COLOR, 0, other);
    glDisable(GL_SCISSOR_TEST);
    glReadPixels(1, 1, 1, 1, GL_RGBA_INTEGER, GL_INT, out);
    CHECK(out[0] == 1 && out[3] == 4);
    glReadPixels(6, 6, 1, 1, GL_RGBA_INTEGER, GL_INT, out);
    CHECK(out[0] == value[0]);
    glClearBufferiv(GL_DEPTH, 0, value);
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_ENUM));
    glClearBufferfv(GL_COLOR, 8, nullptr);
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_VALUE));
    glDeleteFramebuffers(1, &fb);
    glDeleteTextures(1, &tex);
    CHECK_GL();
}

GPU_TEST(gpu_blit_copy_scale_flip) {
    GLuint src = colorTexture(GL_RGBA8, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE);
    std::vector<uint8_t> texels(4 * 4 * 4);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) {
            uint8_t* t = &texels[(y * 4 + x) * 4];
            t[0] = static_cast<uint8_t>(x * 60);
            t[1] = static_cast<uint8_t>(y * 60);
            t[2] = 0;
            t[3] = 255;
        }
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, texels.data());
    GLuint readFb = framebuffer(src);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, readFb);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    // 1:1 copy to (10, 10).
    glBlitFramebuffer(0, 0, 4, 4, 10, 10, 14, 14, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    CHECK_GL();
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    auto p = gpu::readRGBA8(13, 11, 1, 1);
    const uint8_t a[4] = {180, 60, 0, 255};
    CHECK(gpu::near(p.data(), a));
    // 2x scale, flipped horizontally, into (20..28, 20..28).
    glBindFramebuffer(GL_READ_FRAMEBUFFER, readFb);
    glBlitFramebuffer(0, 0, 4, 4, 28, 20, 20, 28, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    CHECK_GL();
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    p = gpu::readRGBA8(20, 20, 1, 1);  // leftmost destination column = source column 3
    const uint8_t b[4] = {180, 0, 0, 255};
    CHECK(gpu::near(p.data(), b));
    glDeleteFramebuffers(1, &readFb);
    glDeleteTextures(1, &src);
    CHECK_GL();
}

GPU_TEST(gpu_blit_multisample_resolve) {
    GLuint rb = 0, msFb = 0;
    glGenRenderbuffers(1, &rb);
    glBindRenderbuffer(GL_RENDERBUFFER, rb);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, 4, GL_RGBA8, 16, 16);
    glGenFramebuffers(1, &msFb);
    glBindFramebuffer(GL_FRAMEBUFFER, msFb);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
    CHECK_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE));
    glClearColor(1, 0.5f, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    GLuint dst = colorTexture(GL_RGBA8, 16, 16, GL_RGBA, GL_UNSIGNED_BYTE);
    GLuint dstFb = framebuffer(dst);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, msFb);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dstFb);
    glBlitFramebuffer(0, 0, 16, 16, 0, 0, 16, 16, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    CHECK_GL();
    // Scaled blits from multisampled sources are errors.
    glBlitFramebuffer(0, 0, 16, 16, 0, 0, 8, 8, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_OPERATION));
    glBindFramebuffer(GL_READ_FRAMEBUFFER, dstFb);
    const uint8_t want[4] = {255, 128, 0, 255};
    CHECK(gpu::near(gpu::readRGBA8(5, 5, 1, 1).data(), want));
    GLfloat position[2] = {};
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, msFb);
    glGetMultisamplefv(GL_SAMPLE_POSITION, 3, position);
    CHECK(position[0] > 0 && position[0] < 1 && position[1] > 0 && position[1] < 1);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &msFb);
    glDeleteFramebuffers(1, &dstFb);
    glDeleteTextures(1, &dst);
    CHECK_GL();
}

GPU_TEST(gpu_blit_depth_and_stencil) {
    GLuint rbs[2], fbs[2];
    glGenRenderbuffers(2, rbs);
    glGenFramebuffers(2, fbs);
    for (int i = 0; i < 2; ++i) {
        glBindRenderbuffer(GL_RENDERBUFFER, rbs[i]);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, 8, 8);
        glBindFramebuffer(GL_FRAMEBUFFER, fbs[i]);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rbs[i]);
        glDrawBuffers(0, nullptr);
        glReadBuffer(GL_NONE);
        CHECK_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE));
    }
    glBindFramebuffer(GL_FRAMEBUFFER, fbs[0]);
    glClearBufferfi(GL_DEPTH_STENCIL, 0, 0.25f, 77);
    glBindFramebuffer(GL_FRAMEBUFFER, fbs[1]);
    glClearBufferfi(GL_DEPTH_STENCIL, 0, 1.0f, 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbs[0]);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbs[1]);
    // Scaled depth goes through the shader path; stencil through the CPU path.
    glBlitFramebuffer(0, 0, 4, 4, 0, 0, 8, 8, GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT, GL_NEAREST);
    CHECK_GL();
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbs[1]);
    GLfloat depth = 0;
    GLubyte stencil = 0;
    glReadPixels(6, 6, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &depth);
    glReadPixels(6, 6, 1, 1, GL_STENCIL_INDEX, GL_UNSIGNED_BYTE, &stencil);
    CHECK(std::abs(depth - 0.25f) < 1e-5f);
    CHECK_EQ(static_cast<int>(stencil), 77);
    glBlitFramebuffer(0, 0, 4, 4, 0, 0, 8, 8, GL_DEPTH_BUFFER_BIT, GL_LINEAR);
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_OPERATION));
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(2, fbs);
    CHECK_GL();
}

GPU_TEST(gpu_copy_tex_image) {
    glClearColor(0.0f, 1.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 0, 0, 8, 8, 0);
    CHECK_GL();
    // Different format: converted through the canonical texel.
    GLuint tex2 = 0;
    glGenTextures(1, &tex2);
    glBindTexture(GL_TEXTURE_2D, tex2);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RG16F, 8, 8, 0, GL_RG, GL_FLOAT, nullptr);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 2, 2, 0, 0, 4, 4);
    CHECK_GL();
    float out[8 * 8 * 2] = {};
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RG, GL_FLOAT, out);
    CHECK(out[(3 * 8 + 3) * 2 + 1] == 1.0f && out[(3 * 8 + 3) * 2] == 0.0f);
    std::vector<uint8_t> texels(8 * 8 * 4);
    glBindTexture(GL_TEXTURE_2D, tex);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels.data());
    const uint8_t green[4] = {0, 255, 0, 255};
    CHECK(gpu::near(&texels[20 * 4], green));
    GLuint texs[2] = {tex, tex2};
    glDeleteTextures(2, texs);
    CHECK_GL();
}

namespace {

// A 2D texture of `internalFormat` attached to a new framebuffer (bound), cleared to `clear`.
GLuint copySource(GLenum internalFormat, GLenum format, GLenum type, const float clear[4], GLuint& tex) {
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, 32, 16, 0, format, type, nullptr);
    GLuint fb = 0;
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    CHECK_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE));
    glClearColor(clear[0], clear[1], clear[2], clear[3]);
    glClear(GL_COLOR_BUFFER_BIT);
    return fb;
}

// Copies the read framebuffer's (4, 2, 8, 6) into a new 16x16 `internalFormat`
// texture at (3, 5), the rest of which is (10, 20, 30, 40); checks the copy
// ran on the GPU (no readback commit) and returns the texture as RGBA8.
std::vector<uint8_t> copiedRgba8(GLenum internalFormat) {
    std::vector<uint8_t> init(16 * 16 * 4);
    for (size_t i = 0; i < init.size(); i += 4) {
        init[i] = 10;
        init[i + 1] = 20;
        init[i + 2] = 30;
        init[i + 3] = 40;
    }
    GLuint dst = 0;
    glGenTextures(1, &dst);
    glBindTexture(GL_TEXTURE_2D, dst);
    glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, 16, 16, 0, GL_RGBA, GL_UNSIGNED_BYTE, init.data());
    unsigned long long commits = 0;
    {
        gpu::Counters counters;
        glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 3, 5, 4, 2, 8, 6);
        commits = counters["commits"];
    }
    CHECK_GL();
    if (commits) std::fprintf(stderr, "  0x%x: the copy committed %llu command buffers (CPU readback)\n", internalFormat, commits);
    CHECK_EQ(commits, 0ull);
    std::vector<uint8_t> out(16 * 16 * 4);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, out.data());
    glDeleteTextures(1, &dst);
    return out;
}

bool texelIs(const std::vector<uint8_t>& image, int x, int y, std::initializer_list<int> want) {
    const uint8_t* p = &image[static_cast<size_t>(y * 16 + x) * 4];
    int i = 0;
    for (int w : want) {
        if (std::abs(p[i] - w) > 1) {
            std::fprintf(stderr, "  texel (%d,%d) = %d %d %d %d\n", x, y, p[0], p[1], p[2], p[3]);
            return false;
        }
        ++i;
    }
    return true;
}

}  // namespace

GPU_TEST(gpu_copy_tex_sub_image_converts_on_gpu) {
    // Copies between formats convert on the GPU (issue #28: per texel on the
    // CPU, DOOM (2016)'s full-screen copies took ~115 ms each at 3840x2160).
    GLuint src = 0;
    const float hdr[4] = {0.25f, 0.5f, 2.0f, -1.0f};
    GLuint fb = copySource(GL_RGBA16F, GL_RGBA, GL_FLOAT, hdr, src);
    std::vector<uint8_t> image = copiedRgba8(GL_RGBA8);  // clamped to [0, 1]
    CHECK(texelIs(image, 3, 5, {64, 128, 255, 0}));
    CHECK(texelIs(image, 10, 10, {64, 128, 255, 0}));
    CHECK(texelIs(image, 11, 10, {10, 20, 30, 40}));  // outside the 8x6 copy
    CHECK(texelIs(image, 3, 11, {10, 20, 30, 40}));
    CHECK(texelIs(image, 2, 5, {10, 20, 30, 40}));
    // An RGB destination keeps its alpha of one.
    image = copiedRgba8(GL_RGB8);
    CHECK(texelIs(image, 5, 7, {64, 128, 255, 255}));
    // Half-float and two-channel destinations.
    image = copiedRgba8(GL_RG16F);
    CHECK(texelIs(image, 5, 7, {64, 128, 0, 255}));
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fb);
    glDeleteTextures(1, &src);

    // Channels the source lacks read (0, 0, 0, 1).
    const float red[4] = {0.5f, 0.75f, 0.75f, 0.75f};
    fb = copySource(GL_R8, GL_RED, GL_UNSIGNED_BYTE, red, src);
    image = copiedRgba8(GL_RGBA8);
    CHECK(texelIs(image, 5, 7, {128, 0, 0, 255}));
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fb);
    glDeleteTextures(1, &src);

    // sRGB encodings are copied as stored (a blit through a non-sRGB view).
    const float mid[4] = {0.5f, 0.25f, 1.0f, 0.5f};
    fb = copySource(GL_SRGB8_ALPHA8, GL_RGBA, GL_UNSIGNED_BYTE, mid, src);
    uint8_t stored[4] = {};
    glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, stored);
    image = copiedRgba8(GL_RGBA8);
    CHECK(texelIs(image, 5, 7, {stored[0], stored[1], stored[2], stored[3]}));
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fb);
    glDeleteTextures(1, &src);
    CHECK_GL();
}

GPU_TEST(gpu_copy_tex_sub_image_depth_and_layers) {
    // Depth into a depth-only texture of another format.
    GLuint depth = 0, fb = 0;
    glGenTextures(1, &depth);
    glBindTexture(GL_TEXTURE_2D, depth);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, 32, 16, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depth, 0);
    const GLenum none = GL_NONE;
    glDrawBuffers(1, &none);
    glReadBuffer(GL_NONE);
    CHECK_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE));
    glClearDepth(0.25);
    glClear(GL_DEPTH_BUFFER_BIT);
    GLuint dst = 0;
    glGenTextures(1, &dst);
    glBindTexture(GL_TEXTURE_2D, dst);
    std::vector<uint16_t> ones(16 * 16, 0xffff);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT16, 16, 16, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_SHORT, ones.data());
    unsigned long long commits = 0;
    {
        gpu::Counters counters;
        glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 2, 2, 0, 0, 4, 4);
        commits = counters["commits"];
    }
    CHECK_GL();
    CHECK_EQ(commits, 0ull);
    std::vector<float> d(16 * 16);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, GL_FLOAT, d.data());
    CHECK(std::abs(d[3 * 16 + 3] - 0.25f) < 1.0f / 65535 + 1e-6f);
    CHECK(d[1 * 16 + 1] == 1.0f);
    glDeleteTextures(1, &dst);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fb);
    glDeleteTextures(1, &depth);

    // Into a layer of an array texture and a slice of a 3D texture.
    GLuint src = 0;
    const float green[4] = {0, 1, 0, 1};
    fb = copySource(GL_RGBA16F, GL_RGBA, GL_FLOAT, green, src);
    for (GLenum target : {GL_TEXTURE_2D_ARRAY, GL_TEXTURE_3D}) {
        GLuint t = 0;
        glGenTextures(1, &t);
        glBindTexture(target, t);
        std::vector<uint8_t> zero(8 * 8 * 3 * 4, 0);
        glTexImage3D(target, 0, GL_RGBA8, 8, 8, 3, 0, GL_RGBA, GL_UNSIGNED_BYTE, zero.data());
        glCopyTexSubImage3D(target, 0, 0, 0, 1, 0, 0, 8, 8);
        CHECK_GL();
        std::vector<uint8_t> out(8 * 8 * 3 * 4);
        glGetTexImage(target, 0, GL_RGBA, GL_UNSIGNED_BYTE, out.data());
        CHECK(out[(8 * 8 * 1 + 9) * 4 + 1] == 255);  // layer 1
        CHECK(out[(8 * 8 * 0 + 9) * 4 + 1] == 0 && out[(8 * 8 * 2 + 9) * 4 + 1] == 0);
        glDeleteTextures(1, &t);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fb);
    glDeleteTextures(1, &src);
    CHECK_GL();
}

GPU_TEST(gpu_depth_only_fbo_read) {
    // Depth texture only; draw/read buffers keep their COLOR_ATTACHMENT0 defaults.
    GLuint tex = 0, fb = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, 8, 8, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, tex, 0);
    CHECK_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE));
    glClearDepth(0.25);
    glClear(GL_DEPTH_BUFFER_BIT);
    GLfloat depth = 0;
    glReadPixels(3, 3, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &depth);
    CHECK_GL();
    CHECK(std::abs(depth - 0.25f) < 1e-6f);
    // Draw buffers may name any attachment in any slot.
    const GLenum bufs[2] = {GL_NONE, GL_COLOR_ATTACHMENT0};
    glDrawBuffers(2, bufs);
    CHECK_GL();
    const GLenum dup[2] = {GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT1};
    glDrawBuffers(2, dup);
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_OPERATION));
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fb);
    glDeleteTextures(1, &tex);
    glClearDepth(1.0);
    CHECK_GL();
}

GPU_TEST(gpu_depth_only_position_only_vertex) {
    // A depth-only pass to a depth texture never drawn with colour (a shadow
    // map) draws with a vertex function that writes only the position: its
    // depth is the full vertex function's (drawn with colour into another
    // depth texture), texel for texel.
    GLuint p = gpu::program("#version 330 core\nlayout(location = 0) in vec2 pos;\nout vec3 normal;\nout vec4 tint;\n"
                            "uniform float tilt;\n"
                            "void main() {\n"
                            "    normal = normalize(vec3(pos, 1.0 + tilt));\n"
                            "    tint = vec4(sin(pos.x * 7.0), cos(pos.y * 5.0), tilt, 1.0);\n"
                            "    gl_Position = vec4(pos, 0.25 + 0.5 * (pos.x + 1.0) * 0.5 + tilt * pos.y, 1.0);\n"
                            "}\n",
                            "#version 330 core\nin vec3 normal;\nin vec4 tint;\nout vec4 o;\n"
                            "void main() { o = vec4(normal * 0.5 + 0.5, 1.0) * tint; }\n");
    const float tri[6] = {-1, -1, 3, -1, -1, 3};
    GLuint vao = 0, vbo = 0;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(tri), tri, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glEnableVertexAttribArray(0);
    glUseProgram(p);
    glUniform1f(glGetUniformLocation(p, "tilt"), 0.125f);
    GLuint depth[2] = {}, color = 0, fb[2] = {};
    glGenTextures(2, depth);
    glGenTextures(1, &color);
    glGenFramebuffers(2, fb);
    for (int i = 0; i < 2; ++i) {
        glBindTexture(GL_TEXTURE_2D, depth[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, 32, 32, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
        glBindFramebuffer(GL_FRAMEBUFFER, fb[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depth[i], 0);
    }
    glBindTexture(GL_TEXTURE_2D, color);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 32, 32, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindFramebuffer(GL_FRAMEBUFFER, fb[1]);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, fb[0]);
    const GLenum none = GL_NONE;
    glDrawBuffers(1, &none);
    glReadBuffer(GL_NONE);
    CHECK_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE));
    glViewport(0, 0, 32, 32);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_ALWAYS);
    std::vector<float> values[2];
    unsigned long long reduced = 0;
    for (int i = 0; i < 2; ++i) {
        glBindFramebuffer(GL_FRAMEBUFFER, fb[i]);
        glClear(GL_DEPTH_BUFFER_BIT | (i ? GL_COLOR_BUFFER_BIT : 0));
        gpu::Counters counters;
        glDrawArrays(GL_TRIANGLES, 0, 3);
        if (!i) reduced = counters["positionOnlyPipelines"];
        values[i].resize(32 * 32);
        glReadPixels(0, 0, 32, 32, GL_DEPTH_COMPONENT, GL_FLOAT, values[i].data());
    }
    CHECK_GL();
    CHECK_EQ(reduced, 1ull);
    int different = 0;
    for (size_t t = 0; t < values[0].size(); ++t) different += values[0][t] != values[1][t];
    if (different) std::fprintf(stderr, "  %d of 1024 depth texels differ\n", different);
    CHECK_EQ(different, 0);
    CHECK(values[0][16 * 32 + 16] > 0.2f && values[0][16 * 32 + 16] < 0.9f);
    glDisable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(2, fb);
    glDeleteTextures(2, depth);
    glDeleteTextures(1, &color);
    glDeleteBuffers(1, &vbo);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
}

// glInvalidateFramebuffer: the invalidated attachments of the pass in
// progress are not stored, unless the pass draws to them afterwards; the
// others keep their contents.
GPU_TEST(gpu_invalidate_framebuffer_stores_later_draws) {
    static const char* vs = R"(#version 330
in vec2 position;
void main() { gl_Position = vec4(position, 0.0, 1.0); })";
    static const char* fs = R"(#version 330
uniform vec4 color;
out vec4 o;
void main() { o = color; })";
    auto glInvalidateFramebuffer =
        reinterpret_cast<PFN_glInvalidateFramebuffer>(wmgl_get_proc_address("glInvalidateFramebuffer"));
    CHECK(glInvalidateFramebuffer != nullptr);
    if (!glInvalidateFramebuffer) return;
    GLuint p = gpu::program(vs, fs);
    GLuint vao = gpu::quad();
    GLuint tex = colorTexture(GL_RGBA8, 16, 16, GL_RGBA, GL_UNSIGNED_BYTE);
    GLuint fb = framebuffer(tex);
    GLuint depth = 0;
    glGenRenderbuffers(1, &depth);
    glBindRenderbuffer(GL_RENDERBUFFER, depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, 16, 16);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, depth);
    CHECK_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE));
    glViewport(0, 0, 16, 16);
    glUseProgram(p);
    glBindVertexArray(vao);
    const GLint color = glGetUniformLocation(p, "color");
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glUniform4f(color, 1, 0, 0, 1);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    // Colour invalidated, then drawn again in the same pass: the draw is stored.
    const GLenum colorAttachment = GL_COLOR_ATTACHMENT0;
    glInvalidateFramebuffer(GL_FRAMEBUFFER, 1, &colorAttachment);
    glUniform4f(color, 0, 1, 0, 1);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    const uint8_t green[4] = {0, 255, 0, 255}, blue[4] = {0, 0, 255, 255};
    CHECK(gpu::near(gpu::readRGBA8(8, 8, 1, 1).data(), green));
    // Depth and stencil invalidated at the end of a pass: the colour stays.
    glUniform4f(color, 0, 0, 1, 1);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    const GLenum depthStencil = GL_DEPTH_STENCIL_ATTACHMENT;
    glInvalidateFramebuffer(GL_FRAMEBUFFER, 1, &depthStencil);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    std::vector<uint8_t> texels(16 * 16 * 4);
    glBindTexture(GL_TEXTURE_2D, tex);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels.data());
    CHECK(gpu::near(&texels[(8 * 16 + 8) * 4], blue));
    // A partial invalidation keeps everything: an upload after an invalidation is kept too.
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glInvalidateFramebuffer(GL_FRAMEBUFFER, 1, &colorAttachment);
    std::vector<uint8_t> greenTexels(16 * 16 * 4);
    for (size_t i = 0; i < greenTexels.size(); i += 4) std::memcpy(&greenTexels[i], green, 4);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 16, 16, GL_RGBA, GL_UNSIGNED_BYTE, greenTexels.data());
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);
    glUniform4f(color, 0, 0, 1, 1);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisable(GL_BLEND);
    const uint8_t cyan[4] = {0, 255, 255, 255};
    CHECK(gpu::near(gpu::readRGBA8(8, 8, 1, 1).data(), cyan));
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fb);
    glDeleteRenderbuffers(1, &depth);
    glDeleteTextures(1, &tex);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
    CHECK_GL();
}
