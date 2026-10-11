// GL 4.0-4.2 features.
#include <string>

#include "gpu.h"

namespace {

GLuint linkStages(const char* vs, const char* gs, const char* fs) {
    GLuint p = glCreateProgram();
    auto attach = [&](GLenum type, const char* src) {
        if (!src) return;
        GLuint s = glCreateShader(type);
        glShaderSource(s, 1, &src, nullptr);
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
    };
    attach(GL_VERTEX_SHADER, vs);
    attach(GL_GEOMETRY_SHADER, gs);
    attach(GL_FRAGMENT_SHADER, fs);
    glBindAttribLocation(p, 0, "position");
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

const char* kQuadVs = R"(#version 420 core
in vec2 position;
void main() { gl_Position = vec4(position, 0.0, 1.0); })";

const char* kColorFs = R"(#version 420 core
uniform vec4 color;
out vec4 o;
void main() { o = color; })";

}  // namespace

GPU_TEST_VERSION(gpu_gl4_draw_indirect, 42) {
    // A quad drawn by glDrawArraysIndirect, then by glDrawElementsIndirect
    // with a base vertex and a base instance from the command.
    GLuint p = gpu::program(kQuadVs, kColorFs);
    glUseProgram(p);
    GLuint vao = gpu::quad();
    const GLuint arrays[4] = {4, 1, 0, 0};  // count, instances, first, base instance
    GLuint indirect = 0;
    glGenBuffers(1, &indirect);
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, indirect);
    glBufferData(GL_DRAW_INDIRECT_BUFFER, sizeof(arrays), arrays, GL_STATIC_DRAW);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glUniform4f(glGetUniformLocation(p, "color"), 0, 1, 0, 1);
    glDrawArraysIndirect(GL_TRIANGLE_STRIP, nullptr);
    CHECK_GL();
    const uint8_t green[4] = {0, 255, 0, 255};
    CHECK(gpu::near(gpu::readRGBA8(32, 32, 1, 1).data(), green));
    // Indices 0..3 shifted down by base vertex -1 from 1..4.
    const GLuint indices[5] = {9, 1, 2, 3, 4};
    GLuint ebo = 0;
    glGenBuffers(1, &ebo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);
    const GLint elements[5] = {4, 1, 1, -1, 0};  // count, instances, first index, base vertex, base instance
    glBufferData(GL_DRAW_INDIRECT_BUFFER, sizeof(elements), elements, GL_STATIC_DRAW);
    glUniform4f(glGetUniformLocation(p, "color"), 0, 0, 1, 1);
    glDrawElementsIndirect(GL_TRIANGLE_STRIP, GL_UNSIGNED_INT, nullptr);
    CHECK_GL();
    const uint8_t blue[4] = {0, 0, 255, 255};
    CHECK(gpu::near(gpu::readRGBA8(32, 32, 1, 1).data(), blue));
    // An unaligned command offset is INVALID_VALUE.
    glDrawArraysIndirect(GL_TRIANGLE_STRIP, reinterpret_cast<const void*>(2));
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_VALUE));
    glDeleteBuffers(1, &ebo);
    glDeleteBuffers(1, &indirect);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
}

GPU_TEST_VERSION(gpu_gl4_texture_gather_offsets, 42) {
    // textureGatherOffsets returns the i0j0 texel of each offset's footprint.
    uint8_t texels[16];
    for (int i = 0; i < 16; ++i) texels[i] = static_cast<uint8_t>(i * 16);
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, 4, 4, 0, GL_RED, GL_UNSIGNED_BYTE, texels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    const char* fs = R"(#version 420 core
uniform sampler2D tex;
out vec4 o;
void main() {
    // (0.25, 0.25) is the corner of texel (0, 0) and (1, 1): the footprint's i0j0 is texel (0, 0).
    o = textureGatherOffsets(tex, vec2(0.25), ivec2[4](ivec2(0, 0), ivec2(1, 0), ivec2(0, 1), ivec2(1, 1)));
})";
    GLuint p = gpu::program(kQuadVs, fs);
    glUseProgram(p);
    glUniform1i(glGetUniformLocation(p, "tex"), 0);
    GLuint vao = gpu::quad();
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    CHECK_GL();
    const uint8_t want[4] = {0, 16, 64, 80};
    CHECK(gpu::near(gpu::readRGBA8(10, 10, 1, 1).data(), want));
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
    glDeleteTextures(1, &tex);
}

