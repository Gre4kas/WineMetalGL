// Queries, conditional rendering and syncs.
#include <chrono>
#include "gpu.h"

namespace {

const char* kVs = R"(#version 330 core
in vec2 position;
uniform float depth;
void main() { gl_Position = vec4(position * 0.5, depth, 1.0); })";
const char* kFs = R"(#version 330 core
out vec4 o;
void main() { o = vec4(1); })";

}  // namespace

GPU_TEST(gpu_occlusion_query) {
    GLuint p = gpu::program(kVs, kFs);
    GLuint vao = gpu::quad();
    glUseProgram(p);
    glEnable(GL_DEPTH_TEST);
    glClearDepth(0.5);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    GLuint q[3];
    glGenQueries(3, q);
    // Behind the cleared depth: no samples pass.
    glUniform1f(glGetUniformLocation(p, "depth"), 0.9f);
    glBeginQuery(GL_SAMPLES_PASSED, q[0]);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glEndQuery(GL_SAMPLES_PASSED);
    // In front: a 32x32 quad passes 1024 samples.
    glUniform1f(glGetUniformLocation(p, "depth"), -0.9f);
    glBeginQuery(GL_SAMPLES_PASSED, q[1]);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glEndQuery(GL_SAMPLES_PASSED);
    glDepthFunc(GL_LEQUAL);  // same depth as the previous draw
    glBeginQuery(GL_ANY_SAMPLES_PASSED, q[2]);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glEndQuery(GL_ANY_SAMPLES_PASSED);
    GLuint hidden = 99, visible = 0, any = 0;
    glGetQueryObjectuiv(q[0], GL_QUERY_RESULT, &hidden);
    glGetQueryObjectuiv(q[1], GL_QUERY_RESULT, &visible);
    glGetQueryObjectuiv(q[2], GL_QUERY_RESULT, &any);
    CHECK_EQ(hidden, 0u);
    CHECK_EQ(visible, 1024u);
    CHECK_EQ(any, 1u);
    GLuint available = 0;
    glGetQueryObjectuiv(q[0], GL_QUERY_RESULT_AVAILABLE, &available);
    CHECK_EQ(available, 1u);
    // Conditional rendering on the hidden query skips the clear.
    glClearColor(1, 0, 0, 1);
    glBeginConditionalRender(q[0], GL_QUERY_WAIT);
    glClear(GL_COLOR_BUFFER_BIT);
    glEndConditionalRender();
    auto pixel = gpu::readRGBA8(1, 1, 1, 1);
    CHECK_EQ(static_cast<int>(pixel[0]), 0);
    glBeginConditionalRender(q[1], GL_QUERY_WAIT);
    glClear(GL_COLOR_BUFFER_BIT);
    glEndConditionalRender();
    pixel = gpu::readRGBA8(1, 1, 1, 1);
    CHECK_EQ(static_cast<int>(pixel[0]), 255);
    glDeleteQueries(3, q);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
    glDisable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    CHECK_GL();
}

GPU_TEST(gpu_primitives_generated_and_timer) {
    GLuint p = gpu::program(kVs, kFs);
    GLuint vao = gpu::quad();
    glUseProgram(p);
    GLuint q[4];
    glGenQueries(4, q);
    glBeginQuery(GL_PRIMITIVES_GENERATED, q[0]);
    glBeginQuery(GL_TIME_ELAPSED, q[1]);
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, 3);
    glEndQuery(GL_TIME_ELAPSED);
    glEndQuery(GL_PRIMITIVES_GENERATED);
    glQueryCounter(q[2], GL_TIMESTAMP);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glQueryCounter(q[3], GL_TIMESTAMP);
    GLuint prims = 0;
    GLuint64 elapsed = 0, t0 = 0, t1 = 0;
    glGetQueryObjectuiv(q[0], GL_QUERY_RESULT, &prims);
    glGetQueryObjectui64v(q[1], GL_QUERY_RESULT, &elapsed);
    glGetQueryObjectui64v(q[2], GL_QUERY_RESULT, &t0);
    glGetQueryObjectui64v(q[3], GL_QUERY_RESULT, &t1);
    CHECK_EQ(prims, 6u);
    CHECK(elapsed > 0 && elapsed < 1000000000ull);
    if (!(t0 > 0 && t1 >= t0)) std::fprintf(stderr, "  t0 %llu t1 %llu\n", (unsigned long long)t0, (unsigned long long)t1);
    CHECK(t0 > 0 && t1 >= t0);
    GLint64 now = 0;
    glGetInteger64v(GL_TIMESTAMP, &now);
    CHECK(static_cast<GLuint64>(now) >= t1);
    glBeginQuery(GL_TIME_ELAPSED, q[2]);  // q[2] is a timestamp query
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_OPERATION));
    glEndQuery(GL_SAMPLES_PASSED);
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_OPERATION));
    glDeleteQueries(4, q);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
    CHECK_GL();
}

