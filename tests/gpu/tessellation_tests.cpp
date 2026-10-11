// Tessellation emulation (core/shader/tessellation.h, core/tess/tessellator.h).
// GL 4.x contexts: gpu_gl4_* tests run with the development override (main.cpp).
#include <algorithm>
#include <cmath>
#include <set>
#include <utility>

#include "gpu.h"

namespace {

PFN_glPatchParameteri patchParameteri() {
    return reinterpret_cast<PFN_glPatchParameteri>(wmgl_get_proc_address("glPatchParameteri"));
}

PFN_glPatchParameterfv patchParameterfv() {
    return reinterpret_cast<PFN_glPatchParameterfv>(wmgl_get_proc_address("glPatchParameterfv"));
}

struct Stages {
    const char* vs = nullptr;
    const char* tcs = nullptr;
    const char* tes = nullptr;
    const char* gs = nullptr;
    const char* fs = nullptr;
};

GLuint link(const Stages& stages, std::initializer_list<const char*> varyings = {}) {
    GLuint p = glCreateProgram();
    auto attach = [&](GLenum type, const char* source) {
        if (!source) return;
        GLuint s = glCreateShader(type);
        glShaderSource(s, 1, &source, nullptr);
        glCompileShader(s);
        GLint ok = 0;
        glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[4096] = {};
            glGetShaderInfoLog(s, sizeof(log), nullptr, log);
            std::fprintf(stderr, "  compile failed:\n%s\n", log);
        }
        CHECK(ok);
        glAttachShader(p, s);
        glDeleteShader(s);
    };
    attach(GL_VERTEX_SHADER, stages.vs);
    attach(GL_TESS_CONTROL_SHADER, stages.tcs);
    attach(GL_TESS_EVALUATION_SHADER, stages.tes);
    attach(GL_GEOMETRY_SHADER, stages.gs);
    attach(GL_FRAGMENT_SHADER, stages.fs);
    if (varyings.size()) {
        std::vector<const char*> names(varyings);
        glTransformFeedbackVaryings(p, static_cast<GLsizei>(names.size()), names.data(), GL_INTERLEAVED_ATTRIBS);
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
    return p;
}

const char* kVertexIdVs = R"(#version 400 core
out float vid;
void main() { vid = float(gl_VertexID); gl_Position = vec4(float(gl_VertexID), 0.0, 0.0, 1.0); })";

// Draws `vertexCount` vertices as patches of `patchVertices` with transform
// feedback (`mode`) and rasterization off; returns the captured floats.
std::vector<float> capture(GLuint program, GLenum mode, int patchVertices, int vertexCount, size_t floats,
                           GLuint* primitives = nullptr) {
    GLuint vao = 0, buffer = 0, query = 0;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &buffer);
    glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, buffer);
    std::vector<float> zero(floats, -1.0f);
    glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, static_cast<GLsizeiptr>(floats * 4), zero.data(), GL_STATIC_READ);
    glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, buffer);
    glGenQueries(1, &query);
    glUseProgram(program);
    patchParameteri()(GL_PATCH_VERTICES, patchVertices);
    glEnable(GL_RASTERIZER_DISCARD);
    glBeginQuery(GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN, query);
    glBeginTransformFeedback(mode);
    glDrawArrays(GL_PATCHES, 0, vertexCount);
    glEndTransformFeedback();
    glEndQuery(GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN);
    glDisable(GL_RASTERIZER_DISCARD);
    CHECK_GL();
    GLuint written = 0;
    glGetQueryObjectuiv(query, GL_QUERY_RESULT, &written);
    if (primitives) *primitives = written;
    std::vector<float> out(floats);
    glGetBufferSubData(GL_TRANSFORM_FEEDBACK_BUFFER, 0, static_cast<GLsizeiptr>(floats * 4), out.data());
    glDeleteQueries(1, &query);
    glDeleteBuffers(1, &buffer);
    glDeleteVertexArrays(1, &vao);
    return out;
}

// Rounds to 1/1000 so sets of coordinates compare exactly.
int milli(float x) { return static_cast<int>(std::lround(x * 1000.0f)); }

}  // namespace

