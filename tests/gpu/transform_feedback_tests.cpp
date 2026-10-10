// Transform feedback capture from the vertex stage.
#include "gpu.h"

GPU_TEST(gpu_transform_feedback_vertex_capture) {
    const char* vs = R"(#version 330 core
in vec2 position;
out vec4 value;
flat out int id;
void main() { value = vec4(position, float(gl_VertexID), 1.0); id = gl_VertexID * 10; gl_Position = vec4(position, 0.0, 1.0); })";
    const char* fs = R"(#version 330 core
in vec4 value; flat in int id; out vec4 o; void main() { o = value; })";
    GLuint p = glCreateProgram();
    auto compile = [](GLenum type, const char* src) {
        GLuint s = glCreateShader(type);
        glShaderSource(s, 1, &src, nullptr);
        glCompileShader(s);
        return s;
    };
    glAttachShader(p, compile(GL_VERTEX_SHADER, vs));
    glAttachShader(p, compile(GL_FRAGMENT_SHADER, fs));
    glBindAttribLocation(p, 0, "position");
    const char* varyings[] = {"value", "id"};
    glTransformFeedbackVaryings(p, 2, varyings, GL_INTERLEAVED_ATTRIBS);
    glLinkProgram(p);
    GLint linked = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &linked);
    CHECK(linked);
    GLuint vao = gpu::quad();  // 4 vertices, triangle strip
    GLuint xfbBuffer = 0;
    glGenBuffers(1, &xfbBuffer);
    glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, xfbBuffer);
    glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, 6 * 5 * 4, nullptr, GL_STATIC_READ);
    glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, xfbBuffer);
    glUseProgram(p);
    GLuint q = 0;
    glGenQueries(1, &q);
    glEnable(GL_RASTERIZER_DISCARD);
    glBeginQuery(GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN, q);
    glBeginTransformFeedback(GL_TRIANGLES);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDrawArrays(GL_POINTS, 0, 1);  // wrong mode while capturing triangles
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_OPERATION));
    glEndTransformFeedback();
    glEndQuery(GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN);
    glDisable(GL_RASTERIZER_DISCARD);
    GLuint written = 0;
    glGetQueryObjectuiv(q, GL_QUERY_RESULT, &written);
    CHECK_EQ(written, 2u);
    float data[6 * 5] = {};
    glGetBufferSubData(GL_TRANSFORM_FEEDBACK_BUFFER, 0, sizeof(data), data);
    // Strip 0,1,2,3 captures triangles (0,1,2) and (2,1,3).
    const int order[6] = {0, 1, 2, 2, 1, 3};
    bool ok = true;
    for (int i = 0; i < 6; ++i) {
        const float* v = &data[i * 5];
        int id;
        std::memcpy(&id, &v[4], 4);
        if (v[2] != static_cast<float>(order[i]) || v[3] != 1.0f || id != order[i] * 10) ok = false;
    }
    CHECK(ok);
    glDeleteQueries(1, &q);
    glDeleteBuffers(1, &xfbBuffer);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
    CHECK_GL();
}

GPU_TEST(gpu_transform_feedback_reserved_names) {
    // Varyings named like the capture code's temporaries or like Vulkan GLSL
    // keywords, captured from a vertex stage made of two shader objects.
    const char* lib = "#version 330 core\nfloat scaled(float x) { return x * 2.0; }\n";
    const char* vs = R"(#version 330 core
in vec2 position;
out float v;
out vec4 sampler;
float scaled(float x);
void main() { v = scaled(float(gl_VertexID)); sampler = vec4(float(gl_VertexID) + 0.5); gl_Position = vec4(position, 0.0, 1.0); })";
    const char* fs = R"(#version 330 core
in float v; in vec4 sampler; out vec4 o; void main() { o = sampler * v; })";
    GLuint p = glCreateProgram();
    for (const auto& [type, src] : {std::pair<GLenum, const char*>{GL_VERTEX_SHADER, lib}, {GL_VERTEX_SHADER, vs}, {GL_FRAGMENT_SHADER, fs}}) {
        GLuint s = glCreateShader(type);
        glShaderSource(s, 1, &src, nullptr);
        glCompileShader(s);
        glAttachShader(p, s);
        glDeleteShader(s);
    }
    glBindAttribLocation(p, 0, "position");
    const char* varyings[] = {"v", "sampler"};
    glTransformFeedbackVaryings(p, 2, varyings, GL_INTERLEAVED_ATTRIBS);
    glLinkProgram(p);
    GLint linked = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &linked);
    CHECK(linked);
    GLuint vao = gpu::quad();
    GLuint buffer = 0;
    glGenBuffers(1, &buffer);
    glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, buffer);
    glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, 3 * 5 * 4, nullptr, GL_STATIC_READ);
    glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, buffer);
    glUseProgram(p);
    glEnable(GL_RASTERIZER_DISCARD);
    glBeginTransformFeedback(GL_POINTS);
    glDrawArrays(GL_POINTS, 0, 3);
    glEndTransformFeedback();
    glDisable(GL_RASTERIZER_DISCARD);
    CHECK_GL();
    float data[3 * 5] = {};
    glGetBufferSubData(GL_TRANSFORM_FEEDBACK_BUFFER, 0, sizeof(data), data);
    bool ok = true;
    for (int i = 0; i < 3; ++i)
        if (data[i * 5] != 2.0f * i || data[i * 5 + 1] != i + 0.5f || data[i * 5 + 4] != i + 0.5f) ok = false;
    if (!ok) std::fprintf(stderr, "  captured %g %g, %g %g, %g %g\n", data[0], data[1], data[5], data[6], data[10], data[11]);
    CHECK(ok);
    glDeleteBuffers(1, &buffer);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
    CHECK_GL();
}
