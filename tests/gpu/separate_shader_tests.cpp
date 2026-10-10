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
