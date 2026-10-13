// GL 4.6 features (contexts above 4.5 go through the development override
// until the version is advertised).
#include <climits>
#include <cstdlib>
#include <cstring>
#include <string>

#include "gpu.h"

namespace {

// A point at the centre of pixel (x, y) of the 64x64 target, its colour the
// draw parameters over 255 (exact in RGBA8).
const char* const kDrawParametersVs =
    "#version 460 core\n"
    "uniform int row;\n"
    "out vec4 color;\n"
    "void main() {\n"
    "    vec2 pixel = vec2(8 + 16 * gl_DrawID, 8 + 16 * row) + 0.5;\n"
    "    gl_Position = vec4(pixel / 32.0 - 1.0, 0.0, 1.0);\n"
    "    color = vec4(gl_DrawID, gl_BaseVertex, gl_BaseInstance, 255) / 255.0;\n"
    "}\n";
const char* const kColorFs = "#version 460 core\nin vec4 color;\nout vec4 o;\nvoid main() { o = color; }\n";

bool pixelIs(int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
    const uint8_t want[4] = {r, g, b, a};
    return gpu::near(gpu::readRGBA8(x, y, 1, 1).data(), want, 0);
}

}  // namespace

// GL_ARB_shader_draw_parameters: gl_DrawID in multi-draws, gl_BaseVertex of
// an elements draw (0 for array draws) and gl_BaseInstance.
GPU_TEST_VERSION(gpu_gl46_draw_parameters, 46) {
    GLuint p = gpu::program(kDrawParametersVs, kColorFs);
    glUseProgram(p);
    GLuint vao = 0, elements = 0;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    const GLint row = glGetUniformLocation(p, "row");
    glUniform1i(row, 0);
    const GLint first[3] = {0, 0, 0};
    const GLsizei count[3] = {1, 1, 1};
    glMultiDrawArrays(GL_POINTS, first, count, 3);
    glUniform1i(row, 1);
    glGenBuffers(1, &elements);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, elements);
    const uint32_t index = 0;
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(index), &index, GL_STATIC_DRAW);
    glDrawElementsBaseVertex(GL_POINTS, 1, GL_UNSIGNED_INT, nullptr, 5);
    glUniform1i(row, 2);
    glDrawArraysInstancedBaseInstance(GL_POINTS, 0, 1, 1, 3);
    CHECK_GL();
    for (int i = 0; i < 3; ++i) CHECK(pixelIs(8 + 16 * i, 8, static_cast<uint8_t>(i), 0, 0));
    CHECK(pixelIs(8, 24, 0, 5, 0));
    CHECK(pixelIs(8, 40, 0, 0, 3));
    glDeleteBuffers(1, &elements);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
}

// GL_ARB_indirect_parameters: the draw count from PARAMETER_BUFFER, clamped
// to maxdrawcount, and the errors.
GPU_TEST_VERSION(gpu_gl46_indirect_count, 46) {
    GLuint p = gpu::program(kDrawParametersVs, kColorFs);
    glUseProgram(p);
    GLuint vao = 0, buffers[2];
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    glUniform1i(glGetUniformLocation(p, "row"), 0);
    glGenBuffers(2, buffers);
    const uint32_t commands[4][4] = {{1, 1, 0, 0}, {1, 1, 0, 0}, {1, 1, 0, 0}, {1, 1, 0, 0}};  // count, instances, first, base instance
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, buffers[0]);
    glBufferData(GL_DRAW_INDIRECT_BUFFER, sizeof(commands), commands, GL_STATIC_DRAW);
    const uint32_t counts[2] = {7, 2};
    glBindBuffer(GL_PARAMETER_BUFFER, buffers[1]);
    glBufferData(GL_PARAMETER_BUFFER, sizeof(counts), counts, GL_STATIC_DRAW);
    GLint bound = 0;
    glGetIntegerv(GL_PARAMETER_BUFFER_BINDING, &bound);
    CHECK_EQ(bound, static_cast<GLint>(buffers[1]));
    glMultiDrawArraysIndirectCount(GL_POINTS, nullptr, 4, 3, 0);  // 2 draws
    CHECK_GL();
    CHECK(pixelIs(8, 8, 0, 0, 0));
    CHECK(pixelIs(24, 8, 1, 0, 0));
    CHECK(pixelIs(40, 8, 0, 0, 0, 0));
    glClear(GL_COLOR_BUFFER_BIT);
    glMultiDrawArraysIndirectCount(GL_POINTS, nullptr, 0, 3, 0);  // 7 clamped to 3
    CHECK(pixelIs(40, 8, 2, 0, 0));
    CHECK(pixelIs(56, 8, 0, 0, 0, 0));
    glMultiDrawArraysIndirectCount(GL_POINTS, nullptr, 2, 3, 0);
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_VALUE));
    glMultiDrawArraysIndirectCount(GL_POINTS, nullptr, 0, 5, 0);  // 5 commands overrun the indirect buffer
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_OPERATION));
    glBindBuffer(GL_PARAMETER_BUFFER, 0);
    glMultiDrawArraysIndirectCount(GL_POINTS, nullptr, 0, 3, 0);
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_OPERATION));
    glDeleteBuffers(2, buffers);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
}

// GL_ARB_transform_feedback_overflow_query: overflow only when a primitive
// does not fit.
GPU_TEST_VERSION(gpu_gl46_transform_feedback_overflow, 46) {
    GLuint p = gpu::program("#version 460 core\nlayout(xfb_buffer = 0, xfb_offset = 0) out float v;\n"
                            "void main() { v = float(gl_VertexID); gl_Position = vec4(0.0, 0.0, 0.0, 1.0); }\n",
                            "#version 460 core\nout vec4 o;\nvoid main() { o = vec4(1.0); }\n");
    glUseProgram(p);
    GLuint vao = 0, buffer = 0, queries[2];
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &buffer);
    glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, buffer);
    glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, 12, nullptr, GL_DYNAMIC_COPY);  // room for 3 points
    glGenQueries(2, queries);
    glEnable(GL_RASTERIZER_DISCARD);
    glBeginTransformFeedback(GL_POINTS);
    glBeginQuery(GL_TRANSFORM_FEEDBACK_OVERFLOW, queries[0]);
    glDrawArrays(GL_POINTS, 0, 2);
    glEndQuery(GL_TRANSFORM_FEEDBACK_OVERFLOW);
    glBeginQueryIndexed(GL_TRANSFORM_FEEDBACK_STREAM_OVERFLOW, 0, queries[1]);
    glDrawArrays(GL_POINTS, 0, 2);  // one of the two fits
    glEndQueryIndexed(GL_TRANSFORM_FEEDBACK_STREAM_OVERFLOW, 0);
    glEndTransformFeedback();
    glDisable(GL_RASTERIZER_DISCARD);
    CHECK_GL();
    GLuint overflow[2] = {9, 9};
    glGetQueryObjectuiv(queries[0], GL_QUERY_RESULT, &overflow[0]);
    glGetQueryObjectuiv(queries[1], GL_QUERY_RESULT, &overflow[1]);
    CHECK_EQ(overflow[0], 0u);
    CHECK_EQ(overflow[1], 1u);
    glBeginQueryIndexed(GL_TRANSFORM_FEEDBACK_OVERFLOW, 1, queries[0]);
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_VALUE));
    glDeleteQueries(2, queries);
    glDeleteBuffers(1, &buffer);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
}

// GL_ARB_pipeline_statistics_query: submitted vertices and primitives,
// clipping input, fragment shader invocations of a fullscreen quad, compute
// shader invocations of a dispatch.
GPU_TEST_VERSION(gpu_gl46_pipeline_statistics, 46) {
    GLuint p = gpu::program("#version 460 core\nlayout(location = 0) in vec2 position;\n"
                            "void main() { gl_Position = vec4(position, 0.0, 1.0); }\n",
                            "#version 460 core\nout vec4 o;\nvoid main() { o = vec4(1.0); }\n");
    glUseProgram(p);
    GLuint vao = gpu::quad();
    const GLenum targets[5] = {GL_VERTICES_SUBMITTED, GL_PRIMITIVES_SUBMITTED, GL_VERTEX_SHADER_INVOCATIONS,
                               GL_CLIPPING_INPUT_PRIMITIVES, GL_FRAGMENT_SHADER_INVOCATIONS};
    const GLuint64 want[5] = {4, 2, 4, 2, 64 * 64};
    for (int i = 0; i < 5; ++i) {
        GLuint query = 0;
        glGenQueries(1, &query);
        glBeginQuery(targets[i], query);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glEndQuery(targets[i]);
        GLuint64 result = 0;
        glGetQueryObjectui64v(query, GL_QUERY_RESULT, &result);
        CHECK_EQ(result, want[i]);
        glDeleteQueries(1, &query);
    }
    CHECK_GL();
    GLuint cs = glCreateShader(GL_COMPUTE_SHADER);
    const char* source = "#version 460 core\nlayout(local_size_x = 8, local_size_y = 2) in;\nvoid main() {}\n";
    glShaderSource(cs, 1, &source, nullptr);
    glCompileShader(cs);
    GLuint compute = glCreateProgram();
    glAttachShader(compute, cs);
    glLinkProgram(compute);
    glUseProgram(compute);
    GLuint query = 0;
    glGenQueries(1, &query);
    glBeginQuery(GL_COMPUTE_SHADER_INVOCATIONS, query);
    glDispatchCompute(3, 1, 1);
    glEndQuery(GL_COMPUTE_SHADER_INVOCATIONS);
    GLuint64 result = 0;
    glGetQueryObjectui64v(query, GL_QUERY_RESULT, &result);
    CHECK_EQ(result, static_cast<GLuint64>(3 * 8 * 2));
    CHECK_GL();
    glDeleteQueries(1, &query);
    glDeleteProgram(compute);
    glDeleteShader(cs);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
}

// GL_ARB_shader_atomic_counter_ops in a vertex shader with two counter
// buffers (the vertex stage keeps them in its argument buffer).
GPU_TEST_VERSION(gpu_gl46_atomic_counter_ops, 46) {
    GLuint p = gpu::program("#version 460 core\n"
                            "layout(binding = 0) uniform atomic_uint sum;\n"
                            "layout(binding = 1, offset = 4) uniform atomic_uint highest;\n"
                            "void main() {\n"
                            "    atomicCounterAdd(sum, 2u);\n"
                            "    atomicCounterMax(highest, uint(gl_VertexID) * 10u);\n"
                            "    gl_Position = vec4(0.0, 0.0, 0.0, 1.0);\n"
                            "}\n",
                            "#version 460 core\nout vec4 o;\nvoid main() { o = vec4(1.0); }\n");
    glUseProgram(p);
    GLuint vao = 0, buffers[2];
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(2, buffers);
    const uint32_t zero[2] = {0, 0};
    for (int i = 0; i < 2; ++i) {
        glBindBufferBase(GL_ATOMIC_COUNTER_BUFFER, static_cast<GLuint>(i), buffers[i]);
        glBufferData(GL_ATOMIC_COUNTER_BUFFER, sizeof(zero), zero, GL_DYNAMIC_COPY);
    }
    GLint limit = 0;
    glGetIntegerv(GL_MAX_VERTEX_ATOMIC_COUNTER_BUFFERS, &limit);
    CHECK(limit >= 2);
    glDrawArrays(GL_POINTS, 0, 4);
    CHECK_GL();
    uint32_t sum = 0, highest[2] = {};
    glBindBuffer(GL_ATOMIC_COUNTER_BUFFER, buffers[0]);
    glGetBufferSubData(GL_ATOMIC_COUNTER_BUFFER, 0, 4, &sum);
    glBindBuffer(GL_ATOMIC_COUNTER_BUFFER, buffers[1]);
    glGetBufferSubData(GL_ATOMIC_COUNTER_BUFFER, 0, 8, highest);
    CHECK_EQ(sum, 8u);
    CHECK_EQ(highest[1], 30u);
    glDeleteBuffers(2, buffers);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
}

