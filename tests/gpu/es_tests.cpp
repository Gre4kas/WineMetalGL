// OpenGL ES contexts (WGL_EXT_create_context_es_profile): their strings,
// GLSL ES shaders and the ES 3.2 features (advanced blending, ASTC).

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

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

// glCopyTexImage2D into LUMINANCE_ALPHA (red to luminance, alpha to alpha),
// read back by sampling (it is not color-renderable).
GPU_TEST_ES(gpu_es_copy_tex_image_luminance_alpha, 30) {
    const uint8_t texels[2 * 2 * 4] = {255, 0, 0, 51, 0, 255, 0, 102, 0, 0, 255, 153, 128, 128, 128, 204};
    GLuint src = 0, dst = 0, fb = 0;
    glGenTextures(1, &src);
    glGenTextures(1, &dst);
    glGenFramebuffers(1, &fb);
    glBindTexture(GL_TEXTURE_2D, src);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fb);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, src, 0);
    glBindTexture(GL_TEXTURE_2D, dst);
    // First as RGBA, then again as LUMINANCE_ALPHA (the CTS reuses its
    // destination texture across formats).
    glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 0, 0, 2, 2, 0);
    glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE_ALPHA, 0, 0, 2, 2, 0);
    CHECK_GL();
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    GLuint p = program("#version 300 es\nin vec2 position; void main() { gl_Position = vec4(position, 0.0, 1.0); }\n",
                       "#version 300 es\nprecision mediump float; uniform sampler2D t; out vec4 o;\n"
                       "void main() { o = texelFetch(t, ivec2(gl_FragCoord.xy) / 32, 0); }\n");
    GLuint vao = quad();
    glUseProgram(p);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    CHECK_GL();
    const uint8_t want[4][4] = {{255, 255, 255, 51}, {0, 0, 0, 102}, {0, 0, 0, 153}, {128, 128, 128, 204}};
    const int at[4][2] = {{10, 10}, {40, 10}, {10, 40}, {40, 40}};
    for (int i = 0; i < 4; ++i) {
        const std::vector<uint8_t> got = readRGBA8(at[i][0], at[i][1], 1, 1);
        CHECK(near(got.data(), want[i], 2));
        if (!near(got.data(), want[i], 2)) std::fprintf(stderr, "  texel %d: got %u %u %u %u\n", i, got[0], got[1], got[2], got[3]);
    }
    glDeleteProgram(p);
    glDeleteVertexArrays(1, &vao);
    glDeleteFramebuffers(1, &fb);
    glDeleteTextures(1, &src);
    glDeleteTextures(1, &dst);
}

// A cube map with an extra level past its last (1x1) one is complete: the
// extra level is beyond q (dEQP-GLES2.functional.texture.completeness.cube.extra_level).
GPU_TEST_ES(gpu_es_cube_extra_level_complete, 20) {
    std::vector<uint8_t> blue(64 * 64 * 4);
    for (size_t i = 0; i < blue.size(); i += 4) blue[i + 2] = blue[i + 3] = 255;
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_CUBE_MAP, tex);
    for (int level = 0; level < 7; ++level)
        for (int face = 0; face < 6; ++face)
            glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, level, GL_RGBA, 64 >> level, 64 >> level, 0, GL_RGBA,
                         GL_UNSIGNED_BYTE, blue.data());
    for (int face = 0; face < 6; ++face)
        glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, 8, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, blue.data());
    CHECK_GL();
    GLuint p = program("attribute vec2 position; varying vec3 dir;\n"
                       "void main() { dir = vec3(1.0, position); gl_Position = vec4(position, 0.0, 1.0); }\n",
                       "precision mediump float; varying vec3 dir; uniform samplerCube t;\n"
                       "void main() { gl_FragColor = textureCube(t, dir); }\n");
    GLuint vao = quad();
    glUseProgram(p);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    CHECK_GL();
    const uint8_t want[4] = {0, 0, 255, 255};
    const std::vector<uint8_t> got = readRGBA8(32, 32, 1, 1);
    CHECK(near(got.data(), want, 2));
    if (!near(got.data(), want, 2)) std::fprintf(stderr, "  got %u %u %u %u\n", got[0], got[1], got[2], got[3]);
    glDeleteProgram(p);
    glDeleteVertexArrays(1, &vao);
    glDeleteTextures(1, &tex);
}

// Vertex-stage sampling captured by transform feedback, separate buffers
// (KHR-GLES3.copy_tex_image_conversions reads non-renderable copies so).
GPU_TEST_ES(gpu_es_vertex_texture_feedback, 30) {
    const uint8_t texels[4] = {64, 128, 192, 32};
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    const char* vs = "#version 300 es\n"
                     "uniform highp sampler2D a; uniform highp samplerCube b; uniform highp sampler2D src;\n"
                     "uniform int which;\n"
                     "layout(location = 0) in vec4 coord;\n"
                     "out vec4 dstValue; out vec4 srcValue; flat out int result;\n"
                     "void main() { vec4 s; vec4 d;\n"
                     "  if (which == 0) s = texture(src, coord.xy); else s = texture(b, coord.xyz);\n"
                     "  d = texture(a, coord.xy);\n"
                     "  result = s.w > 0.5 ? 1 : 0; srcValue = s; dstValue = d; gl_Position = vec4(0.0); }\n";
    const char* fs = "#version 300 es\nvoid main() {}\n";
    GLuint p = glCreateProgram();
    GLuint v = glCreateShader(GL_VERTEX_SHADER), f = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(v, 1, &vs, nullptr);
    glShaderSource(f, 1, &fs, nullptr);
    glCompileShader(v);
    glCompileShader(f);
    glAttachShader(p, v);
    glAttachShader(p, f);
    const char* names[] = {"result", "srcValue", "dstValue"};
    glTransformFeedbackVaryings(p, 3, names, GL_SEPARATE_ATTRIBS);
    glLinkProgram(p);
    GLint linked = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &linked);
    CHECK(linked);
    if (!linked) {
        char log[2048] = {};
        glGetProgramInfoLog(p, sizeof(log), nullptr, log);
        std::fprintf(stderr, "%s\n", log);
        return;
    }
    glUseProgram(p);
    glUniform1i(glGetUniformLocation(p, "src"), 0);
    glUniform1i(glGetUniformLocation(p, "a"), 0);
    glUniform1i(glGetUniformLocation(p, "b"), 1);
    glUniform1i(glGetUniformLocation(p, "which"), 0);
    GLuint buffers[4];
    glGenBuffers(4, buffers);
    static const float coord[4] = {0.5f, 0.5f, 0.0f, 0.0f};
    glBindBuffer(GL_ARRAY_BUFFER, buffers[3]);
    glBufferData(GL_ARRAY_BUFFER, sizeof(coord), coord, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, nullptr);
    glEnableVertexAttribArray(0);
    const GLsizeiptr sizes[3] = {4, 16, 16};
    for (int i = 0; i < 3; ++i) {
        glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, buffers[i]);
        glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, sizes[i], nullptr, GL_STATIC_DRAW);
        glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, static_cast<GLuint>(i), buffers[i]);
    }
    glEnable(GL_RASTERIZER_DISCARD);
    glBeginTransformFeedback(GL_POINTS);
    glDrawArrays(GL_POINTS, 0, 1);
    glEndTransformFeedback();
    glDisable(GL_RASTERIZER_DISCARD);
    CHECK_GL();
    float src[4] = {}, dst[4] = {};
    GLint result = -1;
    glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, buffers[0]);
    glGetBufferSubData(GL_TRANSFORM_FEEDBACK_BUFFER, 0, 4, &result);
    glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, buffers[1]);
    glGetBufferSubData(GL_TRANSFORM_FEEDBACK_BUFFER, 0, 16, src);
    glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, buffers[2]);
    glGetBufferSubData(GL_TRANSFORM_FEEDBACK_BUFFER, 0, 16, dst);
    CHECK_GL();
    std::fprintf(stderr, "  src %.3f %.3f %.3f %.3f dst %.3f %.3f %.3f %.3f result %d\n", src[0], src[1], src[2], src[3], dst[0],
                 dst[1], dst[2], dst[3], result);
    CHECK(std::abs(src[3] - 32.0f / 255.0f) < 0.01f);
    CHECK(std::abs(src[0] - 64.0f / 255.0f) < 0.01f);
    CHECK(result == 0);
    glDeleteBuffers(4, buffers);
    glDeleteProgram(p);
    glDeleteShader(v);
    glDeleteShader(f);
    glDeleteTextures(1, &tex);
}

