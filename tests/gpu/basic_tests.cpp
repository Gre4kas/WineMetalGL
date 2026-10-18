// Clears, draws and read-back on the default framebuffer.
#include <cstdlib>
#include <cstring>

#include "gpu.h"

namespace {

const char* kQuadVs = R"(#version 330 core
in vec2 position;
void main() { gl_Position = vec4(position, 0.0, 1.0); })";

const char* kColorFs = R"(#version 330 core
uniform vec4 color;
out vec4 fragColor;
void main() { fragColor = color; })";

}  // namespace

GPU_TEST(gpu_version_string) {
    // A 3.3 core request gets the highest core version (4.6), which is
    // backward compatible with it (WGL_ARB_create_context).
    const char* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    CHECK(version && std::strncmp(version, "4.6", 3) == 0);
    CHECK_GL();
}

GPU_TEST(gpu_draws_across_commits) {
    // More draws than DrawState::commitLargeWork records in one command
    // buffer: the work is committed while draws are recorded, and every
    // draw keeps its own uniforms.
    GLuint p = gpu::program(R"(#version 330 core
uniform vec4 at;
void main() { gl_Position = vec4(at.xy, 0.0, 1.0); })",
                            kColorFs);
    glUseProgram(p);
    const GLint at = glGetUniformLocation(p, "at"), color = glGetUniformLocation(p, "color");
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    GLuint vao = 0;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 32; ++x) {
            // Pixel (2x, 2y) of the 64x64 target, coloured by its position.
            glUniform4f(at, (2.0f * x + 0.5f) / 32.0f - 1.0f, (2.0f * y + 0.5f) / 32.0f - 1.0f, 0.0f, 0.0f);
            glUniform4f(color, static_cast<float>(x * 8) / 255.0f, static_cast<float>(y * 8) / 255.0f, 1.0f, 1.0f);
            glDrawArrays(GL_POINTS, 0, 1);
        }
    CHECK_GL();
    const auto pixels = gpu::readRGBA8(0, 0, 64, 64);
    int wrong = 0;
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 32; ++x) {
            const uint8_t want[4] = {static_cast<uint8_t>(x * 8), static_cast<uint8_t>(y * 8), 255, 255};
            wrong += !gpu::near(&pixels[(static_cast<size_t>(2 * y) * 64 + 2 * x) * 4], want);
        }
    CHECK_EQ(wrong, 0);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
}

