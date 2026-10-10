// Rasterization rules the Metal defaults differ from: provoking vertex,
// point sprite coordinates, converted vertex formats.
#include "gpu.h"

GPU_TEST(gpu_provoking_vertex) {
    // Flat varyings come from the last vertex by default, the first with FIRST_VERTEX_CONVENTION.
    GLuint p = gpu::program(R"(#version 330 core
in vec2 position; flat out vec4 color; flat out int id;
void main() {
    color = gl_VertexID == 0 ? vec4(1, 0, 0, 1) : gl_VertexID == 1 ? vec4(0, 0, 1, 1) : vec4(0, 1, 0, 1);
    id = gl_VertexID;
    gl_Position = vec4(position, 0.0, 1.0);
})",
                            R"(#version 330 core
flat in vec4 color; flat in int id; out vec4 o; void main() { o = color * (id >= 0 ? 1.0 : 0.0); })");
    static const float tri[] = {-1, -1, 3, -1, -1, 3};
    GLuint vao = 0, vbo = 0;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(tri), tri, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glEnableVertexAttribArray(0);
    glUseProgram(p);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    const uint8_t green[4] = {0, 255, 0, 255}, red[4] = {255, 0, 0, 255};
    CHECK(gpu::near(gpu::readRGBA8(32, 32, 1, 1).data(), green));
    glProvokingVertex(GL_FIRST_VERTEX_CONVENTION);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    CHECK(gpu::near(gpu::readRGBA8(32, 32, 1, 1).data(), red));
    glProvokingVertex(GL_LAST_VERTEX_CONVENTION);
    glDeleteBuffers(1, &vbo);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
    CHECK_GL();
}

GPU_TEST(gpu_point_coord_origin) {
    GLuint p = gpu::program(R"(#version 330 core
in vec2 position; void main() { gl_Position = vec4(0.0, 0.0, 0.0, 1.0); })",
                            R"(#version 330 core
out vec4 o; void main() { o = vec4(gl_PointCoord, 0.0, 1.0); })");
    GLuint vao = gpu::quad();
    glUseProgram(p);
    glPointSize(32.0f);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_POINTS, 0, 1);
    // Default UPPER_LEFT: t grows downwards (GL y decreasing).
    const uint8_t top = gpu::readRGBA8(32, 44, 1, 1)[1], bottom = gpu::readRGBA8(32, 20, 1, 1)[1];
    CHECK(top < 64 && bottom > 192);
    glPointParameteri(GL_POINT_SPRITE_COORD_ORIGIN, GL_LOWER_LEFT);
    glDrawArrays(GL_POINTS, 0, 1);
    const uint8_t top2 = gpu::readRGBA8(32, 44, 1, 1)[1], bottom2 = gpu::readRGBA8(32, 20, 1, 1)[1];
    CHECK(top2 > 192 && bottom2 < 64);
    glPointParameteri(GL_POINT_SPRITE_COORD_ORIGIN, GL_UPPER_LEFT);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
    CHECK_GL();
}

GPU_TEST(gpu_half_float_and_packed_attributes) {
    // Stride 6 is not 4-byte aligned, so the half floats take the conversion path.
    const uint16_t halfs[] = {0xBC00, 0xBC00, 0x0000, 0x4200, 0xBC00, 0x0000, 0xBC00, 0x4200, 0x0000};  // -1, 3
    GLuint p = gpu::program(R"(#version 330 core
in vec3 position; in vec4 wide; out vec4 c;
void main() { c = wide; gl_Position = vec4(position, 1.0); })",
                            R"(#version 330 core
in vec4 c; out vec4 o; void main() { o = c; })");
    GLuint vao = 0, vbo[2] = {};
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(2, vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo[0]);
    glBufferData(GL_ARRAY_BUFFER, sizeof(halfs), halfs, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_HALF_FLOAT, GL_FALSE, 6, nullptr);
    glEnableVertexAttribArray(0);
    // INT_2_10_10_10_REV, not normalized: (1, -1, 511, 1) per vertex, read as a vec4.
    const uint32_t packedValue = 1u | (0x3ffu << 10) | (511u << 20) | (1u << 30);
    const uint32_t packedData[3] = {packedValue, packedValue, packedValue};
    glBindBuffer(GL_ARRAY_BUFFER, vbo[1]);
    glBufferData(GL_ARRAY_BUFFER, sizeof(packedData), packedData, GL_STATIC_DRAW);
    glVertexAttribPointer(1, 4, GL_INT_2_10_10_10_REV, GL_FALSE, 0, nullptr);
    glEnableVertexAttribArray(1);
    glBindAttribLocation(p, 0, "position");
    glBindAttribLocation(p, 1, "wide");
    glLinkProgram(p);
    glUseProgram(p);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    // (1, -1, 511, 1) clamps to (1, 0, 1, 1) in the colour buffer.
    const uint8_t magenta[4] = {255, 0, 255, 255};
    CHECK(gpu::near(gpu::readRGBA8(32, 32, 1, 1).data(), magenta));
    glDeleteBuffers(2, vbo);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
    CHECK_GL();
}