// The CTS form of the copy check: source (RGBA8) and the LUMINANCE_ALPHA
// copy sampled together in a vertex shader, captured by transform feedback.
GPU_TEST_ES(gpu_es_copy_luminance_alpha_vertex_sampled, 30) {
    const uint8_t texels[2 * 2 * 4] = {255, 0, 0, 51, 0, 255, 0, 102, 0, 0, 255, 153, 128, 128, 128, 204};
    GLuint src = 0, dst = 0, fb = 0;
    glGenTextures(1, &src);
    glGenTextures(1, &dst);
    glGenFramebuffers(1, &fb);
    glBindTexture(GL_TEXTURE_2D, src);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fb);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, src, 0);
    glBindTexture(GL_TEXTURE_2D, dst);
    glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE_ALPHA, 0, 0, 2, 2, 0);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    CHECK_GL();
    const char* vs = "#version 300 es\n"
                     "uniform highp sampler2D dst_texture2D; uniform highp samplerCube dst_textureCube;\n"
                     "uniform highp sampler2D src_texture2D; uniform highp sampler3D src_texture3D;\n"
                     "uniform int samplers_to_use;\n"
                     "layout(location = 0) in vec4 dst_texture_coord; layout(location = 1) in vec4 src_texture_coord;\n"
                     "out vec4 dst_values; out vec4 src_values; flat out int compare_result;\n"
                     "void main() { vec4 s; vec4 d; int src_sampler = samplers_to_use & 0xff; int dst_sampler = samplers_to_use >> 8;\n"
                     "  if (src_sampler == 0) s = texture(src_texture2D, src_texture_coord.xy); else s = texture(src_texture3D, src_texture_coord.xyz);\n"
                     "  if (dst_sampler == 0) d = texture(dst_texture2D, dst_texture_coord.xy); else d = texture(dst_textureCube, dst_texture_coord.xyz);\n"
                     "  compare_result = abs(s.w - d.w) > 0.01 ? 0 : 1; dst_values = d; src_values = s; gl_Position = vec4(0.0); }\n";
    const char* fs = "#version 300 es\nvoid main() {}\n";
    GLuint p = glCreateProgram();
    GLuint v = glCreateShader(GL_VERTEX_SHADER), f = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(v, 1, &vs, nullptr);
    glShaderSource(f, 1, &fs, nullptr);
    glCompileShader(v);
    glCompileShader(f);
    glAttachShader(p, v);
    glAttachShader(p, f);
    const char* names[] = {"compare_result", "src_values", "dst_values"};
    glTransformFeedbackVaryings(p, 3, names, GL_SEPARATE_ATTRIBS);
    glLinkProgram(p);
    GLint linked = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &linked);
    CHECK(linked);
    if (!linked) return;
    glUseProgram(p);
    glUniform1i(glGetUniformLocation(p, "src_texture2D"), 0);
    glUniform1i(glGetUniformLocation(p, "dst_texture2D"), 1);
    glUniform1i(glGetUniformLocation(p, "samplers_to_use"), 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, src);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, dst);
    glActiveTexture(GL_TEXTURE0);
    // The four texel centres.
    static const float coords[4 * 4] = {0.25f, 0.25f, 0, 0, 0.75f, 0.25f, 0, 0, 0.25f, 0.75f, 0, 0, 0.75f, 0.75f, 0, 0};
    GLuint buffers[4];
    glGenBuffers(4, buffers);
    glBindBuffer(GL_ARRAY_BUFFER, buffers[3]);
    glBufferData(GL_ARRAY_BUFFER, sizeof(coords), coords, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, nullptr);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 0, nullptr);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    const GLsizeiptr sizes[3] = {16, 64, 64};
    for (int i = 0; i < 3; ++i) {
        glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, buffers[i]);
        glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, sizes[i], nullptr, GL_STATIC_DRAW);
        glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, static_cast<GLuint>(i), buffers[i]);
    }
    glEnable(GL_RASTERIZER_DISCARD);
    glBeginTransformFeedback(GL_POINTS);
    glDrawArrays(GL_POINTS, 0, 4);
    glEndTransformFeedback();
    glDisable(GL_RASTERIZER_DISCARD);
    CHECK_GL();
    float s[16] = {}, d[16] = {};
    GLint result[4] = {};
    glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, buffers[0]);
    glGetBufferSubData(GL_TRANSFORM_FEEDBACK_BUFFER, 0, 16, result);
    glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, buffers[1]);
    glGetBufferSubData(GL_TRANSFORM_FEEDBACK_BUFFER, 0, 64, s);
    glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, buffers[2]);
    glGetBufferSubData(GL_TRANSFORM_FEEDBACK_BUFFER, 0, 64, d);
    for (int i = 0; i < 4; ++i) {
        CHECK(result[i] == 1);
        if (result[i] != 1)
            std::fprintf(stderr, "  texel %d src %.3f %.3f %.3f %.3f dst %.3f %.3f %.3f %.3f\n", i, s[i * 4], s[i * 4 + 1],
                         s[i * 4 + 2], s[i * 4 + 3], d[i * 4], d[i * 4 + 1], d[i * 4 + 2], d[i * 4 + 3]);
    }
    glDeleteBuffers(4, buffers);
    glDeleteProgram(p);
    glDeleteShader(v);
    glDeleteShader(f);
    glDeleteFramebuffers(1, &fb);
    glDeleteTextures(1, &src);
    glDeleteTextures(1, &dst);
}

// The same copy from an RGBA4 source uploaded as UNSIGNED_SHORT_4_4_4_4.
GPU_TEST_ES(gpu_es_copy_rgba4_to_luminance_alpha, 30) {
    // (r, g, b, a) nibbles: (4, 0, 15, 15), (0, 15, 9, 15), (15, 9, 4, 15), (2, 3, 4, 6).
    const uint16_t texels[4] = {0x40ff, 0x0f9f, 0xf94f, 0x2346};
    GLuint src = 0, dst = 0, fb = 0;
    glGenTextures(1, &src);
    glGenTextures(1, &dst);
    glGenFramebuffers(1, &fb);
    glBindTexture(GL_TEXTURE_2D, src);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA4, 2, 2, 0, GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4, texels);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fb);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, src, 0);
    CHECK_EQ(glCheckFramebufferStatus(GL_READ_FRAMEBUFFER), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE));
    glBindTexture(GL_TEXTURE_2D, dst);
    glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE_ALPHA, 0, 0, 2, 2, 0);
    CHECK_GL();
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    GLuint p = program("#version 300 es\nin vec2 position; void main() { gl_Position = vec4(position, 0.0, 1.0); }\n",
                       "#version 300 es\nprecision mediump float; uniform sampler2D t; out vec4 o;\n"
                       "void main() { o = texelFetch(t, ivec2(gl_FragCoord.xy) / 32, 0); }\n");
    GLuint vao = quad();
    glUseProgram(p);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    CHECK_GL();
    const uint8_t want[4][4] = {{68, 68, 68, 255}, {0, 0, 0, 255}, {255, 255, 255, 255}, {34, 34, 34, 102}};
    const int at[4][2] = {{10, 10}, {40, 10}, {10, 40}, {40, 40}};
    for (int i = 0; i < 4; ++i) {
        const std::vector<uint8_t> got = readRGBA8(at[i][0], at[i][1], 1, 1);
        CHECK(near(got.data(), want[i], 2));
        if (!near(got.data(), want[i], 2)) std::fprintf(stderr, "  texel %d: got %u %u %u %u\n", i, got[0], got[1], got[2], got[3]);
    }
    glDeleteProgram(p);
    glDeleteVertexArrays(1, &vao);
    glDeleteFramebuffers(1, &fb);
    glDeleteTextures(1, &src);
    glDeleteTextures(1, &dst);
}

// dEQP-GLES2 texture.completeness.cube.extra_level binds its texture to
// GL_TEXTURE_2D, so its cube uploads define the default cube map (name 0):
// levels 0-6 of 64x64 and an extra level 8.
GPU_TEST_ES(gpu_es_default_cube_extra_level, 20) {
    std::vector<uint8_t> blue(64 * 64 * 4);
    for (size_t i = 0; i < blue.size(); i += 4) blue[i + 2] = blue[i + 3] = 255;
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
    for (int level = 0; level < 7; ++level)
        for (int face = 0; face < 6; ++face)
            glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, level, GL_RGBA, 64 >> level, 64 >> level, 0, GL_RGBA,
                         GL_UNSIGNED_BYTE, blue.data());
    for (int face = 0; face < 6; ++face)
        glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, 8, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, blue.data());
    CHECK_GL();
    GLuint p = program("attribute vec2 position; varying vec3 dir;\n"
                       "void main() { dir = vec3(1.0, position); gl_Position = vec4(position, 0.0, 1.0); }\n",
                       "precision mediump float; varying vec3 dir; uniform samplerCube t;\n"
                       "void main() { gl_FragColor = textureCube(t, dir); }\n");
    GLuint vao = quad();
    glUseProgram(p);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    CHECK_GL();
    const uint8_t want[4] = {0, 0, 255, 255};
    const std::vector<uint8_t> got = readRGBA8(32, 32, 1, 1);
    CHECK(near(got.data(), want, 2));
    if (!near(got.data(), want, 2)) std::fprintf(stderr, "  got %u %u %u %u\n", got[0], got[1], got[2], got[3]);
    glDeleteProgram(p);
    glDeleteVertexArrays(1, &vao);
    glDeleteTextures(1, &tex);
}

// Two dEQP-style cases back to back: each creates a program and an NPOT cube
// map (REPEAT), draws its six faces from client memory arrays in vertex array
// 0, and deletes both. The second case's first draw used to draw nothing.
GPU_TEST_ES(gpu_es_cube_cases_back_to_back, 20) {
    for (int run = 0; run < 2; ++run) {
        const int size = 63;
        std::vector<uint8_t> blue(size * size * 4);
        for (size_t i = 0; i < blue.size(); i += 4) blue[i + 2] = blue[i + 3] = 255;
        GLuint p = program("attribute highp vec4 position; attribute highp vec3 a_texCoord; varying highp vec3 v_texCoord;\n"
                           "void main() { gl_Position = position; v_texCoord = a_texCoord; }\n",
                           "precision mediump float; uniform samplerCube u_sampler; varying highp vec3 v_texCoord;\n"
                           "void main() { gl_FragColor = textureCube(u_sampler, v_texCoord); }\n");
        GLuint tex = 0;
        glGenTextures(1, &tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glBindTexture(GL_TEXTURE_CUBE_MAP, tex);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        for (int face = 0; face < 6; ++face)
            glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, 0, GL_RGBA, size, size, 0, GL_RGBA, GL_UNSIGNED_BYTE, blue.data());
        glUseProgram(p);
        glUniform1i(glGetUniformLocation(p, "u_sampler"), 0);
        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        static const float position[] = {-1, -1, 0, 1, -1, 1, 0, 1, 1, -1, 0, 1, 1, 1, 0, 1};
        static const float coords[] = {1, -1, -1, 1, 1, -1, 1, -1, 1, 1, 1, 1};
        const GLint pos = glGetAttribLocation(p, "position"), tc = glGetAttribLocation(p, "a_texCoord");
        glVertexAttribPointer(static_cast<GLuint>(pos), 4, GL_FLOAT, GL_FALSE, 0, position);
        glVertexAttribPointer(static_cast<GLuint>(tc), 3, GL_FLOAT, GL_FALSE, 0, coords);
        glEnableVertexAttribArray(static_cast<GLuint>(pos));
        glEnableVertexAttribArray(static_cast<GLuint>(tc));
        static const uint16_t indices[] = {0, 1, 2, 2, 1, 3};
        for (int face = 0; face < 6; ++face) {
            glClearColor(0, 0, 0, 0);
            glClear(GL_COLOR_BUFFER_BIT);
            glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, indices);
            const std::vector<uint8_t> got = readRGBA8(32, 32, 1, 1);
            const uint8_t want[4] = {0, 0, 255, 255};
            CHECK(near(got.data(), want, 2));
            if (!near(got.data(), want, 2))
                std::fprintf(stderr, "  run %d face %d: got %u %u %u %u\n", run, face, got[0], got[1], got[2], got[3]);
        }
        CHECK_GL();
        glDisableVertexAttribArray(static_cast<GLuint>(pos));
        glDisableVertexAttribArray(static_cast<GLuint>(tc));
        glDeleteTextures(1, &tex);
        glDeleteProgram(p);
    }
}