GPU_TEST(gpu_commits_wait_for_pass_boundaries) {
    // DrawState::commitLargeWork commits at 256 recorded draws where a draw
    // starts a new render pass, and within one pass only past 8 times as
    // many: a commit ends the pass, whose attachments a tile-based GPU then
    // stores and loads again. (Counts assume the default threshold.)
    if (std::getenv("WINEMETALGL_COMMIT_DRAWS")) return;
    GLuint p = gpu::program(R"(#version 330 core
void main() { gl_Position = vec4(0.0, 0.0, 0.0, 1.0); })",
                            kColorFs);
    glUseProgram(p);
    glUniform4f(glGetUniformLocation(p, "color"), 1.0f, 1.0f, 1.0f, 1.0f);
    GLuint vao = 0, textures[2] = {}, framebuffers[2] = {};
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenTextures(2, textures);
    glGenFramebuffers(2, framebuffers);
    for (int i = 0; i < 2; ++i) {
        glBindTexture(GL_TEXTURE_2D, textures[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffers[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, textures[i], 0);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glFinish();
    {
        // One pass of 1024 draws: no commit splits it.
        gpu::Counters counters;
        for (int i = 0; i < 1024; ++i) glDrawArrays(GL_POINTS, 0, 1);
        CHECK_EQ(counters["passes"], 1ull);
        CHECK_EQ(counters["commits"], 0ull);
    }
    glFinish();
    {
        // One pass past 8 x 256 draws: one commit, which splits it once.
        gpu::Counters counters;
        for (int i = 0; i < 2100; ++i) glDrawArrays(GL_POINTS, 0, 1);
        CHECK_EQ(counters["commits"], 1ull);
        CHECK_EQ(counters["passes"], 2ull);
    }
    glFinish();
    {
        // Ten passes of 64 draws, alternating between two framebuffers: the
        // work is committed where a pass starts anyway (at least at draws 320
        // and 576; also wherever a pass starts a millisecond or more after the
        // last commit), so there are still ten passes.
        gpu::Counters counters;
        for (int pass = 0; pass < 10; ++pass) {
            glBindFramebuffer(GL_FRAMEBUFFER, framebuffers[pass & 1]);
            for (int i = 0; i < 64; ++i) glDrawArrays(GL_POINTS, 0, 1);
        }
        CHECK_EQ(counters["passes"], 10ull);
        CHECK(counters["commits"] >= 2ull && counters["commits"] <= 9ull);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    CHECK_GL();
    glDeleteFramebuffers(2, framebuffers);
    glDeleteTextures(2, textures);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
}

GPU_TEST(gpu_clear_readback) {
    glClearColor(0.25f, 0.5f, 0.75f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    const auto pixels = gpu::readRGBA8(10, 10, 1, 1);
    const uint8_t want[4] = {64, 128, 191, 255};
    CHECK(gpu::near(pixels.data(), want));
    CHECK_GL();
}

GPU_TEST(gpu_scissored_clear) {
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 0, 16, 16);
    glClearColor(1, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
    const auto inside = gpu::readRGBA8(4, 4, 1, 1), outside = gpu::readRGBA8(40, 40, 1, 1);
    const uint8_t red[4] = {255, 0, 0, 255}, black[4] = {0, 0, 0, 255};
    CHECK(gpu::near(inside.data(), red));
    CHECK(gpu::near(outside.data(), black));
    CHECK_GL();
}

GPU_TEST(gpu_quad_draw) {
    GLuint p = gpu::program(kQuadVs, kColorFs);
    GLuint vao = gpu::quad();
    glUseProgram(p);
    glUniform4f(glGetUniformLocation(p, "color"), 0, 1, 0, 1);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    const auto pixels = gpu::readRGBA8(32, 32, 1, 1);
    const uint8_t green[4] = {0, 255, 0, 255};
    CHECK(gpu::near(pixels.data(), green));
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
    CHECK_GL();
}

// GL row 0 is the bottom row: a quad covering the lower half lands in rows 0..31.
GPU_TEST(gpu_orientation) {
    const char* vs = R"(#version 330 core
in vec2 position;
void main() { gl_Position = vec4(position.x, position.y * 0.5 - 0.5, 0.0, 1.0); })";
    GLuint p = gpu::program(vs, kColorFs);
    GLuint vao = gpu::quad();
    glUseProgram(p);
    glUniform4f(glGetUniformLocation(p, "color"), 1, 1, 1, 1);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    const auto bottom = gpu::readRGBA8(32, 8, 1, 1), top = gpu::readRGBA8(32, 56, 1, 1);
    const uint8_t white[4] = {255, 255, 255, 255}, black[4] = {0, 0, 0, 255};
    CHECK(gpu::near(bottom.data(), white));
    CHECK(gpu::near(top.data(), black));
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
    CHECK_GL();
}

GPU_TEST(gpu_packed_generic_attribute) {
    // x = 511 (max positive 10-bit), w = 1 (2-bit, normalized to 1/1 for unsigned).
    glVertexAttribP4ui(3, GL_UNSIGNED_INT_2_10_10_10_REV, GL_TRUE, 1023u | (1u << 30));
    GLfloat v[4] = {};
    glGetVertexAttribfv(3, GL_CURRENT_VERTEX_ATTRIB, v);
    CHECK(v[0] == 1.0f && v[1] == 0.0f && v[2] == 0.0f && v[3] > 0.33f && v[3] < 0.34f);
    glVertexAttribP4ui(3, GL_FLOAT, GL_TRUE, 0);
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_ENUM));
}

GPU_TEST(gpu_array_varying) {
    const char* vs = R"(#version 330 core
in vec2 position;
out vec4 tc[2];
out vec4 color;
void main() { tc[0] = vec4(0.25, 0.5, 0.0, 1.0); tc[1] = vec4(1.0); color = vec4(0.0, 1.0, 0.0, 1.0); gl_Position = vec4(position, 0.0, 1.0); })";
    const char* fs = R"(#version 330 core
in vec4 tc[2];
in vec4 color;
out vec4 o;
void main() { o = vec4(tc[0].x, tc[0].y * tc[1].x, color.g, 1.0); })";
    GLuint p = gpu::program(vs, fs);
    GLuint vao = gpu::quad();
    glUseProgram(p);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    const uint8_t want[4] = {64, 128, 255, 255};
    CHECK(gpu::near(gpu::readRGBA8(32, 32, 1, 1).data(), want));
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
    CHECK_GL();
}

GPU_TEST(gpu_uniform_mat3) {
    const char* fs = R"(#version 330 core
uniform mat3 m;
out vec4 o;
void main() { vec3 n = m * vec3(0.0, 0.0, 1.0); o = vec4(n.x + 0.5, n.y + 0.5, n.z, 1.0); })";
    GLuint p = gpu::program(kQuadVs, fs);
    GLuint vao = gpu::quad();
    glUseProgram(p);
    const float identity[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    glUniformMatrix3fv(glGetUniformLocation(p, "m"), 1, GL_FALSE, identity);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    const uint8_t want[4] = {128, 128, 255, 255};
    CHECK(gpu::near(gpu::readRGBA8(32, 32, 1, 1).data(), want));
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
    CHECK_GL();
}

