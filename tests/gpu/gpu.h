// GPU tests: run the core on Metal through the driver ABI with pbuffer
// surfaces, without Wine. GL entry points come from wmgl_get_proc_address.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "../host/test.h"
#include "gl_enums.gen.h"
#include "gl_pfn.gen.h"
#include "winemetalgl/driver.h"

#define WMGL_GPU_FUNCS(X)                                                                                    \
    X(glGetError) X(glGetString) X(glGetStringi) X(glGetIntegerv) X(glGetFloatv) X(glGetInteger64v)         \
    X(glEnable) X(glDisable) X(glViewport) X(glScissor) X(glClearColor) X(glClearDepth) X(glClearStencil)   \
    X(glClear) X(glClearBufferiv) X(glClearBufferuiv) X(glClearBufferfv) X(glClearBufferfi) X(glColorMask)  \
    X(glDepthFunc) X(glDepthMask) X(glFinish) X(glFlush) X(glReadPixels) X(glPixelStorei) X(glReadBuffer)   \
    X(glDrawBuffers) X(glGenBuffers) X(glDeleteBuffers) X(glBindBuffer) X(glBufferData) X(glBufferSubData) \
    X(glGenVertexArrays) X(glDeleteVertexArrays) X(glBindVertexArray) X(glVertexAttribPointer)             \
    X(glEnableVertexAttribArray) X(glDrawArrays) X(glDrawArraysInstanced) X(glDrawElements)                \
    X(glCreateShader) X(glShaderSource) X(glCompileShader) X(glGetShaderiv) X(glGetShaderInfoLog)           \
    X(glCreateProgram) X(glAttachShader) X(glLinkProgram) X(glGetProgramiv) X(glGetProgramInfoLog)          \
    X(glUseProgram) X(glDeleteProgram) X(glDeleteShader) X(glGetUniformLocation) X(glUniform1i)            \
    X(glUniform1f) X(glUniform4f) X(glBindAttribLocation) X(glBindFragDataLocation)                        \
    X(glGenTextures) X(glDeleteTextures) X(glBindTexture) X(glActiveTexture) X(glTexImage1D)                \
    X(glTexImage2D) X(glTexImage3D) X(glTexSubImage2D) X(glTexSubImage3D) X(glCompressedTexImage2D)       \
    X(glGetCompressedTexImage) X(glCopyTexImage2D) X(glCopyTexSubImage2D) X(glGetTexImage)                \
    X(glGetTexLevelParameteriv) X(glTexParameteri) X(glTexParameterfv) X(glGetTexParameteriv)              \
    X(glGenerateMipmap) X(glTexBuffer) X(glTexImage2DMultisample) X(glIsTexture)                           \
    X(glGenSamplers) X(glDeleteSamplers) X(glBindSampler) X(glSamplerParameteri)                           \
    X(glGenFramebuffers) X(glDeleteFramebuffers) X(glBindFramebuffer) X(glFramebufferTexture2D)            \
    X(glFramebufferTextureLayer) X(glFramebufferRenderbuffer) X(glCheckFramebufferStatus)                  \
    X(glGenRenderbuffers) X(glBindRenderbuffer) X(glRenderbufferStorage)                                   \
    X(glRenderbufferStorageMultisample) X(glBlitFramebuffer) X(glGenQueries) X(glDeleteQueries)            \
    X(glBeginQuery) X(glEndQuery) X(glGetQueryObjectuiv) X(glGetQueryObjectui64v) X(glQueryCounter)        \
    X(glBeginConditionalRender) X(glEndConditionalRender) X(glFenceSync) X(glClientWaitSync)               \
    X(glGetSynciv) X(glDeleteSync) X(glIsSync) X(glVertexAttribP4ui) X(glGetVertexAttribfv)                \
    X(glGetMultisamplefv) X(glGetUniformBlockIndex) X(glUniformBlockBinding) X(glBindBufferBase)        \
    X(glBindBufferRange) X(glUniformMatrix3fv) X(glUniformMatrix4fv) X(glGetAttribLocation) X(glTransformFeedbackVaryings) X(glBeginTransformFeedback)        \
    X(glEndTransformFeedback) X(glGetBufferSubData)

#define WMGL_DECLARE_FUNC(name) extern PFN_##name name;
WMGL_GPU_FUNCS(WMGL_DECLARE_FUNC)
#undef WMGL_DECLARE_FUNC

namespace gpu {

// A 3.3 core context current on a pbuffer for the lifetime of the fixture.
struct Fixture {
    explicit Fixture(int width = 64, int height = 64, bool core = true);
    ~Fixture();
    bool ok = false;
    wmgl_context* context = nullptr;
    wmgl_surface* surface = nullptr;
};

// Compiles and links; records a check failure (with the log) on error.
GLuint program(const char* vs, const char* fs);
// A VAO with a fullscreen quad (attribute 0: vec2 in [-1, 1]); draw with GL_TRIANGLE_STRIP, 4 vertices.
GLuint quad();
std::vector<uint8_t> readRGBA8(int x, int y, int width, int height);
// True if every channel of `got` is within `tolerance` of `want`.
bool near(const uint8_t* got, const uint8_t want[4], int tolerance = 1);

}  // namespace gpu

#define CHECK_GL() CHECK_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR))
#define GPU_TEST(name)                     \
    static void name##_body();             \
    TEST(name) {                           \
        gpu::Fixture fixture_;             \
        CHECK(fixture_.ok);                \
        if (fixture_.ok) name##_body();    \
    }                                      \
    static void name##_body()