// The CTS clears at the start of a case, then uploads a texture and links a
// program before its first draw: the clear must still land before the draw.
GPU_TEST_ES(gpu_es_clear_upload_then_draw, 20) {
    // Something drawn before (the previous case's last frame).
    glClearColor(0, 1, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    readRGBA8(0, 0, 1, 1);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    const int size = 63;
    std::vector<uint8_t> blue(size * size * 4);
    for (size_t i = 0; i < blue.size(); i += 4) blue[i + 2] = blue[i + 3] = 255;
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glBindTexture(GL_TEXTURE_CUBE_MAP, tex);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    for (int face = 0; face < 6; ++face)
        glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, 0, GL_RGBA, size, size, 0, GL_RGBA, GL_UNSIGNED_BYTE, blue.data());
    glViewport(0, 0, 64, 64);
    GLuint p = program("attribute highp vec4 position; attribute highp vec3 a_texCoord; varying highp vec3 v_texCoord;\n"
                       "void main() { gl_Position = position; v_texCoord = a_texCoord; }\n",
                       "precision mediump float; uniform samplerCube u_sampler; varying highp vec3 v_texCoord;\n"
                       "void main() { gl_FragColor = textureCube(u_sampler, v_texCoord); }\n");
    glUseProgram(p);
    glBindVertexArray(0);
    static const float position[] = {-1, -1, 0, 1, -1, 1, 0, 1, 1, -1, 0, 1, 1, 1, 0, 1};
    static const float coords[] = {1, -1, -1, 1, 1, -1, 1, -1, 1, 1, 1, 1};
    const GLint pos = glGetAttribLocation(p, "position"), tc = glGetAttribLocation(p, "a_texCoord");
    glEnableVertexAttribArray(static_cast<GLuint>(pos));
    glVertexAttribPointer(static_cast<GLuint>(pos), 4, GL_FLOAT, GL_FALSE, 0, position);
    glEnableVertexAttribArray(static_cast<GLuint>(tc));
    glVertexAttribPointer(static_cast<GLuint>(tc), 3, GL_FLOAT, GL_FALSE, 0, coords);
    static const uint16_t indices[] = {0, 1, 2, 2, 1, 3};
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, indices);
    const std::vector<uint8_t> got = readRGBA8(32, 32, 1, 1);
    const uint8_t want[4] = {0, 0, 255, 255};
    CHECK(near(got.data(), want, 2));
    if (!near(got.data(), want, 2)) std::fprintf(stderr, "  got %u %u %u %u\n", got[0], got[1], got[2], got[3]);
    glDisableVertexAttribArray(static_cast<GLuint>(pos));
    glDisableVertexAttribArray(static_cast<GLuint>(tc));
    glDeleteTextures(1, &tex);
    glDeleteProgram(p);
    CHECK_GL();
}

// OpenGL ES 3 forbids blits whose source and destination are the same image
// (KHR-GLES3.api.coverage); separate framebuffers still blit.
GPU_TEST_ES(gpu_es_blit_same_buffer, 30) {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBlitFramebuffer(0, 0, 4, 4, 0, 0, 4, 4, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_OPERATION));
    GLuint tex = 0, fb = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 4, 4);
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fb);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    glBlitFramebuffer(0, 0, 4, 4, 0, 0, 4, 4, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    CHECK_GL();
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fb);
    glDeleteTextures(1, &tex);
}

// GLSL ES 3.00: #extension after a non-preprocessor token is an error.
GPU_TEST_ES(gpu_es_extension_after_tokens, 30) {
    auto compiles = [](const char* source) {
        GLuint s = glCreateShader(GL_FRAGMENT_SHADER);
        glShaderSource(s, 1, &source, nullptr);
        glCompileShader(s);
        GLint ok = 0;
        glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        glDeleteShader(s);
        return ok != 0;
    };
    CHECK(compiles("#version 300 es\n#extension all : warn\nprecision mediump float;\nout vec4 c;\n"
                   "void main() { c = vec4(1.0); }\n"));
    CHECK(!compiles("#version 300 es\nprecision mediump float;\nout vec4 c;\n"
                    "void main() {\n#extension all : disable\n c = vec4(1.0); }\n"));
    // Skipped groups and comments are not tokens.
    CHECK(compiles("#version 300 es\n// comment\n#if 0\nfloat x;\n#endif\n#extension all : warn\n"
                   "precision mediump float;\nout vec4 c;\nvoid main() { c = vec4(1.0); }\n"));
}

// OES_required_internalformat's sized luminance/alpha formats, and an R32F
// buffer's RED / FLOAT implementation read format.
GPU_TEST_ES(gpu_es_sized_luminance_alpha, 20) {
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    const uint8_t texels[4 * 2] = {};
    glTexImage2D(GL_TEXTURE_2D, 0, 0x8043 /* LUMINANCE4_ALPHA4_OES */, 2, 2, 0, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, texels);
    CHECK_GL();
    for (GLenum format : {GLenum(GL_ALPHA8), GLenum(GL_LUMINANCE8), GLenum(GL_LUMINANCE8_ALPHA8), GLenum(0x8043)}) {
        glCopyTexImage2D(GL_TEXTURE_2D, 0, format, 0, 0, 2, 2, 0);
        CHECK_GL();
    }
    glDeleteTextures(1, &tex);
}

GPU_TEST_ES(gpu_es_float_read_format, 30) {
    GLuint tex = 0, fb = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_R32F, 2, 2);
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    glClearColor(0.5f, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    GLint format = 0, type = 0;
    glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_FORMAT, &format);
    glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_TYPE, &type);
    CHECK_EQ(format, GL_RED);
    CHECK_EQ(type, GL_FLOAT);
    float value = 0;
    glReadPixels(0, 0, 1, 1, GL_RED, GL_FLOAT, &value);
    CHECK_GL();
    CHECK(value == 0.5f);
    glClearColor(0, 0, 0, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fb);
    glDeleteTextures(1, &tex);
}

// Re-specifying an RGB8 image as RGBA8 (the same Metal format) samples the
// new alpha, not the RGB format's alpha of one
// (KHR-GLES3.copy_tex_image_conversions.required reuses its textures).
GPU_TEST_ES(gpu_es_respecified_format_swizzle, 30) {
    GLuint p = program("#version 300 es\nin vec2 position; void main() { gl_Position = vec4(position, 0.0, 1.0); }\n",
                       "#version 300 es\nprecision mediump float; uniform sampler2D t; out vec4 o;\n"
                       "void main() { o = vec4(texelFetch(t, ivec2(0), 0).a, 0.0, 0.0, 1.0); }\n");
    GLuint vao = quad();
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    const uint8_t rgb[4] = {10, 20, 30, 0};
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, 1, 1, 0, GL_RGB, GL_UNSIGNED_BYTE, rgb);
    glUseProgram(p);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    const uint8_t one[4] = {255, 0, 0, 255};
    CHECK(near(readRGBA8(10, 10, 1, 1).data(), one));
    const uint8_t rgba[4] = {10, 20, 30, 102};
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    CHECK_GL();
    const uint8_t alpha[4] = {102, 0, 0, 255};
    CHECK(near(readRGBA8(10, 10, 1, 1).data(), alpha));
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glDeleteTextures(1, &tex);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
}

// && and || evaluate their right operand only when needed, also when it
// returns an array (dEQP-GLES3 arrays.complex_expression).
GPU_TEST_ES(gpu_es_short_circuit_array_operand, 30) {
    GLuint p = program("#version 300 es\nin vec2 position; void main() { gl_Position = vec4(position, 0.0, 1.0); }\n",
                       "#version 300 es\nprecision mediump float; out vec4 o; int g;\n"
                       "int[2] plus() { ++g; return int[2](g, g); }\n"
                       "bool minus() { --g; return false; }\n"
                       "void main() { g = 0; int a[2] = int[2](0, 0);\n"
                       "  minus() && (a == plus());\n"
                       "  o = vec4(g == -1 ? 1.0 : 0.0, g == 0 ? 1.0 : 0.0, 0.0, 1.0); }\n");
    GLuint vao = quad();
    glUseProgram(p);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    CHECK_GL();
    const uint8_t red[4] = {255, 0, 0, 255};
    const std::vector<uint8_t> got = readRGBA8(10, 10, 1, 1);
    CHECK(near(got.data(), red));
    if (!near(got.data(), red)) std::fprintf(stderr, "  got %d %d %d %d\n", got[0], got[1], got[2], got[3]);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
}

