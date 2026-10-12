// GL 4.3 features.
#include <cmath>
#include <cstdlib>
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

// GL 4.3 vertex attribute bindings: two attributes formatted at relative
// offsets in one binding, a divisor on another binding.
GPU_TEST_VERSION(gpu_gl43_vertex_attrib_binding, 43) {
    const char* vs = R"(#version 430 core
layout(location = 0) in vec2 position;
layout(location = 1) in vec4 color;
layout(location = 2) in float scale;
out vec4 c;
void main() { c = color * scale; gl_Position = vec4(position, 0.0, 1.0); })";
    const char* fs = R"(#version 430 core
in vec4 c;
out vec4 o;
void main() { o = c; })";
    GLuint p = gpu::program(vs, fs);
    // Per vertex: position (2 floats) then colour (4 unsigned bytes); 12 bytes.
    struct Vertex { float x, y; uint8_t r, g, b, a; };
    const Vertex vertices[4] = {{-1, -1, 0, 255, 0, 255}, {1, -1, 0, 255, 0, 255}, {-1, 1, 0, 255, 0, 255}, {1, 1, 0, 255, 0, 255}};
    const float scales[1] = {1.0f};
    GLuint buffers[2], vao;
    glGenBuffers(2, buffers);
    glBindBuffer(GL_ARRAY_BUFFER, buffers[0]);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, buffers[1]);
    glBufferData(GL_ARRAY_BUFFER, sizeof(scales), scales, GL_STATIC_DRAW);
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glVertexAttribFormat(0, 2, GL_FLOAT, GL_FALSE, 0);
    glVertexAttribFormat(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, 8);
    glVertexAttribFormat(2, 1, GL_FLOAT, GL_FALSE, 0);
    glVertexAttribBinding(0, 3);
    glVertexAttribBinding(1, 3);
    glVertexAttribBinding(2, 7);
    glBindVertexBuffer(3, buffers[0], 0, sizeof(Vertex));
    glBindVertexBuffer(7, buffers[1], 0, 4);
    glVertexBindingDivisor(7, 1);
    for (GLuint i = 0; i < 3; ++i) glEnableVertexAttribArray(i);
    GLint binding = -1, stride = -1;
    glGetVertexAttribiv(1, GL_VERTEX_ATTRIB_BINDING, &binding);
    glGetIntegeri_v(GL_VERTEX_BINDING_STRIDE, 3, &stride);
    CHECK_EQ(binding, 3);
    CHECK_EQ(stride, static_cast<GLint>(sizeof(Vertex)));
    glUseProgram(p);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, 1);
    const uint8_t green[4] = {0, 255, 0, 255};
    const std::vector<uint8_t> px = gpu::readRGBA8(32, 32, 1, 1);
    CHECK(gpu::near(px.data(), green));
    CHECK_GL();
    glDeleteVertexArrays(1, &vao);
    glDeleteBuffers(2, buffers);
    glDeleteProgram(p);
}

// glTexBufferRange: a buffer texture over part of a buffer, read by a
// compute shader (KHR-GL43.shader_image_size.advanced-nonMS-*).
GPU_TEST_VERSION(gpu_gl43_texture_buffer_range, 43) {
    const char* cs = R"(#version 430 core
layout(local_size_x = 1) in;
uniform usamplerBuffer t;
layout(std430, binding = 0) buffer Out { uint size; uint first; } o;
void main() { o.size = uint(textureSize(t)); o.first = texelFetch(t, 0).x; })";
    GLuint p = computeProgram(cs);
    std::vector<uint32_t> words(256);
    for (size_t i = 0; i < words.size(); ++i) words[i] = static_cast<uint32_t>(i);
    GLuint data = 0, tex = 0;
    glGenBuffers(1, &data);
    glBindBuffer(GL_TEXTURE_BUFFER, data);
    glBufferData(GL_TEXTURE_BUFFER, static_cast<GLsizeiptr>(words.size() * 4), words.data(), GL_STATIC_DRAW);
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_BUFFER, tex);
    GLint alignment = 0;
    glGetIntegerv(GL_TEXTURE_BUFFER_OFFSET_ALIGNMENT, &alignment);
    CHECK(alignment > 0 && alignment <= 256);
    glTexBufferRange(GL_TEXTURE_BUFFER, GL_R32UI, data, 256, 128);  // texels 64..95
    CHECK_GL();
    glTexBufferRange(GL_TEXTURE_BUFFER, GL_R32UI, data, 4, 128);  // misaligned
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_VALUE));
    GLuint out = storageBuffer(0, {0, 0});
    glUseProgram(p);
    glDispatchCompute(1, 1, 1);
    glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
    const std::vector<uint32_t> got = readBuffer(out, 2);
    CHECK_EQ(got[0], 32u);
    CHECK_EQ(got[1], 64u);
    CHECK_GL();
    glDeleteTextures(1, &tex);
    GLuint buffers[2] = {data, out};
    glDeleteBuffers(2, buffers);
    glDeleteProgram(p);
}