// GL_ARB_shader_group_vote: votes every invocation agrees on.
GPU_TEST_VERSION(gpu_gl46_group_vote, 46) {
    GLuint p = gpu::program("#version 460 core\nlayout(location = 0) in vec2 position;\n"
                            "void main() { gl_Position = vec4(position, 0.0, 1.0); }\n",
                            "#version 460 core\nout vec4 o;\nvoid main() {\n"
                            "    bool inside = gl_FragCoord.x >= 0.0;\n"
                            "    o = vec4(anyInvocation(inside), allInvocations(inside), allInvocationsEqual(inside),\n"
                            "             anyInvocation(!inside) ? 0.0 : 1.0);\n}\n");
    glUseProgram(p);
    GLuint vao = gpu::quad();
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    CHECK_GL();
    CHECK(pixelIs(5, 50, 255, 255, 255));
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
}

// Atomic counter buffers report the stages that use them, the geometry
// stage included (its emulation runs as a compute kernel).
GPU_TEST_VERSION(gpu_gl46_atomic_counter_buffer_stages, 46) {
    const char* const common = "layout(binding = 0) uniform atomic_uint horizontal;\n"
                               "layout(binding = 1) uniform atomic_uint vertical;\n";
    const std::string vs = std::string("#version 460 core\n") + common +
                           "void main() { atomicCounterIncrement(vertical); atomicCounterIncrement(horizontal);\n"
                           "    gl_Position = vec4(0.0, 0.0, 0.0, 1.0); }\n";
    const std::string gs = std::string("#version 460 core\nlayout(points) in;\nlayout(points, max_vertices = 1) out;\n") + common +
                           "out vec4 color;\n"
                           "void main() { color = vec4(atomicCounter(vertical)); gl_Position = gl_in[0].gl_Position; EmitVertex(); }\n";
    const char* const fs = "#version 460 core\nin vec4 color;\nout vec4 o;\nvoid main() { o = color; }\n";
    GLuint p = glCreateProgram();
    const GLenum types[3] = {GL_VERTEX_SHADER, GL_GEOMETRY_SHADER, GL_FRAGMENT_SHADER};
    const char* sources[3] = {vs.c_str(), gs.c_str(), fs};
    for (int i = 0; i < 3; ++i) {
        GLuint s = glCreateShader(types[i]);
        glShaderSource(s, 1, &sources[i], nullptr);
        glCompileShader(s);
        glAttachShader(p, s);
        glDeleteShader(s);
    }
    glLinkProgram(p);
    GLint linked = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &linked);
    CHECK(linked);
    GLint buffers = 0;
    glGetProgramInterfaceiv(p, GL_ATOMIC_COUNTER_BUFFER, GL_ACTIVE_RESOURCES, &buffers);
    CHECK_EQ(buffers, 2);
    for (GLint i = 0; i < buffers; ++i) {
        const GLenum props[3] = {GL_BUFFER_BINDING, GL_REFERENCED_BY_VERTEX_SHADER, GL_REFERENCED_BY_GEOMETRY_SHADER};
        GLint values[3] = {-1, -1, -1};
        glGetProgramResourceiv(p, GL_ATOMIC_COUNTER_BUFFER, static_cast<GLuint>(i), 3, props, 3, nullptr, values);
        CHECK_EQ(values[1], 1);
        CHECK_EQ(values[2], values[0] == 1 ? 1 : 0);  // the geometry shader reads only `vertical`
    }
    CHECK_GL();
    glDeleteProgram(p);
}

// More images in one stage than Metal's 8 read_write textures: images the
// shader only reads or only writes (no qualifiers) are read-only or
// write-only Metal textures; one read and written atomically is read_write.
GPU_TEST_VERSION(gpu_gl46_many_images, 46) {
    const char* const source = "#version 460 core\n"
                               "layout(local_size_x = 1) in;\n"
                               "layout(binding = 0, r32ui) uniform uimage2D sources[6];\n"
                               "layout(binding = 6, r32ui) uniform uimage2D results[6];\n"
                               "layout(binding = 12, r32ui) uniform uimage2D total;\n"
                               "void main() {\n"
                               "    uint sum = 0u;\n"
                               "    for (int i = 0; i < 6; ++i) {\n"
                               "        uint v = imageLoad(sources[i], ivec2(0)).x;\n"
                               "        imageStore(results[i], ivec2(0), uvec4(v * 2u));\n"
                               "        sum += v;\n"
                               "    }\n"
                               "    imageAtomicAdd(total, ivec2(0), sum);\n"
                               "}\n";
    GLint units = 0, perStage = 0;
    glGetIntegerv(GL_MAX_IMAGE_UNITS, &units);
    glGetIntegerv(GL_MAX_COMPUTE_IMAGE_UNIFORMS, &perStage);
    CHECK(units >= 13);
    CHECK_EQ(perStage, 8);
    GLuint p = glCreateShaderProgramv(GL_COMPUTE_SHADER, 1, &source);
    GLint linked = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[2048] = {};
        glGetProgramInfoLog(p, sizeof(log), nullptr, log);
        std::fprintf(stderr, "  link failed:\n%s\n", log);
    }
    CHECK(linked);
    GLuint textures[13];
    glGenTextures(13, textures);
    for (GLuint i = 0; i < 13; ++i) {
        glBindTexture(GL_TEXTURE_2D, textures[i]);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_R32UI, 1, 1);
        const GLuint value = i < 6 ? i + 1 : i == 12 ? 5 : 0;
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RED_INTEGER, GL_UNSIGNED_INT, &value);
    }
    glBindImageTextures(0, 13, textures);
    glUseProgram(p);
    glDispatchCompute(1, 1, 1);
    glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);
    CHECK_GL();
    for (GLuint i = 6; i < 13; ++i) {
        GLuint value = 0;
        glBindTexture(GL_TEXTURE_2D, textures[i]);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RED_INTEGER, GL_UNSIGNED_INT, &value);
        CHECK_EQ(value, i < 12 ? 2 * (i - 5) : 26u);
    }
    glDeleteTextures(13, textures);
    glDeleteProgram(p);
}

// GL_ARB_shader_viewport_layer_array: a vertex shader picks the layer of a
// layered framebuffer and the viewport.
GPU_TEST_VERSION(gpu_gl46_vertex_layer_viewport, 46) {
    GLuint p = gpu::program("#version 460 core\n#extension GL_ARB_shader_viewport_layer_array : require\n"
                            "layout(location = 0) in vec2 position;\n"
                            "uniform int layer;\n"
                            "void main() {\n"
                            "    gl_Position = vec4(position, 0.0, 1.0);\n"
                            "    gl_Layer = layer;\n"
                            "    gl_ViewportIndex = layer;\n"
                            "}\n",
                            "#version 460 core\nuniform vec4 color;\nout vec4 o;\nvoid main() { o = color; }\n");
    glUseProgram(p);
    GLuint tex = 0, fbo = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D_ARRAY, tex);
    glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, GL_RGBA8, 64, 64, 2);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, tex, 0);
    CHECK_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE));
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    // Viewport 0 is the left half, viewport 1 the right half.
    glViewportIndexedf(0, 0, 0, 32, 64);
    glViewportIndexedf(1, 32, 0, 32, 64);
    GLuint vao = gpu::quad();
    const GLint layer = glGetUniformLocation(p, "layer"), color = glGetUniformLocation(p, "color");
    glUniform1i(layer, 0);
    glUniform4f(color, 1, 0, 0, 1);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glUniform1i(layer, 1);
    glUniform4f(color, 0, 1, 0, 1);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    CHECK_GL();
    for (int l = 0; l < 2; ++l) {
        glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, tex, 0, l);
        CHECK(pixelIs(16, 32, l == 0 ? 255 : 0, 0, 0, l == 0 ? 255 : 0));
        CHECK(pixelIs(48, 32, 0, l == 1 ? 255 : 0, 0, l == 1 ? 255 : 0));
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &tex);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
}

// GL_EXT_texture_shadow_lod: textureLod and texture with a bias on a 2D
// array shadow sampler.
GPU_TEST_VERSION(gpu_gl46_texture_shadow_lod, 46) {
    GLuint p = gpu::program("#version 460 core\nlayout(location = 0) in vec2 position;\n"
                            "void main() { gl_Position = vec4(position, 0.0, 1.0); }\n",
                            "#version 460 core\n#extension GL_EXT_texture_shadow_lod : require\n"
                            "uniform sampler2DArrayShadow t;\nout vec4 o;\n"
                            "void main() {\n"
                            "    float lod = textureLod(t, vec4(0.5, 0.5, 1.0, 0.5), 0.0);\n"
                            "    float biased = texture(t, vec4(0.5, 0.5, 0.0, 0.5), 0.0);\n"
                            "    o = vec4(lod, biased, 0.0, 1.0);\n"
                            "}\n");
    glUseProgram(p);
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D_ARRAY, tex);
    // Layer 0 at depth 0.25, layer 1 at 0.75: a reference of 0.5 passes
    // GL_LEQUAL against layer 1 only.
    const float depths[2] = {0.25f, 0.75f};
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_DEPTH_COMPONENT32F, 1, 1, 2, 0, GL_DEPTH_COMPONENT, GL_FLOAT, depths);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
    GLuint vao = gpu::quad();
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    CHECK_GL();
    CHECK(pixelIs(32, 32, 255, 0, 0));
    glDeleteVertexArrays(1, &vao);
    glDeleteTextures(1, &tex);
    glDeleteProgram(p);
}

// wined3d's sRGB views of typeless BC textures: a GL_COMPRESSED_RGBA_S3TC_DXT1
// texture viewed as GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1 samples its texels.
GPU_TEST_VERSION(gpu_gl46_compressed_srgb_view, 46) {
    GLuint p = gpu::program("#version 460 core\nlayout(location = 0) in vec2 position;\n"
                            "void main() { gl_Position = vec4(position, 0.0, 1.0); }\n",
                            "#version 460 core\nuniform sampler2D t;\nout vec4 o;\n"
                            "void main() { o = texelFetch(t, ivec2(1, 1), 0); }\n");
    glUseProgram(p);
    // One DXT1 block: colour 0 white, colour 1 black, every index 0 (white).
    const uint8_t block[8] = {0xff, 0xff, 0x00, 0x00, 0, 0, 0, 0};
    for (GLenum viewFormat : {GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT}) {
        GLuint parent = 0, view = 0;
        glGenTextures(1, &parent);
        glBindTexture(GL_TEXTURE_2D, parent);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, 4, 4);
        glCompressedTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 4, 4, GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, sizeof(block), block);
        glGenTextures(1, &view);
        glTextureView(view, GL_TEXTURE_2D, parent, viewFormat, 0, 1, 0, 1);
        glBindTexture(GL_TEXTURE_2D, view);
        GLuint vao = gpu::quad();
        glClearColor(1, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        CHECK_GL();
        const std::vector<uint8_t> px = gpu::readRGBA8(32, 32, 1, 1);
        if (px[0] != 255 || px[1] != 255 || px[2] != 255)
            std::fprintf(stderr, "  view 0x%x: %u %u %u %u\n", viewFormat, px[0], px[1], px[2], px[3]);
        CHECK(pixelIs(32, 32, 255, 255, 255));
        glDeleteVertexArrays(1, &vao);
        glDeleteTextures(1, &view);
        glDeleteTextures(1, &parent);
    }
    glDeleteProgram(p);
}