// textureGrad on cube maps picks the level of the gradients projected onto
// the face (dEQP-GLES3 texture_functions.texturegrad.samplercube_*).
GPU_TEST_ES(gpu_es_cube_texture_grad_level, 30) {
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_CUBE_MAP, tex);
    glTexStorage2D(GL_TEXTURE_CUBE_MAP, 7, GL_RGBA8, 64, 64);
    std::vector<uint8_t> texels(64 * 64 * 4);
    for (int level = 0; level < 7; ++level) {
        const int size = 64 >> level;
        for (int i = 0; i < size * size; ++i) {
            texels[i * 4] = static_cast<uint8_t>(level * 40);
            texels[i * 4 + 1] = 0;
            texels[i * 4 + 2] = 0;
            texels[i * 4 + 3] = 255;
        }
        for (int face = 0; face < 6; ++face)
            glTexSubImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, level, 0, 0, size, size, GL_RGBA, GL_UNSIGNED_BYTE,
                            texels.data());
    }
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    GLuint p = program("#version 300 es\nin vec2 position; void main() { gl_Position = vec4(position, 0.0, 1.0); }\n",
                       "#version 300 es\nprecision highp float; uniform samplerCube t; uniform vec4 p, dx, dy; out vec4 o;\n"
                       "void main() { o = textureGrad(t, p.xyz, dx.xyz, dy.xyz); }\n");
    GLuint vao = quad();
    glUseProgram(p);
    // Face size 64: |d(s)/dx| = 0.5 * 0.2 / 1.01 per pixel, rho = 6.34, lambda = 2.66: level 3.
    struct Case { float p[3], dx[3], dy[3]; };
    const Case cases[] = {{{0.1f, 0.2f, 1.01f}, {0.2f, 0, 0}, {0, 0, 0}},
                          {{0.1f, 0.2f, -1.01f}, {0, 0, 0}, {0, -0.2f, 0}},
                          {{1.01f, 0.1f, 0.2f}, {0, 0, 0.2f}, {0, 0, 0}},
                          {{0.1f, -1.01f, 0.2f}, {0.2f, 0, 0}, {0, 0, 0}}};
    for (const Case& c : cases) {
        glUniform4f(glGetUniformLocation(p, "p"), c.p[0], c.p[1], c.p[2], 0.0f);
        glUniform4f(glGetUniformLocation(p, "dx"), c.dx[0], c.dx[1], c.dx[2], 0.0f);
        glUniform4f(glGetUniformLocation(p, "dy"), c.dy[0], c.dy[1], c.dy[2], 0.0f);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        const std::vector<uint8_t> got = readRGBA8(10, 10, 1, 1);
        const uint8_t want[4] = {120, 0, 0, 255};
        CHECK(near(got.data(), want));
        if (!near(got.data(), want)) std::fprintf(stderr, "  p (%g %g %g): level %d\n", c.p[0], c.p[1], c.p[2], got[0] / 40);
    }
    CHECK_GL();
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
    glDeleteTextures(1, &tex);
}

// OpenGL ES clamps the depth glTexImage2D gives DEPTH_COMPONENT32F to [0, 1]
// (dEQP-GLES3 texture.specification.teximage2d_depth).
GPU_TEST_ES(gpu_es_depth32f_upload_clamped, 30) {
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    const float depth = 1.5f;
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, 1, 1, 0, GL_DEPTH_COMPONENT, GL_FLOAT, &depth);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    GLuint p = program("#version 300 es\nin vec2 position; void main() { gl_Position = vec4(position, 0.0, 1.0); }\n",
                       "#version 300 es\nprecision highp float; uniform highp sampler2D t; out vec4 o;\n"
                       "void main() { o = vec4(texture(t, vec2(0.5)).r * 0.5, 0.0, 0.0, 1.0); }\n");
    GLuint vao = quad();
    glUseProgram(p);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    CHECK_GL();
    const std::vector<uint8_t> got = readRGBA8(10, 10, 1, 1);
    const uint8_t want[4] = {128, 0, 0, 255};
    CHECK(near(got.data(), want));
    if (!near(got.data(), want)) std::fprintf(stderr, "  got %d\n", got[0]);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
    glDeleteTextures(1, &tex);
}

// Pixels whose centres lie on the edge two triangles share belong to the
// triangle dEQP's verifiers expect (texture.mipmap.2d.projected: the quad's
// upper-right triangle owns its anti-diagonal).
GPU_TEST_ES(gpu_es_shared_edge_owner, 30) {
    GLuint p = program("#version 300 es\nin vec2 position; in vec4 color; out vec4 c;\n"
                       "void main() { c = color; gl_Position = vec4(position, 0.0, 1.0); }\n",
                       "#version 300 es\nprecision mediump float; in vec4 c; out vec4 o; void main() { o = c; }\n");
    static const float verts[] = {-1, -1, 1, 0, 0, 1, -1, 1, 1, 0, 0, 1, 1, -1, 1, 0, 0, 1,
                                  1, -1, 0, 1, 0, 1, -1, 1, 0, 1, 0, 1, 1, 1, 0, 1, 0, 1};
    GLuint vao = 0, vbo = 0;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    glBindAttribLocation(p, 0, "position");
    glVertexAttribPointer(static_cast<GLuint>(glGetAttribLocation(p, "position")), 2, GL_FLOAT, GL_FALSE, 24, nullptr);
    glVertexAttribPointer(static_cast<GLuint>(glGetAttribLocation(p, "color")), 4, GL_FLOAT, GL_FALSE, 24,
                          reinterpret_cast<void*>(8));
    glEnableVertexAttribArray(static_cast<GLuint>(glGetAttribLocation(p, "position")));
    glEnableVertexAttribArray(static_cast<GLuint>(glGetAttribLocation(p, "color")));
    glViewport(0, 0, 64, 64);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(p);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    CHECK_GL();
    const std::vector<uint8_t> px = readRGBA8(0, 0, 64, 64);
    int red = 0, green = 0;
    for (int x = 0; x < 64; ++x) {
        const uint8_t* c = &px[(static_cast<size_t>(63 - x) * 64 + x) * 4];
        red += c[0] > 128;
        green += c[1] > 128;
    }
    std::fprintf(stderr, "  anti-diagonal: %d lower-left, %d upper-right\n", red, green);
    CHECK(green == 64);
    glDeleteBuffers(1, &vbo);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
}

// An indexed perspective quad from client memory: the pixels along the
// edge its triangles share take their own triangle's texture level (Apple
// GPUs shade quads across a shared edge together: dEQP-GLES3
// texture.mipmap.2d.projected, cell 2).
GPU_TEST_ES(gpu_es_shared_edge_texture_level, 30) {
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexStorage2D(GL_TEXTURE_2D, 7, GL_RGBA8, 64, 64);
    std::vector<uint8_t> texels(64 * 64 * 4);
    for (int level = 0; level < 7; ++level) {
        for (size_t i = 0; i < texels.size(); i += 4) {
            texels[i] = static_cast<uint8_t>(level * 40);
            texels[i + 1] = texels[i + 2] = 0;
            texels[i + 3] = 255;
        }
        glTexSubImage2D(GL_TEXTURE_2D, level, 0, 0, 64 >> level, 64 >> level, GL_RGBA, GL_UNSIGNED_BYTE, texels.data());
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
    GLuint p = program("#version 300 es\nin vec4 position; in vec2 tc; out vec2 v;\n"
                       "void main() { v = tc; gl_Position = position; }\n",
                       "#version 300 es\nprecision highp float; in vec2 v; uniform sampler2D t; out vec4 o;\n"
                       "void main() { o = texture(t, v); }\n");
    const float w[4] = {0.8f, 1.0f, 1.7f, 0.6f};
    const float verts[4][6] = {{-w[0], -w[0], 0, w[0], -0.3f, 0.6f},
                               {-w[1], w[1], 0, w[1], -0.3f, -0.9f},
                               {w[2], -w[2], 0, w[2], 0.7f, 0.6f},
                               {w[3], w[3], 0, w[3], 0.7f, -0.9f}};
    static const unsigned short indices[6] = {0, 1, 2, 2, 1, 3};
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    const GLuint pos = static_cast<GLuint>(glGetAttribLocation(p, "position")), tc = static_cast<GLuint>(glGetAttribLocation(p, "tc"));
    glVertexAttribPointer(pos, 4, GL_FLOAT, GL_FALSE, 24, &verts[0][0]);
    glVertexAttribPointer(tc, 2, GL_FLOAT, GL_FALSE, 24, &verts[0][4]);
    glEnableVertexAttribArray(pos);
    glEnableVertexAttribArray(tc);
    glViewport(0, 0, 64, 64);
    glUseProgram(p);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, indices);
    CHECK_GL();
    // Pixel (55, 9): the upper triangle's level is 1.69 (2); the merged quad's 1.49 (1).
    const std::vector<uint8_t> got = readRGBA8(55, 9, 1, 1);
    const uint8_t want[4] = {80, 0, 0, 255};
    CHECK(near(got.data(), want));
    if (!near(got.data(), want)) std::fprintf(stderr, "  level %d\n", got[0] / 40);
    glDisableVertexAttribArray(pos);
    glDisableVertexAttribArray(tc);
    glDeleteProgram(p);
    glDeleteTextures(1, &tex);
}

