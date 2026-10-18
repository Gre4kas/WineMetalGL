// Program pipelines whose stages come from different separable programs
// (shader::linkPipeline): every stage keeps its own program's uniforms.
#include <unistd.h>
#include <vector>

#include "gpu.h"

namespace {

GLuint separable(GLenum type, const char* source, std::initializer_list<const char*> varyings = {}) {
    GLuint p = glCreateProgram();
    glProgramParameteri(p, GL_PROGRAM_SEPARABLE, GL_TRUE);
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &source, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    CHECK(ok);
    glAttachShader(p, s);
    if (varyings.size()) {
        std::vector<const char*> names(varyings);
        glTransformFeedbackVaryings(p, static_cast<GLsizei>(names.size()), names.data(), GL_INTERLEAVED_ATTRIBS);
    }
    glLinkProgram(p);
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096] = {};
        glGetProgramInfoLog(p, sizeof(log), nullptr, log);
        std::fprintf(stderr, "  link failed:\n%s\n", log);
    }
    CHECK(ok);
    glDeleteShader(s);
    return p;
}

void setUniform(GLuint program, const char* name, float value) {
    auto programUniform1f = reinterpret_cast<PFN_glProgramUniform1f>(wmgl_get_proc_address("glProgramUniform1f"));
    programUniform1f(program, glGetUniformLocation(program, name), value);
}

// Captures `floats` floats of the pipeline's output with rasterization off.
std::vector<float> capture(GLuint pipeline, GLenum mode, GLenum drawMode, int vertices, size_t floats) {
    GLuint vao = 0, buffer = 0;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &buffer);
    glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, buffer);
    std::vector<float> fill(floats, -1.0f);
    glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, static_cast<GLsizeiptr>(floats * 4), fill.data(), GL_STATIC_READ);
    glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, buffer);
    glUseProgram(0);
    glBindProgramPipeline(pipeline);
    glEnable(GL_RASTERIZER_DISCARD);
    glBeginTransformFeedback(mode);
    glDrawArrays(drawMode, 0, vertices);
    glEndTransformFeedback();
    glDisable(GL_RASTERIZER_DISCARD);
    CHECK_GL();
    std::vector<float> out(floats);
    glGetBufferSubData(GL_TRANSFORM_FEEDBACK_BUFFER, 0, static_cast<GLsizeiptr>(floats * 4), out.data());
    glBindProgramPipeline(0);
    glDeleteBuffers(1, &buffer);
    glDeleteVertexArrays(1, &vao);
    return out;
}

const char* kVertex = R"(#version 410 core
uniform float bias;
out float vid;
void main() { vid = float(gl_VertexID) + bias; gl_Position = vec4(0.0); })";

}  // namespace

GPU_TEST_VERSION(gpu_pipeline_tessellation_stages_from_three_programs, 41) {
    // Vertex, control and evaluation stages from three programs; "bias" is a
    // different uniform in the vertex and evaluation programs.
    const char* tcs = R"(#version 410 core
layout(vertices = 2) out;
in float vid[];
out float v[];
uniform float level;
void main() {
    v[gl_InvocationID] = vid[gl_InvocationID];
    gl_TessLevelOuter[0] = 1.0;
    gl_TessLevelOuter[1] = level;
})";
    const char* tes = R"(#version 410 core
layout(isolines, point_mode) in;
in float v[];
uniform float bias;
out vec2 result;
void main() { result = vec2(v[0] + v[1] + bias, gl_TessCoord.x); gl_Position = vec4(0.0); })";
    GLuint vs = separable(GL_VERTEX_SHADER, kVertex), tc = separable(GL_TESS_CONTROL_SHADER, tcs),
           te = separable(GL_TESS_EVALUATION_SHADER, tes, {"result"});
    setUniform(vs, "bias", 10.0f);
    setUniform(tc, "level", 2.0f);
    setUniform(te, "bias", 1000.0f);
    GLuint pipeline = 0;
    glGenProgramPipelines(1, &pipeline);
    glUseProgramStages(pipeline, GL_VERTEX_SHADER_BIT, vs);
    glUseProgramStages(pipeline, GL_TESS_CONTROL_SHADER_BIT, tc);
    glUseProgramStages(pipeline, GL_TESS_EVALUATION_SHADER_BIT, te);
    GLint valid = 0;
    glValidateProgramPipeline(pipeline);
    glGetProgramPipelineiv(pipeline, GL_VALIDATE_STATUS, &valid);
    CHECK_EQ(valid, GL_TRUE);
    auto patchParameteri = reinterpret_cast<PFN_glPatchParameteri>(wmgl_get_proc_address("glPatchParameteri"));
    patchParameteri(GL_PATCH_VERTICES, 2);
    // One patch of two vertices; one isoline of two segments: three points.
    const std::vector<float> data = capture(pipeline, GL_POINTS, GL_PATCHES, 2, 2 * 4);
    for (int i = 0; i < 3; ++i) CHECK_EQ(data[i * 2], (0.0f + 10.0f) + (1.0f + 10.0f) + 1000.0f);
    CHECK_EQ(data[6], -1.0f);  // nothing beyond three points
    glDeleteProgramPipelines(1, &pipeline);
    for (GLuint p : {vs, tc, te}) glDeleteProgram(p);
}

