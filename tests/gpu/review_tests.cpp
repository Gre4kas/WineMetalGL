// Regression tests for the checkpoint review findings (draw path, textures,
// transform feedback ranges, copy image, context attributes).
#include "gpu.h"

#define WMGL_REVIEW_FUNCS(X)                                                                                      \
    X(glDrawElementsInstanced) X(glPrimitiveRestartIndex) X(glCopyImageSubData) X(glGetCompressedTexImage)        \
    X(glCompressedTexImage2D) X(glSampleMaski) X(glSampleCoverage) X(glDeleteRenderbuffers)

#define WMGL_REVIEW_DECLARE(name) static PFN_##name name##_;
WMGL_REVIEW_FUNCS(WMGL_REVIEW_DECLARE)
#undef WMGL_REVIEW_DECLARE

namespace {

void loadReview() {
#define WMGL_REVIEW_LOAD(name) name##_ = reinterpret_cast<PFN_##name>(wmgl_get_proc_address(#name));
    WMGL_REVIEW_FUNCS(WMGL_REVIEW_LOAD)
#undef WMGL_REVIEW_LOAD
}

const uint8_t kRed[4] = {255, 0, 0, 255}, kGreen[4] = {0, 255, 0, 255}, kBlue[4] = {0, 0, 255, 255},
              kWhite[4] = {255, 255, 255, 255};

bool pixelIs(int x, int y, const uint8_t want[4]) {
    const std::vector<uint8_t> got = gpu::readRGBA8(x, y, 1, 1);
    if (gpu::near(got.data(), want, 2)) return true;
    std::printf("  pixel (%d,%d) = %d %d %d %d, want %d %d %d %d\n", x, y, got[0], got[1], got[2], got[3], want[0], want[1],
                want[2], want[3]);
    return false;
}

GLuint arrayBuffer(const void* data, size_t bytes) {
    GLuint vbo = 0;
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(bytes), data, GL_STATIC_DRAW);
    return vbo;
}

GLuint captureProgram(const char* vs, const char* varying) {
    GLuint p = glCreateProgram();
    GLuint s = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(s, 1, &vs, nullptr);
    glCompileShader(s);
    glAttachShader(p, s);
    glBindAttribLocation(p, 0, "position");
    glTransformFeedbackVaryings(p, 1, &varying, GL_INTERLEAVED_ATTRIBS);
    glLinkProgram(p);
    GLint linked = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &linked);
    CHECK(linked);
    glDeleteShader(s);
    return p;
}

}  // namespace

GPU_TEST(gpu_review_restart_instancing_order) {
    // Every instance draws all restart-delimited segments before the next instance.
    loadReview();
    GLuint p = captureProgram(R"(#version 330 core
in vec2 position; out float v;
void main() { v = float(gl_VertexID) + 100.0 * float(gl_InstanceID); gl_Position = vec4(position, 0.0, 1.0); })",
                              "v");
    GLuint vao = gpu::quad();
    const GLuint indices[] = {0, 1, 99, 2, 3};
    GLuint ebo = 0, xfb = 0;
    glGenBuffers(1, &ebo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);
    glGenBuffers(1, &xfb);
    glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, xfb);
    glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, 8 * 4, nullptr, GL_STATIC_READ);
    glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, xfb);
    glUseProgram(p);
    glEnable(GL_PRIMITIVE_RESTART);
    glPrimitiveRestartIndex_(99);
    glEnable(GL_RASTERIZER_DISCARD);
    glBeginTransformFeedback(GL_POINTS);
    glDrawElementsInstanced_(GL_POINTS, 5, GL_UNSIGNED_INT, nullptr, 2);
    glEndTransformFeedback();
    glDisable(GL_RASTERIZER_DISCARD);
    glDisable(GL_PRIMITIVE_RESTART);
    float got[8] = {};
    glGetBufferSubData(GL_TRANSFORM_FEEDBACK_BUFFER, 0, sizeof(got), got);
    const float want[8] = {0, 1, 2, 3, 100, 101, 102, 103};
    bool ok = true;
    for (int i = 0; i < 8; ++i) ok &= got[i] == want[i];
    if (!ok) std::printf("  captured %g %g %g %g %g %g %g %g\n", got[0], got[1], got[2], got[3], got[4], got[5], got[6], got[7]);
    CHECK(ok);
    glDeleteBuffers(1, &ebo);
    glDeleteBuffers(1, &xfb);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
    CHECK_GL();
}

