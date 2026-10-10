// GL_ARB_separate_shader_objects.
#include "gpu.h"

GPU_TEST(gpu_separate_shader_objects) {
    // The vertex program has an output the fragment program does not read;
    // the shared varying must still match by name.
    const char* vs = R"(#version 330 core
in vec2 position; out vec4 unused; out vec4 shade;
void main() { unused = vec4(1.0); shade = vec4(0.0, 1.0, 0.0, 1.0); gl_Position = vec4(position, 0.0, 1.0); })";
    const char* fs = R"(#version 330 core
in vec4 shade; uniform vec4 tint; out vec4 o; void main() { o = shade * tint; })";
    GLuint vp = glCreateShaderProgramv(GL_VERTEX_SHADER, 1, &vs);
    GLuint fp = glCreateShaderProgramv(GL_FRAGMENT_SHADER, 1, &fs);
    GLint linked = 0;
    glGetProgramiv(vp, GL_LINK_STATUS, &linked);
    CHECK(linked);
    glGetProgramiv(fp, GL_LINK_STATUS, &linked);
    CHECK(linked);
    GLuint pipeline = 0;
    glGenProgramPipelines(1, &pipeline);
    glUseProgramStages(pipeline, GL_VERTEX_SHADER_BIT, vp);
    glUseProgramStages(pipeline, GL_FRAGMENT_SHADER_BIT, fp);
    glProgramUniform4f(fp, glGetUniformLocation(fp, "tint"), 1.0f, 1.0f, 1.0f, 1.0f);
    GLint stage = 0;
    glGetProgramPipelineiv(pipeline, GL_FRAGMENT_SHADER, &stage);
    CHECK_EQ(static_cast<GLuint>(stage), fp);
    glValidateProgramPipeline(pipeline);
    GLint valid = 0;
    glGetProgramPipelineiv(pipeline, GL_VALIDATE_STATUS, &valid);
    CHECK(valid);
    glUseProgram(0);
    glBindProgramPipeline(pipeline);
    GLuint vao = gpu::quad();
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    CHECK_GL();
    const uint8_t green[4] = {0, 255, 0, 255};
    CHECK(gpu::near(gpu::readRGBA8(32, 32, 1, 1).data(), green));
    // glUniform* goes to the active program; a current program overrides the pipeline.
    glActiveShaderProgram(pipeline, fp);
    glUniform4f(glGetUniformLocation(fp, "tint"), 0.5f, 0.5f, 0.5f, 1.0f);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    const uint8_t half[4] = {0, 128, 0, 255};
    CHECK(gpu::near(gpu::readRGBA8(32, 32, 1, 1).data(), half, 2));
    GLuint plain = gpu::program("#version 330 core\nin vec2 position; void main() { gl_Position = vec4(position, 0.0, 1.0); }",
                                "#version 330 core\nout vec4 o; void main() { o = vec4(1.0, 0.0, 0.0, 1.0); }");
    glUseProgram(plain);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    const uint8_t red[4] = {255, 0, 0, 255};
    CHECK(gpu::near(gpu::readRGBA8(32, 32, 1, 1).data(), red));
    // A non-separable program cannot join a pipeline.
    glUseProgramStages(pipeline, GL_VERTEX_SHADER_BIT, plain);
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_OPERATION));
    glUseProgram(0);
    glBindProgramPipeline(0);
    glDeleteProgramPipelines(1, &pipeline);
    glDeleteProgram(vp);
    glDeleteProgram(fp);
    glDeleteProgram(plain);
    glDeleteVertexArrays(1, &vao);
    CHECK_GL();
}

GPU_TEST(gpu_separate_shader_blocks_locations_geometry) {
    // Blocks match by block name (the instance names differ), explicit
    // locations match by location (the names differ), and a separable
    // vertex + geometry program feeds a separate fragment program.
    const char* vs = R"(#version 330 core
#extension GL_ARB_separate_shader_objects : require
in vec2 position;
out VS_OUT { vec4 c; } vs_out;
layout(location = 3) out vec4 extraOut;
void main() { vs_out.c = vec4(0.0, 1.0, 0.0, 1.0); extraOut = vec4(0.0, 0.0, 0.5, 0.0); gl_Position = vec4(position, 0.0, 1.0); })";
    const char* gs = R"(#version 330 core
#extension GL_ARB_separate_shader_objects : require
layout(triangles) in;
layout(triangle_strip, max_vertices = 3) out;
in VS_OUT { vec4 c; } gs_in[];
layout(location = 3) in vec4 extraOut[];
out VS_OUT { vec4 c; } gs_out;
layout(location = 3) out vec4 extraGs;
void main() {
    for (int i = 0; i < 3; ++i) {
        gs_out.c = gs_in[i].c.grba;
        extraGs = extraOut[i];
        gl_Position = gl_in[i].gl_Position;
        EmitVertex();
    }
})";
    const char* fs = R"(#version 330 core
#extension GL_ARB_separate_shader_objects : require
in VS_OUT { vec4 c; } fs_in;
layout(location = 3) in vec4 extra;
out vec4 o;
void main() { o = fs_in.c + extra; })";
    GLuint vp = glCreateShaderProgramv(GL_VERTEX_SHADER, 1, &vs);
    GLuint fp = glCreateShaderProgramv(GL_FRAGMENT_SHADER, 1, &fs);
    GLint linked = 0;
    glGetProgramiv(vp, GL_LINK_STATUS, &linked);
    CHECK(linked);
    glGetProgramiv(fp, GL_LINK_STATUS, &linked);
    CHECK(linked);
    GLuint pipeline = 0;
    glGenProgramPipelines(1, &pipeline);
    glUseProgramStages(pipeline, GL_VERTEX_SHADER_BIT, vp);
    glUseProgramStages(pipeline, GL_FRAGMENT_SHADER_BIT, fp);
    glBindProgramPipeline(pipeline);
    GLuint vao = gpu::quad();
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    CHECK_GL();
    const uint8_t green[4] = {0, 255, 128, 255};
    CHECK(gpu::near(gpu::readRGBA8(32, 32, 1, 1).data(), green, 2));

    GLuint vgp = glCreateProgram();
    glProgramParameteri(vgp, GL_PROGRAM_SEPARABLE, GL_TRUE);
    for (const auto& [type, src] : {std::pair<GLenum, const char*>{GL_VERTEX_SHADER, vs}, {GL_GEOMETRY_SHADER, gs}}) {
        GLuint s = glCreateShader(type);
        glShaderSource(s, 1, &src, nullptr);
        glCompileShader(s);
        glAttachShader(vgp, s);
        glDeleteShader(s);
    }
    glLinkProgram(vgp);
    glGetProgramiv(vgp, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[4096] = {};
        glGetProgramInfoLog(vgp, sizeof(log), nullptr, log);
        std::fprintf(stderr, "link failed:\n%s\n", log);
    }
    CHECK(linked);
    glUseProgramStages(pipeline, GL_VERTEX_SHADER_BIT | GL_GEOMETRY_SHADER_BIT, vgp);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    CHECK_GL();
    const uint8_t red[4] = {255, 0, 128, 255};
    CHECK(gpu::near(gpu::readRGBA8(32, 32, 1, 1).data(), red, 2));
    glBindProgramPipeline(0);
    glDeleteProgramPipelines(1, &pipeline);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(vp);
    glDeleteProgram(fp);
    glDeleteProgram(vgp);
    CHECK_GL();
}