GPU_TEST_VERSION(gpu_gl4_viewport_array_gs_invocations, 42) {
    // An instanced geometry shader sends invocation i to viewport i: the
    // left half is red (viewport 0), the right half green (viewport 1).
    const char* gs = R"(#version 420 core
layout(points, invocations = 2) in;
layout(triangle_strip, max_vertices = 4) out;
flat out int invocation;
void main() {
    for (int i = 0; i < 4; ++i) {
        invocation = gl_InvocationID;
        gl_ViewportIndex = gl_InvocationID;
        gl_Position = vec4(i % 2 == 0 ? -1.0 : 1.0, i < 2 ? -1.0 : 1.0, 0.0, 1.0);
        EmitVertex();
    }
})";
    const char* fs = R"(#version 420 core
flat in int invocation;
out vec4 o;
void main() { o = invocation == 0 ? vec4(1, 0, 0, 1) : vec4(0, 1, 0, 1); })";
    GLuint p = linkStages(kQuadVs, gs, fs);
    glUseProgram(p);
    GLuint vao = gpu::quad();
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glViewportIndexedf(0, 0, 0, 32, 64);
    glViewportIndexedf(1, 32, 0, 32, 64);
    glDrawArrays(GL_POINTS, 0, 1);
    CHECK_GL();
    GLfloat viewport[4] = {};
    glGetFloati_v(GL_VIEWPORT, 1, viewport);
    CHECK_EQ(viewport[0], 32.0f);
    const uint8_t red[4] = {255, 0, 0, 255}, green[4] = {0, 255, 0, 255};
    CHECK(gpu::near(gpu::readRGBA8(8, 32, 1, 1).data(), red));
    CHECK(gpu::near(gpu::readRGBA8(56, 32, 1, 1).data(), green));
    // A scissor on index 1 clips only that viewport's primitives.
    glClear(GL_COLOR_BUFFER_BIT);
    glScissorIndexed(1, 0, 0, 40, 64);
    glEnablei(GL_SCISSOR_TEST, 1);
    glDrawArrays(GL_POINTS, 0, 1);
    glDisable(GL_SCISSOR_TEST);
    const uint8_t black[4] = {0, 0, 0, 255};
    CHECK(gpu::near(gpu::readRGBA8(8, 32, 1, 1).data(), red));
    CHECK(gpu::near(gpu::readRGBA8(36, 32, 1, 1).data(), green));
    CHECK(gpu::near(gpu::readRGBA8(56, 32, 1, 1).data(), black));
    glViewport(0, 0, 64, 64);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
}

GPU_TEST_VERSION(gpu_gl4_layered_clear, 42) {
    // Clearing a layered framebuffer clears every layer.
    GLuint tex = 0, fbo = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D_ARRAY, tex);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, 8, 8, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, tex, 0);
    CHECK_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE));
    glViewport(0, 0, 8, 8);
    glClearColor(0, 0, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_SCISSOR_TEST);  // a scissored clear is a draw
    glScissor(0, 0, 4, 8);
    glClearColor(1, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
    CHECK_GL();
    const uint8_t red[4] = {255, 0, 0, 255}, blue[4] = {0, 0, 255, 255};
    for (int layer = 0; layer < 2; ++layer) {
        glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, tex, 0, layer);
        CHECK(gpu::near(gpu::readRGBA8(1, 1, 1, 1).data(), red));
        CHECK(gpu::near(gpu::readRGBA8(6, 1, 1, 1).data(), blue));
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, 64, 64);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &tex);
}