GPU_TEST(gpu_review_converted_attributes_far_first) {
    // A converted attribute (unaligned stride) drawn from a far first vertex,
    // with and without gl_VertexID in the program.
    constexpr int kFirst = 100000;
    std::vector<uint8_t> data(static_cast<size_t>(kFirst + 3) * 10, 0);
    const float tri[6] = {-1, -1, 3, -1, -1, 3};
    for (int v = 0; v < 3; ++v) std::memcpy(&data[static_cast<size_t>(kFirst + v) * 10], &tri[v * 2], 8);
    GLuint vao = 0;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    GLuint vbo = arrayBuffer(data.data(), data.size());
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 10, nullptr);  // stride 10: converted on the CPU
    glEnableVertexAttribArray(0);
    GLuint plain = gpu::program(R"(#version 330 core
in vec2 position; void main() { gl_Position = vec4(position, 0.0, 1.0); })",
                                R"(#version 330 core
out vec4 o; void main() { o = vec4(1, 0, 0, 1); })");
    GLuint withId = gpu::program(R"(#version 330 core
in vec2 position; flat out int id; void main() { id = gl_VertexID; gl_Position = vec4(position, 0.0, 1.0); })",
                                 R"(#version 330 core
flat in int id; out vec4 o; void main() { o = id >= 100000 ? vec4(0, 1, 0, 1) : vec4(0, 0, 1, 1); })");
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(plain);
    glDrawArrays(GL_TRIANGLES, kFirst, 3);
    CHECK(pixelIs(32, 32, kRed));
    glUseProgram(withId);
    glDrawArrays(GL_TRIANGLES, kFirst, 3);
    CHECK(pixelIs(32, 32, kGreen));
    // Indexed, from the same far vertices.
    const GLuint indices[] = {kFirst, kFirst + 1, kFirst + 2};
    GLuint ebo = 0;
    glGenBuffers(1, &ebo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(plain);
    glDrawElements(GL_TRIANGLES, 3, GL_UNSIGNED_INT, nullptr);
    CHECK(pixelIs(32, 32, kRed));
    glDeleteBuffers(1, &ebo);
    glDeleteBuffers(1, &vbo);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(plain);
    glDeleteProgram(withId);
    CHECK_GL();
}

GPU_TEST(gpu_review_fan_provoking_vertex) {
    // Fan triangle i is provoked by vertex i+1 (first convention) or i+2 (last).
    GLuint p = gpu::program(R"(#version 330 core
in vec2 position; flat out vec4 color;
void main() {
    color = gl_VertexID == 0 ? vec4(1, 0, 0, 1) : gl_VertexID == 1 ? vec4(0, 1, 0, 1)
          : gl_VertexID == 2 ? vec4(0, 0, 1, 1) : vec4(1, 1, 1, 1);
    gl_Position = vec4(position, 0.0, 1.0);
})",
                            R"(#version 330 core
flat in vec4 color; out vec4 o; void main() { o = color; })");
    const float fan[] = {-1, -1, 1, -1, 1, 1, -1, 1};
    GLuint vao = 0;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    GLuint vbo = arrayBuffer(fan, sizeof(fan));
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glEnableVertexAttribArray(0);
    glUseProgram(p);
    glProvokingVertex(GL_FIRST_VERTEX_CONVENTION);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    CHECK(pixelIs(48, 16, kGreen));  // triangle (0, 1, 2)
    CHECK(pixelIs(16, 48, kBlue));   // triangle (0, 2, 3)
    glProvokingVertex(GL_LAST_VERTEX_CONVENTION);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    CHECK(pixelIs(48, 16, kBlue));
    CHECK(pixelIs(16, 48, kWhite));
    glDeleteBuffers(1, &vbo);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
    CHECK_GL();
}

GPU_TEST(gpu_review_unsigned_short_index_ffff) {
    // Without primitive restart, index 0xFFFF is an ordinary vertex.
    std::vector<float> positions(65536 * 2, 0.0f);
    positions[0] = -1;
    positions[1] = -1;
    positions[2] = 3;
    positions[3] = -1;
    positions[65535 * 2] = -1;
    positions[65535 * 2 + 1] = 3;
    GLuint vao = 0;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    GLuint vbo = arrayBuffer(positions.data(), positions.size() * 4);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glEnableVertexAttribArray(0);
    const uint16_t indices[] = {0, 1, 0xFFFF};
    GLuint ebo = 0;
    glGenBuffers(1, &ebo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);
    GLuint p = gpu::program(R"(#version 330 core
in vec2 position; void main() { gl_Position = vec4(position, 0.0, 1.0); })",
                            R"(#version 330 core
out vec4 o; void main() { o = vec4(1, 0, 0, 1); })");
    glUseProgram(p);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawElements(GL_TRIANGLES, 3, GL_UNSIGNED_SHORT, nullptr);
    CHECK(pixelIs(32, 32, kRed));
    glDeleteBuffers(1, &ebo);
    glDeleteBuffers(1, &vbo);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
    CHECK_GL();
}

