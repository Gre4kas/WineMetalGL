// Compatibility profile vertex and fragment processing (W22).
//
// Fixed-function state reaches shaders through one std140 uniform block,
// wmgl_FixedFunction, declared by the legacy GLSL lowering
// (shader/glsl_upgrade.cpp): application shaders that read gl_ModelViewMatrix,
// gl_LightSource etc. and the generated fixed-function programs use the same
// block, so state changes never need new shaders.
//
// When the compatibility profile draws with no program (and no program
// pipeline), DrawState uses a generated program. Programs are keyed by the
// parts of the state that change the code (which lights, texture targets,
// environment modes, fog mode, ...), in the style of Mesa's ffvertex_prog
// and texenv programs, and cached in the share group.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/gl/types.h"
#include "core/util/ref.h"

namespace wmgl {

class Context;
class Program;

namespace compat {

// The wmgl_FixedFunction block contents for the context's current state.
// Rebuilt only when the state changed since the last call; `generation`
// changes whenever the contents do.
const std::vector<uint8_t>& fixedFunctionBlock(Context& ctx, uint64_t& generation);

// The generated program for the current fixed-function state when drawing
// `mode`, or null (with a logged reason) if it cannot be built.
Program* fixedFunctionProgram(Context& ctx, GLenum mode);

// The fixed-function program draws a point size it computed (distance
// attenuation, or glPointSize clamped to POINT_SIZE_MIN/MAX) rather than
// glPointSize. POINT_FADE_THRESHOLD_SIZE (an alpha fade of multisampled
// points below the threshold) is not applied.
bool fixedFunctionWritesPointSize(Context& ctx);

// TEXTURE_LOD_BIAS of texture unit `unit` (glTexEnv with
// GL_TEXTURE_FILTER_CONTROL), added to the texture or sampler object's bias
// of every lookup through the unit (GL 3.3 compatibility section 3.9.11).
float textureUnitLodBias(const Context& ctx, GLint unit);

// Lines wider than one pixel (GL_ALIASED_LINE_WIDTH_RANGE) are drawn by the
// fixed-function program as two triangles per segment, offset along the
// segment's window-space minor axis (GL 3.3 compatibility section 3.5.2):
// true when a fixed-function draw of `mode` needs that.
constexpr float kMaxLineWidth = 64.0f;
bool fixedFunctionWideLines(Context& ctx, GLenum mode);

// GLSL of the fixed-function vertex and fragment shaders for the current
// state (tests and WINEMETALGL_LOG=trace dumps).
void fixedFunctionSources(Context& ctx, GLenum mode, std::string& vertex, std::string& fragment);

// Links an internal program (not in any name table) from GLSL sources.
// Samplers named "<prefix>N" sample texture unit N.
RefPtr<Program> linkInternalProgram(const std::string& vertex, const std::string& fragment, const char* samplerPrefix,
                                    std::string& log);

}  // namespace compat
}  // namespace wmgl