GPU_TEST(gpu_uniform_mat4_translation) {
    // Column-major translation by (0.5, 0.25): the quad's centre moves right and up.
    const char* vs = R"(#version 330 core
in vec2 position;
uniform mat4 m;
void main() { gl_Position = m * vec4(position * 0.25, 0.0, 1.0); })";
    GLuint p = gpu::program(vs, kColorFs);
    GLuint vao = gpu::quad();
    glUseProgram(p);
    const float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0.5f, 0.25f, 0, 1};
    glUniformMatrix4fv(glGetUniformLocation(p, "m"), 1, GL_FALSE, m);
    glUniform4f(glGetUniformLocation(p, "color"), 1, 1, 1, 1);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    // Quad covers NDC x 0.25..0.75, y 0..0.5 -> pixels x 40..56, y 32..48.
    const uint8_t white[4] = {255, 255, 255, 255}, black[4] = {0, 0, 0, 255};
    CHECK(gpu::near(gpu::readRGBA8(48, 40, 1, 1).data(), white));
    CHECK(gpu::near(gpu::readRGBA8(32, 32, 1, 1).data(), black));
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
    CHECK_GL();
}

GPU_TEST(gpu_attribute_component_defaults) {
    // A vec4 attribute fed from 2 floats reads (x, y, 0, 1).
    const char* vs = R"(#version 330 core
in vec4 position;
out vec4 v;
void main() { v = position; gl_Position = vec4(position.xy, 0.0, 1.0); })";
    const char* fs = R"(#version 330 core
in vec4 v;
out vec4 o;
void main() { o = vec4(v.z, v.w, 0.0, 1.0); })";
    GLuint p = gpu::program(vs, fs);
    GLuint vao = gpu::quad();
    glUseProgram(p);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    const uint8_t want[4] = {0, 255, 0, 255};
    CHECK(gpu::near(gpu::readRGBA8(32, 32, 1, 1).data(), want));
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
    CHECK_GL();
}

GPU_TEST(gpu_attributes_separate_buffers) {
    // Four attributes, each from its own buffer, set up in a different order
    // than their locations (mirrors KHR-GL33.buffer_objects.triangles).
    const char* vs = R"(#version 330 core
in vec3 inNormal;
in vec4 inColor;
in vec4 inVertex;
in vec2 inTex;
uniform mat4 mvp;
out vec4 color;
void main() { color = inColor * (inNormal.z > 0.0 ? 1.0 : 0.5) * (inTex.x >= 0.0 ? 1.0 : 0.0); gl_Position = mvp * inVertex; })";
    const char* fs = R"(#version 330 core