// wined3d's per-instance elements with step rate 0: divisor UINT_MAX, so
// every instance reads the element at the base instance.
GPU_TEST_VERSION(gpu_gl46_divisor_uint_max, 46) {
    GLuint p = gpu::program("#version 460 core\nlayout(location = 1) in float value;\nout float v;\n"
                            "uniform int row;\n"
                            "void main() {\n"
                            "    v = value;\n"
                            "    gl_Position = vec4((vec2(8 + 16 * gl_InstanceID, 8 + 16 * row) + 0.5) / 32.0 - 1.0, 0.0, 1.0);\n"
                            "}\n",
                            "#version 460 core\nin float v;\nout vec4 o;\nvoid main() { o = vec4(v, 0.0, 0.0, 1.0); }\n");
    glUseProgram(p);
    GLuint vao = 0, buffer = 0;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &buffer);
    glBindBuffer(GL_ARRAY_BUFFER, buffer);
    // One normalized byte per instance (stride 1, which Metal cannot fetch:
    // the attribute is converted).
    const uint8_t values[8] = {64, 128, 192, 255, 32, 96, 160, 224};
    glBufferData(GL_ARRAY_BUFFER, sizeof(values), values, GL_STATIC_DRAW);
    glVertexAttribPointer(1, 1, GL_UNSIGNED_BYTE, GL_TRUE, 1, nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribDivisor(1, 0xffffffffu);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    const GLint row = glGetUniformLocation(p, "row");
    glUniform1i(row, 0);
    glDrawArraysInstancedBaseInstance(GL_POINTS, 0, 1, 4, 0);
    glUniform1i(row, 1);
    glDrawArraysInstancedBaseInstance(GL_POINTS, 0, 1, 4, 4);
    CHECK_GL();
    for (int i = 0; i < 4; ++i) {
        const int got0 = gpu::readRGBA8(8 + 16 * i, 8, 1, 1)[0], got1 = gpu::readRGBA8(8 + 16 * i, 24, 1, 1)[0];
        if (got0 != 64 || got1 != 32) std::fprintf(stderr, "  instance %d: %d %d\n", i, got0, got1);
        CHECK(got0 == 64 && got1 == 32);
    }
    glDeleteBuffers(1, &buffer);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
}

// BPTC 3D textures (GL 4.6 table 8.14; wined3d's BC7 3D textures): two
// slices, red and green, each sampled back.
GPU_TEST_VERSION(gpu_gl46_bptc_3d, 46) {
    GLuint p = gpu::program("#version 460 core\nlayout(location = 0) in vec2 position;\n"
                            "void main() { gl_Position = vec4(position, 0.0, 1.0); }\n",
                            "#version 460 core\nuniform sampler3D t;\nuniform int slice;\nout vec4 o;\n"
                            "void main() { o = texelFetch(t, ivec3(1, 1, slice), 0); }\n");
    glUseProgram(p);
    uint8_t texels[2][16][4];
    for (int i = 0; i < 16; ++i) {
        const uint8_t red[4] = {255, 0, 0, 255}, green[4] = {0, 255, 0, 255};
        std::memcpy(texels[0][i], red, 4);
        std::memcpy(texels[1][i], green, 4);
    }
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_3D, tex);
    glTexImage3D(GL_TEXTURE_3D, 0, GL_COMPRESSED_RGBA_BPTC_UNORM, 4, 4, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    CHECK_GL();
    GLint compressed = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_3D, 0, GL_TEXTURE_COMPRESSED, &compressed);
    CHECK(compressed);
    // Each depth reads back as itself: the blocks (red, then green), and
    // decoded, whole or one depth at a time.
    uint8_t blocks[32] = {};
    glGetCompressedTexImage(GL_TEXTURE_3D, 0, blocks);
    CHECK(std::memcmp(blocks, blocks + 16, 16) != 0);
    uint8_t decoded[2][16][4] = {};
    glGetTexImage(GL_TEXTURE_3D, 0, GL_RGBA, GL_UNSIGNED_BYTE, decoded);
    CHECK(gpu::near(decoded[0][5], texels[0][5], 2) && gpu::near(decoded[1][5], texels[1][5], 2));
    uint8_t second[16][4] = {};
    glGetTextureSubImage(tex, 0, 0, 0, 1, 4, 4, 1, GL_RGBA, GL_UNSIGNED_BYTE, sizeof(second), second);
    CHECK(gpu::near(second[5], texels[1][5], 2));
    CHECK_GL();
    // The blocks make the same texture again.
    GLuint copy = 0;
    glGenTextures(1, &copy);
    glBindTexture(GL_TEXTURE_3D, copy);
    glCompressedTexImage3D(GL_TEXTURE_3D, 0, GL_COMPRESSED_RGBA_BPTC_UNORM, 4, 4, 2, 0, sizeof(blocks), blocks);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    CHECK_GL();
    GLuint vao = gpu::quad();
    for (int slice = 0; slice < 2; ++slice) {
        glUniform1i(glGetUniformLocation(p, "slice"), slice);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        const uint8_t want[4] = {static_cast<uint8_t>(slice ? 0 : 255), static_cast<uint8_t>(slice ? 255 : 0), 0, 255};
        CHECK(gpu::near(gpu::readRGBA8(32, 32, 1, 1).data(), want, 2));
    }
    CHECK_GL();
    glDeleteVertexArrays(1, &vao);
    glDeleteTextures(1, &tex);
    glDeleteTextures(1, &copy);
    glDeleteProgram(p);
}

// wined3d's geometry shaders redeclare "invariant gl_Position;". Layered
// rendering from an instanced geometry shader: invocation i draws layer i.
GPU_TEST_VERSION(gpu_gl46_geometry_invariant_layers, 46) {
    const char* const sources[3] = {
        "#version 460 core\nlayout(location = 0) in vec2 position;\n"
        "void main() { gl_Position = vec4(position, 0.0, 1.0); }\n",
        "#version 460 core\nlayout(triangles, invocations = 2) in;\nlayout(triangle_strip, max_vertices = 3) out;\n"
        "invariant gl_Position;\nout float value;\n"
        "void main() {\n"
        "    for (int i = 0; i < 3; ++i) {\n"
        "        gl_Position = gl_in[i].gl_Position;\n"
        "        gl_Layer = gl_InvocationID;\n"
        "        value = float(gl_InvocationID + 1) * 0.25;\n"
        "        EmitVertex();\n"
        "    }\n"
        "}\n",
        "#version 460 core\nin float value;\nout vec4 o;\nvoid main() { o = vec4(value, 0.0, 0.0, 1.0); }\n"};
    const GLenum types[3] = {GL_VERTEX_SHADER, GL_GEOMETRY_SHADER, GL_FRAGMENT_SHADER};
    GLuint p = glCreateProgram();
    for (int i = 0; i < 3; ++i) {
        GLuint s = glCreateShader(types[i]);
        glShaderSource(s, 1, &sources[i], nullptr);
        glCompileShader(s);
        glAttachShader(p, s);
        glDeleteShader(s);
    }
    glLinkProgram(p);
    GLint linked = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[4096] = {};
        glGetProgramInfoLog(p, sizeof(log), nullptr, log);
        std::fprintf(stderr, "  link failed:\n%s\n", log);
    }
    CHECK(linked);
    glUseProgram(p);
    GLuint tex = 0, fbo = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D_ARRAY, tex);
    glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, GL_RGBA8, 64, 64, 2);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, tex, 0);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    GLuint vao = gpu::quad();
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    CHECK_GL();
    for (int l = 0; l < 2; ++l) {
        glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, tex, 0, l);
        const uint8_t want[4] = {static_cast<uint8_t>(l ? 128 : 64), 0, 0, 255};
        CHECK(gpu::near(gpu::readRGBA8(32, 32, 1, 1).data(), want, 1));
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &tex);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
}

// wined3d's literal constants: global consts initialized from bit casts keep
// their exact values.
GPU_TEST_VERSION(gpu_gl46_bitcast_constants, 46) {
    GLuint p = gpu::program("#version 460 core\nlayout(location = 0) in vec2 position;\n"
                            "void main() { gl_Position = vec4(position, 0.0, 1.0); }\n",
                            "#version 440\nconst vec4 lc0 = intBitsToFloat(ivec4(0x3f800000, 0, 0x3f000000, 0x3f800000));\n"
                            "out vec4 o;\nvoid main() { o = lc0; }\n");
    glUseProgram(p);
    GLuint vao = gpu::quad();
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    CHECK_GL();
    const uint8_t want[4] = {255, 0, 128, 255};
    CHECK(gpu::near(gpu::readRGBA8(32, 32, 1, 1).data(), want, 1));
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
}

// A multisample sRGB resolve (glBlitFramebuffer, wined3d's ResolveSubresource)
// with GL_FRAMEBUFFER_SRGB averages linear values: half the samples white,
// half black, resolves to sRGB(0.5) = 188, not 128.
GPU_TEST_VERSION(gpu_gl46_srgb_resolve, 46) {
    GLuint p = gpu::program("#version 460 core\nlayout(location = 0) in vec2 position;\n"
                            "void main() { gl_Position = vec4(position, 0.0, 1.0); }\n",
                            "#version 460 core\nout vec4 o;\nvoid main() { o = vec4(1.0); }\n");
    GLuint rb[2], fbo[2];
    glGenRenderbuffers(2, rb);
    glGenFramebuffers(2, fbo);
    glBindRenderbuffer(GL_RENDERBUFFER, rb[0]);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, 4, GL_SRGB8_ALPHA8, 64, 64);
    glBindRenderbuffer(GL_RENDERBUFFER, rb[1]);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_SRGB8_ALPHA8, 64, 64);
    for (int i = 0; i < 2; ++i) {
        glBindFramebuffer(GL_FRAMEBUFFER, fbo[i]);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb[i]);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, fbo[0]);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_SAMPLE_MASK);
    glSampleMaski(0, 0x3);
    glUseProgram(p);
    GLuint vao = gpu::quad();
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisable(GL_SAMPLE_MASK);
    glEnable(GL_FRAMEBUFFER_SRGB);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo[0]);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo[1]);
    glBlitFramebuffer(0, 0, 64, 64, 0, 0, 64, 64, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glDisable(GL_FRAMEBUFFER_SRGB);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo[1]);
    CHECK_GL();
    const std::vector<uint8_t> px = gpu::readRGBA8(32, 32, 1, 1);
    if (px[0] < 186 || px[0] > 190) std::fprintf(stderr, "  resolved %u\n", px[0]);
    CHECK(px[0] >= 186 && px[0] <= 190);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(2, fbo);
    glDeleteRenderbuffers(2, rb);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
}

