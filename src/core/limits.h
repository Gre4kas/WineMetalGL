// Implementation limits reported through glGet and enforced by validation.
#pragma once

#include "core/gl/types.h"

namespace wmgl::limits {

constexpr GLint kMaxTextureSize = 16384, kMax3DTextureSize = 2048, kMaxArrayLayers = 2048;
// Combined limits cover the five graphics stages of GL 4.0 (5 x per stage).
constexpr GLint kMaxDrawBuffers = 8, kMaxVertexAttribs = 16, kMaxTextureUnitsPerStage = 16, kMaxCombinedTextureUnits = 80;
constexpr GLint kMaxUniformBlocksPerStage = 14, kMaxCombinedUniformBlocks = 70, kMaxUniformBufferBindings = 72;
constexpr GLint kMaxUniformBlockSize = 65536, kMaxDefaultUniformComponents = 4096;
constexpr GLint kMaxTextureBufferSize = 1 << 27;
// Metal clamps a sampler's LOD bias just below 16, so 15 is the largest
// whole value it represents exactly (GL requires at least 2).
// GL_ARB_compute_shader (the values glslang's default resources declare).
constexpr GLint kMaxComputeWorkGroupCount = 65535, kMaxComputeWorkGroupInvocations = 1024;
constexpr GLint kMaxComputeWorkGroupSize[3] = {1024, 1024, 64};
constexpr GLint kMaxComputeSharedMemorySize = 32768;
// Tessellation (GL 4.0): Metal allows 32 control points and factors up to 64.
constexpr GLint kMaxPatchVertices = 32, kMaxTessGenLevel = 64;
// GL 4.2 image load/store and atomic counters. Counters live in fragment,
// geometry (a compute kernel) and compute functions, whose Metal buffer
// slots have room for 8 counter buffers; vertex functions have none left
// (shader/compiler.h).
constexpr GLint kMaxImageUnits = 8, kMaxImageUniformsPerStage = 8, kMaxCombinedImageUniforms = 48;
constexpr GLint kMaxCombinedImageUnitsAndFragmentOutputs = 16;
constexpr GLint kMaxAtomicCounterBufferBindings = 8, kMaxStageAtomicCounterBuffers = 8, kMaxStageAtomicCounters = 8;
constexpr GLint kMaxVertexAtomicCounterBuffers = 1;
constexpr GLint kMaxAtomicCounterBufferSize = 16384;
constexpr GLint kMaxViewports = 16, kMaxGeometryShaderInvocations = 32;
constexpr GLfloat kMaxTextureLodBias = 15.0f, kMaxAnisotropy = 16.0f;

}  // namespace wmgl::limits