GPU_TEST_VERSION(gpu_gl4_atomic_counters, 42) {
    // Every fragment of a 64x64 quad increments the counter at offset 4 of
    // binding 1, starting from the buffer's value.
    const char* fs = R"(#version 420 core
layout(binding = 1, offset = 4) uniform atomic_uint fragments;
out vec4 o;
void main() { atomicCounterIncrement(fragments); o = vec4(1); })";
    GLuint p = gpu::program(kQuadVs, fs);
    glUseProgram(p);
    GLint buffers = 0;
    glGetProgramiv(p, GL_ACTIVE_ATOMIC_COUNTER_BUFFERS, &buffers);
    CHECK_EQ(buffers, 1);
    GLint binding = -1;
    glGetActiveAtomicCounterBufferiv(p, 0, GL_ATOMIC_COUNTER_BUFFER_BINDING, &binding);
    CHECK_EQ(binding, 1);
    const GLuint initial[2] = {7, 100};
    GLuint counter = 0;
    glGenBuffers(1, &counter);
    glBindBuffer(GL_ATOMIC_COUNTER_BUFFER, counter);
    glBufferData(GL_ATOMIC_COUNTER_BUFFER, sizeof(initial), initial, GL_DYNAMIC_DRAW);
    glBindBufferBase(GL_ATOMIC_COUNTER_BUFFER, 1, counter);
    GLuint vao = gpu::quad();
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glMemoryBarrier(GL_ATOMIC_COUNTER_BARRIER_BIT);
    CHECK_GL();
    GLuint result[2] = {};
    glGetBufferSubData(GL_ATOMIC_COUNTER_BUFFER, 0, sizeof(result), result);
    CHECK_EQ(result[0], 7u);
    CHECK_EQ(result[1], 100u + 64u * 64u);
    glDeleteVertexArrays(1, &vao);
    glDeleteBuffers(1, &counter);
    glDeleteProgram(p);
}

GPU_TEST_VERSION(gpu_gl4_transform_feedback_pause_and_draw, 42) {
    // Draws while paused are not captured; glDrawTransformFeedback draws
    // what the object captured.
    const char* vs = R"(#version 420 core
in vec2 position;
out vec2 captured;
void main() { captured = position; gl_Position = vec4(position, 0.0, 1.0); })";
    GLuint p = glCreateProgram();
    GLuint v = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(v, 1, &vs, nullptr);
    glCompileShader(v);
    glAttachShader(p, v);
    glDeleteShader(v);
    glBindAttribLocation(p, 0, "position");
    const char* varying = "captured";
    glTransformFeedbackVaryings(p, 1, &varying, GL_INTERLEAVED_ATTRIBS);
    glLinkProgram(p);
    glUseProgram(p);
    GLuint vao = gpu::quad();
    GLuint buffer = 0, xfb = 0, query = 0, generatedQuery = 0;
    glGenBuffers(1, &buffer);
    glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, buffer);
    glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, 1024, nullptr, GL_DYNAMIC_COPY);
    glGenTransformFeedbacks(1, &xfb);
    glBindTransformFeedback(GL_TRANSFORM_FEEDBACK, xfb);
    glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, buffer);
    glGenQueries(1, &query);
    glGenQueries(1, &generatedQuery);
    glEnable(GL_RASTERIZER_DISCARD);
    glBeginQuery(GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN, query);
    glBeginTransformFeedback(GL_POINTS);
    glDrawArrays(GL_POINTS, 0, 2);
    glPauseTransformFeedback();
    glDrawArrays(GL_POINTS, 0, 4);
    glResumeTransformFeedback();
    glDrawArrays(GL_POINTS, 2, 2);
    glEndTransformFeedback();
    glEndQuery(GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN);
    CHECK_GL();
    GLuint written = 0;
    glGetQueryObjectuiv(query, GL_QUERY_RESULT, &written);
    CHECK_EQ(written, 4u);
    float data[8] = {};
    glGetBufferSubData(GL_TRANSFORM_FEEDBACK_BUFFER, 0, sizeof(data), data);
    const float quad[8] = {-1, -1, 1, -1, -1, 1, 1, 1};
    for (int i = 0; i < 8; ++i) CHECK_EQ(data[i], quad[i]);
    // Drawing from the object draws its 4 captured vertices.
    glBindTransformFeedback(GL_TRANSFORM_FEEDBACK, 0);
    glBeginQuery(GL_PRIMITIVES_GENERATED, generatedQuery);
    glDrawTransformFeedback(GL_POINTS, xfb);
    glEndQuery(GL_PRIMITIVES_GENERATED);
    glDisable(GL_RASTERIZER_DISCARD);
    GLuint generated = 0;
    glGetQueryObjectuiv(generatedQuery, GL_QUERY_RESULT, &generated);
    CHECK_EQ(generated, 4u);
    CHECK_GL();
    glDeleteQueries(1, &query);
    glDeleteQueries(1, &generatedQuery);
    glDeleteTransformFeedbacks(1, &xfb);
    glDeleteBuffers(1, &buffer);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
}

GPU_TEST_VERSION(gpu_gl4_image_store_vertex_and_fragment, 42) {
    // A vertex shader (rasterization discarded) stores one texel per vertex;
    // a fragment shader adds 1 to each texel with an image atomic.
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_R32UI, 4, 1);
    glBindImageTexture(2, tex, 0, GL_FALSE, 0, GL_READ_WRITE, GL_R32UI);
    const char* vs = R"(#version 420 core