// GL_EXT_texture_sRGB_decode: an sRGB texel 188 samples as 0.5 (decoded) or
// as 188 / 255 with GL_SKIP_DECODE_EXT, set on the texture or a sampler.
GPU_TEST_VERSION(gpu_gl46_srgb_decode, 46) {
    GLuint p = gpu::program("#version 460 core\nlayout(location = 0) in vec2 position;\n"
                            "void main() { gl_Position = vec4(position, 0.0, 1.0); }\n",
                            "#version 460 core\nuniform sampler2D t;\nout vec4 o;\n"
                            "void main() { o = vec4(texture(t, vec2(0.5)).rgb, 1.0); }\n");
    glUseProgram(p);
    GLuint tex = 0, sampler = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    const uint8_t texel[4] = {188, 188, 188, 255};
    glTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB8_ALPHA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, texel);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    GLuint vao = gpu::quad();
    auto red = [&]() {
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        return static_cast<int>(gpu::readRGBA8(32, 32, 1, 1)[0]);
    };
    CHECK(std::abs(red() - 128) <= 1);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SRGB_DECODE_EXT, GL_SKIP_DECODE_EXT);
    GLint value = 0;
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SRGB_DECODE_EXT, &value);
    CHECK_EQ(value, GL_SKIP_DECODE_EXT);
    CHECK(std::abs(red() - 188) <= 1);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SRGB_DECODE_EXT, GL_DECODE_EXT);
    glGenSamplers(1, &sampler);
    glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glSamplerParameteri(sampler, GL_TEXTURE_SRGB_DECODE_EXT, GL_SKIP_DECODE_EXT);
    glBindSampler(0, sampler);
    CHECK(std::abs(red() - 188) <= 1);
    glBindSampler(0, 0);
    CHECK(std::abs(red() - 128) <= 1);
    CHECK_GL();
    glDeleteSamplers(1, &sampler);
    glDeleteVertexArrays(1, &vao);
    glDeleteTextures(1, &tex);
    glDeleteProgram(p);
}

// Occlusion queries count every draw between Begin and End (wined3d's
// test_occlusion_query: 100 draws over an 8192 x 8192 target, a uniform
// buffer update before each, more samples than 32 bits hold).
GPU_TEST_VERSION(gpu_gl46_occlusion_many_draws, 46) {
    GLuint p = gpu::program("#version 460 core\nlayout(location = 0) in vec2 position;\n"
                            "void main() { gl_Position = vec4(position, 0.0, 1.0); }\n",
                            "#version 460 core\nlayout(std140, binding = 0) uniform C { vec4 color; };\nout vec4 o;\n"
                            "void main() { o = color; }\n");
    glUseProgram(p);
    GLuint tex = 0, fbo = 0, ubo = 0, vao = gpu::quad(), query = 0;
    glGenBuffers(1, &ubo);
    glBindBufferBase(GL_UNIFORM_BUFFER, 0, ubo);
    const float red[4] = {1, 0, 0, 1};
    glBufferData(GL_UNIFORM_BUFFER, sizeof(red), red, GL_DYNAMIC_DRAW);
    glGenQueries(1, &query);
    // As wined3d does: the target is created and cleared inside the query.
    glBeginQuery(GL_SAMPLES_PASSED, query);
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_R8, 8192, 8192);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    glViewport(0, 0, 8192, 8192);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    // wined3d draws through a second framebuffer object on the same texture.
    GLuint drawFbo = 0;
    glGenFramebuffers(1, &drawFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, drawFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    // A buffer copy between draws ends the render pass each time, so each
    // draw counts in a pass of its own (the first in visibility slot 0).
    GLuint copies[2] = {};
    glGenBuffers(2, copies);
    glBindBuffer(GL_COPY_READ_BUFFER, copies[0]);
    glBufferData(GL_COPY_READ_BUFFER, 16, red, GL_STATIC_DRAW);
    glBindBuffer(GL_COPY_WRITE_BUFFER, copies[1]);
    glBufferData(GL_COPY_WRITE_BUFFER, 16, nullptr, GL_STATIC_DRAW);
    for (int i = 0; i < 100; ++i) {
        glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(red), red);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        if (i % 2 == 0) glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 0, 0, 16);
    }
    glEndQuery(GL_SAMPLES_PASSED);
    GLuint64 samples = 0;
    glGetQueryObjectui64v(query, GL_QUERY_RESULT, &samples);
    const GLuint64 want = 100ull * 8192 * 8192;
    if (samples != want) std::fprintf(stderr, "  %llu, expected %llu\n", (unsigned long long)samples, (unsigned long long)want);
    CHECK(samples == want);
    CHECK_GL();
    glViewport(0, 0, 64, 64);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &drawFbo);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &tex);
    glDeleteBuffers(1, &ubo);
    glDeleteBuffers(2, copies);
    glDeleteQueries(1, &query);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
}

// A framebuffer blit inside an occlusion query is not a draw: a scaled,
// filtered blit into the framebuffer being drawn to counts nothing.
GPU_TEST_VERSION(gpu_gl46_occlusion_ignores_blit, 46) {
    GLuint p = gpu::program("#version 460 core\nlayout(location = 0) in vec2 position;\n"
                            "void main() { gl_Position = vec4(position, 0.0, 1.0); }\n",
                            "#version 460 core\nout vec4 o;\nvoid main() { o = vec4(1.0); }\n");
    glUseProgram(p);
    GLuint vao = gpu::quad(), tex[2] = {}, fbo[2] = {}, query = 0;
    glGenTextures(2, tex);
    glGenFramebuffers(2, fbo);
    for (int i = 0; i < 2; ++i) {
        glBindTexture(GL_TEXTURE_2D, tex[i]);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, i ? 32 : 64, i ? 32 : 64);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex[i], 0);
    }
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo[0]);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo[1]);
    glViewport(0, 0, 64, 64);
    glGenQueries(1, &query);
    glBeginQuery(GL_SAMPLES_PASSED, query);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBlitFramebuffer(0, 0, 32, 32, 0, 0, 64, 64, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glEndQuery(GL_SAMPLES_PASSED);
    GLuint64 samples = 0;
    glGetQueryObjectui64v(query, GL_QUERY_RESULT, &samples);
    if (samples != 2 * 64 * 64) std::fprintf(stderr, "  %llu samples, expected %d\n", (unsigned long long)samples, 2 * 64 * 64);
    CHECK(samples == 2 * 64 * 64);
    CHECK_GL();
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(2, fbo);
    glDeleteTextures(2, tex);
    glDeleteQueries(1, &query);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
}

// textureSize and imageSize of a 100-byte GL_R16F buffer texture: 50 texels
// (wined3d's bufinfo of typed R16_FLOAT views).
GPU_TEST_VERSION(gpu_gl46_r16f_buffer_size, 46) {
    const char* source = "#version 460 core\nlayout(local_size_x = 1) in;\n"
                         "uniform samplerBuffer t;\nlayout(r16f, binding = 0) uniform imageBuffer img;\n"
                         "layout(std430, binding = 0) buffer Out { int sizes[2]; };\n"
                         "void main() { sizes[0] = textureSize(t); sizes[1] = imageSize(img); }\n";
    GLuint p = glCreateShaderProgramv(GL_COMPUTE_SHADER, 1, &source);
    GLint linked = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &linked);
    CHECK(linked);
    glUseProgram(p);
    GLuint buffers[2], tex = 0;
    glGenBuffers(2, buffers);
    glBindBuffer(GL_TEXTURE_BUFFER, buffers[0]);
    glBufferData(GL_TEXTURE_BUFFER, 100, nullptr, GL_STATIC_DRAW);
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_BUFFER, tex);
    glTexBuffer(GL_TEXTURE_BUFFER, GL_R16F, buffers[0]);
    glBindImageTexture(0, tex, 0, GL_FALSE, 0, GL_READ_WRITE, GL_R16F);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, buffers[1]);
    const GLint zero[2] = {0, 0};
    glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(zero), zero, GL_DYNAMIC_READ);
    glDispatchCompute(1, 1, 1);
    glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
    GLint sizes[2] = {};
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(sizes), sizes);
    if (sizes[0] != 50 || sizes[1] != 50) std::fprintf(stderr, "  textureSize %d, imageSize %d\n", sizes[0], sizes[1]);
    CHECK(sizes[0] == 50 && sizes[1] == 50);
    CHECK_GL();
    glDeleteTextures(1, &tex);
    glDeleteBuffers(2, buffers);
    glDeleteProgram(p);
}

// glClearTexImage without data stores zeros as the format would store them
// (an RGB texture's stored alpha stays 1); a multisample 16-bit depth
// texture clears to the depth given; regions past the image are refused.
GPU_TEST_VERSION(gpu_gl46_clear_texture_values, 46) {
    GLuint tex[3] = {}, fbo[2] = {};
    glGenTextures(3, tex);
    glGenFramebuffers(2, fbo);
    glBindTexture(GL_TEXTURE_2D, tex[0]);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGB8, 4, 4);
    glClearTexImage(tex[0], 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
    glClearTexSubImage(tex[0], 0, 1, 0, 0, INT_MAX, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_OPERATION));
    glBindTexture(GL_TEXTURE_2D, tex[1]);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 4, 4);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo[0]);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex[0], 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo[1]);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex[1], 0);
    glBlitFramebuffer(0, 0, 4, 4, 0, 0, 4, 4, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo[1]);
    uint8_t pixel[4] = {1, 1, 1, 1};
    glReadPixels(1, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    const uint8_t black[4] = {0, 0, 0, 255};
    CHECK(gpu::near(pixel, black, 0));
    CHECK_GL();
    // Multisample GL_DEPTH_COMPONENT16 cleared to 1.0, read by a shader.
    glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, tex[2]);
    glTexStorage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, 4, GL_DEPTH_COMPONENT16, 4, 4, GL_TRUE);
    const float one = 1.0f;
    glClearTexImage(tex[2], 0, GL_DEPTH_COMPONENT, GL_FLOAT, &one);
    CHECK_GL();
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    GLuint p = gpu::program("#version 460 core\nlayout(location = 0) in vec2 position;\n"
                            "void main() { gl_Position = vec4(position, 0.0, 1.0); }\n",
                            "#version 460 core\nuniform sampler2DMS t;\nout vec4 o;\n"
                            "void main() { o = vec4(texelFetch(t, ivec2(1, 1), 2).r); }\n");
    glUseProgram(p);
    GLuint vao = gpu::quad();
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    const uint8_t white[4] = {255, 255, 255, 255};
    CHECK(gpu::near(gpu::readRGBA8(32, 32, 1, 1).data(), white, 1));
    CHECK_GL();
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
    glDeleteFramebuffers(2, fbo);
    glDeleteTextures(3, tex);
}

