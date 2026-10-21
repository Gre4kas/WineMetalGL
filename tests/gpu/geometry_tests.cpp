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

GPU_TEST(gpu_geometry_struct_inputs_and_outputs) {
    // Struct-typed inputs, outputs and block members (nested, with arrays)
    // are stored field by field.
    const char* vs = R"(#version 330 core
struct V { vec2 p; float k[2]; };
in vec2 position;
out V v;
void main() { v.p = position; v.k[0] = 0.25; v.k[1] = 0.75; gl_Position = vec4(position, 0.0, 1.0); })";
    const char* gs = R"(#version 330 core
struct V { vec2 p; float k[2]; };
struct Inner { float g; };
struct C { float r; Inner inner[2]; };
layout(points) in;
layout(triangle_strip, max_vertices = 4) out;
in V v[];
out C c;
out Blk { Inner b; } blk;
void main() {
    for (int i = 0; i < 4; ++i) {
        c.r = v[0].k[1];
        c.inner[0].g = 0.0;
        c.inner[1].g = v[0].k[0];
        blk.b.g = v[0].p.x;
        vec2 corner = vec2(i % 2 == 0 ? -0.25 : 0.25, i < 2 ? -0.25 : 0.25);
        gl_Position = gl_in[0].gl_Position + vec4(corner, 0.0, 0.0);
        EmitVertex();
    }
    EndPrimitive();
})";
    const char* fs = R"(#version 330 core
struct Inner { float g; };
struct C { float r; Inner inner[2]; };
in C c;
in Blk { Inner b; } blk;
out vec4 o;
void main() { o = vec4(c.r, c.inner[1].g + c.inner[0].g, blk.b.g, 1.0); })";
    GLuint p = linkWithGeometry(vs, gs, fs);
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
    const uint8_t want[4] = {191, 64, 128, 255};
    const std::vector<uint8_t> got = gpu::readRGBA8(48, 48, 1, 1);
    CHECK(gpu::near(got.data(), want, 2));
    if (!gpu::near(got.data(), want, 2)) std::fprintf(stderr, "  got %d %d %d\n", got[0], got[1], got[2]);
    glDeleteBuffers(1, &vbo);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
    CHECK_GL();
}

// Line input (SpaceEngine's orbits, issue #35): each segment of GL_LINES,
// GL_LINE_STRIP and GL_LINE_LOOP, and of an indexed strip split by primitive
// restart, reaches the geometry shader, which widens it into a quad.
GPU_TEST(gpu_geometry_line_input) {
    const char* gs = R"(#version 330 core
layout(lines) in;
layout(triangle_strip, max_vertices = 4) out;
noperspective out vec2 uv;
out vec4 color;
void main() {
    vec2 a = gl_in[0].gl_Position.xy, b = gl_in[1].gl_Position.xy;
    vec2 n = normalize(vec2(a.y - b.y, b.x - a.x)) * 0.1;
    color = vec4(0.0, 1.0, 0.0, 1.0);
    uv = vec2(0.0); gl_Position = vec4(a - n, 0.0, 1.0); EmitVertex();
    uv = vec2(0.0); gl_Position = vec4(a + n, 0.0, 1.0); EmitVertex();
    uv = vec2(1.0); gl_Position = vec4(b - n, 0.0, 1.0); EmitVertex();
    uv = vec2(1.0); gl_Position = vec4(b + n, 0.0, 1.0); EmitVertex();
    EndPrimitive();
})";
    const char* fs = R"(#version 330 core
