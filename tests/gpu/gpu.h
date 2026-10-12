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
    X(glEndTransformFeedback) X(glGetBufferSubData) X(glDispatchCompute) X(glDispatchComputeIndirect)     \
    X(glGetIntegeri_v) X(glTexParameterf) X(glProvokingVertex) X(glPointSize)                    \
    X(glPointParameteri) X(glDrawArraysInstancedBaseInstance) X(glVertexAttribDivisor)   \
    X(glBlendFunci) X(glEnablei) X(glBindFramebufferEXT) X(glClipControl) X(glDepthRange) X(glBufferStorage) X(glMapBufferRange) X(glUnmapBuffer)             \
    X(glDebugMessageCallback) X(glDebugMessageControl) X(glDebugMessageInsert) X(glGetDebugMessageLog)            \
    X(glPushDebugGroup) X(glPopDebugGroup) X(glObjectLabel) X(glGetObjectLabel)                     \
    X(glCreateShaderProgramv) X(glGenProgramPipelines) X(glBindProgramPipeline) X(glUseProgramStages)             \
    X(glProgramUniform4f) X(glGetProgramPipelineiv) X(glValidateProgramPipeline) X(glDeleteProgramPipelines)      \
    X(glActiveShaderProgram) X(glProgramParameteri) WMGL_GPU_FUNCS_GL4(X)

// GL 4.0-4.2 entry points (gl4_tests.cpp).
#define WMGL_GPU_FUNCS_GL4(X)                                                                                \
    X(glDrawArraysIndirect) X(glDrawElementsIndirect) X(glViewportIndexedf) X(glScissorIndexed)             \
    X(glGetFloati_v) X(glFramebufferTexture) X(glGenTransformFeedbacks) X(glBindTransformFeedback)          \
    X(glDeleteTransformFeedbacks) X(glPauseTransformFeedback) X(glResumeTransformFeedback)                  \
    X(glDrawTransformFeedback) X(glPatchParameteri) X(glGetActiveAtomicCounterBufferiv) X(glMemoryBarrier)           \
    X(glBindImageTexture) X(glTexStorage2D) X(glUniform1iv) X(glDrawElementsInstancedBaseVertex) X(glUniform1uiv)                   \
    X(glVertexAttribFormat) X(glVertexAttribBinding) X(glBindVertexBuffer) X(glVertexBindingDivisor) X(glGetVertexAttribiv)        \
    X(glTexBufferRange) X(glTextureView) X(glTexStorage3D) X(glMultiDrawArraysIndirect) X(glMultiDrawElementsIndirect)      \
    X(glClearBufferData) X(glClearBufferSubData) X(glFramebufferParameteri) X(glGetFramebufferParameteriv)          \
    X(glGetInternalformativ) X(glGetInternalformati64v) X(glBlendFunc) X(glGetSubroutineIndex)              \
    X(glUniformSubroutinesuiv) X(glGetSubroutineUniformLocation) X(glGetProgramResourceIndex)             \
    X(glGetProgramResourceiv) X(glTexParameterIiv) X(glTexParameterIuiv) WMGL_GPU_FUNCS_GL44(X)

// GL 4.4-4.6 entry points (gl44_tests.cpp and later).
#define WMGL_GPU_FUNCS_GL44(X)                                                                        \
    X(glClearTexImage) X(glClearTexSubImage) X(glBindBuffersBase) X(glBindBuffersRange) X(glBindTextures)         \
    X(glBindSamplers) X(glBindImageTextures) X(glBindVertexBuffers) X(glGetInteger64i_v) X(glGetActiveUniformBlockiv)     \
    X(glCreateBuffers) X(glNamedBufferData) X(glNamedBufferSubData) X(glCreateTextures) X(glTextureStorage2D)     \
    X(glTextureSubImage2D) X(glGetTextureImage) X(glCreateFramebuffers) X(glNamedFramebufferTexture)                \
    X(glCheckNamedFramebufferStatus) X(glClearNamedFramebufferfv) X(glCreateVertexArrays) X(glVertexArrayVertexBuffer) \
    X(glVertexArrayAttribFormat) X(glVertexArrayAttribBinding) X(glEnableVertexArrayAttrib) X(glBindTextureUnit)  \
    X(glTextureParameteri) X(glGetNamedBufferSubData) X(glBlitNamedFramebuffer) X(glGetTextureSubImage)          \
    X(glGetCompressedTextureSubImage) X(glTextureStorage3D) X(glTextureSubImage3D) X(glVertexAttrib4fv)          \
    X(glMultiDrawArrays) X(glMultiDrawArraysIndirectCount) X(glDrawElementsBaseVertex) X(glBeginQueryIndexed)     \
    X(glEndQueryIndexed) X(glGetProgramInterfaceiv) X(glCompressedTexSubImage2D) X(glSampleMaski) X(glDeleteRenderbuffers)

#define WMGL_DECLARE_FUNC(name) extern PFN_##name name;
WMGL_GPU_FUNCS(WMGL_DECLARE_FUNC)
#undef WMGL_DECLARE_FUNC

namespace gpu {

// A context current on a pbuffer for the lifetime of the fixture, requesting
// `version` (10 * major + minor); versions above 4.6 go through the
// development override (WINEMETALGL_DEV_MAX_VERSION).
struct Fixture {
    explicit Fixture(int width = 64, int height = 64, bool core = true, int version = 33);
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
// A test with a GL 4.x context (`version` = 10 * major + minor).
#define GPU_TEST_VERSION(name, version)            \
    static void name##_body();                     \
    TEST(name) {                                   \
        gpu::Fixture fixture_(64, 64, true, version); \
        CHECK(fixture_.ok);                        \
        if (fixture_.ok) name##_body();            \
    }                                              \
    static void name##_body()
#define GPU_TEST(name)                     \
    static void name##_body();             \
    TEST(name) {                           \
        gpu::Fixture fixture_;             \
        CHECK(fixture_.ok);                \
        if (fixture_.ok) name##_body();    \
    }                                      \
    static void name##_body()