layout(r32ui, binding = 2) uniform uimage2D img;
void main() { imageStore(img, ivec2(gl_VertexID, 0), uvec4(10u + uint(gl_VertexID))); gl_Position = vec4(0.0); })";
    const char* fs = R"(#version 420 core
out vec4 o;
void main() { o = vec4(1.0); })";
    GLuint p = gpu::program(vs, fs);
    glUseProgram(p);
    GLuint vao = 0;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glEnable(GL_RASTERIZER_DISCARD);
    glDrawArrays(GL_POINTS, 0, 4);
    glDisable(GL_RASTERIZER_DISCARD);
    glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);
    CHECK_GL();
    GLuint texels[4] = {};
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RED_INTEGER, GL_UNSIGNED_INT, texels);
    for (int i = 0; i < 4; ++i) CHECK_EQ(texels[i], 10u + static_cast<GLuint>(i));
    const char* atomicFs = R"(#version 420 core
layout(r32ui, binding = 2) uniform uimage2D img;
out vec4 o;
void main() { imageAtomicAdd(img, ivec2(int(gl_FragCoord.x) % 4, 0), 1u); o = vec4(1.0); })";
    GLuint q = gpu::program(kQuadVs, atomicFs);
    glUseProgram(q);
    GLuint quad = gpu::quad();
    glViewport(0, 0, 4, 1);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glViewport(0, 0, 64, 64);
    glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RED_INTEGER, GL_UNSIGNED_INT, texels);
    for (int i = 0; i < 4; ++i) CHECK_EQ(texels[i], 11u + static_cast<GLuint>(i));
    CHECK_GL();
    glDeleteVertexArrays(1, &quad);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
    glDeleteProgram(q);
    glDeleteTextures(1, &tex);
}

GPU_TEST_VERSION(gpu_gl4_image_store_vertex_only_program, 42) {
    // A program with only a vertex shader stores through images of several
    // targets (rasterization discarded).
    GLuint tex2d = 0, tex3d = 0;
    glGenTextures(1, &tex2d);
    glBindTexture(GL_TEXTURE_2D, tex2d);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA32F, 1, 1);
    glGenTextures(1, &tex3d);
    glBindTexture(GL_TEXTURE_3D, tex3d);
    glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA32F, 1, 1, 2, 0, GL_RGBA, GL_FLOAT, nullptr);
    glBindImageTexture(0, tex2d, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA32F);
    glBindImageTexture(1, tex3d, 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_RGBA32F);
    const char* vs = R"(#version 420 core
layout(rgba32f) writeonly uniform image2D g_image_2d;
layout(rgba32f) writeonly uniform image3D g_image_3d;
uniform vec4 g_value;
uniform int g_index[6];
void main() {
    ivec2 coord = ivec2(gl_VertexID, g_index[0]);
    imageStore(g_image_2d, coord, g_value);
    imageStore(g_image_3d, ivec3(coord, g_index[1]), g_value);
    gl_Position = vec4(0.0);
})";
    GLuint p = glCreateProgram();
    GLuint v = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(v, 1, &vs, nullptr);
    glCompileShader(v);
    glAttachShader(p, v);
    glDeleteShader(v);
    glLinkProgram(p);
    GLint linked = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &linked);
    CHECK(linked);
    glUseProgram(p);
    glUniform1i(glGetUniformLocation(p, "g_image_2d"), 0);
    glUniform1i(glGetUniformLocation(p, "g_image_3d"), 1);
    glUniform4f(glGetUniformLocation(p, "g_value"), 1, 2, 3, 4);
    glUniform1i(glGetUniformLocation(p, "g_index[1]"), 1);
    GLuint vao = 0;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glEnable(GL_RASTERIZER_DISCARD);
    glDrawArrays(GL_POINTS, 0, 1);
    glDisable(GL_RASTERIZER_DISCARD);
    glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);
    CHECK_GL();
    float texel[8] = {};
    glBindTexture(GL_TEXTURE_2D, tex2d);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, texel);
    CHECK_EQ(texel[0], 1.0f);
    CHECK_EQ(texel[3], 4.0f);
    glBindTexture(GL_TEXTURE_3D, tex3d);
    glGetTexImage(GL_TEXTURE_3D, 0, GL_RGBA, GL_FLOAT, texel);
    CHECK_EQ(texel[4], 1.0f);  // slice 1
    CHECK_EQ(texel[7], 4.0f);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
    glDeleteTextures(1, &tex2d);
    glDeleteTextures(1, &tex3d);
}