GPU_TEST_VERSION(gpu_pipeline_geometry_stage_from_another_program, 41) {
    // A geometry stage from its own program, capturing transform feedback.
    const char* gs = R"(#version 410 core
layout(points) in;
layout(points, max_vertices = 2) out;
in float vid[];
uniform float bias;
out float emitted;
void main() {
    for (int i = 0; i < 2; ++i) {
        emitted = vid[0] + bias + float(i);
        gl_Position = vec4(0.0);
        EmitVertex();
    }
})";
    GLuint vs = separable(GL_VERTEX_SHADER, kVertex), geo = separable(GL_GEOMETRY_SHADER, gs, {"emitted"});
    setUniform(vs, "bias", 10.0f);
    setUniform(geo, "bias", 100.0f);
    GLuint pipeline = 0;
    glGenProgramPipelines(1, &pipeline);
    glUseProgramStages(pipeline, GL_VERTEX_SHADER_BIT, vs);
    glUseProgramStages(pipeline, GL_GEOMETRY_SHADER_BIT, geo);
    const std::vector<float> data = capture(pipeline, GL_POINTS, GL_POINTS, 2, 4);
    const float want[4] = {110, 111, 111, 112};
    for (int i = 0; i < 4; ++i) CHECK_EQ(data[i], want[i]);
    // Without a vertex stage the pipeline does not validate.
    glUseProgramStages(pipeline, GL_VERTEX_SHADER_BIT, 0);
    GLint valid = 1;
    glValidateProgramPipeline(pipeline);
    glGetProgramPipelineiv(pipeline, GL_VALIDATE_STATUS, &valid);
    CHECK_EQ(valid, GL_FALSE);
    glDeleteProgramPipelines(1, &pipeline);
    glDeleteProgram(vs);
    glDeleteProgram(geo);
}

// Render pipelines are predicted (PipelineCache): a render state a draw
// needed is built in the background for the other linked programs with the
// same vertex inputs and fragment outputs, so their first draw in that state
// does not compile a pipeline (DOOM's effects hitched on it, issue #36).
GPU_TEST_VERSION(gpu_pipeline_prediction, 46) {
    const char* vs = "#version 460 core\nlayout(location = 0) in vec2 p;\nvoid main() { gl_Position = vec4(p, 0.0, 1.0); }\n";
    GLuint a = gpu::program(vs, "#version 460 core\nout vec4 o;\nvoid main() { o = vec4(1.0, 0.0, 0.0, 1.0); }\n");
    GLuint b = gpu::program(vs, "#version 460 core\nout vec4 o;\nvoid main() { o = vec4(0.0, 1.0, 0.0, 1.0); }\n");
    GLuint vao = gpu::quad();
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    unsigned long long before = 0, predicted = 0;
    CHECK(wmgl_stats_counter("pipelinesPredicted", &before));
    // A render state no draw used yet: additive blending.
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);
    glUseProgram(a);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glFinish();
    for (int i = 0; i < 1000 && predicted <= before; ++i) {
        wmgl_stats_counter("pipelinesPredicted", &predicted);
        if (predicted <= before) usleep(10000);
    }
    CHECK(predicted > before);
    {
        gpu::Counters counters;
        glUseProgram(b);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glFinish();
        CHECK_EQ(counters["pipelineMisses"], 0ull);
    }
    const uint8_t yellow[4] = {255, 255, 0, 255};
    CHECK(gpu::near(gpu::readRGBA8(32, 32, 1, 1).data(), yellow));
    glDisable(GL_BLEND);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(a);
    glDeleteProgram(b);
    CHECK_GL();
}
