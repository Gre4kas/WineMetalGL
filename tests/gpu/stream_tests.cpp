// Geometry shader vertex streams (GL 4.0, GL_ARB_transform_feedback3):
// each transform feedback buffer captures one stream, per-stream queries,
// drawing from a stream, and rasterization of stream 0 only.
#include "gpu.h"

namespace {

PFN_glBeginQueryIndexed pBeginQueryIndexed;
PFN_glEndQueryIndexed pEndQueryIndexed;
PFN_glGenTransformFeedbacks pGenTransformFeedbacks;
PFN_glBindTransformFeedback pBindTransformFeedback;
PFN_glDeleteTransformFeedbacks pDeleteTransformFeedbacks;
PFN_glDrawTransformFeedbackStream pDrawTransformFeedbackStream;

void loadStreamFunctions() {
    pBeginQueryIndexed = reinterpret_cast<PFN_glBeginQueryIndexed>(wmgl_get_proc_address("glBeginQueryIndexed"));
    pEndQueryIndexed = reinterpret_cast<PFN_glEndQueryIndexed>(wmgl_get_proc_address("glEndQueryIndexed"));
    pGenTransformFeedbacks = reinterpret_cast<PFN_glGenTransformFeedbacks>(wmgl_get_proc_address("glGenTransformFeedbacks"));
    pBindTransformFeedback = reinterpret_cast<PFN_glBindTransformFeedback>(wmgl_get_proc_address("glBindTransformFeedback"));
    pDeleteTransformFeedbacks =
        reinterpret_cast<PFN_glDeleteTransformFeedbacks>(wmgl_get_proc_address("glDeleteTransformFeedbacks"));
    pDrawTransformFeedbackStream =
        reinterpret_cast<PFN_glDrawTransformFeedbackStream>(wmgl_get_proc_address("glDrawTransformFeedbackStream"));
}

GLuint compile(GLenum type, const char* source) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &source, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096] = {};
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        std::fprintf(stderr, "  compile failed:\n%s\n", log);
        ++wmgl_test::failures;
    }
    return s;
}

}  // namespace

// Two streams captured into two buffers (gl_NextBuffer), with the
// per-stream PRIMITIVES_GENERATED and TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN
// queries; then each buffer drawn with glDrawTransformFeedbackStream.
TEST(gpu_geometry_vertex_streams) {
    gpu::Fixture fixture(16, 16, true, 40);
    CHECK(fixture.ok);
    if (!fixture.ok) return;
    loadStreamFunctions();
    const char* vs = "#version 400 core\nvoid main() { gl_Position = vec4(float(gl_VertexID), 0.0, 0.0, 1.0); }\n";
    const char* gs = R"(#version 400 core
layout(points) in;
layout(points, max_vertices = 5) out;
layout(stream = 1) out vec4 other;
void main() {
    float id = gl_in[0].gl_Position.x;
    gl_Position = vec4(id, 1.0, 2.0, 3.0); EmitStreamVertex(0);
    gl_Position = vec4(id, 4.0, 5.0, 6.0); EmitStreamVertex(0);
    other = vec4(id, -1.0, -2.0, -3.0); EmitStreamVertex(1);
    EndStreamPrimitive(1);
    other = vec4(id, -4.0, -5.0, -6.0); EmitStreamVertex(1);
    other = vec4(id, -7.0, -8.0, -9.0); EmitStreamVertex(1);
})";
    GLuint p = glCreateProgram();
    glAttachShader(p, compile(GL_VERTEX_SHADER, vs));
    glAttachShader(p, compile(GL_GEOMETRY_SHADER, gs));
    const char* varyings[] = {"gl_Position", "gl_NextBuffer", "other"};
    glTransformFeedbackVaryings(p, 3, varyings, GL_INTERLEAVED_ATTRIBS);
    glLinkProgram(p);
    GLint linked = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &linked);
    CHECK(linked);
    GLuint vao = 0, xfb = 0, buffers[2] = {}, queries[4] = {};
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    pGenTransformFeedbacks(1, &xfb);
    pBindTransformFeedback(GL_TRANSFORM_FEEDBACK, xfb);
    glGenBuffers(2, buffers);
    for (int b = 0; b < 2; ++b) {
        glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, buffers[b]);
        glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, 64 * 16, nullptr, GL_STATIC_READ);
        glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, b, buffers[b]);
    }
    glGenQueries(4, queries);
    glUseProgram(p);
    glEnable(GL_RASTERIZER_DISCARD);
    glBeginTransformFeedback(GL_POINTS);
    pBeginQueryIndexed(GL_PRIMITIVES_GENERATED, 0, queries[0]);
    pBeginQueryIndexed(GL_PRIMITIVES_GENERATED, 1, queries[1]);
    pBeginQueryIndexed(GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN, 1, queries[2]);
    pBeginQueryIndexed(GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN, 2, queries[3]);
    glDrawArrays(GL_POINTS, 0, 2);
    pEndQueryIndexed(GL_PRIMITIVES_GENERATED, 0);
    pEndQueryIndexed(GL_PRIMITIVES_GENERATED, 1);
    pEndQueryIndexed(GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN, 1);
    pEndQueryIndexed(GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN, 2);
    glEndTransformFeedback();
    glDisable(GL_RASTERIZER_DISCARD);
    GLuint results[4] = {};
    for (int q = 0; q < 4; ++q) glGetQueryObjectuiv(queries[q], GL_QUERY_RESULT, &results[q]);
    // Stream 0: 2 points per input point; stream 1: 3; stream 2 has no buffer.
    CHECK_EQ(results[0], 4u);
    CHECK_EQ(results[1], 6u);
    CHECK_EQ(results[2], 6u);
    CHECK_EQ(results[3], 0u);
    float stream0[4][4] = {}, stream1[6][4] = {};
    glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, buffers[0]);
    glGetBufferSubData(GL_TRANSFORM_FEEDBACK_BUFFER, 0, sizeof(stream0), stream0);
    glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, buffers[1]);
    glGetBufferSubData(GL_TRANSFORM_FEEDBACK_BUFFER, 0, sizeof(stream1), stream1);
    int bad = 0;
    for (int v = 0; v < 2; ++v) {
        bad += stream0[2 * v][0] != static_cast<float>(v) || stream0[2 * v][1] != 1.0f || stream0[2 * v + 1][3] != 6.0f;
        for (int k = 0; k < 3; ++k) bad += stream1[3 * v + k][0] != static_cast<float>(v) || stream1[3 * v + k][1] != -1.0f - 3.0f * k;
    }
    CHECK_EQ(bad, 0);
    // Drawing from stream 1 draws its 6 captured vertices.
    const char* drawVs = "#version 400 core\nin vec4 p;\nvoid main() { gl_Position = vec4(0.0, 0.0, 0.0, 1.0); gl_PointSize = 16.0; }\n";
    const char* drawFs = "#version 400 core\nout vec4 c;\nvoid main() { c = vec4(1.0); }\n";
    GLuint draw = gpu::program(drawVs, drawFs);
    glUseProgram(draw);
    glEnable(GL_PROGRAM_POINT_SIZE);
    GLuint counter = 0;
    glGenQueries(1, &counter);
    glBeginQuery(GL_PRIMITIVES_GENERATED, counter);
    pDrawTransformFeedbackStream(GL_POINTS, xfb, 1);
    glEndQuery(GL_PRIMITIVES_GENERATED);
    GLuint drawn = 0;
    glGetQueryObjectuiv(counter, GL_QUERY_RESULT, &drawn);
    CHECK_EQ(drawn, 6u);
    glDeleteQueries(1, &counter);
    glDeleteQueries(4, queries);
    glDeleteBuffers(2, buffers);
    pDeleteTransformFeedbacks(1, &xfb);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(draw);
    glDeleteProgram(p);
    CHECK_GL();
}