GPU_TEST(gpu_fence_sync) {
    glClear(GL_COLOR_BUFFER_BIT);
    GLsync sync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    CHECK(sync != nullptr);
    CHECK(glIsSync(sync));
    const GLenum result = glClientWaitSync(sync, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull);
    CHECK(result == GL_ALREADY_SIGNALED || result == GL_CONDITION_SATISFIED);
    GLint status = 0, length = 0;
    glGetSynciv(sync, GL_SYNC_STATUS, 1, &length, &status);
    CHECK_EQ(status, GL_SIGNALED);
    CHECK_EQ(length, 1);
    glDeleteSync(sync);
    CHECK(!glIsSync(sync));
    glClientWaitSync(sync, 0, 0);
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_VALUE));
    CHECK_GL();
}

// KHR-GL32.api.coverage queries GL_MAX_ELEMENT_INDEX through glGetInteger64v.
GPU_TEST(gpu_max_element_index) {
    GLint64 maxIndex = 0;
    glGetInteger64v(GL_MAX_ELEMENT_INDEX, &maxIndex);
    CHECK_EQ(maxIndex, static_cast<GLint64>(UINT32_MAX));
    CHECK_GL();
}

// GL_TIME_ELAPSED of a GPU-heavy pass (timed by encoder timestamps, without
// commits) is within the wall time from its first command to glFinish, and
// a sizeable part of it.
GPU_TEST(gpu_timer_elapsed_matches_wall_time) {
    static const char* fs = R"(#version 330
out vec4 o;
void main() {
    vec2 p = gl_FragCoord.xy * 0.001;
    for (int i = 0; i < 400; ++i) p = vec2(sin(p.x * 1.7 + p.y), cos(p.y * 1.3 - p.x));
    o = vec4(p, 0.0, 1.0);
})";
    GLuint p = gpu::program(kVs, fs);
    GLuint vao = gpu::quad();
    GLuint tex = 0, fb = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1024, 1024, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    glViewport(0, 0, 1024, 1024);
    glUseProgram(p);
    glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);  // compiles the pipeline
    glFinish();
    GLuint q = 0;
    glGenQueries(1, &q);
    const auto start = std::chrono::steady_clock::now();
    glBeginQuery(GL_TIME_ELAPSED, q);
    for (int i = 0; i < 8; ++i) glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glEndQuery(GL_TIME_ELAPSED);
    glFinish();
    const double wall = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count();
    GLuint64 elapsed = 0;
    glGetQueryObjectui64v(q, GL_QUERY_RESULT, &elapsed);
    if (!(elapsed > 0 && static_cast<double>(elapsed) <= wall * 1.05 && static_cast<double>(elapsed) >= wall * 0.2))
        std::fprintf(stderr, "  elapsed %llu ns, wall %.0f ns\n", static_cast<unsigned long long>(elapsed), wall);
    CHECK(elapsed > 0);
    CHECK(static_cast<double>(elapsed) <= wall * 1.05);
    CHECK(static_cast<double>(elapsed) >= wall * 0.2);
    glDeleteQueries(1, &q);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fb);
    glDeleteTextures(1, &tex);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
    CHECK_GL();
}