// Emulated clamp-to-border: a projective shadow lookup compares the
// reference divided by q, and NEAREST_MIPMAP_LINEAR outside the image is the
// border (the nearest texels of both levels are outside).
GPU_TEST_VERSION(gpu_gl46_border_proj_mip_nearest, 46) {
    const char* cs = R"(#version 460 core
layout(local_size_x = 1) in;
layout(binding = 0) uniform sampler2DShadow depthTex;
layout(binding = 1) uniform sampler2D mipTex;
layout(std430, binding = 0) buffer Out { vec4 f[2]; };
void main() {
    f[0] = vec4(textureProj(depthTex, vec4(-1.0, 1.0, 0.8, 2.0)));
    f[1] = textureLod(mipTex, vec2(-0.1, 0.5), 0.5);
})";
    GLuint shader = glCreateShader(GL_COMPUTE_SHADER);
    glShaderSource(shader, 1, &cs, nullptr);
    glCompileShader(shader);
    GLuint p = glCreateProgram();
    glAttachShader(p, shader);
    glLinkProgram(p);
    glDeleteShader(shader);
    GLint linked = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &linked);
    CHECK(linked);
    GLuint tex[2];
    glGenTextures(2, tex);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex[0]);
    const float depths[4] = {0.9f, 0.9f, 0.9f, 0.9f};
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, 2, 2, 0, GL_DEPTH_COMPONENT, GL_FLOAT, depths);
    const float depthBorder[4] = {0.5f, 0.0f, 0.0f, 0.0f};
    glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, depthBorder);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, tex[1]);
    glTexStorage2D(GL_TEXTURE_2D, 2, GL_RGBA8, 2, 2);
    const uint8_t white[16] = {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255};
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 2, 2, GL_RGBA, GL_UNSIGNED_BYTE, white);
    glTexSubImage2D(GL_TEXTURE_2D, 1, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, white);
    const float color[4] = {0.25f, 0.5f, 0.75f, 1.0f};
    glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, color);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    glActiveTexture(GL_TEXTURE0);
    CHECK_GL();
    GLuint buffer = 0;
    glGenBuffers(1, &buffer);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, buffer);
    const float init[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
    glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(init), init, GL_DYNAMIC_READ);
    glUseProgram(p);
    glDispatchCompute(1, 1, 1);
    glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
    float out[8];
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(out), out);
    if (std::fabs(out[0] - 1.0f) > 0.01f) std::fprintf(stderr, "  projective comparison %g\n", out[0]);
    CHECK(std::fabs(out[0] - 1.0f) < 0.01f);  // 0.8 / 2 <= border depth 0.5
    for (int k = 0; k < 4; ++k) {
        if (std::fabs(out[4 + k] - color[k]) > 0.01f) std::fprintf(stderr, "  mip nearest [%d] %g\n", k, out[4 + k]);
        CHECK(std::fabs(out[4 + k] - color[k]) < 0.01f);
    }
    CHECK_GL();
    glDeleteBuffers(1, &buffer);
    glDeleteTextures(2, tex);
    glDeleteProgram(p);
}

// A renderable view of a texture whose own format is not (GL_RGB9_E5 seen
// as GL_R32UI) can be drawn to; a linearly filtered stencil texture is
// incomplete (GL 4.6 section 8.17).
GPU_TEST_VERSION(gpu_gl46_view_render_stencil_complete, 46) {
    GLuint tex[3] = {}, fbo = 0;
    glGenTextures(3, tex);
    glBindTexture(GL_TEXTURE_2D, tex[0]);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGB9_E5, 4, 4);
    glTextureView(tex[1], GL_TEXTURE_2D, tex[0], GL_R32UI, 0, 1, 0, 1);
    CHECK_GL();
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex[1], 0);
    CHECK_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE));
    const GLuint value[4] = {0x12345678u, 0, 0, 0};
    glClearBufferuiv(GL_COLOR, 0, value);
    GLuint got = 0;
    glReadPixels(1, 1, 1, 1, GL_RED_INTEGER, GL_UNSIGNED_INT, &got);
    CHECK_EQ(got, 0x12345678u);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    CHECK_GL();
    // GL_STENCIL_INDEX8 with the default (linear) magnification filter.
    glBindTexture(GL_TEXTURE_2D, tex[2]);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_STENCIL_INDEX8, 4, 4);
    const uint8_t stencil[16] = {7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7};
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 4, 4, GL_STENCIL_INDEX, GL_UNSIGNED_BYTE, stencil);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    CHECK_GL();
    GLuint p = gpu::program("#version 460 core\nlayout(location = 0) in vec2 position;\n"
                            "void main() { gl_Position = vec4(position, 0.0, 1.0); }\n",
                            "#version 460 core\nuniform usampler2D t;\nout vec4 o;\n"
                            "void main() { o = vec4(texelFetch(t, ivec2(1, 1), 0)) / 255.0; }\n");
    glUseProgram(p);
    GLuint vao = gpu::quad();
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    const uint8_t incomplete[4] = {0, 0, 0, 1};  // (0, 0, 0, 1) of an incomplete unsigned texture
    CHECK(gpu::near(gpu::readRGBA8(32, 32, 1, 1).data(), incomplete, 1));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    const uint8_t seven[4] = {7, 0, 0, 1};
    CHECK(gpu::near(gpu::readRGBA8(32, 32, 1, 1).data(), seven, 1));
    CHECK_GL();
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(3, tex);
}

// Direct state access corners: a cube map face attached by layer, the
// texture target query, a glGenBuffers name never bound as a vertex buffer,
// INVALID_VALUE for a buffer that does not exist, and a cube map's faces
// written and read as one image with images skipped.
GPU_TEST_VERSION(gpu_gl46_dsa_corners, 46) {
    GLuint cube = 0, fbo = 0;
    glCreateTextures(GL_TEXTURE_CUBE_MAP, 1, &cube);
    glTextureStorage2D(cube, 1, GL_RGBA8, 4, 4);
    glCreateFramebuffers(1, &fbo);
    glNamedFramebufferTextureLayer(fbo, GL_COLOR_ATTACHMENT0, cube, 0, 3);
    CHECK_GL();
    CHECK_EQ(glCheckNamedFramebufferStatus(fbo, GL_FRAMEBUFFER), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE));
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glClearColor(0, 0, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glNamedFramebufferTextureLayer(fbo, GL_COLOR_ATTACHMENT0, cube, 0, 6);
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_VALUE));
    GLint target = 0;
    glGetTextureParameteriv(cube, GL_TEXTURE_TARGET, &target);
    CHECK_EQ(target, GL_TEXTURE_CUBE_MAP);
    // Faces 0..5 read as the images of one 3D image after one skipped image.
    std::vector<uint8_t> faces(7 * 4 * 4 * 4, 0xee);
    glPixelStorei(GL_PACK_SKIP_IMAGES, 1);
    glGetTextureImage(cube, 0, GL_RGBA, GL_UNSIGNED_BYTE, static_cast<GLsizei>(faces.size()), faces.data());
    glPixelStorei(GL_PACK_SKIP_IMAGES, 0);
    CHECK_GL();
    CHECK(faces[0] == 0xee);                               // the skipped image
    const uint8_t blue[4] = {0, 0, 255, 255};
    CHECK(gpu::near(&faces[(1 + 3) * 64], blue, 0));        // face 3
    CHECK(faces[(1 + 2) * 64 + 2] != 255);                 // face 2 untouched by the clear
    glGetTextureImage(cube, 0, GL_RGBA, GL_UNSIGNED_BYTE, -1, faces.data());
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_VALUE));
    // Faces written as one image with an image skipped.
    std::vector<uint8_t> red(3 * 4 * 4 * 4, 0);
    for (size_t i = 64; i < red.size(); i += 4) red[i] = red[i + 3] = 255;
    glPixelStorei(GL_UNPACK_SKIP_IMAGES, 1);
    glTextureSubImage3D(cube, 0, 0, 0, 4, 4, 4, 2, GL_RGBA, GL_UNSIGNED_BYTE, red.data());
    glPixelStorei(GL_UNPACK_SKIP_IMAGES, 0);
    glTextureSubImage3D(cube, 0, 0, 0, INT_MAX, 4, 4, 1, GL_RGBA, GL_UNSIGNED_BYTE, red.data());
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_VALUE));
    glGetTextureImage(cube, 0, GL_RGBA, GL_UNSIGNED_BYTE, static_cast<GLsizei>(faces.size()), faces.data());
    const uint8_t opaqueRed[4] = {255, 0, 0, 255};
    CHECK(gpu::near(&faces[4 * 64], opaqueRed, 0) && gpu::near(&faces[5 * 64 + 60], opaqueRed, 0));
    CHECK_GL();
    // A name from glGenBuffers, never bound, is a vertex buffer.
    GLuint vao = 0, buffer = 0;
    glCreateVertexArrays(1, &vao);
    glGenBuffers(1, &buffer);
    glVertexArrayVertexBuffer(vao, 0, buffer, 16, 8);
    CHECK_GL();
    GLint64 offset = 0;
    glGetVertexArrayIndexed64iv(vao, 0, GL_VERTEX_BINDING_OFFSET, &offset);
    CHECK_EQ(offset, 16);
    // A transform feedback buffer that does not exist.
    GLuint xfb = 0;
    glCreateTransformFeedbacks(1, &xfb);
    glTransformFeedbackBufferBase(xfb, 0, 12345);
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_VALUE));
    glDeleteTransformFeedbacks(1, &xfb);
    glDeleteBuffers(1, &buffer);
    glDeleteVertexArrays(1, &vao);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &cube);
    CHECK_GL();
}