// glCopyTexImage2D from each face of a cube map attached to the read
// framebuffer (KHR-GLES3.copy_tex_image_conversions.required.cubemap_neg*).
GPU_TEST_ES(gpu_es_copy_from_cube_faces, 30) {
    GLuint cube = 0, dst = 0, fb = 0;
    glGenTextures(1, &cube);
    glBindTexture(GL_TEXTURE_CUBE_MAP, cube);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    // First as RGBA8, then re-specified as RGB8 negative faces first (the
    // CTS order): the faces given before the base face changes keep their data.
    const uint8_t rgba[2 * 2 * 4] = {};
    for (int face = 0; face < 6; ++face)
        glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, 0, GL_RGBA8, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    for (int face : {1, 3, 5, 0, 2, 4}) {
        const uint8_t texel[2 * 2 * 3] = {static_cast<uint8_t>(40 * face + 10), 20, 30, static_cast<uint8_t>(40 * face + 10), 20, 30,
                                          static_cast<uint8_t>(40 * face + 10), 20, 30, static_cast<uint8_t>(40 * face + 10), 20, 30};
        glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, 0, GL_RGB8, 2, 2, 0, GL_RGB, GL_UNSIGNED_BYTE, texel);
    }
    glGenFramebuffers(1, &fb);
    glGenTextures(1, &dst);
    for (int face = 0; face < 6; ++face) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fb);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, cube, 0);
        uint8_t direct[4] = {};
        glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, direct);
        glBindTexture(GL_TEXTURE_2D, dst);
        glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, 0, 0, 2, 2, 0);
        CHECK_GL();
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dst, 0);
        uint8_t copied[4] = {};
        glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, copied);
        const uint8_t want[4] = {static_cast<uint8_t>(40 * face + 10), 20, 30, 255};
        CHECK(near(direct, want));
        CHECK(near(copied, want));
        if (!near(copied, want) || !near(direct, want))
            std::fprintf(stderr, "  face %d: read %d %d %d, copied %d %d %d\n", face, direct[0], direct[1], direct[2], copied[0],
                         copied[1], copied[2]);
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fb);
    glDeleteTextures(1, &dst);
    glDeleteTextures(1, &cube);
}

// The CTS sequence: one cube map re-specified (negative faces first) with
// another format while its NEGATIVE_X face stays attached to the read
// framebuffer, then copied from.
GPU_TEST_ES(gpu_es_copy_from_respecified_cube_face, 30) {
    GLuint cube = 0, dst = 0, fb = 0, check = 0;
    glGenTextures(1, &cube);
    glGenTextures(1, &dst);
    glGenFramebuffers(1, &fb);
    glGenFramebuffers(1, &check);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    struct Spec { GLenum internal, format; int bytes; uint8_t value; };
    const Spec specs[] = {{GL_R8, GL_RED, 1, 50}, {GL_RGBA8, GL_RGBA, 4, 100}, {GL_RGB8, GL_RGB, 3, 150}, {GL_RGBA8, GL_RGBA, 4, 200}};
    static const GLenum order[6] = {GL_TEXTURE_CUBE_MAP_NEGATIVE_X, GL_TEXTURE_CUBE_MAP_NEGATIVE_Y, GL_TEXTURE_CUBE_MAP_NEGATIVE_Z,
                                    GL_TEXTURE_CUBE_MAP_POSITIVE_X, GL_TEXTURE_CUBE_MAP_POSITIVE_Y, GL_TEXTURE_CUBE_MAP_POSITIVE_Z};
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fb);
    for (const Spec& s : specs) {
        std::vector<uint8_t> data(static_cast<size_t>(4 * s.bytes), s.value);
        glBindTexture(GL_TEXTURE_CUBE_MAP, cube);
        for (GLenum target : order) glTexImage2D(target, 0, s.internal, 2, 2, 0, s.format, GL_UNSIGNED_BYTE, data.data());
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fb);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_CUBE_MAP_NEGATIVE_X, cube, 0);
        glBindTexture(GL_TEXTURE_2D, dst);
        glCopyTexImage2D(GL_TEXTURE_2D, 0, s.internal, 0, 0, 2, 2, 0);
        CHECK_GL();
        glBindFramebuffer(GL_READ_FRAMEBUFFER, check);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dst, 0);
        uint8_t copied[4] = {};
        glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, copied);
        CHECK(copied[0] == s.value);
        if (copied[0] != s.value) std::fprintf(stderr, "  %#x: copied %d, want %d\n", s.internal, copied[0], s.value);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fb);
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fb);
    glDeleteFramebuffers(1, &check);
    glDeleteTextures(1, &dst);
    glDeleteTextures(1, &cube);
}

// An sRGB-capable window is linear to OpenGL ES (dEQP's WGL configurations
// expect it): clears and draws store their values unconverted and the color
// encoding reads LINEAR. Desktop GL encodes with GL_FRAMEBUFFER_SRGB enabled.
TEST(gpu_es_srgb_capable_window_is_linear) {
    for (const bool es : {true, false}) {
        gpu::Fixture fixture(64, 64, true, es ? 30 : 33, es, 24, true);
        CHECK(fixture.ok);
        if (!fixture.ok) continue;
        if (!es) glEnable(GL_FRAMEBUFFER_SRGB);
        GLint encoding = 0;
        glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, es ? GL_BACK : GL_BACK_LEFT,
                                              GL_FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING, &encoding);
        CHECK_EQ(encoding, es ? GL_LINEAR : GL_SRGB);
        glClearColor(0.5f, 0.5f, 0.5f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        uint8_t cleared[4] = {};
        glReadPixels(8, 8, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, cleared);
        const uint8_t want[4] = {static_cast<uint8_t>(es ? 128 : 188), static_cast<uint8_t>(es ? 128 : 188),
                                 static_cast<uint8_t>(es ? 128 : 188), 255};
        CHECK(near(cleared, want, 2));
        GLuint prog = program(es ? "#version 300 es\nlayout(location = 0) in vec2 p;\nvoid main() { gl_Position = vec4(p, 0.0, 1.0); }\n"
                                 : "#version 330 core\nlayout(location = 0) in vec2 p;\nvoid main() { gl_Position = vec4(p, 0.0, 1.0); }\n",
                              es ? "#version 300 es\nprecision mediump float;\nout vec4 c;\nvoid main() { c = vec4(0.5, 0.5, 0.5, 1.0); }\n"
                                 : "#version 330 core\nout vec4 c;\nvoid main() { c = vec4(0.5, 0.5, 0.5, 1.0); }\n");
        GLuint vao = quad();
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glUseProgram(prog);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        uint8_t drawn[4] = {};
        glReadPixels(8, 8, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, drawn);
        CHECK(near(drawn, want, 2));
        if (!near(drawn, want, 2)) std::fprintf(stderr, "  es=%d: drew %d, want %d\n", es, drawn[0], want[0]);
        CHECK_GL();
        glDeleteVertexArrays(1, &vao);
        glDeleteProgram(prog);
    }
}

// A window with 32-bit depth and stencil is DEPTH32F_STENCIL8: its depth
// blits to DEPTH32F_STENCIL8 buffers (not DEPTH24_STENCIL8) and reads FLOAT.
TEST(gpu_es_depth32f_window_blit) {
    gpu::Fixture fixture(64, 64, true, 30, true, 32, false);
    CHECK(fixture.ok);
    if (!fixture.ok) return;
    GLint type = 0;
    glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_DEPTH, GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE, &type);
    CHECK_EQ(type, GL_FLOAT);
    glClearDepth(0.25);
    glClear(GL_DEPTH_BUFFER_BIT);
    for (const GLenum internal : {GL_DEPTH32F_STENCIL8, GL_DEPTH24_STENCIL8}) {
        GLuint rb = 0, fb = 0;
        glGenRenderbuffers(1, &rb);
        glBindRenderbuffer(GL_RENDERBUFFER, rb);
        glRenderbufferStorage(GL_RENDERBUFFER, internal, 64, 64);
        glGenFramebuffers(1, &fb);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fb);
        glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rb);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        glBlitFramebuffer(0, 0, 64, 64, 0, 0, 64, 64, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
        CHECK_EQ(glGetError(), internal == GL_DEPTH32F_STENCIL8 ? static_cast<GLenum>(GL_NO_ERROR)
                                                                : static_cast<GLenum>(GL_INVALID_OPERATION));
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &fb);
        glDeleteRenderbuffers(1, &rb);
    }
}

// Blits into an sRGB-capable window from an RGBA8 framebuffer follow the
// OpenGL ES rules of a linear window: a color blit is valid, and one between
// the window and itself is INVALID_OPERATION.
TEST(gpu_es_srgb_capable_window_blit) {
    gpu::Fixture fixture(64, 64, true, 30, true, 24, true);
    CHECK(fixture.ok);
    if (!fixture.ok) return;
    GLuint fb = 0, color = 0, depth = 0;
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glGenRenderbuffers(1, &color);
    glBindRenderbuffer(GL_RENDERBUFFER, color);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, 32, 32);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, color);
    glGenRenderbuffers(1, &depth);
    glBindRenderbuffer(GL_RENDERBUFFER, depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, 32, 32);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, depth);
    CHECK_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE));
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glBlitFramebuffer(0, 0, 32, 32, 0, 0, 32, 32, 0, GL_NEAREST);
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
    glBlitFramebuffer(0, 0, 32, 32, 0, 0, 32, 32, GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT, GL_LINEAR);
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_OPERATION));
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBlitFramebuffer(0, 0, 32, 32, 0, 0, 32, 32, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_OPERATION));
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fb);
    glBlitFramebuffer(0, 0, 32, 32, 0, 0, 32, 32, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fb);
    glDeleteRenderbuffers(1, &color);
    glDeleteRenderbuffers(1, &depth);
}

// A separable OpenGL ES program may hold one tessellation stage; a program
// that is not separable needs both.
GPU_TEST_ES(gpu_es_separable_single_tessellation_stage, 32) {
    const char* control =
        "#version 320 es\n"
        "layout(vertices = 3) out;\n"
        "void main() {\n"
        "    gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position;\n"
        "    gl_TessLevelInner[0] = 1.0;\n"
        "    gl_TessLevelOuter[0] = gl_TessLevelOuter[1] = gl_TessLevelOuter[2] = 1.0;\n"
        "}\n";
    GLuint separable = glCreateShaderProgramv(GL_TESS_CONTROL_SHADER, 1, &control);
    GLint linked = 0;
    glGetProgramiv(separable, GL_LINK_STATUS, &linked);
    CHECK_EQ(linked, GL_TRUE);
    GLuint shader = glCreateShader(GL_TESS_CONTROL_SHADER);
    glShaderSource(shader, 1, &control, nullptr);
    glCompileShader(shader);
    GLuint whole = glCreateProgram();
    glAttachShader(whole, shader);
    glLinkProgram(whole);
    glGetProgramiv(whole, GL_LINK_STATUS, &linked);
    CHECK_EQ(linked, GL_FALSE);
    CHECK_GL();
    glDeleteProgram(separable);
    glDeleteProgram(whole);
    glDeleteShader(shader);
}