GPU_TEST_VERSION(gpu_gl4_image_buffer_load, 42) {
    // imageLoad from a buffer texture bound to an image unit.
    const float values[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    GLuint buffer = 0, tex = 0;
    glGenBuffers(1, &buffer);
    glBindBuffer(GL_TEXTURE_BUFFER, buffer);
    glBufferData(GL_TEXTURE_BUFFER, sizeof(values), values, GL_STATIC_DRAW);
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_BUFFER, tex);
    glTexBuffer(GL_TEXTURE_BUFFER, GL_RGBA32F, buffer);
    glBindImageTexture(3, tex, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA32F);
    const char* fs = R"(#version 420 core
layout(rgba32f, binding = 3) readonly uniform imageBuffer g_image_buffer;
out vec4 o;
void main() { o = imageLoad(g_image_buffer, 1) == vec4(5, 6, 7, 8) ? vec4(0, 1, 0, 1) : vec4(1, 0, 0, 1); })";
    GLuint p = gpu::program(kQuadVs, fs);
    glUseProgram(p);
    GLuint vao = gpu::quad();
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    CHECK_GL();
    const uint8_t green[4] = {0, 255, 0, 255};
    CHECK(gpu::near(gpu::readRGBA8(8, 8, 1, 1).data(), green));
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
    glDeleteTextures(1, &tex);
    glDeleteBuffers(1, &buffer);
}

GPU_TEST_VERSION(gpu_gl4_image_buffer_load_formats, 42) {
    // Buffer images of 16 texels read at every fragment's x, with float,
    // int and uint formats, one after the other.
    struct Case { GLenum format; const char* type; const char* prefix; };
    const Case cases[] = {{GL_RGBA32F, "rgba32f", ""}, {GL_RGBA32I, "rgba32i", "i"}, {GL_RGBA32UI, "rgba32ui", "u"}};
    for (const Case& c : cases) {
        const int32_t values[64] = {1, 10, 200, 3000, 1, 10, 200, 3000, 1, 10, 200, 3000, 1, 10, 200, 3000,
                                    1, 10, 200, 3000, 1, 10, 200, 3000, 1, 10, 200, 3000, 1, 10, 200, 3000,
                                    1, 10, 200, 3000, 1, 10, 200, 3000, 1, 10, 200, 3000, 1, 10, 200, 3000,
                                    1, 10, 200, 3000, 1, 10, 200, 3000, 1, 10, 200, 3000, 1, 10, 200, 3000};
        float floats[64];
        for (int i = 0; i < 64; ++i) floats[i] = static_cast<float>(values[i]);
        GLuint buffer = 0, tex = 0;
        glGenBuffers(1, &buffer);
        glBindBuffer(GL_TEXTURE_BUFFER, buffer);
        glBufferData(GL_TEXTURE_BUFFER, sizeof(values), c.format == GL_RGBA32F ? static_cast<const void*>(floats) : values,
                     GL_DYNAMIC_DRAW);
        glBindBuffer(GL_TEXTURE_BUFFER, 0);
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_BUFFER, tex);
        glTexBuffer(GL_TEXTURE_BUFFER, c.format, buffer);
        glBindTexture(GL_TEXTURE_BUFFER, 0);
        glBindImageTexture(4, tex, 0, GL_FALSE, 0, GL_READ_ONLY, c.format);
        std::string fs = std::string("#version 420 core\nlayout(") + c.type + ") readonly uniform " + c.prefix +
                         "imageBuffer g_image_buffer;\nout vec4 o;\nvoid main() {\n  " + c.prefix +
                         "vec4 v = imageLoad(g_image_buffer, int(gl_FragCoord.x));\n  o = v == " + c.prefix +
                         "vec4(1, 10, 200, 3000) ? vec4(0, 1, 0, 1) : vec4(1, 0, 0, 1);\n}\n";
        GLuint p = gpu::program(kQuadVs, fs.c_str());
        glUseProgram(p);
        glUniform1i(glGetUniformLocation(p, "g_image_buffer"), 4);
        GLuint vao = gpu::quad();
        glViewport(0, 0, 16, 16);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glViewport(0, 0, 64, 64);
        CHECK_GL();
        const uint8_t green[4] = {0, 255, 0, 255};
        const std::vector<uint8_t> pixels = gpu::readRGBA8(0, 0, 16, 16);
        bool all = true;
        for (int i = 0; i < 256; ++i) all = all && pixels[static_cast<size_t>(i) * 4 + 1] == 255 && pixels[static_cast<size_t>(i) * 4] == 0;
        if (!all) std::fprintf(stderr, "  format %s failed\n", c.type);
        CHECK(all);
        (void)green;
        glDeleteVertexArrays(1, &vao);
        glDeleteProgram(p);
        glDeleteTextures(1, &tex);
        glDeleteBuffers(1, &buffer);
    }
}

