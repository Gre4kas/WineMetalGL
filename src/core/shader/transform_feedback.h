// Transform feedback: varying validation, capture layout and the GLSL that
// captures vertex-stage outputs (GL 3.3 section 2.15).
//
// Metal has no stream output. A program that captures from its vertex stage
// is linked a second time with code appended to the vertex shader: after the
// application's main() it writes the captured outputs, as raw 32-bit words,
// to storage buffers bound in the XFB slots. The draw path runs that vertex
// function over a CPU-expanded, non-indexed vertex list in primitive order
// with rasterization disabled (see DrawState::captureTransformFeedback), so
// vertex k of the list lands at capture slot k.
#pragma once

#include <string>
#include <vector>

#include "core/shader/compiler.h"

namespace wmgl::shader {

struct StageOutputVar {
    std::string name;
    GLenum type = 0;
    int arraySize = 0;       // 0 = not an array
    std::string expression;  // GLSL access path when it differs from name ("inst.member")
    // A member of an output block without an instance name, captured by its
    // plain name: its "Block.member" name (the emulations translate stages
    // through GLSL that gives every block an instance name).
    std::string qualified;
};

// Validates glTransformFeedbackVaryings against the capturing stage's outputs
// and fills result.transformFeedback / xfbStride / xfbBufferCount. Returns
// false with a link error in `log`.
bool layoutTransformFeedback(const std::vector<std::string>& varyings, GLenum mode,
                             const std::vector<StageOutputVar>& outputs, LinkResult& result, std::string& log,
                             uint32_t maxInterleavedComponents = 64);

// Expressions for each 32-bit word of one element of `type`, converted to uint
// (matrices column by column).
std::vector<std::string> captureWords(const std::string& expression, GLenum type);

struct CaptureCode {
    std::string declarations;  // inserted after the #version / #extension lines
    std::string epilogue;      // appended after the shader source
};
CaptureCode vertexCaptureCode(const LinkResult& layout, int glslVersion);

}  // namespace wmgl::shader