// Dispatches inside a failing conditional render block are skipped
// (KHR-GL43.compute_shader.conditional-dispatching).
GPU_TEST_VERSION(gpu_gl43_conditional_dispatch, 43) {
    GLuint p = computeProgram(R"(#version 430 core
layout(local_size_x = 1) in;
layout(std430, binding = 0) buffer Out { uint count; };
void main() { atomicAdd(count, 1u); })");
    GLuint out = storageBuffer(0, {0});
    GLuint query = 0;
    glGenQueries(1, &query);
    glBeginQuery(GL_ANY_SAMPLES_PASSED, query);
    glEndQuery(GL_ANY_SAMPLES_PASSED);  // nothing drawn: no samples passed
    glUseProgram(p);
    glBeginConditionalRender(query, GL_QUERY_WAIT);
    glDispatchCompute(1, 1, 1);
    glEndConditionalRender();
    glDispatchCompute(1, 1, 1);
    glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
    CHECK_EQ(readBuffer(out, 1)[0], 1u);
    CHECK_GL();
    glDeleteQueries(1, &query);
    glDeleteBuffers(1, &out);
    glDeleteProgram(p);
}

// A 1024-invocation work group copying arrays inside a shared struct
// (SPIRV-Cross spvUnsafeArray members; KHR-GL43.compute_shader.shared-max).
GPU_TEST_VERSION(gpu_gl43_shared_struct_arrays, 43) {
    GLuint p = computeProgram(R"(#version 430 core
layout(local_size_x = 1024) in;
shared struct Type { vec4 v[2]; } s[1024];
layout(std430, binding = 0) buffer Out { Type o[1024]; };
void main() {
    const int id = int(gl_LocalInvocationIndex);
    s[id].v = vec4[2](vec4(1.0), vec4(2.0));
    memoryBarrierShared();
    barrier();
    o[1023 - id] = s[1023 - id];
})");
    GLuint out = storageBuffer(0, std::vector<uint32_t>(1024 * 8, 0));
    glUseProgram(p);
    glDispatchCompute(1, 1, 1);
    glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
    const std::vector<uint32_t> got = readBuffer(out, 1024 * 8);
    float first, last;
    std::memcpy(&first, &got[0], 4);
    std::memcpy(&last, &got[1023 * 8 + 4], 4);
    CHECK(first == 1.0f);
    CHECK(last == 2.0f);
    CHECK_GL();
    glDeleteBuffers(1, &out);
    glDeleteProgram(p);
}

// A texture view samples its own level range in every stage, the emulated
// geometry stage included (KHR-GL43.texture_view.view_sampling).
// A texture view samples its own level range in every stage, the emulated
// geometry stage included, through a sampler named `texture` (a GLSL
// built-in function name; KHR-GL43.texture_view.view_sampling).
GPU_TEST_VERSION(gpu_gl43_texture_view_levels, 43) {
    GLuint parent = 0, view = 0;
    glGenTextures(1, &parent);
    glBindTexture(GL_TEXTURE_2D, parent);
    glTexStorage2D(GL_TEXTURE_2D, 3, GL_RGBA8, 4, 4);
    const uint8_t colors[3][4] = {{255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}};
    for (int level = 0; level < 3; ++level) {
        const int size = 4 >> level;
        std::vector<uint8_t> texels(static_cast<size_t>(size * size * 4));
        for (size_t i = 0; i < texels.size(); ++i) texels[i] = colors[level][i % 4];
        glTexSubImage2D(GL_TEXTURE_2D, level, 0, 0, size, size, GL_RGBA, GL_UNSIGNED_BYTE, texels.data());
    }
    glGenTextures(1, &view);
    glTextureView(view, GL_TEXTURE_2D, parent, GL_RGBA8, 1, 2, 0, 1);
    CHECK_GL();
    GLint levels = 0;
    glBindTexture(GL_TEXTURE_2D, view);
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_VIEW_NUM_LEVELS, &levels);
    CHECK_EQ(levels, 2);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
    const char* vs = R"(#version 430 core
in vec2 position;
uniform sampler2D texture;
out vec4 vc;
void main() { vc = textureLod(texture, vec2(0.5), 1.0); gl_Position = vec4(position, 0.0, 1.0); })";
    const char* gs = R"(#version 430 core
layout(triangles) in;
layout(triangle_strip, max_vertices = 3) out;
uniform sampler2D texture;
in vec4 vc[];
out vec4 c;
void main() {
    vec4 g = textureLod(texture, vec2(0.5), 1.0);
    for (int i = 0; i < 3; ++i) { c = g == vc[i] ? g : vec4(1.0); gl_Position = gl_in[i].gl_Position; EmitVertex(); }
})";
    const char* fs = R"(#version 430 core
in vec4 c;
out vec4 o;
void main() { o = c; })";
    GLuint p = glCreateProgram();
    for (auto [type, src] : {std::pair<GLenum, const char*>{GL_VERTEX_SHADER, vs}, {GL_GEOMETRY_SHADER, gs}, {GL_FRAGMENT_SHADER, fs}}) {
        GLuint sh = glCreateShader(type);
        glShaderSource(sh, 1, &src, nullptr);
        glCompileShader(sh);
        glAttachShader(p, sh);
        glDeleteShader(sh);
    }
    glBindAttribLocation(p, 0, "position");
    glLinkProgram(p);
    GLint linked = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &linked);
    CHECK(linked);
    GLuint vao = gpu::quad();
    glUseProgram(p);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    const std::vector<uint8_t> px = gpu::readRGBA8(32, 32, 1, 1);
    std::fprintf(stderr, "  sampled %u %u %u %u\n", px[0], px[1], px[2], px[3]);
    CHECK(gpu::near(px.data(), colors[2]));  // view level 1 = parent level 2
    CHECK_GL();
    glDeleteVertexArrays(1, &vao);
    GLuint textures[2] = {view, parent};
    glDeleteTextures(2, textures);
    glDeleteProgram(p);
}