// A program pipeline of separable vertex and geometry programs, without a
// fragment stage, captures the geometry program's transform feedback.
GPU_TEST_ES(gpu_es_pipeline_geometry_capture, 32) {
    auto separable = [](GLenum type, const char* source, const char* varying) {
        GLuint p = glCreateProgram();
        glProgramParameteri(p, GL_PROGRAM_SEPARABLE, GL_TRUE);
        GLuint s = glCreateShader(type);
        glShaderSource(s, 1, &source, nullptr);
        glCompileShader(s);
        glAttachShader(p, s);
        glTransformFeedbackVaryings(p, 1, &varying, GL_INTERLEAVED_ATTRIBS);
        glLinkProgram(p);
        GLint linked = 0;
        glGetProgramiv(p, GL_LINK_STATUS, &linked);
        if (!linked) {
            char log[2048] = {};
            glGetProgramInfoLog(p, sizeof(log), nullptr, log);
            std::fprintf(stderr, "  link failed: %s\n", log);
        }
        CHECK_EQ(linked, GL_TRUE);
        glDeleteShader(s);
        return p;
    };
    GLuint vs = separable(GL_VERTEX_SHADER,
                          "#version 320 es\n"
                          "flat out ivec4 out_vs_1;\nflat out int vertexID;\n"
                          "void main() {\n"
                          "    vertexID = gl_VertexID;\n"
                          "    out_vs_1 = ivec4(gl_VertexID, gl_VertexID + 1, gl_VertexID + 2, gl_VertexID + 3);\n"
                          "    gl_Position = vec4(1.0, 0.0, 0.0, 1.0);\n"
                          "}\n",
                          "out_vs_1");
    GLuint gs = separable(GL_GEOMETRY_SHADER,
                          "#version 320 es\n"
                          "layout(points) in;\nlayout(points, max_vertices = 1) out;\n"
                          "flat in int vertexID[];\nflat in ivec4 out_vs_1[];\nout vec4 out_gs_1;\n"
                          "void main() {\n"
                          "    out_gs_1 = vec4(vertexID[0] * 2, vertexID[0] * 2 + 1, vertexID[0] * 2 + 2, vertexID[0] * 2 + 3);\n"
                          "    gl_Position = vec4(0, 0, 0, 1);\n"
                          "    EmitVertex();\n"
                          "}\n",
                          "out_gs_1");
    GLuint pipeline = 0, vao = 0, buffer = 0;
    glGenProgramPipelines(1, &pipeline);
    glUseProgramStages(pipeline, GL_GEOMETRY_SHADER_BIT, gs);
    glUseProgramStages(pipeline, GL_VERTEX_SHADER_BIT, vs);
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &buffer);
    glBindBuffer(GL_ARRAY_BUFFER, buffer);
    glBufferData(GL_ARRAY_BUFFER, 16, nullptr, GL_STREAM_READ);
    glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, buffer);
    glUseProgram(0);
    glBindProgramPipeline(pipeline);
    glEnable(GL_RASTERIZER_DISCARD);
    glBeginTransformFeedback(GL_POINTS);
    glDrawArrays(GL_POINTS, 0, 1);
    CHECK_GL();
    glEndTransformFeedback();
    glDisable(GL_RASTERIZER_DISCARD);
    const float* data = static_cast<const float*>(glMapBufferRange(GL_TRANSFORM_FEEDBACK_BUFFER, 0, 16, GL_MAP_READ_BIT));
    CHECK(data && data[0] == 0.0f && data[1] == 1.0f && data[2] == 2.0f && data[3] == 3.0f);
    if (data) glUnmapBuffer(GL_TRANSFORM_FEEDBACK_BUFFER);
    CHECK_GL();
    glBindProgramPipeline(0);
    glDeleteProgramPipelines(1, &pipeline);
    glDeleteBuffers(1, &buffer);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(vs);
    glDeleteProgram(gs);
}

// OpenGL ES stages agree on whether a varying is flat; centroid may differ.
GPU_TEST_ES(gpu_es_interpolation_qualifiers_match, 32) {
    auto links = [](const char* vsOut, const char* gsIn) {
        const std::string vs = std::string("#version 320 es\n") + vsOut + " vec4 test;\nvoid main() { test = vec4(1.0); }\n";
        const std::string gs = std::string("#version 320 es\nlayout(points) in;\nlayout(points, max_vertices = 1) out;\n") +
                               gsIn + " vec4 test[];\nvoid main() { gl_Position = test[0]; EmitVertex(); }\n";
        const char* fs = "#version 320 es\nprecision mediump float;\nout vec4 o;\nvoid main() { o = vec4(1.0); }\n";
        GLuint p = glCreateProgram();
        for (const auto& [type, source] : {std::pair<GLenum, const char*>{GL_VERTEX_SHADER, vs.c_str()},
                                           {GL_GEOMETRY_SHADER, gs.c_str()}, {GL_FRAGMENT_SHADER, fs}}) {
            GLuint s = glCreateShader(type);
            glShaderSource(s, 1, &source, nullptr);
            glCompileShader(s);
            glAttachShader(p, s);
            glDeleteShader(s);
        }
        glLinkProgram(p);
        GLint linked = 0;
        glGetProgramiv(p, GL_LINK_STATUS, &linked);
        glDeleteProgram(p);
        return linked == GL_TRUE;
    };
    CHECK(!links("out", "in flat"));
    CHECK(links("flat out", "flat in"));
    CHECK(links("out", "centroid in"));
    CHECK_GL();
}

// An OpenGL ES program pipeline whose vertex output and fragment input differ
// in precision does not validate; the same precisions do.
GPU_TEST_ES(gpu_es_pipeline_varying_precisions, 31) {
    auto validates = [](const char* vsPrecision, const char* fsPrecision) {
        const std::string vs = std::string("#version 310 es\nout ") + vsPrecision +
                               " vec4 v;\nvoid main() { v = vec4(1.0); gl_Position = vec4(0.0); }\n";
        const std::string fs = std::string("#version 310 es\nprecision mediump float;\nin ") + fsPrecision +
                               " vec4 v;\nout vec4 o;\nvoid main() { o = v; }\n";
        const char* vsSource = vs.c_str();
        const char* fsSource = fs.c_str();
        GLuint a = glCreateShaderProgramv(GL_VERTEX_SHADER, 1, &vsSource);
        GLuint b = glCreateShaderProgramv(GL_FRAGMENT_SHADER, 1, &fsSource);
        GLuint pipeline = 0;
        glGenProgramPipelines(1, &pipeline);
        glUseProgramStages(pipeline, GL_VERTEX_SHADER_BIT, a);
        glUseProgramStages(pipeline, GL_FRAGMENT_SHADER_BIT, b);
        glValidateProgramPipeline(pipeline);
        GLint status = 0;
        glGetProgramPipelineiv(pipeline, GL_VALIDATE_STATUS, &status);
        glDeleteProgramPipelines(1, &pipeline);
        glDeleteProgram(a);
        glDeleteProgram(b);
        return status == GL_TRUE;
    };
    CHECK(!validates("highp", "mediump"));
    CHECK(validates("mediump", "mediump"));
    CHECK_GL();
}

// GL_SAMPLE_MASK applies on top of the gl_SampleMask a shader writes, in
// multisample framebuffers of one sample too: the inverse masks cover nothing.
GPU_TEST_ES(gpu_es_sample_mask_with_shader_mask, 32) {
    GLuint prog = program("#version 320 es\nlayout(location = 0) in vec2 p;\nvoid main() { gl_Position = vec4(p, 0.0, 1.0); }\n",
                          "#version 320 es\nprecision mediump float;\nlayout(location = 0) out vec4 o;\nuniform highp uint u_mask;\n"
                          "void main() { gl_SampleMask[0] = int(~u_mask); o = vec4(0.0, 1.0, 0.0, 1.0); }\n");
    GLuint vao = quad();
    for (const int samples : {1, 4}) {
        GLuint ms = 0, single = 0, msFb = 0, singleFb = 0;
        glGenRenderbuffers(1, &ms);
        glBindRenderbuffer(GL_RENDERBUFFER, ms);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_RGBA8, 16, 16);
        glGenFramebuffers(1, &msFb);
        glBindFramebuffer(GL_FRAMEBUFFER, msFb);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, ms);
        glViewport(0, 0, 16, 16);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glUseProgram(prog);
        glUniform1ui(glGetUniformLocation(prog, "u_mask"), 0xAAAAAAAAu);
        glEnable(GL_SAMPLE_MASK);
        glSampleMaski(0, 0xAAAAAAAAu);
        glBindVertexArray(vao);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glDisable(GL_SAMPLE_MASK);
        glSampleMaski(0, ~0u);
        glGenRenderbuffers(1, &single);
        glBindRenderbuffer(GL_RENDERBUFFER, single);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, 16, 16);
        glGenFramebuffers(1, &singleFb);
        glBindFramebuffer(GL_FRAMEBUFFER, singleFb);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, single);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, msFb);
        glBlitFramebuffer(0, 0, 16, 16, 0, 0, 16, 16, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, singleFb);
        const uint8_t black[4] = {0, 0, 0, 255};
        const std::vector<uint8_t> got = readRGBA8(8, 8, 1, 1);
        CHECK(near(got.data(), black, 2));
        if (!near(got.data(), black, 2)) std::fprintf(stderr, "  %d samples: %d %d %d\n", samples, got[0], got[1], got[2]);
        CHECK_GL();
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &msFb);
        glDeleteFramebuffers(1, &singleFb);
        glDeleteRenderbuffers(1, &ms);
        glDeleteRenderbuffers(1, &single);
    }
    glViewport(0, 0, 64, 64);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(prog);
}

