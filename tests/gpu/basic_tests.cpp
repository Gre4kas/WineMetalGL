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

GPU_TEST(gpu_array_varying) {
    const char* vs = R"(#version 330 core
in vec2 position;
out vec4 tc[2];
out vec4 color;
void main() { tc[0] = vec4(0.25, 0.5, 0.0, 1.0); tc[1] = vec4(1.0); color = vec4(0.0, 1.0, 0.0, 1.0); gl_Position = vec4(position, 0.0, 1.0); })";
    const char* fs = R"(#version 330 core
in vec4 tc[2];
in vec4 color;
out vec4 o;
void main() { o = vec4(tc[0].x, tc[0].y * tc[1].x, color.g, 1.0); })";
    GLuint p = gpu::program(vs, fs);
    GLuint vao = gpu::quad();
    glUseProgram(p);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    const uint8_t want[4] = {64, 128, 255, 255};
    CHECK(gpu::near(gpu::readRGBA8(32, 32, 1, 1).data(), want));
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
    CHECK_GL();
}

GPU_TEST(gpu_uniform_mat3) {
    const char* fs = R"(#version 330 core
uniform mat3 m;
out vec4 o;
void main() { vec3 n = m * vec3(0.0, 0.0, 1.0); o = vec4(n.x + 0.5, n.y + 0.5, n.z, 1.0); })";
    GLuint p = gpu::program(kQuadVs, fs);
    GLuint vao = gpu::quad();
    glUseProgram(p);
    const float identity[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    glUniformMatrix3fv(glGetUniformLocation(p, "m"), 1, GL_FALSE, identity);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    const uint8_t want[4] = {128, 128, 255, 255};
    CHECK(gpu::near(gpu::readRGBA8(32, 32, 1, 1).data(), want));
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
    CHECK_GL();
}

GPU_TEST(gpu_uniform_mat4_translation) {
    // Column-major translation by (0.5, 0.25): the quad's centre moves right and up.
    const char* vs = R"(#version 330 core
in vec2 position;
uniform mat4 m;
void main() { gl_Position = m * vec4(position * 0.25, 0.0, 1.0); })";
    GLuint p = gpu::program(vs, kColorFs);
    GLuint vao = gpu::quad();
    glUseProgram(p);
    const float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0.5f, 0.25f, 0, 1};
    glUniformMatrix4fv(glGetUniformLocation(p, "m"), 1, GL_FALSE, m);
    glUniform4f(glGetUniformLocation(p, "color"), 1, 1, 1, 1);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    // Quad covers NDC x 0.25..0.75, y 0..0.5 -> pixels x 40..56, y 32..48.
    const uint8_t white[4] = {255, 255, 255, 255}, black[4] = {0, 0, 0, 255};
    CHECK(gpu::near(gpu::readRGBA8(48, 40, 1, 1).data(), white));
    CHECK(gpu::near(gpu::readRGBA8(32, 32, 1, 1).data(), black));
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
    CHECK_GL();
}

GPU_TEST(gpu_attribute_component_defaults) {
    // A vec4 attribute fed from 2 floats reads (x, y, 0, 1).
    const char* vs = R"(#version 330 core
in vec4 position;
out vec4 v;
void main() { v = position; gl_Position = vec4(position.xy, 0.0, 1.0); })";
    const char* fs = R"(#version 330 core
in vec4 v;
out vec4 o;
void main() { o = vec4(v.z, v.w, 0.0, 1.0); })";
    GLuint p = gpu::program(vs, fs);
    GLuint vao = gpu::quad();
    glUseProgram(p);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    const uint8_t want[4] = {0, 255, 0, 255};
    CHECK(gpu::near(gpu::readRGBA8(32, 32, 1, 1).data(), want));
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
    CHECK_GL();
}

GPU_TEST(gpu_attributes_separate_buffers) {
    // Four attributes, each from its own buffer, set up in a different order
    // than their locations (mirrors KHR-GL33.buffer_objects.triangles).
    const char* vs = R"(#version 330 core
in vec3 inNormal;
in vec4 inColor;
in vec4 inVertex;
in vec2 inTex;
uniform mat4 mvp;
out vec4 color;
void main() { color = inColor * (inNormal.z > 0.0 ? 1.0 : 0.5) * (inTex.x >= 0.0 ? 1.0 : 0.0); gl_Position = mvp * inVertex; })";
    const char* fs = R"(#version 330 core
in vec4 color;
out vec4 o;
void main() { o = color; })";
    GLuint p = gpu::program(vs, fs);
    glUseProgram(p);
    const float W = 64, H = 64;
    const float mvp[16] = {2 / W, 0, 0, 0, 0, 2 / H, 0, 0, 0, 0, -1, 0, -1, -1, 0, 1};
    glUniformMatrix4fv(glGetUniformLocation(p, "mvp"), 1, GL_FALSE, mvp);
    const float verts[12] = {W / 4, H / 4, 3 * W / 4, H / 4, 3 * W / 4, 3 * H / 4, W / 4, H / 4, 3 * W / 4, 3 * H / 4, W / 4, 3 * H / 4};
    const float normals[18] = {0, 0, 1, 0, 0, -1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1};
    const float colors[24] = {1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1};
    const float tex[12] = {0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1};
    GLuint vao = 0, buffers[4];
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(4, buffers);
    const void* data[4] = {verts, normals, colors, tex};
    const size_t sizes[4] = {sizeof(verts), sizeof(normals), sizeof(colors), sizeof(tex)};
    for (int i = 0; i < 4; ++i) {
        glBindBuffer(GL_ARRAY_BUFFER, buffers[i]);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(sizes[i]), nullptr, GL_STATIC_DRAW);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(sizes[i]), data[i], GL_STATIC_DRAW);
    }
    const GLint locVertex = glGetAttribLocation(p, "inVertex"), locNormal = glGetAttribLocation(p, "inNormal"),
                locColor = glGetAttribLocation(p, "inColor"), locTex = glGetAttribLocation(p, "inTex");
    for (GLint l : {locVertex, locNormal, locColor, locTex}) glEnableVertexAttribArray(static_cast<GLuint>(l));
    glBindBuffer(GL_ARRAY_BUFFER, buffers[1]);
    glVertexAttribPointer(static_cast<GLuint>(locNormal), 3, GL_FLOAT, GL_FALSE, 0, nullptr);
    glBindBuffer(GL_ARRAY_BUFFER, buffers[2]);
    glVertexAttribPointer(static_cast<GLuint>(locColor), 4, GL_FLOAT, GL_FALSE, 0, nullptr);
    glBindBuffer(GL_ARRAY_BUFFER, buffers[3]);
    glVertexAttribPointer(static_cast<GLuint>(locTex), 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glBindBuffer(GL_ARRAY_BUFFER, buffers[0]);
    glVertexAttribPointer(static_cast<GLuint>(locVertex), 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    const uint8_t green[4] = {0, 255, 0, 255};
    CHECK(gpu::near(gpu::readRGBA8(17, 47, 1, 1).data(), green));   // inside the second triangle
    const uint8_t black[4] = {0, 0, 0, 255};
    CHECK(gpu::near(gpu::readRGBA8(60, 60, 1, 1).data(), black));
    glDeleteBuffers(4, buffers);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
    CHECK_GL();
}