noperspective in vec2 uv; in vec4 color; out vec4 o;
void main() { o = color * (uv.x >= 0.0 ? 1.0 : 0.0); })";
    GLuint p = linkWithGeometry(kPassVs, gs, fs);
    glUseProgram(p);
    // A square: bottom, right, top and left edges between its corners.
    static const float corners[] = {-0.5f, -0.5f, 0.5f, -0.5f, 0.5f, 0.5f, -0.5f, 0.5f};
    static const GLushort restarted[] = {0, 1, 0xffff, 2, 3};
    GLuint vao = 0, buffers[2] = {};
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(2, buffers);
    glBindBuffer(GL_ARRAY_BUFFER, buffers[0]);
    glBufferData(GL_ARRAY_BUFFER, sizeof(corners), corners, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glEnableVertexAttribArray(0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffers[1]);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(restarted), restarted, GL_STATIC_DRAW);
    struct Case {
        const char* name;
        GLenum mode;
        bool indexed;
        bool bottom, right, top, left;
    } cases[] = {
        {"GL_LINES", GL_LINES, false, true, false, true, false},
        {"GL_LINE_STRIP", GL_LINE_STRIP, false, true, true, true, false},
        {"GL_LINE_LOOP", GL_LINE_LOOP, false, true, true, true, true},
        {"GL_LINE_STRIP with primitive restart", GL_LINE_STRIP, true, true, false, true, false},
    };
    const uint8_t green[4] = {0, 255, 0, 255}, black[4] = {0, 0, 0, 255};
    for (const Case& c : cases) {
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        if (c.indexed) {
            glEnable(GL_PRIMITIVE_RESTART);
            glPrimitiveRestartIndex(0xffff);
            glDrawElements(c.mode, 5, GL_UNSIGNED_SHORT, nullptr);
            glDisable(GL_PRIMITIVE_RESTART);
        } else {
            glDrawArrays(c.mode, 0, 4);
        }
        CHECK_GL();
        const struct { int x, y; bool want; const char* edge; } probes[] = {
            {32, 16, c.bottom, "bottom"}, {48, 32, c.right, "right"}, {32, 48, c.top, "top"}, {16, 32, c.left, "left"}};
        for (const auto& probe : probes) {
            const auto px = gpu::readRGBA8(probe.x, probe.y, 1, 1);
            if (!gpu::near(px.data(), probe.want ? green : black, 2)) {
                std::printf("  %s: %s edge %s, got %d %d %d\n", c.name, probe.edge, probe.want ? "missing" : "drawn", px[0],
                            px[1], px[2]);
                CHECK(false);
            }
        }
    }
    glDeleteBuffers(2, buffers);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
    CHECK_GL();
}

// Stages matched by location, not name (GL 4.6 section 4.4.1): the vertex
// stage's "vsPos" feeds the geometry stage's "gsIn" at location 0, and
// "gsColor" the fragment stage's "fsColor". The emulation captured the
// vertex outputs by the geometry inputs' names, and SpaceEngine's orbits
// (outPos -> inPos, outColor -> inColor) drew nothing (issue #35).
GPU_TEST(gpu_geometry_inputs_by_location) {
    const char* vs = R"(#version 410 core
layout(location = 0) in vec2 position;
layout(location = 0) out vec4 vsPos;
void main() { vsPos = vec4(position, 0.0, 1.0); })";
    const char* gs = R"(#version 410 core
layout(lines) in;
layout(triangle_strip, max_vertices = 4) out;
layout(location = 0) in vec4 gsIn[2];
layout(location = 0) out vec4 gsColor;
void main() {
    vec2 a = gsIn[0].xy, b = gsIn[1].xy;
    vec2 n = normalize(vec2(a.y - b.y, b.x - a.x)) * 0.1;
    gsColor = vec4(0.0, 1.0, 0.0, 1.0);
    gl_Position = vec4(a - n, 0.0, 1.0); EmitVertex();
    gl_Position = vec4(a + n, 0.0, 1.0); EmitVertex();
    gl_Position = vec4(b - n, 0.0, 1.0); EmitVertex();
    gl_Position = vec4(b + n, 0.0, 1.0); EmitVertex();
    EndPrimitive();
})";
    const char* fs = R"(#version 410 core
layout(location = 0) in vec4 fsColor; out vec4 o; void main() { o = fsColor; })";
    // The same shaders with matching names draw the same.
    for (int sameNames = 0; sameNames < 2; ++sameNames) {
        std::string v = vs, g = gs, f = fs;
        if (sameNames) {
            auto all = [](std::string& t, const std::string& a, const std::string& b) {
                for (size_t at = t.find(a); at != std::string::npos; at = t.find(a, at + b.size())) t.replace(at, a.size(), b);
            };
            all(v, "vsPos", "pos");
            all(g, "gsIn", "pos");
            all(g, "gsColor", "color");
            all(f, "fsColor", "color");
        }
        GLuint p = linkWithGeometry(v.c_str(), g.c_str(), f.c_str());
        glUseProgram(p);
        static const float segment[] = {-0.5f, 0.0f, 0.5f, 0.0f};
        GLuint vao = 0, vbo = 0;
        glGenVertexArrays(1, &vao);
        glBindVertexArray(vao);
        glGenBuffers(1, &vbo);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(segment), segment, GL_STATIC_DRAW);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
        glEnableVertexAttribArray(0);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glDrawArrays(GL_LINES, 0, 2);
        CHECK_GL();
        const uint8_t green[4] = {0, 255, 0, 255};
        if (!gpu::near(gpu::readRGBA8(32, 32, 1, 1).data(), green)) {
            std::printf("  %s names: the segment is missing\n", sameNames ? "same" : "different");
            CHECK(false);
        }
        glDeleteBuffers(1, &vbo);
        glDeleteVertexArrays(1, &vao);
        glDeleteProgram(p);
    }
}