// A fragment shader run per sample sees only its own sample in
// gl_SampleMaskIn, both when it reads gl_SampleID and under
// GL_SAMPLE_SHADING.
GPU_TEST_ES(gpu_es_sample_mask_in_per_sample, 32) {
    const char* vs = "#version 320 es\nlayout(location = 0) in vec2 p;\nvoid main() { gl_Position = vec4(p, 0.0, 1.0); }\n";
    GLuint bySampleId = program(vs, "#version 320 es\nprecision mediump float;\nlayout(location = 0) out vec4 o;\n"
                                    "void main() { highp float id = float(gl_SampleID);\n"
                                    "    o = vec4(bitCount(gl_SampleMaskIn[0]) == 1 ? 0.0 : 1.0, 1.0, id * 0.0, 1.0); }\n");
    GLuint byShading = program(vs, "#version 320 es\nprecision mediump float;\nlayout(location = 0) out vec4 o;\n"
                                   "void main() { o = vec4(bitCount(gl_SampleMaskIn[0]) == 1 ? 0.0 : 1.0, 1.0, 0.0, 1.0); }\n");
    GLuint vao = quad();
    GLuint ms = 0, single = 0, msFb = 0, singleFb = 0;
    glGenRenderbuffers(1, &ms);
    glBindRenderbuffer(GL_RENDERBUFFER, ms);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, 4, GL_RGBA8, 16, 16);
    glGenFramebuffers(1, &msFb);
    glGenRenderbuffers(1, &single);
    glBindRenderbuffer(GL_RENDERBUFFER, single);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, 16, 16);
    glGenFramebuffers(1, &singleFb);
    glBindFramebuffer(GL_FRAMEBUFFER, singleFb);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, single);
    for (const bool shading : {false, true}) {
        glBindFramebuffer(GL_FRAMEBUFFER, msFb);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, ms);
        glViewport(0, 0, 16, 16);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glUseProgram(shading ? byShading : bySampleId);
        if (shading) {
            glEnable(GL_SAMPLE_SHADING);
            reinterpret_cast<PFN_glMinSampleShading>(wmgl_get_proc_address("glMinSampleShading"))(0.5f);
        }
        glBindVertexArray(vao);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glDisable(GL_SAMPLE_SHADING);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, singleFb);
        glBlitFramebuffer(0, 0, 16, 16, 0, 0, 16, 16, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, singleFb);
        const uint8_t green[4] = {0, 255, 0, 255};
        const std::vector<uint8_t> got = readRGBA8(8, 8, 1, 1);
        CHECK(near(got.data(), green, 2));
        if (!near(got.data(), green, 2)) std::fprintf(stderr, "  shading=%d: %d %d %d\n", shading, got[0], got[1], got[2]);
        CHECK_GL();
    }
    // Reading gl_SampleMaskIn is not writing a mask: single-sample targets draw too.
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, 64, 64);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(byShading);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    const uint8_t green[4] = {0, 255, 0, 255};
    CHECK(near(readRGBA8(32, 32, 1, 1).data(), green, 2));
    glDeleteFramebuffers(1, &msFb);
    glDeleteFramebuffers(1, &singleFb);
    glDeleteRenderbuffers(1, &ms);
    glDeleteRenderbuffers(1, &single);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(bySampleId);
    glDeleteProgram(byShading);
}

// OpenGL ES 3.1 with GL_OES_sample_shading at 0.5: each invocation covers
// at most numSamples - ceil(numSamples / 2) + 1 samples.
GPU_TEST_ES(gpu_es31_sample_mask_in_half_rate, 31) {
    GLuint prog = program("#version 310 es\nlayout(location = 0) in vec2 p;\nvoid main() { gl_Position = vec4(p, 0.0, 1.0); }\n",
                          "#version 310 es\n#extension GL_OES_sample_variables : require\n"
                          "layout(location = 0) out mediump vec4 fragColor;\n"
                          "uniform highp int u_minBitCount;\nuniform highp int u_maxBitCount;\n"
                          "void main (void)\n{\n    mediump int maskBitCount = 0;\n"
                          "    for (int i = 0; i < 32; ++i)\n"
                          "        if (((gl_SampleMaskIn[0] >> i) & 0x01) == 0x01)\n            ++maskBitCount;\n"
                          "    if (maskBitCount < u_minBitCount || maskBitCount > u_maxBitCount)\n"
                          "        fragColor = vec4(1.0, 0.0, 0.0, 1.0);\n    else\n"
                          "        fragColor = vec4(0.0, 1.0, 0.0, 1.0);\n}\n");
    GLuint vao = quad();
    GLuint ms = 0, single = 0, msFb = 0, singleFb = 0;
    glGenRenderbuffers(1, &ms);
    glBindRenderbuffer(GL_RENDERBUFFER, ms);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, 4, GL_RGBA8, 16, 16);
    glGenFramebuffers(1, &msFb);
    glBindFramebuffer(GL_FRAMEBUFFER, msFb);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, ms);
    glViewport(0, 0, 16, 16);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(prog);
    glUniform1i(glGetUniformLocation(prog, "u_minBitCount"), 1);
    glUniform1i(glGetUniformLocation(prog, "u_maxBitCount"), 3);
    glEnable(GL_SAMPLE_SHADING);
    reinterpret_cast<PFN_glMinSampleShading>(wmgl_get_proc_address("glMinSampleShadingOES"))(0.5f);
    glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisable(GL_SAMPLE_SHADING);
    glGenRenderbuffers(1, &single);
    glBindRenderbuffer(GL_RENDERBUFFER, single);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, 16, 16);
    glGenFramebuffers(1, &singleFb);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, singleFb);
    glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, single);
    glBlitFramebuffer(0, 0, 16, 16, 0, 0, 16, 16, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, singleFb);
    const uint8_t green[4] = {0, 255, 0, 255};
    const std::vector<uint8_t> got = readRGBA8(8, 8, 1, 1);
    CHECK(near(got.data(), green, 2));
    if (!near(got.data(), green, 2)) std::fprintf(stderr, "  got %d %d %d\n", got[0], got[1], got[2]);
    CHECK_GL();
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, 64, 64);
    glDeleteFramebuffers(1, &msFb);
    glDeleteFramebuffers(1, &singleFb);
    glDeleteRenderbuffers(1, &ms);
    glDeleteRenderbuffers(1, &single);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(prog);
}

// Separable programs pass varyings matched by location (different names),
// matrices among them, and by name.
GPU_TEST_ES(gpu_es_pipeline_varyings_by_location, 31) {
    const char* vsSource =
        "#version 310 es\n"
        "layout(location=0) smooth out highp vec3 vtxVar0;\n"
        "layout(location=1) centroid out highp mat3x4 vtxVar1;\n"
        "layout(location=4) flat out highp uvec3 vtxVar2;\n"
        "out highp vec4 var3;\n"
        "const vec2 corners[4] = vec2[4](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(-1.0, 1.0), vec2(1.0, 1.0));\n"
        "void main() {\n"
        "    gl_Position = vec4(corners[gl_VertexID], 0.0, 1.0);\n"
        "    vtxVar0 = vec3(0.25, 0.5, 0.75);\n"
        "    vtxVar1 = mat3x4(0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 1.0, 0.0, 0.5);\n"
        "    vtxVar2 = uvec3(10u, 20u, 30u);\n"
        "    var3 = vec4(0.125, 0.375, 0.625, 0.875);\n"
        "}\n";
    const char* fsSource =
        "#version 310 es\nprecision highp float;\n"
        "layout(location = 0) out vec4 o[4];\n"
        "layout(location=0) in highp vec3 frgVar0;\n"
        "layout(location=1) centroid in highp mat3x4 frgVar1;\n"
        "layout(location=4) flat in highp uvec3 frgVar2;\n"
        "centroid in highp vec4 var3;\n"
        "void main() {\n"
        "    o[0] = vec4(frgVar0, 1.0);\n"
        "    o[1] = vec4(frgVar1[0].x, frgVar1[1].y, frgVar1[2].z, frgVar1[2].w);\n"
        "    o[2] = vec4(vec3(frgVar2) / 255.0, 1.0);\n"
        "    o[3] = var3;\n"
        "}\n";
    GLuint vs = glCreateShaderProgramv(GL_VERTEX_SHADER, 1, &vsSource);
    GLuint fs = glCreateShaderProgramv(GL_FRAGMENT_SHADER, 1, &fsSource);
    GLuint pipeline = 0, fb = 0, textures[4] = {}, vao = 0;
    glGenProgramPipelines(1, &pipeline);
    glUseProgramStages(pipeline, GL_VERTEX_SHADER_BIT, vs);
    glUseProgramStages(pipeline, GL_FRAGMENT_SHADER_BIT, fs);
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glGenTextures(4, textures);
    const GLenum buffers[4] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2, GL_COLOR_ATTACHMENT3};
    for (int i = 0; i < 4; ++i) {
        glBindTexture(GL_TEXTURE_2D, textures[i]);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 8, 8);
        glFramebufferTexture2D(GL_FRAMEBUFFER, buffers[i], GL_TEXTURE_2D, textures[i], 0);
    }
    glDrawBuffers(4, buffers);
    glViewport(0, 0, 8, 8);
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glUseProgram(0);
    glBindProgramPipeline(pipeline);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    CHECK_GL();
    const uint8_t want[4][4] = {{64, 128, 191, 255}, {26, 153, 0, 128}, {10, 20, 30, 255}, {32, 96, 159, 223}};
    for (int i = 0; i < 4; ++i) {
        glReadBuffer(buffers[i]);
        const std::vector<uint8_t> got = readRGBA8(4, 4, 1, 1);
        CHECK(near(got.data(), want[i], 3));
        if (!near(got.data(), want[i], 3)) std::fprintf(stderr, "  output %d: %d %d %d %d\n", i, got[0], got[1], got[2], got[3]);
    }
    glBindProgramPipeline(0);
    // The same stages linked into one program match the same way.
    GLuint whole = program(vsSource, fsSource);
    glUseProgram(whole);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    CHECK_GL();
    for (int i = 0; i < 4; ++i) {
        glReadBuffer(buffers[i]);
        const std::vector<uint8_t> got = readRGBA8(4, 4, 1, 1);
        CHECK(near(got.data(), want[i], 3));
        if (!near(got.data(), want[i], 3))
            std::fprintf(stderr, "  program output %d: %d %d %d %d\n", i, got[0], got[1], got[2], got[3]);
    }
    glUseProgram(0);
    glDeleteProgram(whole);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, 64, 64);
    glDeleteFramebuffers(1, &fb);
    glDeleteTextures(4, textures);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgramPipelines(1, &pipeline);
    glDeleteProgram(vs);
    glDeleteProgram(fs);
}