// A view of a 2D array texture's layers 1..2 samples them as its layers 0..1.
// A view of level 1, layers 1..2 of a 2D array texture samples them as its
// level 0, layers 0..1.
GPU_TEST_VERSION(gpu_gl43_texture_view_layers, 43) {
    GLuint parent = 0, view = 0;
    glGenTextures(1, &parent);
    glBindTexture(GL_TEXTURE_2D_ARRAY, parent);
    glTexStorage3D(GL_TEXTURE_2D_ARRAY, 3, GL_RGBA8, 4, 4, 4);
    // Level 1 of layer L holds grey 64 * L (level 0 stays black).
    for (int layer = 0; layer < 4; ++layer) {
        const uint8_t c = static_cast<uint8_t>(64 * layer);
        std::vector<uint8_t> texels(16);
        for (size_t i = 0; i < texels.size(); ++i) texels[i] = i % 4 == 3 ? 255 : c;
        glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 1, 0, 0, layer, 2, 2, 1, GL_RGBA, GL_UNSIGNED_BYTE, texels.data());
        // Level 2 of layer L holds 32 + 64 * L.
        const uint8_t c2[4] = {static_cast<uint8_t>(32 + 64 * layer), static_cast<uint8_t>(32 + 64 * layer),
                               static_cast<uint8_t>(32 + 64 * layer), 255};
        glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 2, 0, 0, layer, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, c2);
    }
    glGenTextures(1, &view);
    glTextureView(view, GL_TEXTURE_2D_ARRAY, parent, GL_RGBA8, 1, 2, 1, 2);  // levels 1..2, layers 1..2
    CHECK_GL();
    glBindTexture(GL_TEXTURE_2D_ARRAY, view);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    const char* fs = R"(#version 430 core
uniform sampler2DArray t;
uniform float layer, lod;
out vec4 o;
void main() { o = textureLod(t, vec3(0.5, 0.5, layer), lod); })";
    GLuint p = gpu::program(R"(#version 430 core
in vec2 position;
void main() { gl_Position = vec4(position, 0.0, 1.0); })", fs);
    GLuint vao = gpu::quad();
    glUseProgram(p);
    for (int lod = 0; lod < 2; ++lod)
        for (int layer = 0; layer < 2; ++layer) {
            glUniform1f(glGetUniformLocation(p, "layer"), static_cast<float>(layer));
            glUniform1f(glGetUniformLocation(p, "lod"), static_cast<float>(lod));
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            const std::vector<uint8_t> px = gpu::readRGBA8(32, 32, 1, 1);
            const uint8_t g = static_cast<uint8_t>(32 * lod + 64 * (layer + 1));
            const uint8_t want[4] = {g, g, g, 255};
            std::fprintf(stderr, "  view lod %d layer %d: %u %u %u %u\n", lod, layer, px[0], px[1], px[2], px[3]);
            CHECK(gpu::near(px.data(), want));
        }
    CHECK_GL();
    glDeleteVertexArrays(1, &vao);
    GLuint textures[2] = {view, parent};
    glDeleteTextures(2, textures);
    glDeleteProgram(p);
}

// Cube faces cleared through a framebuffer at level 1, sampled through a
// cube view starting at that level (KHR-GL43.texture_view.view_sampling).
GPU_TEST_VERSION(gpu_gl43_texture_view_cube_levels, 43) {
    GLuint parent = 0, view = 0, fbo = 0;
    glGenTextures(1, &parent);
    glBindTexture(GL_TEXTURE_CUBE_MAP, parent);
    glTexStorage2D(GL_TEXTURE_CUBE_MAP, 3, GL_RGBA8, 4, 4);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
    for (int face = 0; face < 6; ++face)
        for (int level = 0; level < 3; ++level) {
            glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, parent, level);
            glClearColor(static_cast<float>(face) / 8.0f, static_cast<float>(level) / 4.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
        }
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glGenTextures(1, &view);
    glTextureView(view, GL_TEXTURE_CUBE_MAP, parent, GL_RGBA8, 1, 2, 0, 6);
    CHECK_GL();
    glBindTexture(GL_TEXTURE_CUBE_MAP, view);
    GLuint p = gpu::program(R"(#version 430 core
in vec2 position;
void main() { gl_Position = vec4(position, 0.0, 1.0); })", R"(#version 430 core
uniform samplerCube t;
uniform float lod;
out vec4 o;
void main() { o = textureLod(t, vec3(-1.0, 0.0, 0.0), lod); })");
    GLuint vao = gpu::quad();
    glUseProgram(p);
    for (int lod = 0; lod < 2; ++lod) {
        glUniform1f(glGetUniformLocation(p, "lod"), static_cast<float>(lod));
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        const std::vector<uint8_t> px = gpu::readRGBA8(32, 32, 1, 1);
        // -X is face 1; view level `lod` is parent level lod + 1.
        const uint8_t want[4] = {32, static_cast<uint8_t>(64 * (lod + 1)), 0, 255};
        std::fprintf(stderr, "  cube view lod %d: %u %u %u %u\n", lod, px[0], px[1], px[2], px[3]);
        CHECK(gpu::near(px.data(), want, 2));
    }
    CHECK_GL();
    glDeleteVertexArrays(1, &vao);
    glDeleteFramebuffers(1, &fbo);
    GLuint textures[2] = {view, parent};
    glDeleteTextures(2, textures);
    glDeleteProgram(p);
}

