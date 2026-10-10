// Clears, draws and read-back on the default framebuffer.
#include <cstring>

#include "gpu.h"

namespace {

const char* kQuadVs = R"(#version 330 core
in vec2 position;
void main() { gl_Position = vec4(position, 0.0, 1.0); })";

const char* kColorFs = R"(#version 330 core
uniform vec4 color;
out vec4 fragColor;
void main() { fragColor = color; })";

}  // namespace

GPU_TEST(gpu_version_string) {
    const char* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    CHECK(version && std::strncmp(version, "3.3", 3) == 0);
    CHECK_GL();
}

GPU_TEST(gpu_clear_readback) {
    glClearColor(0.25f, 0.5f, 0.75f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    const auto pixels = gpu::readRGBA8(10, 10, 1, 1);
    const uint8_t want[4] = {64, 128, 191, 255};
    CHECK(gpu::near(pixels.data(), want));
    CHECK_GL();
}

GPU_TEST(gpu_scissored_clear) {
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 0, 16, 16);
    glClearColor(1, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
    const auto inside = gpu::readRGBA8(4, 4, 1, 1), outside = gpu::readRGBA8(40, 40, 1, 1);
    const uint8_t red[4] = {255, 0, 0, 255}, black[4] = {0, 0, 0, 255};
    CHECK(gpu::near(inside.data(), red));
    CHECK(gpu::near(outside.data(), black));
    CHECK_GL();
}

GPU_TEST(gpu_quad_draw) {
    GLuint p = gpu::program(kQuadVs, kColorFs);
    GLuint vao = gpu::quad();
    glUseProgram(p);
    glUniform4f(glGetUniformLocation(p, "color"), 0, 1, 0, 1);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    const auto pixels = gpu::readRGBA8(32, 32, 1, 1);
    const uint8_t green[4] = {0, 255, 0, 255};
    CHECK(gpu::near(pixels.data(), green));
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
    CHECK_GL();
}

// GL row 0 is the bottom row: a quad covering the lower half lands in rows 0..31.
GPU_TEST(gpu_orientation) {
    const char* vs = R"(#version 330 core
in vec2 position;
void main() { gl_Position = vec4(position.x, position.y * 0.5 - 0.5, 0.0, 1.0); })";
    GLuint p = gpu::program(vs, kColorFs);
    GLuint vao = gpu::quad();
    glUseProgram(p);
    glUniform4f(glGetUniformLocation(p, "color"), 1, 1, 1, 1);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    const auto bottom = gpu::readRGBA8(32, 8, 1, 1), top = gpu::readRGBA8(32, 56, 1, 1);
    const uint8_t white[4] = {255, 255, 255, 255}, black[4] = {0, 0, 0, 255};
    CHECK(gpu::near(bottom.data(), white));
    CHECK(gpu::near(top.data(), black));
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
    CHECK_GL();
}

GPU_TEST(gpu_packed_generic_attribute) {
    // x = 511 (max positive 10-bit), w = 1 (2-bit, normalized to 1/1 for unsigned).
    glVertexAttribP4ui(3, GL_UNSIGNED_INT_2_10_10_10_REV, GL_TRUE, 1023u | (1u << 30));
    GLfloat v[4] = {};
    glGetVertexAttribfv(3, GL_CURRENT_VERTEX_ATTRIB, v);
    CHECK(v[0] == 1.0f && v[1] == 0.0f && v[2] == 0.0f && v[3] > 0.33f && v[3] < 0.34f);
    glVertexAttribP4ui(3, GL_FLOAT, GL_TRUE, 0);
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_ENUM));
}
