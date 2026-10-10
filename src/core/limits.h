// Implementation limits reported through glGet and enforced by validation.
#pragma once

#include "core/gl/types.h"

namespace wmgl::limits {

constexpr GLint kMaxTextureSize = 16384, kMax3DTextureSize = 2048, kMaxArrayLayers = 2048;
constexpr GLint kMaxDrawBuffers = 8, kMaxVertexAttribs = 16, kMaxTextureUnitsPerStage = 16, kMaxCombinedTextureUnits = 48;
constexpr GLint kMaxUniformBlocksPerStage = 12, kMaxCombinedUniformBlocks = 36, kMaxUniformBufferBindings = 36;
constexpr GLint kMaxUniformBlockSize = 65536, kMaxDefaultUniformComponents = 4096;
constexpr GLint kMaxTextureBufferSize = 1 << 27;
constexpr GLfloat kMaxTextureLodBias = 16.0f, kMaxAnisotropy = 16.0f;

}  // namespace wmgl::limits