GPU_TEST_VERSION(gpu_gl4_image_cube_array_layers, 42) {
    // imageLoad of every layer-face of a cube map array (two cubes).
    std::vector<float> data(4 * 4 * 12 * 4);
    for (int layer = 0; layer < 12; ++layer)
        for (int i = 0; i < 16; ++i)
            for (int c = 0; c < 4; ++c) data[static_cast<size_t>((layer * 16 + i) * 4 + c)] = static_cast<float>(layer + 1);
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_CUBE_MAP_ARRAY, tex);
    glTexImage3D(GL_TEXTURE_CUBE_MAP_ARRAY, 0, GL_RGBA32F, 4, 4, 12, 0, GL_RGBA, GL_FLOAT, data.data());
    glTexParameteri(GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    {
        std::vector<float> back(data.size());
        glGetTexImage(GL_TEXTURE_CUBE_MAP_ARRAY, 0, GL_RGBA, GL_FLOAT, back.data());
        int wrong = -1;
        for (size_t i = 0; i < back.size() && wrong < 0; ++i)
            if (back[i] != data[i]) wrong = static_cast<int>(i / 64);
        if (wrong >= 0) std::fprintf(stderr, "  stored layer-face %d differs\n", wrong);
        CHECK(wrong < 0);
    }
    glBindImageTexture(0, tex, 0, GL_TRUE, 0, GL_READ_ONLY, GL_RGBA32F);
    const char* fs = R"(#version 420 core
layout(rgba32f, binding = 0) readonly uniform imageCubeArray g_image;
out vec4 o;
void main() {
    int bad = 0;
    for (int layer = 0; layer < 12; ++layer)
        if (imageLoad(g_image, ivec3(1, 1, layer)) != vec4(float(layer + 1))) bad |= 1 << layer;
    o = vec4(float(bad & 255) / 255.0, float(bad >> 8) / 255.0, 0.0, 1.0);
})";
    GLuint p = gpu::program(kQuadVs, fs);
    glUseProgram(p);
    GLuint vao = gpu::quad();
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    CHECK_GL();
    const std::vector<uint8_t> pixel = gpu::readRGBA8(8, 8, 1, 1);
    if (pixel[0] || pixel[1]) std::fprintf(stderr, "  bad layers mask: 0x%x\n", pixel[0] | (pixel[1] << 8));
    CHECK(pixel[0] == 0 && pixel[1] == 0);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
    glDeleteTextures(1, &tex);
}

GPU_TEST_VERSION(gpu_gl4_builtin_uniforms, 42) {
    // gl_DepthRange and gl_NumSamples are hidden default-block uniforms the
    // draw fills (glsl_upgrade.h lowerBuiltinUniforms).
    const char* fs = R"(#version 400 core
out vec4 o;
void main() { o = vec4(gl_DepthRange.near, gl_DepthRange.far, gl_DepthRange.diff, float(gl_NumSamples) / 4.0); })";
    GLuint p = gpu::program(kQuadVs, fs);
    glUseProgram(p);
    GLint count = 0;
    glGetProgramiv(p, GL_ACTIVE_UNIFORMS, &count);
    CHECK_EQ(count, 0);  // built-in uniforms are not active uniforms of the program
    GLuint quad = gpu::quad();
    glDepthRange(0.25, 0.75);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDepthRange(0.0, 1.0);
    const std::vector<uint8_t> px = gpu::readRGBA8(0, 0, 1, 1);
    CHECK(std::abs(px[0] - 64) <= 1);
    CHECK(std::abs(px[1] - 191) <= 1);
    CHECK(std::abs(px[2] - 128) <= 1);
    CHECK(std::abs(px[3] - 64) <= 1);  // one sample
    CHECK_GL();
    glDeleteVertexArrays(1, &quad);
    glDeleteProgram(p);
}
