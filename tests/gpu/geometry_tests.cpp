// Geometry shader emulation (core/shader/geometry.h).
#include "gpu.h"

namespace {

GLuint linkWithGeometry(const char* vs, const char* gs, const char* fs, const char* varying = nullptr) {
    GLuint p = glCreateProgram();
    auto attach = [&](GLenum type, const char* src) {
        GLuint s = glCreateShader(type);
        glShaderSource(s, 1, &src, nullptr);
        glCompileShader(s);
        GLint ok = 0;
        glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        CHECK(ok);
        glAttachShader(p, s);
        glDeleteShader(s);
    };
    attach(GL_VERTEX_SHADER, vs);
    attach(GL_GEOMETRY_SHADER, gs);
    attach(GL_FRAGMENT_SHADER, fs);
    glBindAttribLocation(p, 0, "position");
    if (varying) glTransformFeedbackVaryings(p, 1, &varying, GL_INTERLEAVED_ATTRIBS);
    glLinkProgram(p);
    GLint linked = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[4096] = {};
        glGetProgramInfoLog(p, sizeof(log), nullptr, log);
        std::fprintf(stderr, "link failed:\n%s\n", log);
    }
    CHECK(linked);
    return p;
}

const char* kPassVs = R"(#version 330 core
in vec2 position;
out vec2 pos;
void main() { pos = position; gl_Position = vec4(position, 0.0, 1.0); })";

const char* kColorFs = R"(#version 330 core
in vec4 color; out vec4 o; void main() { o = color; })";

}  // namespace

GPU_TEST(gpu_geometry_triangles_primitive_id) {
    // Each input triangle is re-emitted with a colour picked by gl_PrimitiveIDIn.
    const char* gs = R"(#version 330 core
layout(triangles) in;
layout(triangle_strip, max_vertices = 3) out;
in vec2 pos[];
uniform vec4 colors[2];
out vec4 color;
void main() {
    for (int i = 0; i < 3; ++i) {
        color = colors[gl_PrimitiveIDIn];
        gl_Position = vec4(pos[i], 0.0, 1.0);
        EmitVertex();
    }
    EndPrimitive();
})";
    GLuint p = linkWithGeometry(kPassVs, gs, kColorFs);
    GLint type = 0;
    glGetProgramiv(p, GL_GEOMETRY_INPUT_TYPE, &type);
    CHECK_EQ(type, GL_TRIANGLES);
    glGetProgramiv(p, GL_GEOMETRY_OUTPUT_TYPE, &type);
    CHECK_EQ(type, GL_TRIANGLE_STRIP);
    glUseProgram(p);
    const GLint colors = glGetUniformLocation(p, "colors[0]");
    glUniform4f(colors, 1, 0, 0, 1);
    glUniform4f(colors + 1, 0, 1, 0, 1);
    GLuint vao = gpu::quad();
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    CHECK_GL();
    glDrawArrays(GL_POINTS, 0, 4);  // not the geometry shader's input type
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_OPERATION));
    const uint8_t red[4] = {255, 0, 0, 255}, green[4] = {0, 255, 0, 255};
    CHECK(gpu::near(gpu::readRGBA8(4, 4, 1, 1).data(), red));
    CHECK(gpu::near(gpu::readRGBA8(60, 60, 1, 1).data(), green));
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
    CHECK_GL();
}

GPU_TEST(gpu_geometry_point_expansion) {
    // Points become quads; the second EndPrimitive splits nothing.
    const char* gs = R"(#version 330 core
layout(points) in;
layout(triangle_strip, max_vertices = 4) out;
in vec2 pos[];
out vec4 color;
void main() {
    color = vec4(0.0, 0.0, 1.0, 1.0);
    for (int i = 0; i < 4; ++i) {
        vec2 corner = vec2(i % 2 == 0 ? -0.25 : 0.25, i < 2 ? -0.25 : 0.25);
        gl_Position = gl_in[0].gl_Position + vec4(corner, 0.0, 0.0);
        EmitVertex();
    }
    EndPrimitive();
    EndPrimitive();
})";
    GLuint p = linkWithGeometry(kPassVs, gs, kColorFs);
    glUseProgram(p);
    static const float point[] = {0.5f, 0.5f};
    GLuint vao = 0, vbo = 0;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(point), point, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glEnableVertexAttribArray(0);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_POINTS, 0, 1);
    CHECK_GL();
    const uint8_t blue[4] = {0, 0, 255, 255}, black[4] = {0, 0, 0, 255};
    CHECK(gpu::near(gpu::readRGBA8(48, 48, 1, 1).data(), blue));   // quad centre (0.5, 0.5)
    CHECK(gpu::near(gpu::readRGBA8(16, 16, 1, 1).data(), black));
    CHECK(gpu::near(gpu::readRGBA8(48, 16, 1, 1).data(), black));
    glDeleteBuffers(1, &vbo);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
    CHECK_GL();
}