// glGenerateMipmap on a depth texture (KHR-GL43.texture_query_levels.*Shadow*).
GPU_TEST_VERSION(gpu_gl43_depth_mipmaps, 43) {
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    std::vector<float> depth(16, 0.25f);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, 4, 4, 0, GL_DEPTH_COMPONENT, GL_FLOAT, depth.data());
    glGenerateMipmap(GL_TEXTURE_2D);
    CHECK_GL();
    float level2 = 0.0f;
    glGetTexImage(GL_TEXTURE_2D, 2, GL_DEPTH_COMPONENT, GL_FLOAT, &level2);
    CHECK(level2 == 0.25f);
    CHECK_GL();
    glDeleteTextures(1, &tex);
}

// Uncompressed data uploaded into BPTC, ETC2 and EAC textures is compressed
// by the driver, and glGetTexImage decodes it again (GPU decode).
GPU_TEST_VERSION(gpu_gl43_compress_on_upload, 43) {
    struct Case { GLenum internal; GLenum format; int components; };
    const Case cases[] = {{GL_COMPRESSED_RGBA_BPTC_UNORM, GL_RGBA, 4}, {GL_COMPRESSED_RGB8_ETC2, GL_RGB, 3},
                          {GL_COMPRESSED_RGBA8_ETC2_EAC, GL_RGBA, 4}, {GL_COMPRESSED_R11_EAC, GL_RED, 1},
                          {GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, GL_RGBA, 4}};
    const uint8_t solid[4] = {200, 100, 50, 255};
    for (const Case& c : cases) {
        GLuint tex = 0;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        std::vector<uint8_t> texels(16 * static_cast<size_t>(c.components));
        for (size_t i = 0; i < texels.size(); ++i) texels[i] = solid[i % static_cast<size_t>(c.components)];
        glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(c.internal), 4, 4, 0, c.format, GL_UNSIGNED_BYTE, texels.data());
        CHECK_GL();
        std::vector<uint8_t> back(64, 0);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, back.data());
        CHECK_GL();
        for (int ch = 0; ch < c.components; ++ch) {
            const int diff = std::abs(static_cast<int>(back[static_cast<size_t>(ch)]) - solid[ch]);
            if (diff > 12) std::fprintf(stderr, "  format 0x%x channel %d: %u, expected %u\n", c.internal, ch, back[static_cast<size_t>(ch)], solid[ch]);
            CHECK(diff <= 12);
        }
        glDeleteTextures(1, &tex);
    }
}

// BC6H (signed and unsigned float) from float data, decoded back.
GPU_TEST_VERSION(gpu_gl43_compress_bc6h, 43) {
    for (GLenum internal : {GL_COMPRESSED_RGB_BPTC_UNSIGNED_FLOAT, GL_COMPRESSED_RGB_BPTC_SIGNED_FLOAT}) {
        const bool isSigned = internal == GL_COMPRESSED_RGB_BPTC_SIGNED_FLOAT;
        const float solid[3] = {1.5f, isSigned ? -0.75f : 0.75f, 4.0f};
        std::vector<float> texels(48);
        for (size_t i = 0; i < texels.size(); ++i) texels[i] = solid[i % 3];
        GLuint tex = 0;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(internal), 4, 4, 0, GL_RGB, GL_FLOAT, texels.data());
        std::vector<float> back(48, 0.0f);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGB, GL_FLOAT, back.data());
        CHECK_GL();
        for (int ch = 0; ch < 3; ++ch) {
            const float rel = std::fabs(back[static_cast<size_t>(ch)] - solid[ch]) / std::fabs(solid[ch]);
            if (rel > 0.02f) std::fprintf(stderr, "  0x%x channel %d: %g, expected %g\n", internal, ch, back[static_cast<size_t>(ch)], solid[ch]);
            CHECK(rel <= 0.02f);
        }
        glDeleteTextures(1, &tex);
    }
}

// Storage blocks in the emulated geometry stage (a compute kernel): each
// primitive counts itself (KHR-GL43.shader_storage_buffer_object.advanced-write-geometry).
GPU_TEST_VERSION(gpu_gl43_ssbo_geometry_stage, 43) {
    const char* gs = R"(#version 430 core
layout(triangles) in;
layout(triangle_strip, max_vertices = 3) out;
layout(std430, binding = 1) buffer Count { uint primitives; uint vertices; };
void main() {
    atomicAdd(primitives, 1u);
    for (int i = 0; i < 3; ++i) { atomicAdd(vertices, 1u); gl_Position = gl_in[i].gl_Position; EmitVertex(); }
})";
    GLuint p = glCreateProgram();
    const char* vs = R"(#version 430 core
in vec2 position;
void main() { gl_Position = vec4(position, 0.0, 1.0); })";
    const char* fs = R"(#version 430 core
