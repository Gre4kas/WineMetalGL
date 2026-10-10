// Implementation limits reported through glGet and enforced by validation.
#pragma once

#include "core/gl/types.h"

namespace wmgl::limits {

constexpr GLint kMaxTextureSize = 16384, kMax3DTextureSize = 2048, kMaxArrayLayers = 2048;
constexpr GLint kMaxDrawBuffers = 8, kMaxVertexAttribs = 16, kMaxTextureUnitsPerStage = 16, kMaxCombinedTextureUnits = 48;
constexpr GLint kMaxUniformBlocksPerStage = 12, kMaxCombinedUniformBlocks = 36, kMaxUniformBufferBindings = 36;
constexpr GLint kMaxUniformBlockSize = 65536, kMaxDefaultUniformComponents = 4096;
constexpr GLint kMaxTextureBufferSize = 1 << 27;
// Metal clamps a sampler's LOD bias just below 16, so 15 is the largest
// whole value it represents exactly (GL requires at least 2).
// GL_ARB_compute_shader (the values glslang's default resources declare).
constexpr GLint kMaxComputeWorkGroupCount = 65535, kMaxComputeWorkGroupInvocations = 1024;
constexpr GLint kMaxComputeWorkGroupSize[3] = {1024, 1024, 64};
constexpr GLint kMaxComputeSharedMemorySize = 32768;
constexpr GLfloat kMaxTextureLodBias = 15.0f, kMaxAnisotropy = 16.0f;

}  // namespace wmgl::limits
