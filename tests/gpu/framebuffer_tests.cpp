// Framebuffer objects, blits, buffer clears and copies.
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