out vec4 o;
void main() { o = vec4(1.0); })";
    for (auto [type, src] : {std::pair<GLenum, const char*>{GL_VERTEX_SHADER, vs}, {GL_GEOMETRY_SHADER, gs}, {GL_FRAGMENT_SHADER, fs}}) {
        GLuint sh = glCreateShader(type);
        glShaderSource(sh, 1, &src, nullptr);
        glCompileShader(sh);
        glAttachShader(p, sh);
        glDeleteShader(sh);
    }
    glBindAttribLocation(p, 0, "position");
    glLinkProgram(p);
    GLint linked = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &linked);
    CHECK(linked);
    GLint maxBlocks = 0;
    glGetIntegerv(GL_MAX_GEOMETRY_SHADER_STORAGE_BLOCKS, &maxBlocks);
    CHECK(maxBlocks >= 8);
    GLuint counts = storageBuffer(1, {0, 0});
    GLuint vao = gpu::quad();
    glUseProgram(p);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
    const std::vector<uint32_t> got = readBuffer(counts, 2);
    CHECK_EQ(got[0], 2u);
    CHECK_EQ(got[1], 6u);
    CHECK_GL();
    glDeleteVertexArrays(1, &vao);
    glDeleteBuffers(1, &counts);
    glDeleteProgram(p);
}

// Several indirect commands in one call, with a command stride, and a
// command with no instances (KHR-GL43.multi_draw_indirect).
GPU_TEST_VERSION(gpu_gl43_multi_draw_indirect, 43) {
    const char* vs = R"(#version 430 core
in vec2 position;
void main() { gl_Position = vec4(position, 0.0, 1.0); })";
    const char* fs = R"(#version 430 core
out vec4 o;
void main() { o = vec4(0.25, 0.0, 0.0, 1.0); })";
    GLuint p = gpu::program(vs, fs);
    glUseProgram(p);
    GLuint vao = gpu::quad();
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    // count, instances, first, base instance, padding: three quads drawn.
    const GLuint arrays[15] = {4, 1, 0, 0, 0, 4, 0, 0, 0, 0, 4, 2, 0, 0, 0};
    GLuint indirect = 0;
    glGenBuffers(1, &indirect);
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, indirect);
    glBufferData(GL_DRAW_INDIRECT_BUFFER, sizeof(arrays), arrays, GL_STATIC_DRAW);
    glMultiDrawArraysIndirect(GL_TRIANGLE_STRIP, nullptr, 3, 20);
    CHECK_GL();
    std::vector<uint8_t> got = gpu::readRGBA8(32, 32, 1, 1);
    std::fprintf(stderr, "  arrays red %u\n", got[0]);
    CHECK(got[0] >= 189 && got[0] <= 193);
    // Elements: indices 1..4 moved to 0..3 by base vertex -1, tightly packed commands.
    glClear(GL_COLOR_BUFFER_BIT);
    const GLuint indices[5] = {9, 1, 2, 3, 4};
    GLuint ebo = 0;
    glGenBuffers(1, &ebo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);
    const GLint elements[10] = {4, 1, 1, -1, 0, 4, 1, 1, -1, 0};
    glBufferData(GL_DRAW_INDIRECT_BUFFER, sizeof(elements), elements, GL_STATIC_DRAW);
    glMultiDrawElementsIndirect(GL_TRIANGLE_STRIP, GL_UNSIGNED_INT, nullptr, 2, 0);
    CHECK_GL();
    got = gpu::readRGBA8(32, 32, 1, 1);
    std::fprintf(stderr, "  elements red %u\n", got[0]);
    CHECK(got[0] >= 126 && got[0] <= 130);
    // A stride that is not a multiple of four is INVALID_VALUE.
    glMultiDrawArraysIndirect(GL_TRIANGLE_STRIP, nullptr, 1, 18);
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_VALUE));
    glDisable(GL_BLEND);
    glDeleteBuffers(1, &ebo);
    glDeleteBuffers(1, &indirect);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
}

// glClearBufferData/SubData convert one texel and repeat it (KHR-GL43.clear_buffer).
GPU_TEST_VERSION(gpu_gl43_clear_buffer_data, 43) {
    GLuint b = storageBuffer(0, std::vector<uint32_t>(12, 0xffffffffu));
    const GLuint seven = 7;
    glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &seven);
    CHECK_GL();
    const uint8_t rgba[4] = {1, 2, 3, 4};
    glClearBufferSubData(GL_SHADER_STORAGE_BUFFER, GL_RGBA8, 16, 16, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    CHECK_GL();
    glClearBufferSubData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, 32, 8, GL_RED_INTEGER, GL_UNSIGNED_INT, nullptr);
    CHECK_GL();
    const std::vector<uint32_t> got = readBuffer(b, 12);
    for (int i = 0; i < 12; ++i) {
        const uint32_t want = i < 4 ? 7u : i < 8 ? 0x04030201u : i < 10 ? 0u : 7u;
        if (got[static_cast<size_t>(i)] != want) std::fprintf(stderr, "  word %d: 0x%x, expected 0x%x\n", i, got[static_cast<size_t>(i)], want);
        CHECK_EQ(got[static_cast<size_t>(i)], want);
    }
    // An offset that is not a multiple of the texel size is INVALID_VALUE.
    glClearBufferSubData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, 2, 4, GL_RED_INTEGER, GL_UNSIGNED_INT, &seven);
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_VALUE));
    glDeleteBuffers(1, &b);
}

// A framebuffer with no attachments rasterises at its default size; the
// fragment shader counts itself (KHR-GL43.framebuffer_no_attachments).
GPU_TEST_VERSION(gpu_gl43_framebuffer_no_attachments, 43) {
    const char* vs = R"(#version 430 core
in vec2 position;
void main() { gl_Position = vec4(position, 0.0, 1.0); })";
    const char* fs = R"(#version 430 core
