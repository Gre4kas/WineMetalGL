// GL 4.6 features (contexts above 4.5 go through the development override
// until the version is advertised).
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