in vec4 color;
out vec4 o;
void main() { o = color; })";
    GLuint p = gpu::program(vs, fs);
    glUseProgram(p);
    const float W = 64, H = 64;
    const float mvp[16] = {2 / W, 0, 0, 0, 0, 2 / H, 0, 0, 0, 0, -1, 0, -1, -1, 0, 1};
    glUniformMatrix4fv(glGetUniformLocation(p, "mvp"), 1, GL_FALSE, mvp);
    const float verts[12] = {W / 4, H / 4, 3 * W / 4, H / 4, 3 * W / 4, 3 * H / 4, W / 4, H / 4, 3 * W / 4, 3 * H / 4, W / 4, 3 * H / 4};
    const float normals[18] = {0, 0, 1, 0, 0, -1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1};
    const float colors[24] = {1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1};
    const float tex[12] = {0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1};
    GLuint vao = 0, buffers[4];
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(4, buffers);
    const void* data[4] = {verts, normals, colors, tex};
    const size_t sizes[4] = {sizeof(verts), sizeof(normals), sizeof(colors), sizeof(tex)};
    for (int i = 0; i < 4; ++i) {
        glBindBuffer(GL_ARRAY_BUFFER, buffers[i]);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(sizes[i]), nullptr, GL_STATIC_DRAW);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(sizes[i]), data[i], GL_STATIC_DRAW);
    }
    const GLint locVertex = glGetAttribLocation(p, "inVertex"), locNormal = glGetAttribLocation(p, "inNormal"),
                locColor = glGetAttribLocation(p, "inColor"), locTex = glGetAttribLocation(p, "inTex");
    for (GLint l : {locVertex, locNormal, locColor, locTex}) glEnableVertexAttribArray(static_cast<GLuint>(l));
    glBindBuffer(GL_ARRAY_BUFFER, buffers[1]);
    glVertexAttribPointer(static_cast<GLuint>(locNormal), 3, GL_FLOAT, GL_FALSE, 0, nullptr);
    glBindBuffer(GL_ARRAY_BUFFER, buffers[2]);
    glVertexAttribPointer(static_cast<GLuint>(locColor), 4, GL_FLOAT, GL_FALSE, 0, nullptr);
    glBindBuffer(GL_ARRAY_BUFFER, buffers[3]);
    glVertexAttribPointer(static_cast<GLuint>(locTex), 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glBindBuffer(GL_ARRAY_BUFFER, buffers[0]);
    glVertexAttribPointer(static_cast<GLuint>(locVertex), 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    const uint8_t green[4] = {0, 255, 0, 255};
    CHECK(gpu::near(gpu::readRGBA8(17, 47, 1, 1).data(), green));   // inside the second triangle
    const uint8_t black[4] = {0, 0, 0, 255};
    CHECK(gpu::near(gpu::readRGBA8(60, 60, 1, 1).data(), black));
    glDeleteBuffers(4, buffers);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
    CHECK_GL();
}

// Unnormalized 8- and 16-bit integer attributes read as floats keep their
// integer values (GL 4.6 section 10.3.1), fetched by Metal without a CPU
// conversion; missing components read 0, 0, 1 (DOOM's GL_SHORT data, issue #36).
GPU_TEST_VERSION(gpu_unnormalized_integer_attributes, 46) {
    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    const char* source = "#version 460 core\nlayout(location = 0) in vec4 a;\nout vec4 captured;\n"
                         "void main() { captured = a; gl_Position = vec4(0.0); }\n";
    glShaderSource(vs, 1, &source, nullptr);
    glCompileShader(vs);
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    const char* varyings[] = {"captured"};
    glTransformFeedbackVaryings(p, 1, varyings, GL_INTERLEAVED_ATTRIBS);
    glLinkProgram(p);
    glUseProgram(p);
    struct Case { GLenum type; GLint size; float want[2][4]; };
    const Case cases[] = {
        {GL_SHORT, 4, {{1234, -5, 32767, -32768}, {7, -9, 0, 0}}},
        {GL_UNSIGNED_SHORT, 3, {{1234, 65531, 32767, 1}, {7, 65527, 0, 1}}},
        {GL_BYTE, 2, {{-46, 4, 0, 1}, {-1, -128, 0, 1}}},
        {GL_UNSIGNED_BYTE, 1, {{210, 0, 0, 1}, {255, 0, 0, 1}}},
    };
    // Two vertices, 8 bytes apart.
    const int16_t shorts[8] = {1234, -5, 32767, -32768, 7, -9, 0, 0};
    const uint8_t bytes[16] = {210, 4, 0, 0, 0, 0, 0, 0, 255, 128, 0, 0, 0, 0, 0, 0};
    GLuint vao = 0, buffers[2] = {};
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(2, buffers);
    glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, buffers[1]);
    glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, 2 * 16, nullptr, GL_STATIC_READ);
    glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, buffers[1]);
    glEnable(GL_RASTERIZER_DISCARD);
    for (const Case& c : cases) {
        const bool wide = c.type == GL_SHORT || c.type == GL_UNSIGNED_SHORT;
        glBindBuffer(GL_ARRAY_BUFFER, buffers[0]);
        if (wide) glBufferData(GL_ARRAY_BUFFER, sizeof(shorts), shorts, GL_STATIC_DRAW);
        else glBufferData(GL_ARRAY_BUFFER, sizeof(bytes), bytes, GL_STATIC_DRAW);
        glVertexAttribPointer(0, c.size, c.type, GL_FALSE, 8, nullptr);
        glEnableVertexAttribArray(0);
        glBeginTransformFeedback(GL_POINTS);
        glDrawArrays(GL_POINTS, 0, 2);
        glEndTransformFeedback();
        float out[8] = {};
        glGetBufferSubData(GL_TRANSFORM_FEEDBACK_BUFFER, 0, sizeof(out), out);
        for (int v = 0; v < 2; ++v)
            for (int k = 0; k < 4; ++k) {
                const float want = c.want[v][k];
                if (out[v * 4 + k] != want) {
                    std::printf("  type 0x%x vertex %d component %d: %g, want %g\n", c.type, v, k, out[v * 4 + k], want);
                    CHECK(false);
                }
            }
    }
    glDisable(GL_RASTERIZER_DISCARD);
    CHECK_GL();
    glDeleteBuffers(2, buffers);
    glDeleteVertexArrays(1, &vao);
    glDeleteShader(vs);
    glDeleteProgram(p);
}