layout(std430, binding = 0) buffer Count { uint fragments; };
void main() { atomicAdd(fragments, 1u); })";
    GLuint p = gpu::program(vs, fs);
    GLuint counter = storageBuffer(0, {0});
    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    CHECK_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), static_cast<GLenum>(GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT));
    glFramebufferParameteri(GL_FRAMEBUFFER, GL_FRAMEBUFFER_DEFAULT_WIDTH, 16);
    glFramebufferParameteri(GL_FRAMEBUFFER, GL_FRAMEBUFFER_DEFAULT_HEIGHT, 8);
    CHECK_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE));
    GLint width = 0;
    glGetFramebufferParameteriv(GL_FRAMEBUFFER, GL_FRAMEBUFFER_DEFAULT_WIDTH, &width);
    CHECK_EQ(width, 16);
    glViewport(0, 0, 16, 8);
    GLuint vao = gpu::quad();
    glUseProgram(p);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
    CHECK_GL();
    const std::vector<uint32_t> got = readBuffer(counter, 1);
    std::fprintf(stderr, "  fragments %u\n", got[0]);
    CHECK_EQ(got[0], 128u);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, 64, 64);
    glDeleteFramebuffers(1, &fbo);
    glDeleteVertexArrays(1, &vao);
    glDeleteBuffers(1, &counter);
    glDeleteProgram(p);
}

// glGetInternalformat with the GL 4.3 pnames (KHR-GL43.internalformat_query2 subset).
GPU_TEST_VERSION(gpu_gl43_internalformat_query2, 43) {
    GLint v = -1;
    glGetInternalformativ(GL_TEXTURE_2D, GL_RGBA8, GL_INTERNALFORMAT_SUPPORTED, 1, &v);
    CHECK_EQ(v, GL_TRUE);
    glGetInternalformativ(GL_TEXTURE_2D, GL_R32F, GL_IMAGE_TEXEL_SIZE, 1, &v);
    CHECK_EQ(v, 32);
    glGetInternalformativ(GL_TEXTURE_2D, GL_RGBA8, GL_VIEW_COMPATIBILITY_CLASS, 1, &v);
    CHECK_EQ(v, GL_VIEW_CLASS_32_BITS);
    glGetInternalformativ(GL_TEXTURE_2D, GL_DEPTH_COMPONENT32F, GL_DEPTH_RENDERABLE, 1, &v);
    CHECK_EQ(v, GL_TRUE);
    glGetInternalformativ(GL_TEXTURE_3D, GL_DEPTH_COMPONENT32F, GL_INTERNALFORMAT_SUPPORTED, 1, &v);
    CHECK_EQ(v, GL_FALSE);
    GLint64 counts = -1;
    glGetInternalformati64v(GL_RENDERBUFFER, GL_RGBA8, GL_NUM_SAMPLE_COUNTS, 1, &counts);
    CHECK(counts >= 1);
    CHECK_GL();
    glGetInternalformativ(GL_TEXTURE_2D, GL_RGBA8, GL_TEXTURE_2D, 1, &v);
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_ENUM));
    glGetInternalformativ(GL_ARRAY_BUFFER, GL_RGBA8, GL_INTERNALFORMAT_SUPPORTED, 1, &v);
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_ENUM));
}

// GLSL ES 3.00 shaders (GL 4.3, GL_ARB_ES3_compatibility): precision
// statements, a uniform block, a sampler and a flat integer varying.
GPU_TEST_VERSION(gpu_gl43_es3_shaders, 43) {
    const char* vs = R"(#version 300 es
precision highp float;
layout(location = 0) in vec2 position;
flat out int k;
out vec2 uv;
uniform Block { vec4 tint; mat2 rot; };
void main() { k = 3; uv = rot * position * 0.5 + 0.5; gl_Position = vec4(position, 0.0, 1.0); })";
    const char* fs = R"(#version 300 es
precision mediump float;
precision lowp sampler2D;
flat in int k;
in vec2 uv;
uniform sampler2D tex;
uniform Block { vec4 tint; mat2 rot; };
uniform float scale;
layout(location = 0) out vec4 o;
void main() { o = texture(tex, uv) * tint * scale + vec4(0.0, 0.0, float(k) / 3.0, 0.0); })";
    GLuint p = gpu::program(vs, fs);
    glUseProgram(p);
    const float block[12] = {1, 1, 1, 1, 1, 0, 0, 0, 0, 1, 0, 0};
    GLuint ubo = 0;
    glGenBuffers(1, &ubo);
    glBindBufferBase(GL_UNIFORM_BUFFER, 0, ubo);
    glBufferData(GL_UNIFORM_BUFFER, sizeof(block), block, GL_STATIC_DRAW);
    glUniformBlockBinding(p, glGetUniformBlockIndex(p, "Block"), 0);
    glUniform1f(glGetUniformLocation(p, "scale"), 1.0f);
    const uint8_t red[4] = {255, 0, 0, 255};
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, red);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glUniform1i(glGetUniformLocation(p, "tex"), 0);
    GLuint vao = gpu::quad();
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    CHECK_GL();
    const uint8_t magenta[4] = {255, 0, 255, 255};
    CHECK(gpu::near(gpu::readRGBA8(32, 32, 1, 1).data(), magenta));
    glDeleteVertexArrays(1, &vao);
    glDeleteTextures(1, &tex);
    glDeleteBuffers(1, &ubo);
    glDeleteProgram(p);
}

