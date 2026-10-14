// Draw-call overhead benchmark shaped like DOOM (2016)'s OpenGL renderer
// (WineMetalGL's per-command profile of its gameplay): about 700 indexed
// draws a frame into a few framebuffers, each after two attribute pointers
// into one big vertex buffer, a vec4-array uniform update and a texture and
// sampler bind, with a program change every few draws. Runs only with
// WMGL_BENCH set (frames, default 300); prints CPU microseconds per draw.

#include <thread>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <string>

#include "gpu.h"

namespace {

constexpr int kPrograms = 24, kTextures = 64, kDrawsPerFrame = 700, kTargets = 6, kIndices = 3000;

GLuint makeProgram(int variant) {
    const std::string vs = "#version 460 core\nlayout(location = 0) in vec4 position;\nlayout(location = 1) in vec4 attrib;\n"
                           "uniform vec4 _va_[48];\nout vec4 v;\nflat out int material;\nvoid main() {\n"
                           "  gl_Position = vec4(dot(position, _va_[0]), dot(position, _va_[1]), dot(position, _va_[2]), dot(position, _va_[3]));\n"
                           "  v = attrib * _va_[" + std::to_string(4 + variant % 40) + "];\n  material = gl_VertexID & 3;\n}\n";
    // WMGL_BENCH_FLAT: a flat varying, as engines pass material ids
    // (GL's last-vertex convention then rotates every primitive).
    const bool flat = std::getenv("WMGL_BENCH_FLAT") != nullptr;
    const std::string fs = std::string("#version 460 core\nin vec4 v;\n") + (flat ? "flat in int material;\n" : "") +
                           "uniform sampler2D t0, t1, t2, t3;\nuniform vec4 _fa_[16];\nout vec4 o;\n"
                           "void main() { o = v * texture(t0, v.xy) + texture(t1, v.zw) * _fa_[" + std::to_string(variant % 16) +
                           "] + texture(t2, v.yx) + texture(t3, v.wz)" + (flat ? " + float(material) * 0.01" : "") + "; }\n";
    GLuint p = gpu::program(vs.c_str(), fs.c_str());
    glUseProgram(p);
    for (int i = 0; i < 4; ++i) glUniform1i(glGetUniformLocation(p, ("t" + std::to_string(i)).c_str()), i);
    return p;
}

}  // namespace

TEST(bench_draw_overhead) {
    const char* env = std::getenv("WMGL_BENCH");
    if (!env) return;
    const int frames = std::atoi(env) > 0 ? std::atoi(env) : 300;
    // WMGL_BENCH_CONVERT: the second attribute is unnormalized bytes, which Metal cannot fetch as floats.
    const bool convert = std::getenv("WMGL_BENCH_CONVERT") != nullptr;
    gpu::Fixture fixture(1920, 1080, false, 46);
    CHECK(fixture.ok);
    if (!fixture.ok) return;
    GLuint programs[kPrograms];
    GLint va[kPrograms], fa[kPrograms];
    for (int i = 0; i < kPrograms; ++i) {
        programs[i] = makeProgram(i);
        va[i] = glGetUniformLocation(programs[i], "_va_");
        fa[i] = glGetUniformLocation(programs[i], "_fa_");
    }
    GLuint textures[kTextures], samplers[4];
    glGenTextures(kTextures, textures);
    std::vector<uint8_t> pixels(256 * 256 * 4, 128);
    for (GLuint t : textures) {
        glBindTexture(GL_TEXTURE_2D, t);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 256, 256);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 256, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    }
    glGenSamplers(4, samplers);
    for (GLuint s : samplers) {
        glSamplerParameteri(s, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glSamplerParameteri(s, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }
    // One big vertex buffer and index buffer; draws point into them.
    std::vector<float> vertices(65536 * 8);
    for (size_t i = 0; i < vertices.size(); ++i) vertices[i] = static_cast<float>((i * 7919) % 1000) / 1000.0f;
    std::vector<uint16_t> indices(kIndices * kDrawsPerFrame);
    for (size_t i = 0; i < indices.size(); ++i) indices[i] = static_cast<uint16_t>((i * 31) % 4096);
    GLuint vao = 0, buffers[2] = {};
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(2, buffers);
    glBindBuffer(GL_ARRAY_BUFFER, buffers[0]);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertices.size() * 4), vertices.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffers[1]);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(indices.size() * 2), indices.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    // Render targets: RGBA16F colour and 32-bit float depth, 1920x1080.
    GLuint fbos[kTargets], color[kTargets], depth[kTargets];
    glGenFramebuffers(kTargets, fbos);
    glGenTextures(kTargets, color);
    glGenTextures(kTargets, depth);
    for (int i = 0; i < kTargets; ++i) {
        glBindTexture(GL_TEXTURE_2D, color[i]);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA16F, 1920, 1080);
        glBindTexture(GL_TEXTURE_2D, depth[i]);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_DEPTH_COMPONENT32F, 1920, 1080);
        glBindFramebuffer(GL_FRAMEBUFFER, fbos[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color[i], 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depth[i], 0);
    }
    glEnable(GL_DEPTH_TEST);
    glViewport(0, 0, 1920, 1080);
    float uniforms[48 * 4];
    for (int i = 0; i < 48 * 4; ++i) uniforms[i] = 0.01f * static_cast<float>(i % 97);
    using Clock = std::chrono::steady_clock;
    double cpuMicros = 0;
    const auto begin = Clock::now();
    for (int frame = 0; frame < frames; ++frame) {
        const auto start = Clock::now();
        for (int d = 0; d < kDrawsPerFrame; ++d) {
            if (d % (kDrawsPerFrame / kTargets) == 0) {
                glBindFramebuffer(GL_FRAMEBUFFER, fbos[(d / (kDrawsPerFrame / kTargets)) % kTargets]);
                glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            }
            const int p = (d / 4) % kPrograms;
            if (d % 4 == 0) glUseProgram(programs[p]);
            const GLintptr base = static_cast<GLintptr>((d * 977) % 60000) * 32;
            glBindBuffer(GL_ARRAY_BUFFER, buffers[0]);
            glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 32, reinterpret_cast<void*>(base));
            if (convert) glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_FALSE, 32, reinterpret_cast<void*>(base + 16));
            else glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 32, reinterpret_cast<void*>(base + 16));
            uniforms[0] = static_cast<float>(d);
            glUniform4fv(va[p], 12, uniforms);
            if (d % 2 == 0) glUniform4fv(fa[p], 4, uniforms + 16);
            glActiveTexture(GL_TEXTURE0 + (d % 4));
            glBindTexture(GL_TEXTURE_2D, textures[(d * 13) % kTextures]);
            glBindSampler(static_cast<GLuint>(d % 4), samplers[d % 4]);
            glDrawElementsBaseVertex(GL_TRIANGLES, kIndices, GL_UNSIGNED_SHORT,
                                     reinterpret_cast<void*>(static_cast<uintptr_t>(d * kIndices * 2)), 0);  // a range of its own, as a scene's meshes
        }
        glFlush();
        cpuMicros += std::chrono::duration<double, std::micro>(Clock::now() - start).count();
        if (frame % 3 == 2) glFinish();  // keep at most a few frames queued, as a swap would
    }
    glFinish();
    const double wall = std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
    std::fprintf(stderr, "  bench: %d frames x %d draws: %.2f us CPU per draw, %.2f ms CPU per frame, %.2f ms wall per frame\n",
                 frames, kDrawsPerFrame, cpuMicros / (static_cast<double>(frames) * kDrawsPerFrame), cpuMicros / frames / 1000.0,
                 wall / frames);
    CHECK_GL();
}

