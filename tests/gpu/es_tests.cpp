// OpenGL ES contexts (WGL_EXT_create_context_es_profile): their strings,
// GLSL ES shaders and the ES 3.2 features (advanced blending, ASTC).

#include <cstring>
#include <string>

#include "gpu.h"

using namespace gpu;

GPU_TEST_ES(gpu_es_context_strings, 32) {
    const char* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    const char* glsl = reinterpret_cast<const char*>(glGetString(GL_SHADING_LANGUAGE_VERSION));
    CHECK(version && std::strncmp(version, "OpenGL ES 3.2", 13) == 0);
    CHECK(glsl && std::strncmp(glsl, "OpenGL ES GLSL ES 3.20", 22) == 0);
    GLint major = 0, minor = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    glGetIntegerv(GL_MINOR_VERSION, &minor);
    CHECK(major == 3 && minor == 2);
    // ES has glGetString(GL_EXTENSIONS), and no profile mask.
    const char* extensions = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
    CHECK(extensions && std::strstr(extensions, "GL_OES_texture_3D"));
    CHECK_GL();
    GLint mask = 0;
    glGetIntegerv(GL_CONTEXT_PROFILE_MASK, &mask);
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_ENUM));
}

// GLSL ES 1.00 (no #version) draws from vertex array 0 with client memory arrays.
GPU_TEST_ES(gpu_es_glsl100_client_arrays, 20) {
    GLuint p = program("attribute vec2 position; void main() { gl_Position = vec4(position, 0.0, 1.0); }\n",
                       "precision mediump float; uniform vec4 u_color; void main() { gl_FragColor = u_color; }\n");
    glUseProgram(p);
    glUniform4f(glGetUniformLocation(p, "u_color"), 0.0f, 1.0f, 0.0f, 1.0f);
    static const float quadVerts[] = {-1, -1, 1, -1, -1, 1, 1, 1};
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quadVerts);
    glEnableVertexAttribArray(0);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    CHECK_GL();
    const uint8_t green[4] = {0, 255, 0, 255};
    CHECK(near(readRGBA8(10, 10, 1, 1).data(), green));
    glDeleteProgram(p);
}

// glCopyTexImage2D from a texture attachment that glTexImage2D re-specified
// while attached (KHR-GLES3.copy_tex_image_conversions.required).
GPU_TEST_ES(gpu_es_copy_tex_image_from_texture, 30) {
    const uint8_t texels[2 * 2 * 3] = {255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 0};
    GLuint src = 0, dst = 0, fb = 0;
    glGenTextures(1, &src);
    glGenTextures(1, &dst);
    glGenFramebuffers(1, &fb);
    glBindTexture(GL_TEXTURE_2D, src);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, 2, 2, 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fb);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, src, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, 2, 2, 0, GL_RGB, GL_UNSIGNED_BYTE, texels);
    CHECK_EQ(glCheckFramebufferStatus(GL_READ_FRAMEBUFFER), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE));
    glBindTexture(GL_TEXTURE_2D, dst);
    glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, 0, 0, 2, 2, 0);
    CHECK_GL();
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dst, 0);
    uint8_t out[2 * 2 * 4] = {};
    glReadPixels(0, 0, 2, 2, GL_RGBA, GL_UNSIGNED_BYTE, out);
    CHECK_GL();
    const uint8_t red[4] = {255, 0, 0, 255}, white[4] = {255, 255, 0, 255};
    CHECK(near(out, red));
    CHECK(near(out + 12, white));
    if (!near(out, red)) std::fprintf(stderr, "  got %u %u %u %u\n", out[0], out[1], out[2], out[3]);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fb);
    glDeleteTextures(1, &src);
    glDeleteTextures(1, &dst);
}

// KHR_blend_equation_advanced: MULTIPLY_KHR blends in the fragment function.
GPU_TEST_ES(gpu_es_advanced_blend_multiply, 32) {
    GLuint p = program("#version 320 es\nin vec2 position; void main() { gl_Position = vec4(position, 0.0, 1.0); }\n",
                       "#version 320 es\nprecision mediump float;\nlayout(blend_support_multiply) out;\n"
                       "layout(location = 0) out vec4 o; uniform vec4 u_color; void main() { o = u_color; }\n");
    GLuint vao = quad();
    glClearColor(0.5f, 1.0f, 1.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(p);
    glUniform4f(glGetUniformLocation(p, "u_color"), 0.5f, 0.5f, 0.0f, 1.0f);
    glEnable(GL_BLEND);
    glBlendEquation(0x9294 /* GL_MULTIPLY_KHR */);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    CHECK_GL();
    // Opaque source over opaque destination: cs * cd.
    const uint8_t want[4] = {64, 128, 0, 255};
    const std::vector<uint8_t> got = readRGBA8(10, 10, 1, 1);
    CHECK(near(got.data(), want, 2));
    if (!near(got.data(), want, 2)) std::fprintf(stderr, "  got %u %u %u %u\n", got[0], got[1], got[2], got[3]);
    // A program without blend_support for the equation cannot draw with it.
    GLuint plain = program("#version 320 es\nin vec2 position; void main() { gl_Position = vec4(position, 0.0, 1.0); }\n",
                           "#version 320 es\nprecision mediump float;\nlayout(location = 0) out vec4 o;\n"
                           "void main() { o = vec4(1.0); }\n");
    glUseProgram(plain);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_OPERATION));
    glDisable(GL_BLEND);
    glDeleteProgram(p);
    glDeleteProgram(plain);
    glDeleteVertexArrays(1, &vao);
}

// KHR_texture_compression_astc_ldr: a 4x4 void-extent block (one color).
GPU_TEST_ES(gpu_es_astc_void_extent, 32) {
    const char* extensions = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
    if (!std::strstr(extensions, "GL_KHR_texture_compression_astc_ldr")) return;  // GPUs without ASTC
    // Void-extent block: 0xDFC header, extents all ones, RGBA16 = (1, 0.5, 0, 1).
    const uint8_t block[16] = {0xFC, 0xFD, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                               0xFF, 0xFF, 0x00, 0x80, 0x00, 0x00, 0xFF, 0xFF};
    GLuint tex = 0, fb = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glCompressedTexImage2D(GL_TEXTURE_2D, 0, 0x93B0 /* GL_COMPRESSED_RGBA_ASTC_4x4_KHR */, 4, 4, 0, 16, block);
    CHECK_GL();
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    GLuint p = program("#version 300 es\nin vec2 position; out vec2 uv;\n"
                       "void main() { uv = position * 0.5 + 0.5; gl_Position = vec4(position, 0.0, 1.0); }\n",
                       "#version 300 es\nprecision mediump float; in vec2 uv; uniform sampler2D t; out vec4 o;\n"
                       "void main() { o = texture(t, uv); }\n");
    GLuint vao = quad();
    glUseProgram(p);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    CHECK_GL();
    const uint8_t want[4] = {255, 128, 0, 255};
    const std::vector<uint8_t> got = readRGBA8(10, 10, 1, 1);
    CHECK(near(got.data(), want, 2));
    if (!near(got.data(), want, 2)) std::fprintf(stderr, "  got %u %u %u %u\n", got[0], got[1], got[2], got[3]);
    glDeleteProgram(p);
    glDeleteVertexArrays(1, &vao);
    glDeleteTextures(1, &tex);
    (void)fb;
}