// Layered rendering into each level of a 1D array texture: gl_Layer selects
// the layer (KHR-GL43.texture_view.view_sampling fills its parents this way).
GPU_TEST_VERSION(gpu_gl43_layered_1d_array, 43) {
    const char* vs = R"(#version 400
void main() { gl_Position = vec4(-1.0, -1.0, 0.0, 1.0); })";
    const char* gs = R"(#version 400
layout(points) in;
layout(triangle_strip, max_vertices = 4) out;
uniform int layer_id;
void main() {
    gl_Layer = layer_id; gl_Position = vec4(-1.0, -1.0, 0.0, 1.0); EmitVertex();
    gl_Layer = layer_id; gl_Position = vec4(-1.0,  1.0, 0.0, 1.0); EmitVertex();
    gl_Layer = layer_id; gl_Position = vec4( 1.0, -1.0, 0.0, 1.0); EmitVertex();
    gl_Layer = layer_id; gl_Position = vec4( 1.0,  1.0, 0.0, 1.0); EmitVertex();
    EndPrimitive();
})";
    const char* fs = R"(#version 400
uniform vec4 color;
out vec4 o;
void main() { o = color; })";
    GLuint p = glCreateProgram();
    for (auto [type, src] : {std::pair<GLenum, const char*>{GL_VERTEX_SHADER, vs}, {GL_GEOMETRY_SHADER, gs}, {GL_FRAGMENT_SHADER, fs}}) {
        GLuint sh = glCreateShader(type);
        glShaderSource(sh, 1, &src, nullptr);
        glCompileShader(sh);
        glAttachShader(p, sh);
        glDeleteShader(sh);
    }
    glLinkProgram(p);
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_1D_ARRAY, tex);
    glTexStorage2D(GL_TEXTURE_1D_ARRAY, 3, GL_RGBA8, 4, 4);
    GLuint fbo = 0, vao = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glUseProgram(p);
    for (int layer = 0; layer < 4; ++layer)
        for (int level = 0; level < 3; ++level) {
            glFramebufferTexture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, tex, level);
            glUniform4f(glGetUniformLocation(p, "color"), layer / 8.0f, level / 4.0f, 1.0f, 1.0f);
            glUniform1i(glGetUniformLocation(p, "layer_id"), layer);
            glDrawArrays(GL_POINTS, 0, 1);
        }
    CHECK_GL();
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    for (int level = 0; level < 3; ++level) {
        const int width = 4 >> level;
        std::vector<uint8_t> back(static_cast<size_t>(width * 4 * 4), 0);
        glGetTexImage(GL_TEXTURE_1D_ARRAY, level, GL_RGBA, GL_UNSIGNED_BYTE, back.data());
        for (int layer = 0; layer < 4; ++layer)
            for (int x = 0; x < width; ++x) {
                const uint8_t* texel = &back[static_cast<size_t>((layer * width + x) * 4)];
                CHECK_EQ(static_cast<int>(texel[0]), layer * 32);
                CHECK_EQ(static_cast<int>(texel[1]), level * 64);
            }
    }
    glDeleteFramebuffers(1, &fbo);
    glDeleteVertexArrays(1, &vao);
    glDeleteTextures(1, &tex);
    glDeleteProgram(p);
}

// A two-dimensional subroutine uniform array walked with length()
// (GL_ARB_arrays_of_arrays; KHR-GL43.shader_subroutine.arrays_of_arrays_of_uniforms).
GPU_TEST_VERSION(gpu_gl43_subroutine_arrays_of_arrays, 43) {
    const char* vs = R"(#version 400 core
in vec2 position;
void main() { gl_Position = vec4(position, 0.0, 1.0); })";
    const char* fs = R"(#version 400 core
#extension GL_ARB_arrays_of_arrays : require
subroutine float step_type(float x);
subroutine(step_type) float up(float x) { return x + 0.125; }
subroutine(step_type) float down(float x) { return x - 0.125; }
subroutine uniform step_type steps[2][3];
out vec4 o;
void main() {
    float x = 0.5;
    for (int j = 0; j < steps.length(); ++j)
        for (int i = 0; i < steps[j].length(); ++i) x = steps[j][i](x);
    o = vec4(x, 0.0, 0.0, 1.0);
})";
    GLuint p = gpu::program(vs, fs);
    glUseProgram(p);
    const GLuint up = glGetSubroutineIndex(p, GL_FRAGMENT_SHADER, "up");
    const GLuint down = glGetSubroutineIndex(p, GL_FRAGMENT_SHADER, "down");
    CHECK_EQ(glGetSubroutineUniformLocation(p, GL_FRAGMENT_SHADER, "steps[1][2]"), 5);
    // Four steps up, two down: 0.5 + 0.25.
    const GLuint selection[6] = {up, up, down, up, down, up};
    glUniformSubroutinesuiv(GL_FRAGMENT_SHADER, 6, selection);
    GLuint vao = gpu::quad();
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    CHECK_GL();
    const uint8_t want[4] = {191, 0, 0, 255};
    const std::vector<uint8_t> got = gpu::readRGBA8(32, 32, 1, 1);
    if (!gpu::near(got.data(), want)) std::fprintf(stderr, "  red %u, up %u down %u\n", got[0], up, down);
    CHECK(gpu::near(got.data(), want));
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
}

// A counter without an offset takes its binding's default offset from a
// declaration without a name (KHR-GL44.layout_binding.atomic_uint_layout_binding_atomic_*).
GPU_TEST_VERSION(gpu_gl43_atomic_counter_default_offset, 43) {
    const char* vs = R"(#version 430 core
in vec2 position;
void main() { gl_Position = vec4(position, 0.0, 1.0); })";
    const char* fs = R"(#version 430 core
