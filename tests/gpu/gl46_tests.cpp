// GL 4.6 features (contexts above 4.5 go through the development override
// until the version is advertised).
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
    uint8_t blocks[32] = {};
    glGetCompressedTexImage(GL_TEXTURE_3D, 0, blocks);
    CHECK(blocks[0] || blocks[1] || blocks[16] || blocks[17]);
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
    for (int i = 0; i < 100; ++i) {
        glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(red), red);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
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
