// GL_ARB_compute_shader.
#include "gpu.h"

GPU_TEST(gpu_compute_dispatch) {
    const char* source = R"(#version 330 core
#extension GL_ARB_compute_shader : require
layout(local_size_x = 8, local_size_y = 4) in;
uniform vec4 scale;
shared float values[32];
void main() {
    values[gl_LocalInvocationIndex] = scale.x * float(gl_GlobalInvocationID.x);
    barrier();
})";
    GLuint shader = glCreateShader(GL_COMPUTE_SHADER);
    CHECK_GL();
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048] = {};
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        std::fprintf(stderr, "%s\n", log);
    }
    CHECK(ok);
    GLuint p = glCreateProgram();
    glAttachShader(p, shader);
    glLinkProgram(p);
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    CHECK(ok);
    GLint size[3] = {};
    glGetProgramiv(p, GL_COMPUTE_WORK_GROUP_SIZE, size);
    CHECK(size[0] == 8 && size[1] == 4 && size[2] == 1);
    GLint maxCount = 0;
    glGetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_COUNT, 0, &maxCount);
    CHECK(maxCount >= 65535);
    glDispatchCompute(1, 1, 1);  // no compute program current
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_OPERATION));
    glUseProgram(p);
    glDispatchCompute(4, 2, 1);
    CHECK_GL();
    glDispatchCompute(70000, 1, 1);
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_VALUE));
    GLuint indirect = 0;
    glGenBuffers(1, &indirect);
    glBindBuffer(GL_DISPATCH_INDIRECT_BUFFER, indirect);
    const GLuint groups[3] = {2, 2, 1};
    glBufferData(GL_DISPATCH_INDIRECT_BUFFER, sizeof(groups), groups, GL_STATIC_DRAW);
    glDispatchComputeIndirect(0);
    CHECK_GL();
    glDispatchComputeIndirect(4);  // reads past the end
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_OPERATION));
    glFinish();
    // A compute shader cannot share a program with other stages.
    GLuint mixed = glCreateProgram();
    glAttachShader(mixed, shader);
    const char* vs = "#version 330 core\nvoid main() { gl_Position = vec4(0.0); }\n";
    GLuint v = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(v, 1, &vs, nullptr);
    glCompileShader(v);
    glAttachShader(mixed, v);
    glLinkProgram(mixed);
    glGetProgramiv(mixed, GL_LINK_STATUS, &ok);
    CHECK(!ok);
    glDeleteProgram(mixed);
    glDeleteShader(v);
    glDeleteBuffers(1, &indirect);
    glUseProgram(0);
    glDeleteProgram(p);
    glDeleteShader(shader);
    CHECK_GL();
}
