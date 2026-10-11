// Double precision (GL_ARB_gpu_shader_fp64, GL_ARB_vertex_attrib_64bit) on
// the soft-float lowering (shader/fp64.h), in 4.2 contexts.
#include <cmath>

#include "gpu.h"

// The expected values are computed with one rounding per operation, as the
// shaders compute them.
#pragma STDC FP_CONTRACT OFF

namespace {

PFN_glUniform1d pUniform1d;
PFN_glUniform3dv pUniform3dv;
PFN_glUniformMatrix3dv pUniformMatrix3dv;
PFN_glGetUniformdv pGetUniformdv;
PFN_glVertexAttribLPointer pVertexAttribLPointer;
PFN_glVertexAttribL4d pVertexAttribL4d;

void loadDoubleFunctions() {
    pUniform1d = reinterpret_cast<PFN_glUniform1d>(wmgl_get_proc_address("glUniform1d"));
    pUniform3dv = reinterpret_cast<PFN_glUniform3dv>(wmgl_get_proc_address("glUniform3dv"));
    pUniformMatrix3dv = reinterpret_cast<PFN_glUniformMatrix3dv>(wmgl_get_proc_address("glUniformMatrix3dv"));
    pGetUniformdv = reinterpret_cast<PFN_glGetUniformdv>(wmgl_get_proc_address("glGetUniformdv"));
    pVertexAttribLPointer = reinterpret_cast<PFN_glVertexAttribLPointer>(wmgl_get_proc_address("glVertexAttribLPointer"));
    pVertexAttribL4d = reinterpret_cast<PFN_glVertexAttribL4d>(wmgl_get_proc_address("glVertexAttribL4d"));
}

GLuint shader(GLenum type, const char* source) {
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

// Links the stages (vertex, then optional geometry and fragment) capturing `varyings`.
GLuint linkCapture(const char* vs, const char* gs, const char* fs, std::initializer_list<const char*> varyings) {
    GLuint p = glCreateProgram();
    glAttachShader(p, shader(GL_VERTEX_SHADER, vs));
    if (gs) glAttachShader(p, shader(GL_GEOMETRY_SHADER, gs));
    if (fs) glAttachShader(p, shader(GL_FRAGMENT_SHADER, fs));
    std::vector<const char*> names(varyings);
    glTransformFeedbackVaryings(p, static_cast<GLsizei>(names.size()), names.data(), GL_INTERLEAVED_ATTRIBS);
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096] = {};
        glGetProgramInfoLog(p, sizeof(log), nullptr, log);
        std::fprintf(stderr, "  link failed:\n%s\n", log);
        ++wmgl_test::failures;
    }
    return p;
}

// Draws `count` points with rasterization off and returns the captured doubles.
std::vector<double> capture(GLuint program, int count, size_t doubles) {
    GLuint buffer = 0;
    glGenBuffers(1, &buffer);
    glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, buffer);
    glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, static_cast<GLsizeiptr>(doubles * 8), nullptr, GL_STATIC_READ);
    glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, buffer);
    glUseProgram(program);
    glEnable(GL_RASTERIZER_DISCARD);
    glBeginTransformFeedback(GL_POINTS);
    glDrawArrays(GL_POINTS, 0, count);
    glEndTransformFeedback();
    glDisable(GL_RASTERIZER_DISCARD);
    std::vector<double> out(doubles);
    glGetBufferSubData(GL_TRANSFORM_FEEDBACK_BUFFER, 0, static_cast<GLsizeiptr>(doubles * 8), out.data());
    glDeleteBuffers(1, &buffer);
    return out;
}

bool sameBits(double a, double b) { return std::memcmp(&a, &b, 8) == 0; }

}  // namespace