// Storage blocks declared shared or std140 are active even when unused,
// those ending in an unsized array too.
GPU_TEST_ES(gpu_es_unused_shared_storage_blocks, 31) {
    const char* source =
        "#version 310 es\nlayout(local_size_x = 1) in;\n"
        "layout(column_major, binding = 0) buffer BlockB { mediump uvec2 a; } blockB;\n"
        "layout(shared, binding = 1) buffer BlockC { mediump mat2 b[]; } blockC;\n"
        "layout(std430, binding = 2) buffer Used { highp uint n; } used;\n"
        "void main() { used.n = 1u; }\n";
    GLuint shader = glCreateShader(GL_COMPUTE_SHADER);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLuint prog = glCreateProgram();
    glAttachShader(prog, shader);
    glLinkProgram(prog);
    GLint linked = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    CHECK_EQ(linked, GL_TRUE);
    for (const char* name : {"BlockB", "BlockC", "Used"}) {
        const GLuint index = glGetProgramResourceIndex(prog, GL_SHADER_STORAGE_BLOCK, name);
        CHECK(index != GL_INVALID_INDEX);
        if (index == GL_INVALID_INDEX) std::fprintf(stderr, "  %s is not active\n", name);
    }
    CHECK_GL();
    glDeleteProgram(prog);
    glDeleteShader(shader);
}

// gl_SampleMaskIn holds no sample GL_SAMPLE_MASK removes.
GPU_TEST_ES(gpu_es_sample_mask_in_with_sample_mask, 31) {
    GLuint prog = program("#version 310 es\nlayout(location = 0) in vec2 p;\nvoid main() { gl_Position = vec4(p, 0.0, 1.0); }\n",
                          "#version 310 es\n#extension GL_OES_sample_variables : require\n"
                          "layout(location = 0) out mediump vec4 o;\nuniform highp uint u_mask;\n"
                          "void main() { o = (uint(gl_SampleMaskIn[0]) & ~u_mask) != 0u ? vec4(1.0, 0.0, 0.0, 1.0)\n"
                          "                                                             : vec4(0.0, 1.0, 0.0, 1.0); }\n");
    GLuint vao = quad();
    GLuint ms = 0, single = 0, msFb = 0, singleFb = 0;
    glGenRenderbuffers(1, &ms);
    glBindRenderbuffer(GL_RENDERBUFFER, ms);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, 4, GL_RGBA8, 16, 16);
    glGenFramebuffers(1, &msFb);
    glBindFramebuffer(GL_FRAMEBUFFER, msFb);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, ms);
    glViewport(0, 0, 16, 16);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(prog);
    glUniform1ui(glGetUniformLocation(prog, "u_mask"), 0x5u);
    glEnable(GL_SAMPLE_MASK);
    glSampleMaski(0, 0x5u);
    glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisable(GL_SAMPLE_MASK);
    glSampleMaski(0, ~0u);
    glGenRenderbuffers(1, &single);
    glBindRenderbuffer(GL_RENDERBUFFER, single);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, 16, 16);
    glGenFramebuffers(1, &singleFb);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, singleFb);
    glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, single);
    glBlitFramebuffer(0, 0, 16, 16, 0, 0, 16, 16, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, singleFb);
    // Half the samples drawn green, none red.
    const std::vector<uint8_t> got = readRGBA8(8, 8, 1, 1);
    CHECK(got[0] < 3 && got[1] > 100);
    if (!(got[0] < 3 && got[1] > 100)) std::fprintf(stderr, "  got %d %d %d\n", got[0], got[1], got[2]);
    CHECK_GL();
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, 64, 64);
    glDeleteFramebuffers(1, &msFb);
    glDeleteFramebuffers(1, &singleFb);
    glDeleteRenderbuffers(1, &ms);
    glDeleteRenderbuffers(1, &single);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(prog);
}

// imageAtomicExchange on an r32f image (GL_OES_shader_image_atomic): the
// old values come back and the exchanged values stay.
GPU_TEST_ES(gpu_es_image_atomic_exchange_r32f, 31) {
    const char* source =
        "#version 310 es\n"
        "#extension GL_OES_shader_image_atomic : require\n"
        "layout(local_size_x = 4) in;\n"
        "layout(r32f, binding = 0) coherent uniform highp image2D img;\n"
        "layout(std430, binding = 0) buffer Out { highp float old[4]; };\n"
        "void main() {\n"
        "    old[gl_LocalInvocationID.x] = imageAtomicExchange(img, ivec2(gl_LocalInvocationID.x, 0), 2.5 + float(gl_LocalInvocationID.x));\n"
        "}\n";
    GLuint shader = glCreateShader(GL_COMPUTE_SHADER);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLuint prog = glCreateProgram();
    glAttachShader(prog, shader);
    glLinkProgram(prog);
    GLint linked = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    CHECK_EQ(linked, GL_TRUE);
    GLuint tex = 0, buffer = 0, fb = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_R32F, 4, 1);
    const float initial[4] = {1.0f, -2.0f, 0.5f, 7.0f};
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 4, 1, GL_RED, GL_FLOAT, initial);
    glBindImageTexture(0, tex, 0, GL_FALSE, 0, GL_READ_WRITE, GL_R32F);
    glGenBuffers(1, &buffer);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffer);
    glBufferData(GL_SHADER_STORAGE_BUFFER, 16, nullptr, GL_STREAM_READ);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, buffer);
    glUseProgram(prog);
    glDispatchCompute(1, 1, 1);
    glMemoryBarrier(GL_ALL_BARRIER_BITS);
    CHECK_GL();
    const float* old = static_cast<const float*>(glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, 16, GL_MAP_READ_BIT));
    for (int i = 0; old && i < 4; ++i) {
        CHECK(old[i] == initial[i]);
        if (old[i] != initial[i]) std::fprintf(stderr, "  old[%d] = %g, want %g\n", i, old[i], initial[i]);
    }
    glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    float stored[4] = {};
    glReadPixels(0, 0, 4, 1, GL_RED, GL_FLOAT, stored);
    for (int i = 0; i < 4; ++i) {
        CHECK(stored[i] == 2.5f + static_cast<float>(i));
        if (stored[i] != 2.5f + static_cast<float>(i)) std::fprintf(stderr, "  stored[%d] = %g\n", i, stored[i]);
    }
    CHECK_GL();
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fb);
    glDeleteBuffers(1, &buffer);
    glDeleteTextures(1, &tex);
    glDeleteProgram(prog);
    glDeleteShader(shader);
}

// GL_KHR_texture_compression_astc_sliced_3d: ASTC 3D textures, 2D blocks per
// slice (Metal's 3D ASTC textures).
GPU_TEST_ES(gpu_es_astc_sliced_3d, 32) {
    // ASTC void-extent blocks: one LDR color each (16-bit RGBA after the header).
    auto block = [](uint16_t r, uint16_t g, uint16_t b, uint16_t a) {
        std::vector<uint8_t> v = {0xFC, 0xFD, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        for (uint16_t c : {r, g, b, a}) v.push_back(static_cast<uint8_t>(c & 0xff)), v.push_back(static_cast<uint8_t>(c >> 8));
        return v;
    };
    std::vector<uint8_t> data;
    for (int slice = 0; slice < 2; ++slice)
        for (int i = 0; i < 4; ++i) {
            const std::vector<uint8_t> b = slice == 0 ? block(0xffff, 0, 0, 0xffff) : block(0, 0xffff, 0, 0xffff);
            data.insert(data.end(), b.begin(), b.end());
        }
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_3D, tex);
    glTexStorage3D(GL_TEXTURE_3D, 1, 0x93B0 /* GL_COMPRESSED_RGBA_ASTC_4x4_KHR */, 8, 8, 2);
    CHECK_GL();
    reinterpret_cast<PFN_glCompressedTexSubImage3D>(wmgl_get_proc_address("glCompressedTexSubImage3D"))(
        GL_TEXTURE_3D, 0, 0, 0, 0, 8, 8, 2, 0x93B0, static_cast<GLsizei>(data.size()), data.data());
    CHECK_GL();
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    GLuint prog = program("#version 320 es\nlayout(location = 0) in vec2 p;\nvoid main() { gl_Position = vec4(p, 0.0, 1.0); }\n",
                          "#version 320 es\nprecision highp float;\nuniform highp sampler3D s;\nuniform float z;\nout vec4 o;\n"
                          "void main() { o = texture(s, vec3(0.5, 0.5, z)); }\n");
    GLuint vao = quad();
    glUseProgram(prog);
    for (const float z : {0.25f, 0.75f}) {
        glUniform1f(glGetUniformLocation(prog, "z"), z);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        const uint8_t want[4] = {static_cast<uint8_t>(z < 0.5f ? 255 : 0), static_cast<uint8_t>(z < 0.5f ? 0 : 255), 0, 255};
        const std::vector<uint8_t> got = readRGBA8(32, 32, 1, 1);
        CHECK(near(got.data(), want, 2));
        if (!near(got.data(), want, 2)) std::fprintf(stderr, "  z %g: %d %d %d %d\n", z, got[0], got[1], got[2], got[3]);
    }
    CHECK_GL();
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(prog);
    glDeleteTextures(1, &tex);
}