GPU_TEST(gpu_gl4_tess_triangles_default_levels_point_mode) {
    // No control shader: the default levels (2) apply. A triangle with every
    // level 2 has 7 distinct vertices: corners, edge midpoints and the centre.
    const char* tes = R"(#version 400 core
layout(triangles, equal_spacing, point_mode) in;
out vec3 coord;
void main() { coord = gl_TessCoord; gl_Position = vec4(0.0); })";
    GLuint p = link({kVertexIdVs, nullptr, tes, nullptr, nullptr}, {"coord"});
    const float two[4] = {2, 2, 2, 2};
    patchParameterfv()(GL_PATCH_DEFAULT_OUTER_LEVEL, two);
    patchParameterfv()(GL_PATCH_DEFAULT_INNER_LEVEL, two);
    GLuint primitives = 0;
    const std::vector<float> data = capture(p, GL_POINTS, 3, 3, 3 * 16, &primitives);
    CHECK_EQ(primitives, 7u);
    std::set<std::vector<int>> got, want = {{1000, 0, 0}, {0, 1000, 0}, {0, 0, 1000}, {500, 500, 0},
                                            {500, 0, 500}, {0, 500, 500}, {333, 333, 333}};
    for (GLuint i = 0; i < std::min(primitives, 16u); ++i)
        got.insert({milli(data[i * 3]), milli(data[i * 3 + 1]), milli(data[i * 3 + 2])});
    CHECK(got == want);
    const float one[4] = {1, 1, 1, 1};
    patchParameterfv()(GL_PATCH_DEFAULT_OUTER_LEVEL, one);
    patchParameterfv()(GL_PATCH_DEFAULT_INNER_LEVEL, one);
    glDeleteProgram(p);
}

GPU_TEST(gpu_gl4_tess_quads_grid) {
    // Levels 2 everywhere: a 3x3 grid of domain points.
    const char* tcs = R"(#version 400 core
layout(vertices = 1) out;
void main() {
    gl_TessLevelOuter[0] = 2.0; gl_TessLevelOuter[1] = 2.0; gl_TessLevelOuter[2] = 2.0; gl_TessLevelOuter[3] = 2.0;
    gl_TessLevelInner[0] = 2.0; gl_TessLevelInner[1] = 2.0;
})";
    const char* tes = R"(#version 400 core
layout(quads, point_mode) in;
out vec2 coord;
void main() { coord = gl_TessCoord.xy; gl_Position = vec4(0.0); })";
    GLuint p = link({kVertexIdVs, tcs, tes, nullptr, nullptr}, {"coord"});
    GLuint primitives = 0;
    const std::vector<float> data = capture(p, GL_POINTS, 1, 1, 2 * 16, &primitives);
    CHECK_EQ(primitives, 9u);
    std::set<std::pair<int, int>> got, want;
    for (int u = 0; u <= 1000; u += 500)
        for (int v = 0; v <= 1000; v += 500) want.insert({u, v});
    for (GLuint i = 0; i < std::min(primitives, 16u); ++i) got.insert({milli(data[i * 2]), milli(data[i * 2 + 1])});
    CHECK(got == want);
    glDeleteProgram(p);
}

GPU_TEST(gpu_gl4_tess_isolines) {
    // outer[0] = 2 lines (v = 0, 1/2; v = 1 is not generated) of outer[1] = 3 segments.
    const char* tcs = R"(#version 400 core
layout(vertices = 1) out;
void main() { gl_TessLevelOuter[0] = 2.0; gl_TessLevelOuter[1] = 3.0; })";
    const char* tes = R"(#version 400 core