// Arithmetic, built-ins and conversions in a vertex shader, captured as
// doubles: correctly rounded results must match the host bit for bit.
TEST(gpu_fp64_vertex_arithmetic_is_exact) {
    gpu::Fixture fixture(64, 64, true, 42);
    CHECK(fixture.ok);
    if (!fixture.ok) return;
    loadDoubleFunctions();
    const char* vs = R"(#version 400 core
uniform double a;
uniform dvec3 b;
uniform dmat3 m;
flat out dvec4 r0;
flat out dvec4 r1;
flat out double r2;
void main() {
    double x = a + double(gl_VertexID);
    r0 = dvec4(x / 3.0lf, sqrt(x), x * 1.1lf - 0.1lf, fma(x, x, -1.0lf));
    r1 = dvec4(m * b, double(float(x) * 0.5));
    r2 = floor(x * 2.5lf) + fract(x / 7.0lf) + double(int(x * -3.25lf));
    gl_Position = vec4(0.0);
})";
    GLuint p = linkCapture(vs, nullptr, nullptr, {"r0", "r1", "r2"});
    glUseProgram(p);
    const double bv[3] = {0.1, -2.5, 1e10};
    const double mv[9] = {1.0, 0.5, 0.25, -3.0, 2.0, 1e-3, 7.0, 0.0, -1.0};
    pUniform1d(glGetUniformLocation(p, "a"), 1.0 / 3.0);
    pUniform3dv(glGetUniformLocation(p, "b"), 1, bv);
    pUniformMatrix3dv(glGetUniformLocation(p, "m"), 1, GL_FALSE, mv);
    double readBack = 0;
    pGetUniformdv(p, glGetUniformLocation(p, "a"), &readBack);
    CHECK(sameBits(readBack, 1.0 / 3.0));
    GLuint vao = 0;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    const int count = 5;
    const std::vector<double> got = capture(p, count, count * 9);
    glDeleteVertexArrays(1, &vao);
    int bad = 0;
    for (int v = 0; v < count; ++v) {
        const double x = 1.0 / 3.0 + v;
        double mb[3];
        for (int r = 0; r < 3; ++r) mb[r] = mv[r] * bv[0] + mv[3 + r] * bv[1] + mv[6 + r] * bv[2];
        const double want[9] = {x / 3.0, std::sqrt(x), x * 1.1 - 0.1, x * x + -1.0, mb[0], mb[1], mb[2],
                                static_cast<double>(static_cast<float>(x) * 0.5f),
                                std::floor(x * 2.5) + ((x / 7.0) - std::floor(x / 7.0)) + static_cast<int>(x * -3.25)};
        for (int i = 0; i < 9; ++i) {
            // fma is a multiply then an add (two roundings) in this core.
            const double w = i == 3 ? (x * x) - 1.0 : want[i];
            if (!sameBits(got[static_cast<size_t>(v * 9 + i)], w) && bad++ < 4)
                std::fprintf(stderr, "  vertex %d value %d: got %.17g want %.17g\n", v, i, got[static_cast<size_t>(v * 9 + i)], w);
        }
    }
    CHECK_EQ(bad, 0);
    glDeleteProgram(p);
    CHECK_GL();
}