// Transform feedback with a vertex shader that writes memory and a flat
// varying: the vertex stage runs once per captured vertex, and strips and fans take
// their flat value from GL's provoking vertex in both conventions.
GPU_TEST_VERSION(gpu_gl46_xfb_memory_flat_provoking, 46) {
    const char* vs = "#version 460 core\nlayout(location = 0) in vec2 position;\n"
                     "layout(binding = 0, offset = 0) uniform atomic_uint runs;\n"
                     "flat out int id;\nout vec4 captured;\n"
                     "void main() { atomicCounterIncrement(runs); id = gl_VertexID; captured = vec4(position, 0.0, 1.0);\n"
                     "              gl_Position = captured; }\n";
    const char* fs = "#version 460 core\nflat in int id;\nout vec4 o;\nvoid main() { o = vec4(float(id) * 0.25, 0.0, 0.0, 1.0); }\n";
    GLuint vsh = glCreateShader(GL_VERTEX_SHADER), fsh = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(vsh, 1, &vs, nullptr);
    glCompileShader(vsh);
    glShaderSource(fsh, 1, &fs, nullptr);
    glCompileShader(fsh);
    GLuint p = glCreateProgram();
    glAttachShader(p, vsh);
    glAttachShader(p, fsh);
    const char* varyings[1] = {"captured"};
    glTransformFeedbackVaryings(p, 1, varyings, GL_INTERLEAVED_ATTRIBS);
    glLinkProgram(p);
    glDeleteShader(vsh);
    glDeleteShader(fsh);
    GLint linked = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &linked);
    CHECK(linked);
    glUseProgram(p);
    GLuint vao = 0, buffers[3] = {};
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(3, buffers);
    glBindBufferBase(GL_ATOMIC_COUNTER_BUFFER, 0, buffers[1]);
    glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, buffers[2]);
    glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, 1024, nullptr, GL_DYNAMIC_COPY);
    glBindBuffer(GL_ARRAY_BUFFER, buffers[0]);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glEnableVertexAttribArray(0);
    // Strip: T0 = (v0, v1, v2) lower left, T1 = (v1, v2, v3) upper right.
    // Fan: T0 = (v0, v1, v2) lower right, T1 = (v0, v2, v3) upper left.
    const float strip[8] = {-1, -1, 1, -1, -1, 1, 1, 1}, fan[8] = {-1, -1, 1, -1, 1, 1, -1, 1};
    struct Case { GLenum mode; const float* positions; GLenum convention; int x0, y0, id0, x1, y1, id1; };
    const Case cases[4] = {
        {GL_TRIANGLE_STRIP, strip, GL_LAST_VERTEX_CONVENTION, 8, 8, 2, 56, 56, 3},
        {GL_TRIANGLE_STRIP, strip, GL_FIRST_VERTEX_CONVENTION, 8, 8, 0, 56, 56, 1},
        {GL_TRIANGLE_FAN, fan, GL_LAST_VERTEX_CONVENTION, 56, 8, 2, 8, 56, 3},
        {GL_TRIANGLE_FAN, fan, GL_FIRST_VERTEX_CONVENTION, 56, 8, 1, 8, 56, 2},
    };
    for (const Case& c : cases) {
        glBufferData(GL_ARRAY_BUFFER, sizeof(strip), c.positions, GL_STATIC_DRAW);
        const GLuint zero = 0;
        glBufferData(GL_ATOMIC_COUNTER_BUFFER, 4, &zero, GL_DYNAMIC_COPY);
        glProvokingVertex(c.convention);
        glClearColor(0, 0, 1, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glBeginTransformFeedback(GL_TRIANGLES);
        glDrawArrays(c.mode, 0, 4);
        glEndTransformFeedback();
        GLuint runs = 0;
        glGetBufferSubData(GL_ATOMIC_COUNTER_BUFFER, 0, 4, &runs);
        // Once per captured vertex (two triangles): not again to rasterize.
        if (runs != 6) std::fprintf(stderr, "  mode %#x convention %#x: %u vertex runs\n", c.mode, c.convention, runs);
        CHECK_EQ(runs, 6u);
        const uint8_t want0[4] = {static_cast<uint8_t>(c.id0 * 64 - (c.id0 == 4)), 0, 0, 255};
        const uint8_t want1[4] = {static_cast<uint8_t>(c.id1 * 64 - (c.id1 == 4)), 0, 0, 255};
        const std::vector<uint8_t> got0 = gpu::readRGBA8(c.x0, c.y0, 1, 1), got1 = gpu::readRGBA8(c.x1, c.y1, 1, 1);
        if (!gpu::near(got0.data(), want0, 2) || !gpu::near(got1.data(), want1, 2))
            std::fprintf(stderr, "  mode %#x convention %#x: flat ids %d, %d (want %d, %d)\n", c.mode, c.convention,
                         (got0[0] + 32) / 64, (got1[0] + 32) / 64, c.id0, c.id1);
        CHECK(gpu::near(got0.data(), want0, 2) && gpu::near(got1.data(), want1, 2));
    }
    glProvokingVertex(GL_LAST_VERTEX_CONVENTION);
    CHECK_GL();
    glDeleteBuffers(3, buffers);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
}

// An array of arrays of images: a uniform per innermost array ("img[1][0]",
// size 3), its elements in consecutive units from the explicit binding.
GPU_TEST_VERSION(gpu_gl46_image_arrays_of_arrays, 46) {
    const char* cs = R"(#version 460 core
layout(local_size_x = 1) in;
layout(rgba8, binding = 0) readonly uniform image2D img[2][3];
layout(std430, binding = 0) buffer Out { vec4 f[2]; };
void main() { f[0] = imageLoad(img[1][2], ivec2(0)); f[1] = imageLoad(img[0][1], ivec2(0)); }
)";
    GLuint shader = glCreateShader(GL_COMPUTE_SHADER);
    glShaderSource(shader, 1, &cs, nullptr);
    glCompileShader(shader);
    GLuint p = glCreateProgram();
    glAttachShader(p, shader);
    glLinkProgram(p);
    glDeleteShader(shader);
    GLint linked = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &linked);
    CHECK(linked);
    CHECK(glGetUniformLocation(p, "img[1][2]") >= 0);
    CHECK(glGetUniformLocation(p, "img[0][0]") >= 0);
    GLuint tex[6];
    glGenTextures(6, tex);
    for (int i = 0; i < 6; ++i) {
        glBindTexture(GL_TEXTURE_2D, tex[i]);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 1, 1);
        const uint8_t texel[4] = {static_cast<uint8_t>(40 * i), 0, 0, 255};
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, texel);
        glBindImageTexture(static_cast<GLuint>(i), tex[i], 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA8);
    }
    GLuint buffer = 0;
    glGenBuffers(1, &buffer);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, buffer);
    glBufferData(GL_SHADER_STORAGE_BUFFER, 32, nullptr, GL_DYNAMIC_READ);
    glUseProgram(p);
    glDispatchCompute(1, 1, 1);
    glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
    float out[8] = {};
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(out), out);
    if (std::fabs(out[0] - 200.0f / 255.0f) > 0.01f || std::fabs(out[4] - 40.0f / 255.0f) > 0.01f)
        std::fprintf(stderr, "  img[1][2] %g, img[0][1] %g\n", out[0] * 255.0f, out[4] * 255.0f);
    CHECK(std::fabs(out[0] - 200.0f / 255.0f) < 0.01f);  // unit 5
    CHECK(std::fabs(out[4] - 40.0f / 255.0f) < 0.01f);   // unit 1
    CHECK_GL();
    glDeleteBuffers(1, &buffer);
    glDeleteTextures(6, tex);
    glDeleteProgram(p);
}

// Malformed SPIR-V fails to specialize (an id past the header's bound, an
// instruction past the end) instead of reaching SPIRV-Cross.
GPU_TEST_VERSION(gpu_gl46_spirv_malformed, 46) {
    const uint32_t outOfBound[] = {0x07230203u, 0x00010000u, 0, 2, 0,
                                   (2u << 16) | 19u, 5000};  // OpTypeVoid %5000 with bound 2
    const uint32_t truncated[] = {0x07230203u, 0x00010000u, 0, 8, 0,
                                  (9u << 16) | 15u, 5, 1};  // OpEntryPoint claiming 9 words
    for (const auto& module : {std::vector<uint32_t>(std::begin(outOfBound), std::end(outOfBound)),
                               std::vector<uint32_t>(std::begin(truncated), std::end(truncated))}) {
        GLuint shader = glCreateShader(GL_VERTEX_SHADER);
        glShaderBinary(1, &shader, GL_SHADER_BINARY_FORMAT_SPIR_V, module.data(), static_cast<GLsizei>(module.size() * 4));
        CHECK_GL();
        glSpecializeShader(shader, "main", 0, nullptr, nullptr);
        GLint compiled = 1;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
        CHECK_EQ(compiled, 0);
        glDeleteShader(shader);
    }
    CHECK_GL();
}

// atomicCounterDecrement in SPIR-V (glslang: OpAtomicIDecrement, then the
// value minus one): with the counter at 10 the shader sees 9.
GPU_TEST_VERSION(gpu_gl46_spirv_counter_decrement, 46) {
    // glslangValidator -G: layout(binding = 0, offset = 0) uniform atomic_uint counter;
    // buffer Out { uint before; }; void main() { before = atomicCounterDecrement(counter); }
    static const uint32_t module[] = {
        0x07230203u, 0x00010000u, 0x0008000bu, 0x00000017u, 0x00000000u, 0x00020011u, 0x00000001u, 0x00020011u,
        0x00000015u, 0x0006000bu, 0x00000001u, 0x4c534c47u, 0x6474732eu, 0x3035342eu, 0x00000000u, 0x0003000eu,
        0x00000000u, 0x00000001u, 0x0005000fu, 0x00000005u, 0x00000004u, 0x6e69616du, 0x00000000u, 0x00060010u,
        0x00000004u, 0x00000011u, 0x00000001u, 0x00000001u, 0x00000001u, 0x00030003u, 0x00000002u, 0x000001ccu,
        0x00040005u, 0x00000004u, 0x6e69616du, 0x00000000u, 0x00030005u, 0x00000007u, 0x0074754fu, 0x00050006u,
        0x00000007u, 0x00000000u, 0x6f666562u, 0x00006572u, 0x00030005u, 0x00000009u, 0x00000000u, 0x00040005u,
        0x0000000du, 0x6e756f63u, 0x00726574u, 0x00030047u, 0x00000007u, 0x00000003u, 0x00050048u, 0x00000007u,
        0x00000000u, 0x00000023u, 0x00000000u, 0x00040047u, 0x00000009u, 0x00000021u, 0x00000000u, 0x00040047u,
        0x00000009u, 0x00000022u, 0x00000000u, 0x00040047u, 0x0000000du, 0x00000021u, 0x00000000u, 0x00040047u,
        0x0000000du, 0x00000023u, 0x00000000u, 0x00040047u, 0x00000016u, 0x0000000bu, 0x00000019u, 0x00020013u,
        0x00000002u, 0x00030021u, 0x00000003u, 0x00000002u, 0x00040015u, 0x00000006u, 0x00000020u, 0x00000000u,
        0x0003001eu, 0x00000007u, 0x00000006u, 0x00040020u, 0x00000008u, 0x00000002u, 0x00000007u, 0x0004003bu,
        0x00000008u, 0x00000009u, 0x00000002u, 0x00040015u, 0x0000000au, 0x00000020u, 0x00000001u, 0x0004002bu,
        0x0000000au, 0x0000000bu, 0x00000000u, 0x00040020u, 0x0000000cu, 0x0000000au, 0x00000006u, 0x0004003bu,
        0x0000000cu, 0x0000000du, 0x0000000au, 0x0004002bu, 0x00000006u, 0x0000000eu, 0x00000001u, 0x0004002bu,
        0x00000006u, 0x0000000fu, 0x00000000u, 0x0004002bu, 0x0000000au, 0x00000011u, 0x00000001u, 0x00040020u,
        0x00000013u, 0x00000002u, 0x00000006u, 0x00040017u, 0x00000015u, 0x00000006u, 0x00000003u, 0x0006002cu,
        0x00000015u, 0x00000016u, 0x0000000eu, 0x0000000eu, 0x0000000eu, 0x00050036u, 0x00000002u, 0x00000004u,
        0x00000000u, 0x00000003u, 0x000200f8u, 0x00000005u, 0x000600e9u, 0x00000006u, 0x00000010u, 0x0000000du,
        0x0000000eu, 0x0000000fu, 0x00050082u, 0x00000006u, 0x00000012u, 0x00000010u, 0x00000011u, 0x00050041u,
        0x00000013u, 0x00000014u, 0x00000009u, 0x0000000bu, 0x0003003eu, 0x00000014u, 0x00000012u, 0x000100fdu,
        0x00010038u
    };
    GLuint shader = glCreateShader(GL_COMPUTE_SHADER);
    glShaderBinary(1, &shader, GL_SHADER_BINARY_FORMAT_SPIR_V, module, sizeof(module));
    glSpecializeShader(shader, "main", 0, nullptr, nullptr);
    GLint compiled = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    CHECK(compiled);
    GLuint p = glCreateProgram();
    glAttachShader(p, shader);
    glLinkProgram(p);
    glDeleteShader(shader);
    GLint linked = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &linked);
    CHECK(linked);
    GLuint buffers[2] = {};
    glGenBuffers(2, buffers);
    const GLuint ten = 10, unset = 0xffffffffu;
    glBindBufferBase(GL_ATOMIC_COUNTER_BUFFER, 0, buffers[0]);
    glBufferData(GL_ATOMIC_COUNTER_BUFFER, 4, &ten, GL_DYNAMIC_COPY);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, buffers[1]);
    glBufferData(GL_SHADER_STORAGE_BUFFER, 4, &unset, GL_DYNAMIC_COPY);
    glUseProgram(p);
    glDispatchCompute(1, 1, 1);
    glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT | GL_ATOMIC_COUNTER_BARRIER_BIT);
    GLuint before = 0, counter = 0;
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, 4, &before);
    glGetBufferSubData(GL_ATOMIC_COUNTER_BUFFER, 0, 4, &counter);
    CHECK_EQ(before, 9u);
    CHECK_EQ(counter, 9u);
    CHECK_GL();
    glDeleteBuffers(2, buffers);
    glDeleteProgram(p);
}