GPU_TEST(gpu_review_rgb_storage_alpha_copy) {
    // A never-written RGB texture copied into RGBA keeps alpha one.
    GLuint rgb = 0, rgba = 0, fbo = 0;
    glGenTextures(1, &rgb);
    glBindTexture(GL_TEXTURE_2D, rgb);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, 4, 4, 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rgb, 0);
    CHECK_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE));
    glGenTextures(1, &rgba);
    glBindTexture(GL_TEXTURE_2D, rgba);
    glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 0, 0, 4, 4, 0);
    uint8_t texels[4 * 4 * 4] = {};
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels);
    bool ok = true;
    for (int i = 0; i < 16; ++i) ok &= texels[i * 4 + 3] == 255;
    CHECK(ok);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &rgb);
    glDeleteTextures(1, &rgba);
    CHECK_GL();
}

GPU_TEST(gpu_review_copy_image_multisample) {
    loadReview();
    GLuint tex[2] = {}, fbo[3] = {}, resolved = 0;
    glGenTextures(2, tex);
    for (GLuint t : tex) {
        glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, t);
        glTexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, 4, GL_RGBA8, 8, 8, GL_TRUE);
    }
    glGenFramebuffers(3, fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo[0]);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D_MULTISAMPLE, tex[0], 0);
    glClearColor(1, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glCopyImageSubData_(tex[0], GL_TEXTURE_2D_MULTISAMPLE, 0, 0, 0, 0, tex[1], GL_TEXTURE_2D_MULTISAMPLE, 0, 0, 0, 0, 8, 8, 1);
    CHECK_GL();
    glGenTextures(1, &resolved);
    glBindTexture(GL_TEXTURE_2D, resolved);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo[1]);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D_MULTISAMPLE, tex[1], 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo[2]);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, resolved, 0);
    glBlitFramebuffer(0, 0, 8, 8, 0, 0, 8, 8, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo[2]);
    CHECK(pixelIs(4, 4, kRed));
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(3, fbo);
    glDeleteTextures(2, tex);
    glDeleteTextures(1, &resolved);
    CHECK_GL();
}

GPU_TEST(gpu_review_copy_image_compressed_mip_tail) {
    // Copying a 2x2 compressed level between formats that go through the
    // staging buffer (RGTC1 unsigned to signed) copies the one partial block.
    loadReview();
    uint8_t block[8] = {0x80, 0x10, 0x24, 0x92, 0x49, 0x24, 0x92, 0x49};
    GLuint tex[2] = {};
    glGenTextures(2, tex);
    const GLenum formats[2] = {GL_COMPRESSED_RED_RGTC1, GL_COMPRESSED_SIGNED_RED_RGTC1};
    for (int i = 0; i < 2; ++i) {
        glBindTexture(GL_TEXTURE_2D, tex[i]);
        glCompressedTexImage2D_(GL_TEXTURE_2D, 0, formats[i], 4, 4, 0, 8, block);
        glCompressedTexImage2D_(GL_TEXTURE_2D, 1, formats[i], 2, 2, 0, 8, i == 0 ? block : nullptr);
    }
    glCopyImageSubData_(tex[0], GL_TEXTURE_2D, 1, 0, 0, 0, tex[1], GL_TEXTURE_2D, 1, 0, 0, 0, 2, 2, 1);
    CHECK_GL();
    uint8_t got[8] = {};
    glBindTexture(GL_TEXTURE_2D, tex[1]);
    glGetCompressedTexImage_(GL_TEXTURE_2D, 1, got);
    CHECK(std::memcmp(got, block, 8) == 0);
    glDeleteTextures(2, tex);
    CHECK_GL();
}

GPU_TEST(gpu_review_transform_feedback_range_clamped) {
    // A bound range larger than the buffer's (new) size captures only what fits.
    GLuint p = captureProgram(R"(#version 330 core
in vec2 position; out vec4 v; void main() { v = vec4(float(gl_VertexID)); gl_Position = vec4(position, 0.0, 1.0); })",
                              "v");
    std::vector<float> points(100 * 2, 0.0f);
    GLuint vao = 0;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    GLuint vbo = arrayBuffer(points.data(), points.size() * 4);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glEnableVertexAttribArray(0);
    GLuint xfb = 0, q = 0;
    glGenBuffers(1, &xfb);
    glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, xfb);
    glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, 4096, nullptr, GL_STATIC_READ);
    glBindBufferRange(GL_TRANSFORM_FEEDBACK_BUFFER, 0, xfb, 0, 4096);
    glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, 256, nullptr, GL_STATIC_READ);  // 16 vec4s
    glUseProgram(p);
    glGenQueries(1, &q);
    glEnable(GL_RASTERIZER_DISCARD);
    glBeginQuery(GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN, q);
    glBeginTransformFeedback(GL_POINTS);
    glDrawArrays(GL_POINTS, 0, 100);
    glEndTransformFeedback();
    glEndQuery(GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN);
    glDisable(GL_RASTERIZER_DISCARD);
    GLuint written = 0;
    glGetQueryObjectuiv(q, GL_QUERY_RESULT, &written);
    CHECK_EQ(written, 16u);
    float last[4] = {};
    glGetBufferSubData(GL_TRANSFORM_FEEDBACK_BUFFER, 240, 16, last);
    CHECK(last[0] == 15.0f);
    glDeleteQueries(1, &q);
    glDeleteBuffers(1, &xfb);
    glDeleteBuffers(1, &vbo);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
    CHECK_GL();
}

