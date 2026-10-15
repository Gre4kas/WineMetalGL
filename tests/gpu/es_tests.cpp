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