// Numbers too long for an int in shader text and varying names fail or
// compile normally instead of throwing out of the GL call.
GPU_TEST_VERSION(gpu_gl46_long_numbers, 46) {
    const char* vs = "// #version 99999999999\n#version 460 core\nout int drawId;\n"
                     "void main() { drawId = gl_DrawID; gl_Position = vec4(0.0); }\n";
    GLuint shader = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(shader, 1, &vs, nullptr);
    glCompileShader(shader);
    GLint compiled = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    CHECK(compiled);
    GLuint p = glCreateProgram();
    glAttachShader(p, shader);
    const char* varyings[1] = {"drawId[99999999999]"};
    glTransformFeedbackVaryings(p, 1, varyings, GL_INTERLEAVED_ATTRIBS);
    glLinkProgram(p);
    GLint linked = 1;
    glGetProgramiv(p, GL_LINK_STATUS, &linked);
    CHECK_EQ(linked, 0);
    glDeleteShader(shader);
    glDeleteProgram(p);
    CHECK_GL();
}

// SPIR-V stages match uniforms by location, whatever their debug names:
// the vertex stage's "offsetAndScale" and the fragment stage's "tint" are
// both location 3, one uniform.
GPU_TEST_VERSION(gpu_gl46_spirv_uniform_locations, 46) {
    // glslangValidator -G: layout(location = 3) uniform vec4 offsetAndScale; (vertex: position * .w + .xy)
    static const uint32_t vertex[] = {
        0x07230203u, 0x00010000u, 0x0008000bu, 0x00000028u, 0x00000000u, 0x00020011u, 0x00000001u, 0x0006000bu,
        0x00000001u, 0x4c534c47u, 0x6474732eu, 0x3035342eu, 0x00000000u, 0x0003000eu, 0x00000000u, 0x00000001u,
        0x0009000fu, 0x00000000u, 0x00000004u, 0x6e69616du, 0x00000000u, 0x0000000du, 0x00000012u, 0x00000026u,
        0x00000027u, 0x00030003u, 0x00000002u, 0x000001ccu, 0x00040005u, 0x00000004u, 0x6e69616du, 0x00000000u,
        0x00060005u, 0x0000000bu, 0x505f6c67u, 0x65567265u, 0x78657472u, 0x00000000u, 0x00060006u, 0x0000000bu,
        0x00000000u, 0x505f6c67u, 0x7469736fu, 0x006e6f69u, 0x00070006u, 0x0000000bu, 0x00000001u, 0x505f6c67u,
        0x746e696fu, 0x657a6953u, 0x00000000u, 0x00070006u, 0x0000000bu, 0x00000002u, 0x435f6c67u, 0x4470696cu,
        0x61747369u, 0x0065636eu, 0x00070006u, 0x0000000bu, 0x00000003u, 0x435f6c67u, 0x446c6c75u, 0x61747369u,
        0x0065636eu, 0x00030005u, 0x0000000du, 0x00000000u, 0x00050005u, 0x00000012u, 0x69736f70u, 0x6e6f6974u,
        0x00000000u, 0x00060005u, 0x00000015u, 0x7366666fu, 0x6e417465u, 0x61635364u, 0x0000656cu, 0x00050005u,
        0x00000026u, 0x565f6c67u, 0x65747265u, 0x00444978u, 0x00060005u, 0x00000027u, 0x495f6c67u, 0x6174736eu,
        0x4965636eu, 0x00000044u, 0x00030047u, 0x0000000bu, 0x00000002u, 0x00050048u, 0x0000000bu, 0x00000000u,
        0x0000000bu, 0x00000000u, 0x00050048u, 0x0000000bu, 0x00000001u, 0x0000000bu, 0x00000001u, 0x00050048u,
        0x0000000bu, 0x00000002u, 0x0000000bu, 0x00000003u, 0x00050048u, 0x0000000bu, 0x00000003u, 0x0000000bu,
        0x00000004u, 0x00040047u, 0x00000012u, 0x0000001eu, 0x00000000u, 0x00040047u, 0x00000015u, 0x0000001eu,
        0x00000003u, 0x00040047u, 0x00000026u, 0x0000000bu, 0x00000005u, 0x00040047u, 0x00000027u, 0x0000000bu,
        0x00000006u, 0x00020013u, 0x00000002u, 0x00030021u, 0x00000003u, 0x00000002u, 0x00030016u, 0x00000006u,
        0x00000020u, 0x00040017u, 0x00000007u, 0x00000006u, 0x00000004u, 0x00040015u, 0x00000008u, 0x00000020u,
        0x00000000u, 0x0004002bu, 0x00000008u, 0x00000009u, 0x00000001u, 0x0004001cu, 0x0000000au, 0x00000006u,
        0x00000009u, 0x0006001eu, 0x0000000bu, 0x00000007u, 0x00000006u, 0x0000000au, 0x0000000au, 0x00040020u,
        0x0000000cu, 0x00000003u, 0x0000000bu, 0x0004003bu, 0x0000000cu, 0x0000000du, 0x00000003u, 0x00040015u,
        0x0000000eu, 0x00000020u, 0x00000001u, 0x0004002bu, 0x0000000eu, 0x0000000fu, 0x00000000u, 0x00040017u,
        0x00000010u, 0x00000006u, 0x00000002u, 0x00040020u, 0x00000011u, 0x00000001u, 0x00000010u, 0x0004003bu,
        0x00000011u, 0x00000012u, 0x00000001u, 0x00040020u, 0x00000014u, 0x00000000u, 0x00000007u, 0x0004003bu,
        0x00000014u, 0x00000015u, 0x00000000u, 0x0004002bu, 0x00000008u, 0x00000016u, 0x00000003u, 0x00040020u,
        0x00000017u, 0x00000000u, 0x00000006u, 0x0004002bu, 0x00000006u, 0x0000001eu, 0x00000000u, 0x0004002bu,
        0x00000006u, 0x0000001fu, 0x3f800000u, 0x00040020u, 0x00000023u, 0x00000003u, 0x00000007u, 0x00040020u,
        0x00000025u, 0x00000001u, 0x0000000eu, 0x0004003bu, 0x00000025u, 0x00000026u, 0x00000001u, 0x0004003bu,
        0x00000025u, 0x00000027u, 0x00000001u, 0x00050036u, 0x00000002u, 0x00000004u, 0x00000000u, 0x00000003u,
        0x000200f8u, 0x00000005u, 0x0004003du, 0x00000010u, 0x00000013u, 0x00000012u, 0x00050041u, 0x00000017u,
        0x00000018u, 0x00000015u, 0x00000016u, 0x0004003du, 0x00000006u, 0x00000019u, 0x00000018u, 0x0005008eu,
        0x00000010u, 0x0000001au, 0x00000013u, 0x00000019u, 0x0004003du, 0x00000007u, 0x0000001bu, 0x00000015u,
        0x0007004fu, 0x00000010u, 0x0000001cu, 0x0000001bu, 0x0000001bu, 0x00000000u, 0x00000001u, 0x00050081u,
        0x00000010u, 0x0000001du, 0x0000001au, 0x0000001cu, 0x00050051u, 0x00000006u, 0x00000020u, 0x0000001du,
        0x00000000u, 0x00050051u, 0x00000006u, 0x00000021u, 0x0000001du, 0x00000001u, 0x00070050u, 0x00000007u,
        0x00000022u, 0x00000020u, 0x00000021u, 0x0000001eu, 0x0000001fu, 0x00050041u, 0x00000023u, 0x00000024u,
        0x0000000du, 0x0000000fu, 0x0003003eu, 0x00000024u, 0x00000022u, 0x000100fdu, 0x00010038u
    };
    // glslangValidator -G: layout(location = 3) uniform vec4 tint; (fragment: vec4(tint.z, 1.0 - tint.w, 0, 1))
    static const uint32_t fragment[] = {
        0x07230203u, 0x00010000u, 0x0008000bu, 0x00000018u, 0x00000000u, 0x00020011u, 0x00000001u, 0x0006000bu,
        0x00000001u, 0x4c534c47u, 0x6474732eu, 0x3035342eu, 0x00000000u, 0x0003000eu, 0x00000000u, 0x00000001u,
        0x0006000fu, 0x00000004u, 0x00000004u, 0x6e69616du, 0x00000000u, 0x00000009u, 0x00030010u, 0x00000004u,
        0x00000008u, 0x00030003u, 0x00000002u, 0x000001ccu, 0x00040005u, 0x00000004u, 0x6e69616du, 0x00000000u,
        0x00030005u, 0x00000009u, 0x0000006fu, 0x00040005u, 0x0000000bu, 0x746e6974u, 0x00000000u, 0x00040047u,
        0x00000009u, 0x0000001eu, 0x00000000u, 0x00040047u, 0x0000000bu, 0x0000001eu, 0x00000003u, 0x00020013u,
        0x00000002u, 0x00030021u, 0x00000003u, 0x00000002u, 0x00030016u, 0x00000006u, 0x00000020u, 0x00040017u,
        0x00000007u, 0x00000006u, 0x00000004u, 0x00040020u, 0x00000008u, 0x00000003u, 0x00000007u, 0x0004003bu,
        0x00000008u, 0x00000009u, 0x00000003u, 0x00040020u, 0x0000000au, 0x00000000u, 0x00000007u, 0x0004003bu,
        0x0000000au, 0x0000000bu, 0x00000000u, 0x00040015u, 0x0000000cu, 0x00000020u, 0x00000000u, 0x0004002bu,
        0x0000000cu, 0x0000000du, 0x00000002u, 0x00040020u, 0x0000000eu, 0x00000000u, 0x00000006u, 0x0004002bu,
        0x00000006u, 0x00000011u, 0x3f800000u, 0x0004002bu, 0x0000000cu, 0x00000012u, 0x00000003u, 0x0004002bu,
        0x00000006u, 0x00000016u, 0x00000000u, 0x00050036u, 0x00000002u, 0x00000004u, 0x00000000u, 0x00000003u,
        0x000200f8u, 0x00000005u, 0x00050041u, 0x0000000eu, 0x0000000fu, 0x0000000bu, 0x0000000du, 0x0004003du,
        0x00000006u, 0x00000010u, 0x0000000fu, 0x00050041u, 0x0000000eu, 0x00000013u, 0x0000000bu, 0x00000012u,
        0x0004003du, 0x00000006u, 0x00000014u, 0x00000013u, 0x00050083u, 0x00000006u, 0x00000015u, 0x00000011u,
        0x00000014u, 0x00070050u, 0x00000007u, 0x00000017u, 0x00000010u, 0x00000015u, 0x00000016u, 0x00000011u,
        0x0003003eu, 0x00000009u, 0x00000017u, 0x000100fdu, 0x00010038u
    };
    GLuint shaders[2] = {glCreateShader(GL_VERTEX_SHADER), glCreateShader(GL_FRAGMENT_SHADER)};
    glShaderBinary(1, &shaders[0], GL_SHADER_BINARY_FORMAT_SPIR_V, vertex, sizeof(vertex));
    glShaderBinary(1, &shaders[1], GL_SHADER_BINARY_FORMAT_SPIR_V, fragment, sizeof(fragment));
    GLuint p = glCreateProgram();
    for (GLuint s : shaders) {
        glSpecializeShader(s, "main", 0, nullptr, nullptr);
        glAttachShader(p, s);
    }
    glLinkProgram(p);
    GLint linked = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &linked);
    CHECK(linked);
    if (!linked) {
        char log[1024] = {};
        glGetProgramInfoLog(p, sizeof(log), nullptr, log);
        std::fprintf(stderr, "  %s\n", log);
    }
    glUseProgram(p);
    glUniform4f(3, 0.0f, 0.0f, 1.0f, 1.0f);  // whole quad; red 1, green 0
    GLuint vao = gpu::quad();
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    const uint8_t red[4] = {255, 0, 0, 255};
    CHECK(gpu::near(gpu::readRGBA8(32, 32, 1, 1).data(), red, 1));
    CHECK_GL();
    glDeleteVertexArrays(1, &vao);
    glDeleteShader(shaders[0]);
    glDeleteShader(shaders[1]);
    glDeleteProgram(p);
}