GPU_TEST(gpu_review_context_attributes) {
    // WGL_ARB_create_context: profile masks and flags.
    auto create = [](const std::vector<int>& attribs, int& error) {
        error = -1;
        wmgl_context* c = wmgl_context_create(1, nullptr, attribs.data(), &error);
        if (c) wmgl_context_destroy(c);
        return c != nullptr;
    };
    int error = 0;
    CHECK(!create({0x2091, 3, 0x2092, 3, 0x9126, 3, 0}, error));  // core | compatibility
    CHECK_EQ(error, WMGL_ERROR_INVALID_PROFILE);
    CHECK(!create({0x2091, 3, 0x2092, 3, 0x2094, 0x10, 0}, error));  // unknown flag
    CHECK_EQ(error, WMGL_ERROR_INVALID_VERSION);
    CHECK(!create({0x2091, 2, 0x2092, 1, 0x2094, 2, 0}, error));  // forward compatible before 3.0
    CHECK_EQ(error, WMGL_ERROR_INVALID_VERSION);
    CHECK(create({0x2091, 2, 0x2092, 1, 0x9126, 0, 0}, error));  // the mask is ignored before 3.2
    CHECK(create({0x2091, 3, 0x2092, 3, 0x9126, 1, 0x2094, 0x5, 0}, error));  // debug | robust
}

GPU_TEST(gpu_review_sample_mask_and_coverage) {
    // Two of four samples (GL_SAMPLE_MASK), then one (GL_SAMPLE_COVERAGE 0.25):
    // the resolved red is 50% and 25%.
    loadReview();
    GLuint rb = 0, fbo[2] = {}, resolved = 0;
    glGenRenderbuffers(1, &rb);
    glBindRenderbuffer(GL_RENDERBUFFER, rb);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, 4, GL_RGBA8, 8, 8);
    glGenFramebuffers(2, fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo[0]);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
    glGenTextures(1, &resolved);
    glBindTexture(GL_TEXTURE_2D, resolved);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo[1]);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, resolved, 0);
    GLuint p = gpu::program(R"(#version 330 core
in vec2 position; void main() { gl_Position = vec4(position, 0.0, 1.0); })",
                            R"(#version 330 core
out vec4 o; void main() { o = vec4(1, 0, 0, 1); })");
    GLuint vao = gpu::quad();
    glUseProgram(p);
    glViewport(0, 0, 8, 8);
    auto drawAndResolve = [&]() {
        glBindFramebuffer(GL_FRAMEBUFFER, fbo[0]);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo[0]);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo[1]);
        glBlitFramebuffer(0, 0, 8, 8, 0, 0, 8, 8, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo[1]);
        return gpu::readRGBA8(4, 4, 1, 1)[0];
    };
    glEnable(GL_SAMPLE_MASK);
    glSampleMaski_(0, 0x3);
    const int half = drawAndResolve();
    glDisable(GL_SAMPLE_MASK);
    glEnable(GL_SAMPLE_COVERAGE);
    glSampleCoverage_(0.25f, GL_FALSE);
    const int quarter = drawAndResolve();
    glSampleCoverage_(0.25f, GL_TRUE);
    const int threeQuarters = drawAndResolve();
    glDisable(GL_SAMPLE_COVERAGE);
    const int full = drawAndResolve();
    std::printf("  resolved red: mask 0x3 %d, coverage 0.25 %d, inverted %d, off %d\n", half, quarter, threeQuarters, full);
    CHECK(half > 110 && half < 145);
    CHECK(quarter > 50 && quarter < 80);
    CHECK(threeQuarters > 175 && threeQuarters < 205);
    CHECK(full == 255);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, 64, 64);
    glDeleteFramebuffers(2, fbo);
    glDeleteRenderbuffers_(1, &rb);
    glDeleteTextures(1, &resolved);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
    CHECK_GL();
}