layout(isolines, equal_spacing) in;
out vec2 coord;
void main() { coord = gl_TessCoord.xy; gl_Position = vec4(0.0); })";
    GLuint p = link({kVertexIdVs, tcs, tes, nullptr, nullptr}, {"coord"});
    GLuint primitives = 0;
    const std::vector<float> data = capture(p, GL_LINES, 1, 1, 4 * 16, &primitives);
    CHECK_EQ(primitives, 6u);
    // Segment ends at u = 0, 1/3, 2/3, 1.
    const int ends[4] = {0, 333, 667, 1000};
    std::set<std::vector<int>> got, want;
    for (int line = 0; line < 2; ++line)
        for (int s = 0; s < 3; ++s) want.insert({ends[s], line * 500, ends[s + 1], line * 500});
    for (GLuint i = 0; i < std::min(primitives, 16u); ++i) {
        std::vector<int> segment = {milli(data[i * 4]), milli(data[i * 4 + 1]), milli(data[i * 4 + 2]), milli(data[i * 4 + 3])};
        if (segment[0] > segment[2]) segment = {segment[2], segment[3], segment[0], segment[1]};
        got.insert(segment);
    }
    CHECK(got == want);
    glDeleteProgram(p);
}

GPU_TEST(gpu_gl4_tess_fractional_odd_spacing) {
    // An isoline at level 2.5, fractional odd: three segments, the two
    // outer ones equal and shorter than the middle one, placed symmetrically.
    const char* tcs = R"(#version 400 core
layout(vertices = 1) out;
void main() { gl_TessLevelOuter[0] = 1.0; gl_TessLevelOuter[1] = 2.5; })";
    const char* tes = R"(#version 400 core
layout(isolines, fractional_odd_spacing, point_mode) in;
out float u;
void main() { u = gl_TessCoord.x; gl_Position = vec4(0.0); })";
    GLuint p = link({kVertexIdVs, tcs, tes, nullptr, nullptr}, {"u"});
    GLuint primitives = 0;
    std::vector<float> data = capture(p, GL_POINTS, 1, 1, 16, &primitives);
    CHECK_EQ(primitives, 4u);
    std::sort(data.begin(), data.begin() + std::min<GLuint>(primitives, 16));
    if (primitives == 4) {
        CHECK(std::fabs(data[0]) < 1e-6f && std::fabs(data[3] - 1.0f) < 1e-6f);
        CHECK(std::fabs(data[1] - (1.0f - data[2])) < 1e-5f);              // symmetric
        CHECK(data[1] > 0.0f && data[1] < data[2] - data[1]);             // outer segments shorter
    }
    glDeleteProgram(p);
}

GPU_TEST(gpu_gl4_tess_control_barrier_and_patch_outputs) {
    // Each invocation writes its output, then (after barrier()) invocation 0
    // sums every invocation's output into a per-patch output.
    const char* tcs = R"(#version 400 core
layout(vertices = 4) out;
in float vid[];
out float value[];
patch out float sum;
patch out float verticesIn;
void main() {
    value[gl_InvocationID] = vid[gl_InvocationID] * 10.0 + float(gl_PrimitiveID) * 100.0;
    barrier();
    if (gl_InvocationID == 0) {
        sum = value[0] + value[1] + value[2] + value[3];
        verticesIn = float(gl_PatchVerticesIn);
        gl_TessLevelOuter[0] = 1.0; gl_TessLevelOuter[1] = 1.0;
    }
})";
    const char* tes = R"(#version 400 core
layout(isolines, point_mode) in;
in float value[];
patch in float sum;
patch in float verticesIn;
out vec3 result;
void main() { result = vec3(sum, value[2], verticesIn); gl_Position = vec4(0.0); })";
    GLuint p = link({kVertexIdVs, tcs, tes, nullptr, nullptr}, {"result"});
    GLuint primitives = 0;
    // Two patches of 4 input vertices; isolines at level 1 give 2 points each.
    const std::vector<float> data = capture(p, GL_POINTS, 4, 8, 3 * 8, &primitives);
    CHECK_EQ(primitives, 4u);
    for (GLuint i = 0; i < std::min(primitives, 4u); ++i) {
        const int patch = static_cast<int>(i / 2);
        const float first = static_cast<float>(patch * 4);
        CHECK_EQ(data[i * 3], (first * 4 + 6) * 10.0f + 400.0f * patch);
        CHECK_EQ(data[i * 3 + 1], (first + 2) * 10.0f + 100.0f * patch);
        CHECK_EQ(data[i * 3 + 2], 4.0f);
    }
    glDeleteProgram(p);
}