GPU_TEST(gpu_geometry_transform_feedback) {
    // Each point emits two points; transform feedback captures the geometry
    // shader's outputs and PRIMITIVES_GENERATED counts them.
    const char* vs = R"(#version 330 core
in vec2 position;
out float id;
void main() { id = float(gl_VertexID); gl_Position = vec4(position, 0.0, 1.0); })";
    const char* gs = R"(#version 330 core
layout(points) in;
layout(points, max_vertices = 2) out;
in float id[];
out float value;
out vec4 color;
void main() {
    for (int i = 0; i < 2; ++i) {
        value = id[0] * 10.0 + float(i);
        color = vec4(1.0);
        gl_Position = gl_in[0].gl_Position;
        EmitVertex();
    }
})";
    GLuint p = linkWithGeometry(vs, gs, kColorFs, "value");
    glUseProgram(p);
    GLuint vao = gpu::quad();
    GLuint buffer = 0;
    glGenBuffers(1, &buffer);
    glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, buffer);
    glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, 8 * 4, nullptr, GL_STATIC_READ);
    glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, buffer);
    GLuint queries[2] = {};
    glGenQueries(2, queries);
    glEnable(GL_RASTERIZER_DISCARD);
    glBeginQuery(GL_PRIMITIVES_GENERATED, queries[0]);
    glBeginQuery(GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN, queries[1]);
    glBeginTransformFeedback(GL_TRIANGLES);
    glDrawArrays(GL_POINTS, 0, 3);  // geometry output is points, not triangles
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_OPERATION));
    glEndTransformFeedback();
    glBeginTransformFeedback(GL_POINTS);
    glDrawArrays(GL_POINTS, 0, 3);
    glEndTransformFeedback();
    glEndQuery(GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN);
    glEndQuery(GL_PRIMITIVES_GENERATED);
    glDisable(GL_RASTERIZER_DISCARD);
    CHECK_GL();
    GLuint generated = 0, written = 0;
    glGetQueryObjectuiv(queries[0], GL_QUERY_RESULT, &generated);
    glGetQueryObjectuiv(queries[1], GL_QUERY_RESULT, &written);
    CHECK_EQ(generated, 6u);
    CHECK_EQ(written, 6u);
    float data[8] = {};
    glGetBufferSubData(GL_TRANSFORM_FEEDBACK_BUFFER, 0, sizeof(data), data);
    const float want[6] = {0, 1, 10, 11, 20, 21};
    bool ok = true;
    for (int i = 0; i < 6; ++i)
        if (data[i] != want[i]) ok = false;
    if (!ok) std::fprintf(stderr, "  captured %g %g %g %g %g %g\n", data[0], data[1], data[2], data[3], data[4], data[5]);
    CHECK(ok);
    glDeleteQueries(2, queries);
    glDeleteBuffers(1, &buffer);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
    CHECK_GL();
}

GPU_TEST(gpu_geometry_generated_names_and_array_members) {
    // Variables named like the emulation's own temporaries (b, o), and an
    // input block with an array member.
    const char* vs = R"(#version 330 core
in vec2 position;
out vec4 b;
out VS { vec4 tint; float w[2]; } vs;
void main() {
    b = vec4(0.0, 1.0, 0.0, 1.0);
    vs.tint = vec4(1.0, 0.0, 0.0, 0.0);
    vs.w[0] = 0.25;
    vs.w[1] = 0.5;
    gl_Position = vec4(position, 0.0, 1.0);
})";
    const char* gs = R"(#version 330 core
layout(triangles) in;
layout(triangle_strip, max_vertices = 3) out;
in vec4 b[];
in VS { vec4 tint; float w[2]; } vs[];
out vec4 o;
void main() {
    for (int i = 0; i < 3; ++i) {
        o = b[i] + vs[i].tint * vs[i].w[1];
        gl_Position = gl_in[i].gl_Position;
        EmitVertex();
    }
    EndPrimitive();
})";
    const char* fs = R"(#version 330 core
in vec4 o; out vec4 color; void main() { color = o; })";
    GLuint p = linkWithGeometry(vs, gs, fs);
    glUseProgram(p);
    GLuint vao = gpu::quad();
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    CHECK_GL();
    const uint8_t want[4] = {128, 255, 0, 255};
    CHECK(gpu::near(gpu::readRGBA8(32, 32, 1, 1).data(), want, 2));
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
    CHECK_GL();
}