layout(binding=2, offset=4) uniform atomic_uint;
layout(binding=2) uniform atomic_uint counter;
out vec4 o;
void main() { o = vec4(float(atomicCounterIncrement(counter))); })";
    GLuint p = gpu::program(vs, fs);
    GLuint index = glGetProgramResourceIndex(p, GL_UNIFORM, "counter");
    const GLenum props[2] = {GL_OFFSET, GL_ATOMIC_COUNTER_BUFFER_INDEX};
    GLint values[2] = {-1, -1};
    glGetProgramResourceiv(p, GL_UNIFORM, index, 2, props, 2, nullptr, values);
    CHECK_EQ(values[0], 4);
    CHECK_GL();
    glDeleteProgram(p);
}

// glFramebufferTexture and glTexBufferRange errors
// (KHR-GL44.geometry_shader.layered_fbo.fb_texture_*, texture_buffer.texture_buffer_errors).
GPU_TEST_VERSION(gpu_gl43_attach_and_range_errors, 43) {
    GLuint fbo = 0, tex = 0, buffer = 0, view = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, 4321, 0);  // not a texture
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_VALUE));
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_3D, tex);
    glTexStorage3D(GL_TEXTURE_3D, 2, GL_RGBA8, 4, 4, 4);
    glFramebufferTexture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, tex, 2);  // past the immutable levels
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_VALUE));
    glFramebufferTexture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, tex, 1);
    CHECK_GL();
    glGenBuffers(1, &buffer);
    glBindBuffer(GL_TEXTURE_BUFFER, buffer);
    glBufferData(GL_TEXTURE_BUFFER, 1024, nullptr, GL_STATIC_DRAW);
    glGenTextures(1, &view);
    glBindTexture(GL_TEXTURE_BUFFER, view);
    glTexBufferRange(GL_TEXTURE_BUFFER, GL_R32F, buffer, 0, -16);
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_VALUE));
    glTexBufferRange(GL_TEXTURE_BUFFER, GL_R32F, buffer, 0, 512);
    CHECK_GL();
    glFramebufferTexture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, view, 0);  // a buffer texture
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_OPERATION));
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &tex);
    glDeleteTextures(1, &view);
    glDeleteBuffers(1, &buffer);
}

// GL_RGB32F/I/UI buffer textures (GL 4.0, GL_ARB_texture_buffer_object_rgb32):
// texels of 12 bytes, alpha 1, updated by glBufferSubData between draws.
GPU_TEST_VERSION(gpu_gl43_texture_buffer_rgb32, 43) {
    const char* vs = R"(#version 430 core
in vec2 position;
void main() { gl_Position = vec4(position, 0.0, 1.0); })";
    const char* fs = R"(#version 430 core
layout(binding = 0) uniform samplerBuffer colors;
layout(binding = 1) uniform usamplerBuffer counts;
out vec4 o;
void main() {
    int i = gl_FragCoord.x < 32.0 ? 0 : 1;
    uvec4 c = texelFetch(counts, i);
    o = vec4(texelFetch(colors, i).rgb, c.w == 1u && c.z == uint(i + 3) ? 1.0 : 0.0);
})";
    GLuint p = gpu::program(vs, fs);
    glUseProgram(p);
    const float colors[6] = {0.25f, 0.5f, 0.75f, 1.0f, 0.0f, 0.0f};
    const GLuint counts[6] = {1, 2, 3, 4, 5, 4};
    GLuint buffers[2], textures[2];
    glGenBuffers(2, buffers);
    glGenTextures(2, textures);
    glBindBuffer(GL_TEXTURE_BUFFER, buffers[0]);
    glBufferData(GL_TEXTURE_BUFFER, sizeof(colors), colors, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_TEXTURE_BUFFER, buffers[1]);
    glBufferData(GL_TEXTURE_BUFFER, sizeof(counts), counts, GL_STATIC_DRAW);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_BUFFER, textures[0]);
    glTexBuffer(GL_TEXTURE_BUFFER, GL_RGB32F, buffers[0]);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_BUFFER, textures[1]);
    glTexBuffer(GL_TEXTURE_BUFFER, GL_RGB32UI, buffers[1]);
    CHECK_GL();
    GLuint vao = gpu::quad();
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    const uint8_t left[4] = {64, 128, 191, 255}, right[4] = {255, 0, 0, 255};
    CHECK(gpu::near(gpu::readRGBA8(16, 32, 1, 1).data(), left));
    CHECK(gpu::near(gpu::readRGBA8(48, 32, 1, 1).data(), right));
    // The next draw sees an update of the buffer.
    const float green[3] = {0.0f, 1.0f, 0.0f};
    glBindBuffer(GL_TEXTURE_BUFFER, buffers[0]);
    glBufferSubData(GL_TEXTURE_BUFFER, 0, sizeof(green), green);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    const uint8_t updated[4] = {0, 255, 0, 255};
    CHECK(gpu::near(gpu::readRGBA8(16, 32, 1, 1).data(), updated));
    CHECK_GL();
    glActiveTexture(GL_TEXTURE0);
    glDeleteVertexArrays(1, &vao);
    glDeleteTextures(2, textures);
    glDeleteBuffers(2, buffers);
    glDeleteProgram(p);
}