GPU_TEST(gpu_gl4_tess_geometry_after_evaluation) {
    // Tessellated points feed a geometry shader that emits two points each.
    const char* tes = R"(#version 400 core
layout(triangles, point_mode) in;
out vec3 coord;
void main() { coord = gl_TessCoord; gl_Position = vec4(gl_TessCoord, 1.0); })";
    const char* gs = R"(#version 400 core
layout(points) in;
layout(points, max_vertices = 2) out;
in vec3 coord[];
out vec4 emitted;
void main() {
    for (int i = 0; i < 2; ++i) {
        emitted = vec4(coord[0], float(i));
        gl_Position = gl_in[0].gl_Position;
        EmitVertex();
    }
})";
    GLuint p = link({kVertexIdVs, nullptr, tes, gs, nullptr}, {"emitted"});
    const float two[4] = {2, 2, 2, 2};
    patchParameterfv()(GL_PATCH_DEFAULT_OUTER_LEVEL, two);
    patchParameterfv()(GL_PATCH_DEFAULT_INNER_LEVEL, two);
    GLuint primitives = 0;
    const std::vector<float> data = capture(p, GL_POINTS, 3, 3, 4 * 16, &primitives);
    CHECK_EQ(primitives, 14u);
    for (GLuint i = 0; i < std::min(primitives, 14u); ++i) {
        CHECK(std::fabs(data[i * 4] + data[i * 4 + 1] + data[i * 4 + 2] - 1.0f) < 1e-5f);
        CHECK_EQ(data[i * 4 + 3], static_cast<float>(i % 2));
    }
    const float one[4] = {1, 1, 1, 1};
    patchParameterfv()(GL_PATCH_DEFAULT_OUTER_LEVEL, one);
    patchParameterfv()(GL_PATCH_DEFAULT_INNER_LEVEL, one);
    glDeleteProgram(p);
}

GPU_TEST(gpu_gl4_tess_render_quad_with_culling) {
    // A quad patch tessellated and rendered over the viewport; with back faces
    // culled, a counter-clockwise domain must stay visible.
    const char* vs = R"(#version 400 core
in vec2 position;
out vec2 pos;
void main() { pos = position; gl_Position = vec4(position, 0.0, 1.0); })";
    const char* tcs = R"(#version 400 core
layout(vertices = 4) out;
in vec2 pos[];
out vec2 cp[];
void main() {
    cp[gl_InvocationID] = pos[gl_InvocationID];
    gl_TessLevelOuter[0] = 4.0; gl_TessLevelOuter[1] = 4.0; gl_TessLevelOuter[2] = 4.0; gl_TessLevelOuter[3] = 4.0;
    gl_TessLevelInner[0] = 4.0; gl_TessLevelInner[1] = 4.0;
})";
    const char* tes = R"(#version 400 core
layout(quads, ccw) in;
in vec2 cp[];
out vec4 color;
void main() {
    // Control points: (-1,-1), (1,-1), (-1,1), (1,1) (the quad() strip order).
    vec2 bottom = mix(cp[0], cp[1], gl_TessCoord.x), top = mix(cp[2], cp[3], gl_TessCoord.x);
    gl_Position = vec4(mix(bottom, top, gl_TessCoord.y), 0.0, 1.0);
    color = vec4(0.0, 1.0, 0.0, 1.0);
})";
    const char* fs = R"(#version 400 core
in vec4 color; out vec4 o; void main() { o = color; })";
    GLuint p = link({vs, tcs, tes, nullptr, fs});
    GLuint vao = gpu::quad();
    glUseProgram(p);
    patchParameteri()(GL_PATCH_VERTICES, 4);
    glClearColor(1, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_CULL_FACE);
    glDrawArrays(GL_PATCHES, 0, 4);
    glDisable(GL_CULL_FACE);
    CHECK_GL();
    const std::vector<uint8_t> pixels = gpu::readRGBA8(0, 0, 64, 64);
    const uint8_t green[4] = {0, 255, 0, 255};
    for (int y : {1, 32, 62})
        for (int x : {1, 32, 62}) CHECK(gpu::near(&pixels[(static_cast<size_t>(y) * 64 + x) * 4], green));
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
}