// A std140 block of doubles (column- and row-major matrices, padding) read
// in the vertex stage, the geometry emulation and the fragment stage.
TEST(gpu_fp64_uniform_block_in_every_stage) {
    gpu::Fixture fixture(16, 16, true, 42);
    CHECK(fixture.ok);
    if (!fixture.ok) return;
    loadDoubleFunctions();
    const char* block = R"(
layout(std140) uniform Doubles {
    int pad0;
    double d;
    dvec3 v3;
    bool pad1;
    dmat2 m2;
    layout(row_major) dmat2x3 r23;
    dmat3x2 m32;
    layout(row_major) dmat4 r4;
} u;
bool good() {
    return u.d == 1.5lf && u.v3 == dvec3(2.0, 3.0, 4.0) && u.m2 == dmat2(5.0, 6.0, 7.0, 8.0) &&
           u.r23 == dmat2x3(9.0, 10.0, 11.0, 12.0, 13.0, 14.0) && u.m32[2] == dvec2(19.0, 20.0) &&
           u.r4[1] == dvec4(22.0, 26.0, 30.0, 34.0) && u.r4[3][2] == 32.0lf;
}
)";
    const std::string vs = std::string("#version 400 core\n") + block +
                           "flat out int vsGood;\nvoid main() { vsGood = good() ? 1 : 0; gl_Position = vec4(0.0, 0.0, 0.0, 1.0); }\n";
    const std::string gs = std::string("#version 400 core\nlayout(points) in;\nlayout(points, max_vertices = 1) out;\n") + block +
                           "flat in int vsGood[];\nflat out int gsGood;\nflat out int vsSeen;\n"
                           "void main() { gsGood = good() ? 1 : 0; vsSeen = vsGood[0]; gl_Position = vec4(0.0, 0.0, 0.0, 1.0); "
                           "EmitVertex(); EndPrimitive(); }\n";
    const std::string fs = std::string("#version 400 core\n") + block +
                           "flat in int gsGood;\nflat in int vsSeen;\nout vec4 color;\n"
                           "void main() { color = vec4(good() ? 1.0 : 0.0, float(gsGood), float(vsSeen), 1.0); }\n";
    GLuint p = glCreateProgram();
    glAttachShader(p, shader(GL_VERTEX_SHADER, vs.c_str()));
    glAttachShader(p, shader(GL_GEOMETRY_SHADER, gs.c_str()));
    glAttachShader(p, shader(GL_FRAGMENT_SHADER, fs.c_str()));
    glLinkProgram(p);
    GLint linked = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &linked);
    CHECK(linked);
    // The std140 image: offsets as glslang lays the block out.
    std::vector<uint8_t> data(1024, 0);
    auto put = [&](size_t offset, double v) { std::memcpy(&data[offset], &v, 8); };
    // pad0 0, d 8, v3 32, pad1 56, m2 64 (cols 16), r23 96 (3 rows of dvec2, stride 16), m32 144 (cols 16),
    // r4 192 (rows of dvec4, stride 32).
    put(8, 1.5);
    for (int i = 0; i < 3; ++i) put(32 + 8 * i, 2.0 + i);
    for (int i = 0; i < 4; ++i) put(64 + 8 * i, 5.0 + i);
    // dmat2x3(9..14) column-major: col0 = (9,10,11), col1 = (12,13,14); row r = (col0[r], col1[r]).
    for (int r = 0; r < 3; ++r) {
        put(96 + 16 * r, 9.0 + r);
        put(96 + 16 * r + 8, 12.0 + r);
    }
    for (int i = 0; i < 6; ++i) put(144 + 8 * i, 15.0 + i);
    // r4: element [col][row] = 21 + col * 4 + row... stored by rows: row r = (m[0][r], m[1][r], m[2][r], m[3][r]).
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) put(192 + 32 * r + 8 * c, 21.0 + c + 4 * r);
    GLuint ubo = 0;
    glGenBuffers(1, &ubo);
    glBindBuffer(GL_UNIFORM_BUFFER, ubo);
    glBufferData(GL_UNIFORM_BUFFER, static_cast<GLsizeiptr>(data.size()), data.data(), GL_STATIC_DRAW);
    glBindBufferBase(GL_UNIFORM_BUFFER, 0, ubo);
    glUniformBlockBinding(p, glGetUniformBlockIndex(p, "Doubles"), 0);
    GLuint vao = 0;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glUseProgram(p);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    glPointSize(16.0f);
    glDrawArrays(GL_POINTS, 0, 1);
    const std::vector<uint8_t> px = gpu::readRGBA8(8, 8, 1, 1);
    const uint8_t want[4] = {255, 255, 255, 255};
    if (!gpu::near(px.data(), want)) std::fprintf(stderr, "  pixel %d %d %d %d\n", px[0], px[1], px[2], px[3]);
    CHECK(gpu::near(px.data(), want));
    glDeleteBuffers(1, &ubo);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
    CHECK_GL();
}