// An indexed, instanced draw through a geometry shader: the vertex stage
// runs once per vertex of the index range (skipped vertices included), its
// attributes fetched as a draw fetches them, and its outputs are copied into
// gl_in order. gl_VertexID is the GL vertex number (index plus base vertex),
// and an attribute converted on the CPU (unsigned short read as ivec2) is
// read for the right vertices.
GPU_TEST(gpu_geometry_indexed_instanced_direct_capture) {
    const char* vs = R"(#version 330 core
in vec2 position;
in ivec2 tag;
out vec4 vcolor;
void main() {
    bool listed = gl_VertexID == 6 || gl_VertexID == 8 || gl_VertexID == 9 || gl_VertexID == 11;
    vcolor = vec4(tag.x == 40000 ? 1.0 : 0.0, listed ? 1.0 : 0.0, 0.0, 1.0);
    gl_Position = vec4(position.x + float(gl_InstanceID), position.y, 0.0, 1.0);
})";
    const char* gs = R"(#version 330 core
layout(triangles) in;
layout(triangle_strip, max_vertices = 3) out;
in vec4 vcolor[];
out vec4 color;
void main() {
    for (int i = 0; i < 3; ++i) {
        color = vcolor[i];
        gl_Position = gl_in[i].gl_Position;
        EmitVertex();
    }
    EndPrimitive();
})";
    GLuint p = linkWithGeometry(vs, gs, kColorFs);
    glUseProgram(p);
    struct Vertex {
        float x, y;
        uint16_t tag[2];
    };
    // Vertices 6, 8, 9 and 11 are the left half's corners; 7 and 10, inside
    // the range, are never listed.
    Vertex v[12] = {};
    const Vertex corners[4] = {{-1, -1, {40000, 0}}, {0, -1, {40000, 0}}, {-1, 1, {40000, 0}}, {0, 1, {40000, 0}}};
    v[6] = corners[0];
    v[8] = corners[1];
    v[9] = corners[2];
    v[11] = corners[3];
    v[7] = v[10] = {5, 5, {1, 1}};
    const uint16_t indices[6] = {2, 4, 5, 5, 4, 7};  // base vertex 4
    GLuint vao = 0, buffers[2] = {};
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(2, buffers);
    glBindBuffer(GL_ARRAY_BUFFER, buffers[0]);
    glBufferData(GL_ARRAY_BUFFER, sizeof(v), v, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), nullptr);
    glEnableVertexAttribArray(0);
    const GLint tag = glGetAttribLocation(p, "tag");
    CHECK(tag > 0);
    glVertexAttribIPointer(static_cast<GLuint>(tag), 2, GL_UNSIGNED_SHORT, sizeof(Vertex), reinterpret_cast<void*>(8));
    glEnableVertexAttribArray(static_cast<GLuint>(tag));
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffers[1]);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);
    glClearColor(0, 0, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawElementsInstancedBaseVertex(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, nullptr, 2, 4);
    CHECK_GL();
    const uint8_t yellow[4] = {255, 255, 0, 255};
    CHECK(gpu::near(gpu::readRGBA8(16, 32, 1, 1).data(), yellow));  // instance 0
    CHECK(gpu::near(gpu::readRGBA8(48, 32, 1, 1).data(), yellow));  // instance 1
    glDeleteBuffers(2, buffers);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
    CHECK_GL();
}

