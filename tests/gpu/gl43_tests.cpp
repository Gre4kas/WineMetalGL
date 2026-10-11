// GL 4.3 features: shader storage blocks and compute shaders.
#include <string>

#include "gpu.h"

namespace {

GLuint computeProgram(const char* cs) {
    GLuint p = glCreateProgram();
    GLuint s = glCreateShader(GL_COMPUTE_SHADER);
    glShaderSource(s, 1, &cs, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048] = {};
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        std::fprintf(stderr, "  compile failed:\n%s\n", log);
    }
    CHECK(ok);
    glAttachShader(p, s);
    glDeleteShader(s);
    glLinkProgram(p);
    GLint linked = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[4096] = {};
        glGetProgramInfoLog(p, sizeof(log), nullptr, log);
        std::fprintf(stderr, "  link failed:\n%s\n", log);
    }
    CHECK(linked);
    return p;
}

std::vector<uint32_t> readBuffer(GLuint buffer, size_t words) {
    std::vector<uint32_t> out(words, 0xdeadbeefu);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffer);
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, static_cast<GLsizeiptr>(words * 4), out.data());
    return out;
}

GLuint storageBuffer(GLuint binding, const std::vector<uint32_t>& data) {
    GLuint b = 0;
    glGenBuffers(1, &b);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, binding, b);
    glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(data.size() * 4), data.data(), GL_DYNAMIC_DRAW);
    return b;
}

}  // namespace

// Compute writes through two storage blocks with atomics, with uniforms
// (KHR-GL43.shader_storage_buffer_object.basic-atomic-case1-cs).
GPU_TEST_VERSION(gpu_gl43_ssbo_compute_atomics, 43) {
    const char* cs = R"(#version 430 core
layout(local_size_x = 4) in;
layout(std430, binding = 2) coherent buffer U { uint data[4]; } u;
layout(std430, binding = 3) coherent buffer I { int data[4]; } i;
uniform uint values[8];
void main() {
    uint k = gl_LocalInvocationIndex;
    if (atomicExchange(u.data[k], values[1]) != 0u) return;
    if (atomicAdd(u.data[k], values[2]) != 1u) return;
    atomicMax(u.data[k], values[7]);
    atomicAdd(i.data[k], int(k) - 5);
})";
    GLuint p = computeProgram(cs);
    GLuint u = storageBuffer(2, {0, 0, 0, 0});
    GLuint i = storageBuffer(3, {0, 0, 0, 0});
    glUseProgram(p);
    const GLuint values[8] = {3, 1, 2, 1, 3, 1, 2, 7};
    glUniform1uiv(glGetUniformLocation(p, "values[0]"), 8, values);
    glDispatchCompute(1, 1, 1);
    glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
    const std::vector<uint32_t> got = readBuffer(u, 4), gotInt = readBuffer(i, 4);
    std::fprintf(stderr, "  u = %u %u %u %u, i = %d %d %d %d\n", got[0], got[1], got[2], got[3], static_cast<int32_t>(gotInt[0]),
                 static_cast<int32_t>(gotInt[1]), static_cast<int32_t>(gotInt[2]), static_cast<int32_t>(gotInt[3]));
    for (int k = 0; k < 4; ++k) {
        CHECK_EQ(got[k], 7u);
        CHECK_EQ(static_cast<int32_t>(gotInt[k]), k - 5);
    }
    CHECK_GL();
    glDeleteBuffers(1, &u);
    glDeleteBuffers(1, &i);
    glDeleteProgram(p);
}

// The vertex stage writes a storage block that the same draw's fragment
// stage reads (KHR-GL43.shader_storage_buffer_object.advanced-matrix).
GPU_TEST_VERSION(gpu_gl43_ssbo_vertex_write_fragment_read, 43) {
    const char* vs = R"(#version 430 core
in vec2 position;
layout(std430, binding = 0) coherent buffer B { vec4 color; float flag; };
void main() {
    if (gl_VertexID == 0) { color = vec4(0.0, 1.0, 0.0, 1.0); flag = 1.0; }
    memoryBarrier();
    gl_Position = vec4(position, 0.0, 1.0);
})";
    const char* fs = R"(#version 430 core
layout(std430, binding = 0) coherent buffer B { vec4 color; float flag; };
out vec4 o;
void main() { o = flag == 1.0 ? color : vec4(1.0, 0.0, 0.0, 1.0); })";
    GLuint p = gpu::program(vs, fs);
    GLuint b = storageBuffer(0, std::vector<uint32_t>(8, 0));
    GLuint vao = gpu::quad();
    glUseProgram(p);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    const std::vector<uint32_t> words = readBuffer(b, 5);
    float flag;
    std::memcpy(&flag, &words[4], 4);
    CHECK(flag == 1.0f);
    const uint8_t green[4] = {0, 255, 0, 255};
    const std::vector<uint8_t> px = gpu::readRGBA8(32, 32, 1, 1);
    std::fprintf(stderr, "  flag %g, pixel %u %u %u %u\n", flag, px[0], px[1], px[2], px[3]);
    CHECK(gpu::near(px.data(), green));
    CHECK_GL();
    glDeleteVertexArrays(1, &vao);
    glDeleteBuffers(1, &b);
    glDeleteProgram(p);
}

// Matrices in a std430 block, written per element with dynamic indices
// (KHR-GL43.shader_storage_buffer_object.advanced-matrix).
GPU_TEST_VERSION(gpu_gl43_ssbo_matrix_elements, 43) {
    const char* vs = R"(#version 430 core
in vec2 position;
layout(binding = 0, std430) coherent buffer Buffer0 { mat3x4 g_transform[4]; mat4x3 g_color; mat3 g_data0; };
layout(binding = 1, std430) readonly buffer Buffer1 { mat4 color; } g_buffer1;
uniform int g_index1 = 1;
uniform int g_index2 = 2;
void main() {
    gl_Position = vec4(position, 0.0, 1.0);
    g_color[0] = g_buffer1.color[1].rgb;
    if (gl_VertexID == 0) { g_data0[1][1] = 1.0; g_data0[g_index1][g_index2] += 3.0; }
})";
    const char* fs = R"(#version 430 core
out vec4 o;
void main() { o = vec4(1.0); })";
    GLuint p = gpu::program(vs, fs);
    std::vector<uint32_t> zero(76, 0);
    GLuint b0 = storageBuffer(0, zero);
    std::vector<uint32_t> colors(16, 0);
    for (int i = 0; i < 16; ++i) {
        const float v = static_cast<float>(i + 1);
        std::memcpy(&colors[static_cast<size_t>(i)], &v, 4);
    }
    GLuint b1 = storageBuffer(1, colors);
    GLuint vao = gpu::quad();
    glUseProgram(p);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    const std::vector<uint32_t> words = readBuffer(b0, 76);
    float f[76];
    std::memcpy(f, words.data(), sizeof(f));
    // g_color column 0 at byte 192 (vec3 columns, 16-byte stride); g_data0 at 256.
    CHECK(f[48] == 5.0f && f[49] == 6.0f && f[50] == 7.0f);
    CHECK(f[64 + 4 + 1] == 1.0f);  // g_data0[1][1]
    CHECK(f[64 + 4 + 2] == 3.0f);  // g_data0[1][2]
    std::fprintf(stderr, "  g_color[0] %g %g %g, g_data0[1] %g %g %g\n", f[48], f[49], f[50], f[68], f[69], f[70]);
    CHECK_GL();
    glDeleteVertexArrays(1, &vao);
    glDeleteBuffers(1, &b0);
    glDeleteBuffers(1, &b1);
    glDeleteProgram(p);
}
