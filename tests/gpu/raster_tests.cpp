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

GPU_TEST(gpu_base_instance) {
    // The base instance offsets instanced attributes but not gl_InstanceID.
    GLuint p = gpu::program(R"(#version 330 core
in vec2 position; in vec4 color; flat out vec4 c;
void main() { c = gl_InstanceID == 0 ? color : vec4(1.0, 0.0, 1.0, 1.0); gl_Position = vec4(position, 0.0, 1.0); })",
                            R"(#version 330 core
flat in vec4 c; out vec4 o; void main() { o = c; })");
    glBindAttribLocation(p, 1, "color");
    glLinkProgram(p);
    glUseProgram(p);
    GLuint vao = gpu::quad();
    const float colors[] = {1, 0, 0, 1, 0, 1, 0, 1};
    GLuint vbo = 0;
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(colors), colors, GL_STATIC_DRAW);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 0, nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribDivisor(1, 1);
    glDrawArraysInstancedBaseInstance(GL_TRIANGLE_STRIP, 0, 4, 1, 1);
    const uint8_t green[4] = {0, 255, 0, 255};
    CHECK(gpu::near(gpu::readRGBA8(32, 32, 1, 1).data(), green));
    glDeleteBuffers(1, &vbo);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
    CHECK_GL();
}

GPU_TEST(gpu_draw_buffers_blend) {
    // Blend state per draw buffer: buffer 0 adds, buffer 1 replaces.
    GLuint p = gpu::program("#version 330 core\nin vec2 position; void main() { gl_Position = vec4(position, 0.0, 1.0); }",
                            "#version 330 core\nlayout(location = 0) out vec4 a; layout(location = 1) out vec4 b;\n"
                            "void main() { a = vec4(0.25); b = vec4(0.25); }");
    GLuint tex[2], fbo;
    glGenTextures(2, tex);
    for (int i = 0; i < 2; ++i) {
        glBindTexture(GL_TEXTURE_2D, tex[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    }
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex[0], 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, tex[1], 0);
    const GLenum bufs[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
    glDrawBuffers(2, bufs);
    glViewport(0, 0, 4, 4);
    glClearColor(0.5f, 0.5f, 0.5f, 0.5f);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_BLEND);
    glBlendFunci(0, GL_ONE, GL_ONE);
    glBlendFunci(1, GL_ONE, GL_ZERO);
    GLuint vao = gpu::quad();
    glUseProgram(p);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    const uint8_t sum[4] = {191, 191, 191, 191}, replaced[4] = {64, 64, 64, 64};
    CHECK(gpu::near(gpu::readRGBA8(1, 1, 1, 1).data(), sum, 2));
    glReadBuffer(GL_COLOR_ATTACHMENT1);
    CHECK(gpu::near(gpu::readRGBA8(1, 1, 1, 1).data(), replaced, 2));
    glDisable(GL_BLEND);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(2, tex);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
    CHECK_GL();
}

GPU_TEST(gpu_clip_control) {
    // A quad covering the top half (NDC y > 0) at depth z = 0.5.
    GLuint p = gpu::program("#version 330 core\nin vec2 position;\n"
                            "void main() { gl_Position = vec4(position.x, position.y * 0.5 + 0.5, 0.5, 1.0); }",
                            "#version 330 core\nout vec4 o; void main() { o = vec4(1.0); }");
    GLuint vao = gpu::quad();
    glUseProgram(p);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_ALWAYS);
    for (int upper = 0; upper < 2; ++upper) {
        glClipControl(upper ? GL_UPPER_LEFT : GL_LOWER_LEFT, GL_ZERO_TO_ONE);
        glClearColor(0, 0, 0, 1);
        glClearDepth(1.0);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        const uint8_t top = gpu::readRGBA8(32, 56, 1, 1)[0], bottom = gpu::readRGBA8(32, 8, 1, 1)[0];
        CHECK(upper ? (top == 0 && bottom == 255) : (top == 255 && bottom == 0));
        float depth = 0;
        glReadPixels(32, upper ? 8 : 56, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &depth);
        CHECK(depth > 0.49f && depth < 0.51f);  // ZERO_TO_ONE: z = 0.5 is depth 0.5
    }
    GLint origin = 0, mode = 0;
    glGetIntegerv(GL_CLIP_ORIGIN, &origin);
    glGetIntegerv(GL_CLIP_DEPTH_MODE, &mode);
    CHECK(origin == GL_UPPER_LEFT && mode == GL_ZERO_TO_ONE);
    glClipControl(GL_LOWER_LEFT, GL_NEGATIVE_ONE_TO_ONE);
    glDisable(GL_DEPTH_TEST);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
    CHECK_GL();
}