TEST(bench_call_overhead) {
    // The core's own cost per GL call (no Wine): compare with
    // tests/wine/opengl_call_overhead.c, which adds the Wine module's.
    if (!std::getenv("WMGL_BENCH")) return;
    gpu::Fixture fixture(64, 64, false, 46);
    CHECK(fixture.ok);
    if (!fixture.ok) return;
    using IsEnabled = GLboolean (*)(GLenum);
    const auto isEnabledCall = reinterpret_cast<IsEnabled>(wmgl_get_proc_address("glIsEnabled"));
    CHECK(isEnabledCall != nullptr);
    if (!isEnabledCall) return;
    constexpr int kCalls = 2000000;
    volatile GLboolean sink = 0;
    for (int i = 0; i < 10000; ++i) sink = sink ^ isEnabledCall(GL_BLEND);
    using Clock = std::chrono::steady_clock;
    auto start = Clock::now();
    for (int i = 0; i < kCalls; ++i) sink = sink ^ isEnabledCall(GL_BLEND);
    const double isEnabled = std::chrono::duration<double, std::nano>(Clock::now() - start).count() / kCalls;
    start = Clock::now();
    for (int i = 0; i < kCalls; ++i) glColorMask(i & 1, GL_TRUE, GL_TRUE, GL_TRUE);
    const double colorMask = std::chrono::duration<double, std::nano>(Clock::now() - start).count() / kCalls;
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    std::fprintf(stderr, "  bench: glIsEnabled %.1f ns/call, glColorMask %.1f ns/call (core only)\n", isEnabled, colorMask);
}