// 64-bit vertex attributes: a dvec3 array, a dmat4 (one GL location per
// column, the second halves on spare Metal attributes) and a constant
// value, captured back unchanged.
TEST(gpu_fp64_vertex_attributes_round_trip) {
    gpu::Fixture fixture(64, 64, true, 42);
    CHECK(fixture.ok);
    if (!fixture.ok) return;
    loadDoubleFunctions();
    const char* vs = R"(#version 410 core
layout(location = 0) in dvec3 a;
layout(location = 1) in dmat4 m;
layout(location = 5) in float f;
layout(location = 6) in dvec4 c;
flat out dvec3 ra;
flat out dvec4 rm;
flat out double rf;
flat out dvec4 rc;
void main() { ra = a; rm = m[2]; rf = double(f); rc = c; gl_Position = vec4(0.0); })";
    GLuint p = linkCapture(vs, nullptr, nullptr, {"ra", "rm", "rf", "rc"});
    const int count = 3;
    // Per vertex: a (3 doubles), m (16), f (float, padded to 8 bytes).
    struct Vertex {
        double a[3];
        double m[16];
        float f, pad;
    } vertices[count];
    for (int v = 0; v < count; ++v) {
        for (int i = 0; i < 3; ++i) vertices[v].a[i] = 1.0 / (3 + i + v) + 1e-300 * i;
        for (int i = 0; i < 16; ++i) vertices[v].m[i] = std::ldexp(1.0 + i * 0.125, v * 100 - 50);
        vertices[v].f = 0.25f * static_cast<float>(v);
    }
    GLuint vao = 0, vbo = 0;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    pVertexAttribLPointer(0, 3, GL_DOUBLE, sizeof(Vertex), reinterpret_cast<const void*>(offsetof(Vertex, a)));
    glEnableVertexAttribArray(0);
    for (int c = 0; c < 4; ++c) {
        pVertexAttribLPointer(1 + c, 4, GL_DOUBLE, sizeof(Vertex),
                              reinterpret_cast<const void*>(offsetof(Vertex, m) + 32 * static_cast<size_t>(c)));
        glEnableVertexAttribArray(1 + c);
    }
    glVertexAttribPointer(5, 1, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<const void*>(offsetof(Vertex, f)));
    glEnableVertexAttribArray(5);
    pVertexAttribL4d(6, 1e100, -2.0, 0.5, 1.0 / 7.0);
    const std::vector<double> got = capture(p, count, count * 12);
    int bad = 0;
    for (int v = 0; v < count; ++v) {
        const double* out = &got[static_cast<size_t>(v * 12)];
        for (int i = 0; i < 3; ++i) bad += !sameBits(out[i], vertices[v].a[i]);
        for (int i = 0; i < 4; ++i) bad += !sameBits(out[3 + i], vertices[v].m[8 + i]);
        bad += !sameBits(out[7], static_cast<double>(vertices[v].f));
        const double c[4] = {1e100, -2.0, 0.5, 1.0 / 7.0};
        for (int i = 0; i < 4; ++i) bad += !sameBits(out[8 + i], c[i]);
    }
    CHECK_EQ(bad, 0);
    glDeleteBuffers(1, &vbo);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
    CHECK_GL();
}

// Double varyings through the geometry shader emulation, captured from the
// geometry stage (two vertices per input point).
TEST(gpu_fp64_geometry_varyings) {
    gpu::Fixture fixture(64, 64, true, 42);
    CHECK(fixture.ok);
    if (!fixture.ok) return;
    const char* vs = R"(#version 400 core
flat out dvec3 v;
void main() { v = dvec3(double(gl_VertexID) / 3.0lf, packDouble2x32(uvec2(0x9abcdef0u, 0x00012345u)), -0.0lf); gl_Position = vec4(0.0); })";
    const char* gs = R"(#version 400 core
layout(points) in;
layout(points, max_vertices = 2) out;
flat in dvec3 v[];
flat out dvec2 g;
void main() {
    g = v[0].xy * 2.0lf; EmitVertex();
    g = dvec2(v[0].z, v[0].x - 1.0lf); EmitVertex();
    EndPrimitive();
})";
    GLuint p = linkCapture(vs, gs, nullptr, {"g"});
    GLuint vao = 0;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    const int count = 3;
    const std::vector<double> got = capture(p, count, count * 4);
    int bad = 0;
    for (int v = 0; v < count; ++v) {
        const double x = v / 3.0;
        // A subnormal (glslang flushes subnormal literals, so it is built from bits).
        const uint64_t subnormalBits = 0x000123459abcdef0ull;
        double subnormal;
        std::memcpy(&subnormal, &subnormalBits, 8);
        const double want[4] = {x * 2.0, subnormal * 2.0, -0.0, x - 1.0};
        for (int i = 0; i < 4; ++i)
            if (!sameBits(got[static_cast<size_t>(v * 4 + i)], want[i]) && bad++ < 4)
                std::fprintf(stderr, "  vertex %d value %d: got %.17g want %.17g\n", v, i, got[static_cast<size_t>(v * 4 + i)], want[i]);
    }
    CHECK_EQ(bad, 0);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
    CHECK_GL();
}
