// ARB assembly program objects and their draw-time programs (issue #25).
//
// GL_ARB_vertex_program and GL_ARB_fragment_program objects live in their
// own name space (BindProgramARB, not glUseProgram). With VERTEX_PROGRAM_ARB
// or FRAGMENT_PROGRAM_ARB enabled and no GLSL program or pipeline current,
// a draw uses a generated program: the enabled stages' ARB programs
// translated to GLSL (compat/arb_program.h), paired with the generated
// fixed-function code (compat/fixed_function.h) for a stage without one.
// Those programs are cached in the share group by the programs and the state
// that changes their code; program.env/program.local parameters and program
// matrices are written into their default uniforms before each draw.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>

#include "core/compat/arb_program.h"
#include "core/gl/types.h"
#include "core/util/ref.h"

namespace wmgl {

class Context;
class Program;

namespace compat {

using Vec4 = std::array<float, 4>;

struct ArbProgramObject {
    GLuint name = 0;
    GLenum target = 0;  // GL_VERTEX_PROGRAM_ARB or GL_FRAGMENT_PROGRAM_ARB, 0 until first bound
    // The program that loaded last (a string that fails to load leaves it).
    std::string source;
    std::shared_ptr<const arb::Parsed> parsed;
    uint64_t serial = 0;  // unique per successful load (cache keys)
    std::array<Vec4, arb::Limits::kLocalParameters> local{};
};

// Per-context ARB program state outside ContextState (glPushAttrib copies
// that): bound programs, environment parameters and the last load's error.
struct ArbContextState {
    // Vertex, fragment; null: name 0, the context's default object of the
    // target (created when first used, kept across bindings).
    std::shared_ptr<ArbProgramObject> bound[2], defaults[2];
    std::array<Vec4, arb::Limits::kEnvParameters> env[2]{};
    GLint errorPosition = -1;
    std::string errorString;
};

// A generated program with an ARB stage, and where its parameters go.
struct ArbLinked {
    RefPtr<Program> program;
    int envOffset[2] = {-1, -1}, localOffset[2] = {-1, -1};  // default-block byte offsets, per stage
    int envCount[2] = {}, localCount[2] = {};  // elements the linked program kept
    int matrixOffset = -1, inverseOffset = -1, matrixCount = 0, inverseCount = 0;
    std::shared_ptr<const arb::Parsed> parsed[2];  // keeps the counts' programs alive
};

// 0 vertex, 1 fragment; -1 for another target.
int arbTargetIndex(GLenum target);

// The program bound to target index `i` (the default object for name 0).
ArbProgramObject& arbBound(Context& ctx, int i);

// An ARB program stage is enabled for the next draw (compatibility profile,
// no GLSL program or pipeline current).
bool arbProgramsEnabled(const Context& ctx);
// An enabled stage's bound program has not loaded (draws are INVALID_OPERATION).
bool arbProgramsInvalid(const Context& ctx);
// The vertex stage is an ARB program for the next draw.
bool arbVertexProgramActive(const Context& ctx);

// The generated program for the next draw of `mode`, with its parameters
// written; null (logged once) when it cannot be built.
Program* arbDrawProgram(Context& ctx, GLenum mode);

}  // namespace compat
}  // namespace wmgl