// An array of output blocks with xfb_offset is captured by consecutive
// buffers: element k into buffer xfb_buffer + k (GLSL 4.60 section 4.4.2.1).
GPU_TEST_VERSION(gpu_gl46_xfb_block_array_buffers, 46) {
    const char* vs = "#version 460 core\n"
                     "layout(xfb_buffer = 0, xfb_offset = 0) out B { vec4 a; } b[2];\n"
                     "void main() { b[0].a = vec4(1.0, 2.0, 3.0, 4.0); b[1].a = vec4(5.0, 6.0, 7.0, 8.0);\n"
                     "              gl_Position = vec4(0.0, 0.0, 0.0, 1.0); }\n";
    GLuint shader = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(shader, 1, &vs, nullptr);
    glCompileShader(shader);
    GLuint p = glCreateProgram();
    glAttachShader(p, shader);
    glLinkProgram(p);
    glDeleteShader(shader);
    GLint linked = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &linked);
    CHECK(linked);
    GLuint vao = 0, buffers[2] = {};
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(2, buffers);
    const float unset[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
    for (int i = 0; i < 2; ++i) {
        glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, static_cast<GLuint>(i), buffers[i]);
        glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, sizeof(unset), unset, GL_DYNAMIC_COPY);
    }
    glUseProgram(p);
    glEnable(GL_RASTERIZER_DISCARD);
    glBeginTransformFeedback(GL_POINTS);
    glDrawArrays(GL_POINTS, 0, 1);
    glEndTransformFeedback();
    glDisable(GL_RASTERIZER_DISCARD);
    float first[8], second[8];
    glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, buffers[0]);
    glGetBufferSubData(GL_TRANSFORM_FEEDBACK_BUFFER, 0, sizeof(first), first);
    glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, buffers[1]);
    glGetBufferSubData(GL_TRANSFORM_FEEDBACK_BUFFER, 0, sizeof(second), second);
    CHECK(first[0] == 1.0f && first[3] == 4.0f && first[4] == -1.0f);
    CHECK(second[0] == 5.0f && second[3] == 8.0f);
    if (second[0] != 5.0f) std::fprintf(stderr, "  buffer 0: %g %g .. %g; buffer 1: %g\n", first[0], first[3], first[4], second[0]);
    CHECK_GL();
    glDeleteBuffers(2, buffers);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
}

// An array of arrays of atomic counters: a uniform per innermost array
// ("c[1][0]", 3 counters from byte 12), and every counter counts.
GPU_TEST_VERSION(gpu_gl46_counter_arrays_of_arrays, 46) {
    const char* cs = R"(#version 460 core
layout(local_size_x = 1) in;
layout(binding = 0, offset = 0) uniform atomic_uint c[2][3];
void main() { atomicCounterIncrement(c[1][2]); atomicCounterIncrement(c[0][1]); }
)";
    GLuint shader = glCreateShader(GL_COMPUTE_SHADER);
    glShaderSource(shader, 1, &cs, nullptr);
    glCompileShader(shader);
    GLuint p = glCreateProgram();
    glAttachShader(p, shader);
    glLinkProgram(p);
    glDeleteShader(shader);
    GLint linked = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &linked);
    CHECK(linked);
    const GLuint second = glGetProgramResourceIndex(p, GL_UNIFORM, "c[1][0]");
    CHECK(second != GL_INVALID_INDEX);
    if (second != GL_INVALID_INDEX) {
        const GLenum props[2] = {GL_ARRAY_SIZE, GL_OFFSET};
        GLint values[2] = {};
        glGetProgramResourceiv(p, GL_UNIFORM, second, 2, props, 2, nullptr, values);
        CHECK_EQ(values[0], 3);
        CHECK_EQ(values[1], 12);
    }
    GLuint buffer = 0;
    glGenBuffers(1, &buffer);
    const GLuint zeros[6] = {};
    glBindBufferBase(GL_ATOMIC_COUNTER_BUFFER, 0, buffer);
    glBufferData(GL_ATOMIC_COUNTER_BUFFER, sizeof(zeros), zeros, GL_DYNAMIC_COPY);
    glUseProgram(p);
    glDispatchCompute(1, 1, 1);
    glMemoryBarrier(GL_ATOMIC_COUNTER_BARRIER_BIT);
    GLuint counters[6] = {};
    glGetBufferSubData(GL_ATOMIC_COUNTER_BUFFER, 0, sizeof(counters), counters);
    CHECK(counters[5] == 1 && counters[1] == 1 && counters[0] == 0 && counters[3] == 0);
    CHECK_GL();
    glDeleteBuffers(1, &buffer);
    glDeleteProgram(p);
}

// Framebuffers are shared between contexts that share objects, as desktop
// drivers (Mesa, NVIDIA, AMD) share them: DOOM (2016) attaches its targets on
// one context and draws with another.
TEST(gpu_shared_framebuffers) {
    gpu::Fixture first(64, 64, false, 46);
    CHECK(first.ok);
    if (!first.ok) return;
    GLuint tex = 0, fbo = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 16, 16);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glFinish();
    const int attribs[] = {0x2091, 4, 0x2092, 6, 0x9126, 2, 0};
    int error = 0;
    wmgl_context* second = wmgl_context_create(first.format, first.context, attribs, &error);
    CHECK(second != nullptr);
    if (!second) return;
    CHECK(wmgl_make_current(second, first.surface, first.surface));
    CHECK(glIsFramebuffer(fbo));
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    CHECK_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE));
    glViewport(0, 0, 16, 16);
    glClearColor(1, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    uint8_t pixel[4] = {};
    glReadPixels(4, 4, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    const uint8_t red[4] = {255, 0, 0, 255};
    CHECK(gpu::near(pixel, red, 0));
    CHECK_GL();
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    // Deleted on one context, gone on the other.
    glDeleteFramebuffers(1, &fbo);
    CHECK(wmgl_make_current(first.context, first.surface, first.surface));
    CHECK(!glIsFramebuffer(fbo));
    glDeleteTextures(1, &tex);
    wmgl_context_destroy(second);
}

// glReadPixels into a pixel pack buffer (copied by the GPU without a wait
// when no conversion is needed): the texels land at the pack layout's
// places, row length and skips included, and read back through the buffer.
GPU_TEST_VERSION(gpu_gl46_read_pixels_pack_buffer, 46) {
    GLuint tex = 0, fbo = 0, pbo = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 8, 8);
    uint8_t texels[8][8][4];
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) {
            texels[y][x][0] = static_cast<uint8_t>(x * 10);
            texels[y][x][1] = static_cast<uint8_t>(y * 10);
            texels[y][x][2] = 7;
            texels[y][x][3] = 255;
        }
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 8, 8, GL_RGBA, GL_UNSIGNED_BYTE, texels);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    glGenBuffers(1, &pbo);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo);
    glBufferData(GL_PIXEL_PACK_BUFFER, 1024, nullptr, GL_STREAM_READ);
    // A 3x2 region at (2, 3), into rows of 5 pixels after 1 skipped row and pixel.
    glPixelStorei(GL_PACK_ROW_LENGTH, 5);
    glPixelStorei(GL_PACK_SKIP_ROWS, 1);
    glPixelStorei(GL_PACK_SKIP_PIXELS, 1);
    glReadPixels(2, 3, 3, 2, GL_RGBA, GL_UNSIGNED_BYTE, reinterpret_cast<void*>(16));
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glPixelStorei(GL_PACK_SKIP_ROWS, 0);
    glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
    // The whole image, tightly packed, after it.
    glReadPixels(0, 0, 8, 8, GL_RGBA, GL_UNSIGNED_BYTE, reinterpret_cast<void*>(512));
    CHECK_GL();
    uint8_t out[1024] = {};
    glGetBufferSubData(GL_PIXEL_PACK_BUFFER, 0, sizeof(out), out);
    for (int r = 0; r < 2; ++r)
        for (int c = 0; c < 3; ++c) {
            const uint8_t* p = out + 16 + ((1 + r) * 5 + 1 + c) * 4;
            CHECK(p[0] == (2 + c) * 10 && p[1] == (3 + r) * 10 && p[2] == 7 && p[3] == 255);
        }
    CHECK(out[16] == 0 && out[16 + 4 * 5] == 0);  // skipped pixels untouched
    CHECK(std::memcmp(out + 512, texels, sizeof(texels)) == 0);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteBuffers(1, &pbo);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &tex);
}

GPU_TEST_VERSION(gpu_gl46_read_pixels_pack_buffer_swap_bytes, 46) {
    // GL_PACK_SWAP_BYTES reverses each component's bytes, into a pack
    // buffer too (the GPU copy cannot; such reads take the converting path).
    GLuint tex = 0, fbo = 0, pbo = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_R32UI, 4, 4);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    const GLuint value[4] = {0x11223344u, 0, 0, 0};
    glClearBufferuiv(GL_COLOR, 0, value);
    glGenBuffers(1, &pbo);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo);
    glBufferData(GL_PIXEL_PACK_BUFFER, 64, nullptr, GL_STREAM_READ);
    glReadPixels(0, 0, 4, 4, GL_RED_INTEGER, GL_UNSIGNED_INT, nullptr);  // the GPU copy
    glPixelStorei(GL_PACK_SWAP_BYTES, GL_TRUE);
    glReadPixels(0, 0, 1, 1, GL_RED_INTEGER, GL_UNSIGNED_INT, reinterpret_cast<void*>(4));
    glPixelStorei(GL_PACK_SWAP_BYTES, GL_FALSE);
    CHECK_GL();
    GLuint out[2] = {};
    glGetBufferSubData(GL_PIXEL_PACK_BUFFER, 0, sizeof(out), out);
    CHECK(out[0] == 0x11223344u);
    CHECK(out[1] == 0x44332211u);
    if (out[1] != 0x44332211u) std::fprintf(stderr, "  swapped read 0x%08x\n", out[1]);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteBuffers(1, &pbo);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &tex);
}