// Only stream 0 is rasterized; a buffer may not capture two streams.
TEST(gpu_geometry_streams_rasterize_stream_zero) {
    gpu::Fixture fixture(16, 16, true, 40);
    CHECK(fixture.ok);
    if (!fixture.ok) return;
    const char* vs = "#version 400 core\nvoid main() { gl_Position = vec4(0.0); }\n";
    const char* gs = R"(#version 400 core
layout(points) in;
layout(points, max_vertices = 2) out;
layout(stream = 1) out vec4 other;
void main() {
    gl_Position = vec4(-0.5, 0.0, 0.0, 1.0); gl_PointSize = 8.0; EmitStreamVertex(0);
    gl_Position = vec4(0.5, 0.0, 0.0, 1.0); gl_PointSize = 8.0; other = vec4(1.0); EmitStreamVertex(1);
})";
    const char* fs = "#version 400 core\nout vec4 c;\nvoid main() { c = vec4(0.0, 1.0, 0.0, 1.0); }\n";
    GLuint p = glCreateProgram();
    glAttachShader(p, compile(GL_VERTEX_SHADER, vs));
    glAttachShader(p, compile(GL_GEOMETRY_SHADER, gs));
    glAttachShader(p, compile(GL_FRAGMENT_SHADER, fs));
    glLinkProgram(p);
    GLint linked = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &linked);
    CHECK(linked);
    GLuint vao = 0;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glUseProgram(p);
    glEnable(GL_PROGRAM_POINT_SIZE);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_POINTS, 0, 1);
    const std::vector<uint8_t> left = gpu::readRGBA8(4, 8, 1, 1), right = gpu::readRGBA8(12, 8, 1, 1);
    const uint8_t green[4] = {0, 255, 0, 255}, clear[4] = {0, 0, 0, 0};
    CHECK(gpu::near(left.data(), green));
    CHECK(gpu::near(right.data(), clear));
    glDeleteProgram(p);
    // gl_Position (stream 0) and other (stream 1) in one buffer is a link error.
    GLuint mixed = glCreateProgram();
    glAttachShader(mixed, compile(GL_VERTEX_SHADER, vs));
    glAttachShader(mixed, compile(GL_GEOMETRY_SHADER, gs));
    const char* varyings[] = {"gl_Position", "other"};
    glTransformFeedbackVaryings(mixed, 2, varyings, GL_INTERLEAVED_ATTRIBS);
    glLinkProgram(mixed);
    glGetProgramiv(mixed, GL_LINK_STATUS, &linked);
    CHECK(!linked);
    glDeleteProgram(mixed);
    glDeleteVertexArrays(1, &vao);
    CHECK_GL();
}
