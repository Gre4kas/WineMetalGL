// ARB assembly programs (GL_ARB_vertex_program, GL_ARB_fragment_program;
// issue #25): the "!!ARBvp1.0" and "!!ARBfp1.0" languages, translated to
// compatibility-profile GLSL that the shader compiler links like the
// generated fixed-function programs (core/compat/fixed_function.h).
//
// parse() checks a program string when glProgramStringARB loads it (the
// error position and string are what the GL reports); emit() writes the
// GLSL of a parsed program for the state at a draw (shading model, two-sided
// colours, user clip planes, point sprites). The stages exchange the
// fixed-function program's varyings (ff_front, ff_frontSecondary, ff_back,
// ff_backSecondary, ff_tc0..7, ff_fogCoord), so an ARB program of one stage
// links with the generated fixed-function code of the other.
//
// GLSL names: program.env[] and program.local[] are uniform arrays (envName)
// sized to the highest index the program uses; vertex.attrib[n] (n > 0) is
// the input arb_attrib<n>, bound to generic attribute n (vertex.attrib[0]
// is the position, gl_Vertex); program matrices (state.matrix.program[n]) are the uniforms arb_pm[8] and,
// when used, arb_pmInv[8]; texture image unit n is the sampler ff_tex<n>.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace wmgl::compat::arb {

enum class Kind : uint8_t { Vertex, Fragment };

// Implementation limits (GetProgramivARB MAX_PROGRAM_*: the native limits
// are the same, as every program the GL takes runs as GLSL).
struct Limits {
    static constexpr int kEnvParameters = 256, kLocalParameters = 256;
    static constexpr int kInstructions = 16384, kTemporaries = 256, kParameters = 1024;
    static constexpr int kVertexAttribs = 16, kFragmentAttribs = 16, kAddressRegisters = 1;
    static constexpr int kTexCoords = 8, kTextureUnits = 16, kProgramMatrices = 8;
};

// Texture targets of TEX/TXP/TXB (per image unit).
enum class TexTarget : uint8_t { None, T1D, T2D, T3D, Cube, Rect, Shadow1D, Shadow2D, ShadowRect };

// Conventional vertex inputs (bits of Parsed::conventionalRead), in the
// order of ConventionalAttrib (core/compat/fixed_state.h) from kConvVertex.
enum ConventionalBit : uint32_t {
    kInPosition = 1u << 0, kInNormal = 1u << 1, kInColor0 = 1u << 2, kInColor1 = 1u << 3, kInFogCoord = 1u << 4,
    kInTexCoord0 = 1u << 5,  // ... kInTexCoord0 << 7
};

struct Instruction;
struct Operand;

struct Parsed {
    Kind kind = Kind::Vertex;
    bool ok = false;
    int errorPosition = -1;  // byte offset in the string, -1 when it loaded
    std::string error;

    // Options.
    bool positionInvariant = false;   // ARB_position_invariant
    int fog = 0;                      // ARB_fog_linear/exp/exp2: GL_LINEAR, GL_EXP, GL_EXP2, or 0
    bool drawBuffers = false;         // ARB_draw_buffers: result.color[n]

    // Resource counts (GetProgramivARB PROGRAM_* and PROGRAM_NATIVE_*).
    int instructions = 0, aluInstructions = 0, texInstructions = 0, texIndirections = 0;
    int temporaries = 0, parameters = 0, attribs = 0, addressRegisters = 0;

    // What the program reads and writes.
    uint32_t conventionalRead = 0;    // vertex: ConventionalBit
    uint32_t genericRead = 0;         // vertex: vertex.attrib[n], n > 0 (attrib[0] is the position)
    uint32_t texCoordRead = 0;        // fragment: fragment.texcoord[n]
    bool colorRead[2] = {};           // fragment: primary, secondary
    bool fogCoordRead = false, positionRead = false;
    uint32_t texCoordWritten = 0;     // vertex: result.texcoord[n]
    bool colorWritten[4] = {};        // vertex: front, front secondary, back, back secondary
    bool fogCoordWritten = false, pointSizeWritten = false, positionWritten = false;
    uint32_t colorOutputs = 0;        // fragment: result.color[n] written (bit 0: result.color)
    bool depthWritten = false, kills = false;
    TexTarget textures[Limits::kTextureUnits] = {};
    int envCount = 0, localCount = 0;  // highest index used + 1
    uint8_t programMatrices = 0, programMatrixInverses = 0;  // bit per program matrix

    std::shared_ptr<const struct Body> body;
};

// Parses `length` bytes of an "!!ARBvp1.0" (Kind::Vertex) or "!!ARBfp1.0"
// program string.
std::shared_ptr<const Parsed> parse(Kind kind, const char* source, size_t length);

struct EmitOptions {
    bool flat = false;          // GL_FLAT: the colours are flat varyings
    bool twoSide = false;       // fragment: back colours for back faces
    uint8_t clipPlanes = 0;     // vertex (position-invariant): user clip planes enabled
    uint32_t pointCoordReplace = 0;  // fragment: texture coordinates replaced by gl_PointCoord
};

// The uniform array of a stage's program.env[] (`env`) or program.local[]:
// arb_vpEnv, arb_vpLocal, arb_fpEnv, arb_fpLocal.
const char* envName(Kind kind, bool env);

// The bits of EmitOptions that change emit()'s output for this program
// (cache keys).
std::string emitKey(const Parsed& program, const EmitOptions& options);

// GLSL of a parsed program (ok == true).
std::string emit(const Parsed& program, const EmitOptions& options);

}  // namespace wmgl::compat::arb