TEST(bench_first_draw) {
    // WMGL_BENCH: programs linked at "load" (then a pause, as a loading
    // screen gives), and the time of each one's first draw. A per-run
    // constant keeps the system's shader cache from answering.
    if (!std::getenv("WMGL_BENCH")) return;
    gpu::Fixture fixture(64, 64, false, 46);
    CHECK(fixture.ok);
    if (!fixture.ok) return;
    constexpr int kPrograms = 24;
    const unsigned salt = static_cast<unsigned>(std::chrono::steady_clock::now().time_since_epoch().count() & 0xffffff);
    GLuint programs[kPrograms];
    for (int i = 0; i < kPrograms; ++i) {
        const std::string vs = "#version 460 core\nlayout(location = 0) in vec2 p;\nout vec2 uv;\n"
                               "void main() { uv = p * " + std::to_string(i + 1) + ".0 + " + std::to_string(salt % 997) +
                               ".0; gl_Position = vec4(p, 0.0, 1.0); }\n";
        const std::string fs = "#version 460 core\nin vec2 uv;\nout vec4 o;\nvoid main() {\n vec4 a = vec4(0.0);\n"
                               " for (int k = 0; k < " + std::to_string(8 + i) + "; ++k) a += sin(vec4(uv.xyxy * float(k) + " +
                               std::to_string(salt) + ".0));\n o = a * 0.01;\n}\n";
        programs[i] = gpu::program(vs.c_str(), fs.c_str());
    }
    std::this_thread::sleep_for(std::chrono::seconds(3));
    GLuint vao = 0, vbo = 0;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    const float tri[] = {-1, -1, 3, -1, -1, 3};
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(tri), tri, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glEnableVertexAttribArray(0);
    double total = 0, worst = 0;
    for (GLuint program : programs) {
        const auto start = std::chrono::steady_clock::now();
        glUseProgram(program);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        total += ms;
        worst = std::max(worst, ms);
    }
    glFinish();
    std::fprintf(stderr, "  bench: first draws of %d programs: %.2f ms total, %.2f ms worst\n", kPrograms, total, worst);
    for (GLuint program : programs) glDeleteProgram(program);
    glDeleteBuffers(1, &vbo);
    glDeleteVertexArrays(1, &vao);
}

TEST(bench_small_passes) {
    // WMGL_BENCH: frames of 100 render passes of 6 small draws (by default), cycling over
    // 3 colour-only 640x360 framebuffers (as a frame's many short passes:
    // shadow cascades, decals, post effects). Prints wall time per frame
    // with the GPU finished each frame, so it measures the GPU's per-pass cost.
    const char* env = std::getenv("WMGL_BENCH");
    if (!env) return;
    const int frames = std::atoi(env) > 0 ? std::atoi(env) : 300;
    // WMGL_BENCH_PASSES, WMGL_BENCH_DRAWS (per pass), WMGL_BENCH_SIZE (target width; 16:9).
    auto setting = [](const char* name, int fallback) {
        const char* v = std::getenv(name);
        return v && std::atoi(v) > 0 ? std::atoi(v) : fallback;
    };
    const int passes = setting("WMGL_BENCH_PASSES", 100), draws = setting("WMGL_BENCH_DRAWS", 6);
    const int width = setting("WMGL_BENCH_SIZE", 640), height = width * 9 / 16;
    gpu::Fixture fixture(64, 64, true, 46);
    CHECK(fixture.ok);
    if (!fixture.ok) return;
    GLuint p = gpu::program("#version 460 core\nlayout(location = 0) in vec2 pos;\nuniform vec4 at;\n"
                            "void main() { gl_Position = vec4(pos * at.zw + at.xy, 0.0, 1.0); }\n",
                            "#version 460 core\nuniform vec4 color;\nout vec4 o;\nvoid main() { o = color; }\n");
    glUseProgram(p);
    const GLint at = glGetUniformLocation(p, "at"), color = glGetUniformLocation(p, "color");
    const float quad[] = {0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1};
    GLuint vao = 0, vbo = 0, textures[3], fbos[3];
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glEnableVertexAttribArray(0);
    glGenTextures(3, textures);
    glGenFramebuffers(3, fbos);
    for (int i = 0; i < 3; ++i) {
        glBindTexture(GL_TEXTURE_2D, textures[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glBindFramebuffer(GL_FRAMEBUFFER, fbos[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, textures[i], 0);
    }
    glViewport(0, 0, width, height);
    using Clock = std::chrono::steady_clock;
    glFinish();
    double cpuMs = 0;
    const auto begin = Clock::now();
    for (int frame = 0; frame < frames; ++frame) {
        const auto start = Clock::now();
        for (int pass = 0; pass < passes; ++pass) {
            glBindFramebuffer(GL_FRAMEBUFFER, fbos[pass % 3]);
            for (int d = 0; d < draws; ++d) {
                glUniform4f(at, -1.0f + 0.3f * static_cast<float>(d % 6), -1.0f + 0.02f * static_cast<float>(pass % 100), 0.25f, 0.25f);
                glUniform4f(color, static_cast<float>(d) / 6.0f, static_cast<float>(pass) / 100.0f, 0.5f, 1.0f);
                glDrawArrays(GL_TRIANGLES, 0, 6);
            }
        }
        glFlush();
        cpuMs += std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        glFinish();
    }
    const double wall = std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
    std::fprintf(stderr, "  bench: %d frames x %d passes x %d draws (%dx%d): %.3f ms wall per frame (%.1f us per pass), %.3f ms CPU per frame\n",
                 frames, passes, draws, width, height, wall / frames, wall / frames * 1000.0 / passes, cpuMs / frames);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(3, fbos);
    glDeleteTextures(3, textures);
    glDeleteBuffers(1, &vbo);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
}