GPU_TEST(gpu_gl4_tess_primitives_generated) {
    // Every outer and inner level 1: one triangle per patch.
    const char* tes = R"(#version 400 core
layout(triangles) in;
void main() { gl_Position = vec4(gl_TessCoord, 1.0); })";
    const char* fs = R"(#version 400 core
out vec4 o; void main() { o = vec4(1.0); })";
    GLuint p = link({kVertexIdVs, nullptr, tes, nullptr, fs});
    GLuint vao = 0, query = 0;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenQueries(1, &query);
    glUseProgram(p);
    patchParameteri()(GL_PATCH_VERTICES, 3);
    glBeginQuery(GL_PRIMITIVES_GENERATED, query);
    glDrawArrays(GL_PATCHES, 0, 11);  // three patches; the last two vertices are dropped
    glEndQuery(GL_PRIMITIVES_GENERATED);
    GLuint generated = 0;
    glGetQueryObjectuiv(query, GL_QUERY_RESULT, &generated);
    CHECK_EQ(generated, 3u);
    CHECK_GL();
    glDeleteQueries(1, &query);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
}

GPU_TEST(gpu_gl4_tess_validation_and_queries) {
    const char* tcs = R"(#version 400 core
layout(vertices = 3) out;
void main() { gl_TessLevelOuter[0] = 1.0; })";
    const char* tes = R"(#version 400 core
layout(quads, fractional_even_spacing, cw, point_mode) in;
void main() { gl_Position = vec4(0.0); })";
    const char* fs = R"(#version 400 core
out vec4 o; void main() { o = vec4(1.0); })";
    GLuint p = link({kVertexIdVs, tcs, tes, nullptr, fs});
    GLint value = 0;
    glGetProgramiv(p, GL_TESS_CONTROL_OUTPUT_VERTICES, &value);
    CHECK_EQ(value, 3);
    glGetProgramiv(p, GL_TESS_GEN_MODE, &value);
    CHECK_EQ(value, GL_QUADS);
    glGetProgramiv(p, GL_TESS_GEN_SPACING, &value);
    CHECK_EQ(value, GL_FRACTIONAL_EVEN);
    glGetProgramiv(p, GL_TESS_GEN_VERTEX_ORDER, &value);
    CHECK_EQ(value, GL_CW);
    glGetProgramiv(p, GL_TESS_GEN_POINT_MODE, &value);
    CHECK_EQ(value, GL_TRUE);
    CHECK_GL();
    GLuint vao = 0;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glUseProgram(p);
    glDrawArrays(GL_TRIANGLES, 0, 3);  // a tessellation program draws patches only
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_OPERATION));
    GLuint plain = gpu::program(kVertexIdVs, fs);
    glUseProgram(plain);
    glDrawArrays(GL_PATCHES, 0, 3);  // patches need a tessellation evaluation shader
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_OPERATION));
    patchParameteri()(GL_PATCH_VERTICES, 0);
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_VALUE));
    patchParameteri()(GL_PATCH_VERTICES, 33);
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_VALUE));
    // A control shader without an evaluation shader does not link.
    GLuint lone = glCreateProgram();
    for (auto [type, source] : {std::pair<GLenum, const char*>{GL_VERTEX_SHADER, kVertexIdVs}, {GL_TESS_CONTROL_SHADER, tcs}}) {
        GLuint s = glCreateShader(type);
        glShaderSource(s, 1, &source, nullptr);
        glCompileShader(s);
        glAttachShader(lone, s);
        glDeleteShader(s);
    }
    glLinkProgram(lone);
    glGetProgramiv(lone, GL_LINK_STATUS, &value);
    CHECK_EQ(value, GL_FALSE);
    glDeleteProgram(lone);
    glDeleteProgram(plain);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
}