// Under a geometry shader the vertex stage still runs for listed vertices
// only: a vertex shader that counts its invocations per gl_VertexID through
// image atomics sees each listed vertex and never the vertices of the index
// range the list skips (Rethinking Voxels writes voxels from its shadow
// pass's vertex and geometry stages).
GPU_TEST(gpu_geometry_vertex_stage_skips_unlisted_vertices) {
    const char* vs = R"(#version 430 core
in vec2 position;
layout(r32ui) uniform uimage2D counts;
void main() {
    imageAtomicAdd(counts, ivec2(gl_VertexID, 0), 1u);
    gl_Position = vec4(position, 0.0, 1.0);
})";
    const char* gs = R"(#version 430 core
layout(triangles) in;
layout(triangle_strip, max_vertices = 3) out;
out vec4 color;
void main() {
    for (int i = 0; i < 3; ++i) {
        color = vec4(0.0, 1.0, 0.0, 1.0);
        gl_Position = gl_in[i].gl_Position;
        EmitVertex();
    }
    EndPrimitive();
})";
    GLuint p = linkWithGeometry(vs, gs, kColorFs);
    glUseProgram(p);
    GLuint counts = 0;
    glGenTextures(1, &counts);
    glBindTexture(GL_TEXTURE_2D, counts);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_R32UI, 8, 1);
    const uint32_t zeros[8] = {};
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 8, 1, GL_RED_INTEGER, GL_UNSIGNED_INT, zeros);
    glBindImageTexture(0, counts, 0, GL_FALSE, 0, GL_READ_WRITE, GL_R32UI);
    glUniform1i(glGetUniformLocation(p, "counts"), 0);
    // Vertices 1, 3, 4 and 6 are listed; 2 and 5, inside the range, are not.
    const float positions[16] = {9, 9, -1, -1, 9, 9, 1, -1, -1, 1, 9, 9, 1, 1, 9, 9};
    const uint16_t indices[6] = {1, 3, 4, 4, 3, 6};
    GLuint vao = 0, buffers[2] = {};
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(2, buffers);
    glBindBuffer(GL_ARRAY_BUFFER, buffers[0]);
    glBufferData(GL_ARRAY_BUFFER, sizeof(positions), positions, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glEnableVertexAttribArray(0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffers[1]);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, nullptr);
    glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);
    CHECK_GL();
    uint32_t got[8] = {};
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RED_INTEGER, GL_UNSIGNED_INT, got);
    for (int v : {1, 3, 4, 6}) CHECK(got[v] >= 1);
    for (int v : {0, 2, 5, 7}) CHECK_EQ(got[v], 0u);
    const uint8_t green[4] = {0, 255, 0, 255};
    CHECK(gpu::near(gpu::readRGBA8(32, 32, 1, 1).data(), green));
    glDeleteTextures(1, &counts);
    glDeleteBuffers(2, buffers);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
    CHECK_GL();
}

// A strip a geometry shader emits takes its flat values from GL's last
// vertex of each triangle (the default convention), as Metal's first vertex
// after the emulation rotates each triangle.
GPU_TEST(gpu_geometry_strip_flat_last_vertex) {
    const char* gs = R"(#version 330 core
layout(points) in;
layout(triangle_strip, max_vertices = 4) out;
flat out vec4 color;
void main() {
    const vec2 corners[4] = vec2[4](vec2(-1, -1), vec2(1, -1), vec2(-1, 1), vec2(1, 1));
    const vec4 colors[4] = vec4[4](vec4(0, 0, 1, 1), vec4(0, 0, 1, 1), vec4(1, 0, 0, 1), vec4(0, 1, 0, 1));
    for (int i = 0; i < 4; ++i) {
        color = colors[i];
        gl_Position = vec4(corners[i], 0.0, 1.0);
        EmitVertex();
    }
    EndPrimitive();
})";
    const char* fs = R"(#version 330 core
flat in vec4 color; out vec4 o; void main() { o = color; })";
    GLuint p = linkWithGeometry(kPassVs, gs, fs);
    glUseProgram(p);
    GLuint vao = gpu::quad();
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_POINTS, 0, 1);
    CHECK_GL();
    const uint8_t red[4] = {255, 0, 0, 255}, green[4] = {0, 255, 0, 255};
    CHECK(gpu::near(gpu::readRGBA8(8, 8, 1, 1).data(), red));     // triangle 0, 1, 2: vertex 2
    CHECK(gpu::near(gpu::readRGBA8(56, 56, 1, 1).data(), green));  // triangle 1, 2, 3: vertex 3
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
    CHECK_GL();
}
