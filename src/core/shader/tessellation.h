// Tessellation shader emulation (GL 4.0, GL_ARB_tessellation_shader).
//
// Metal's tessellator has no isolines or point mode and cannot feed a
// geometry shader or transform feedback, so a draw of GL_PATCHES runs
// through compute, the way geometry shaders do (geometry.h):
//
//   1. capture: the program's vertex shader, linked with capture code,
//      writes every output the control shader (or, without one, the
//      evaluation shader) reads, one record per patch vertex;
//   2. control: the control shader runs as a compute kernel, one threadgroup
//      per patch and one thread per output vertex. Its outputs and the
//      tessellation levels live in threadgroup memory, so barrier() and
//      reads of other invocations' outputs behave as in GL; when the
//      shader returns they are written to the patch record;
//   3. tessellator: the draw reads the levels back and tessellates each
//      patch on the CPU (core/tess/tessellator.h);
//   4. evaluation: the evaluation shader becomes a vertex shader that runs
//      once per domain point, reading gl_TessCoord, its patch number and its
//      patch from buffers. It is linked with the geometry shader (if any) and
//      the program's transform feedback varyings, so geometry emulation and
//      capture work unchanged, and draws with the program's fragment shader.
//
// Rasterized triangle and quad patches that nothing else needs on the CPU
// (no geometry shader, transform feedback or counting query) skip steps 3
// and 4's domain points: a kernel turns the levels into Metal tessellation
// factors, and Metal's tessellator feeds the evaluation vertex shader as a
// post-tessellation vertex function (postTessellationVertexMsl), so the
// draw does not wait for the control stage. GL leaves the order of a
// tessellated triangle's vertices (so its provoking vertex) and of the
// triangles to the implementation; Metal's and the CPU tessellator's differ.
// Patches are discarded and levels clamped as GL specifies on both.
//
// A multi-draw indirect of patches read on the CPU draws runs of commands
// that each draw one instance of the same number of patches as one batch
// (DrawState::drawPatchCommands): one vertex capture, control dispatch and
// tessellation draw instead of a pass of each per command. gl_PrimitiveID
// still starts again at 0 in each command (the patch number modulo the
// patches per command); programs reading gl_DrawID, gl_BaseVertex or
// gl_BaseInstance draw each command on its own.
//
// The generated kernels come from GLSL that SPIRV-Cross emits for the linked
// stages, so the default uniform block keeps the program's layout and
// uniform blocks and samplers keep their names.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "core/shader/compiler.h"

namespace wmgl::shader {

// A control or evaluation shader input or output (block members are
// flattened). Per-vertex variables are arrays indexed by vertex; arraySize
// is any further dimension (of the variable, or of the block member).
struct TessVar {
    std::string glslType;     // "vec4", "int", "mat3" ...
    GLenum type = 0;
    std::string name;         // variable, or block instance name
    std::string member;       // block member, empty for plain variables
    std::string blockName;    // interface block name for block members
    // A field of a struct-typed variable or block member, which is stored
    // field by field: the declared struct, its declared array size (0 = not
    // an array) and the field's path below the variable or member (".a",
    // "[1].b.c").
    std::string structType;
    int structArray = 0;
    std::string path;
    int arraySize = 0;        // 0 = not an array
    bool patch = false;       // per-patch (patch in/out)
    int location = -1;        // layout(location) of the variable (plain variables)
    std::string declaration;  // the whole original declaration (evaluation outputs)
};

struct TessStageInfo {
    std::vector<TessVar> inputs, outputs;
    int verticesOut = 0;  // control: layout(vertices = N)
    bool readsPointSize = false, readsClipDistance = false, readsCullDistance = false;
    int inputClipDistances = 0, outputClipDistances = 0, inputCullDistances = 0, outputCullDistances = 0;
    bool writesPointSize = false;
    std::string globals;    // uniforms, constants, structs, extensions
    std::string outputDeclarations;  // evaluation: output declarations (vertex shader outputs)
    std::string functions;  // function definitions, built-ins renamed; main is wmgl_tess_main
};

// Parses SPIRV-Cross GLSL of a linked control or evaluation stage.
bool parseTessellationGlsl(Stage stage, const std::string& glsl, TessStageInfo& info, std::string& log);

// Varyings the vertex capture must write for `first` (the control stage, or
// the evaluation stage without one), named as transform feedback varyings.
std::vector<std::string> tessellationCaptureVaryings(const TessStageInfo& first);

// Patch records (32-bit words): `verticesOut` vertex records of vertexWords,
// then the patch section: the outer levels (4 words), the inner levels
// (2 words), then the per-patch outputs.
struct TessRecordField {
    std::string key;        // "gl_Position", "gl_PointSize", "gl_ClipDistance", "gl_CullDistance", "name" or "Block.member"
    GLenum type = 0;
    int elements = 1;       // array elements
    uint32_t offset = 0;    // words, within the vertex record or the patch section
    int location = -1;
};

struct TessControlSources {
    std::string kernel;     // GLSL 4.50 compute
    int verticesOut = 0;
    std::vector<TessRecordField> vertexFields, patchFields;
    uint32_t vertexWords = 0, patchWords = 0;
    // Patches each threadgroup of the kernel runs (local size verticesOut x
    // this): one 4-vertex patch alone fills an eighth of a SIMD-group.
    uint32_t patchesPerGroup = 1;
};

// `capture` is the layout of the captured vertex-shader outputs.
TessControlSources tessControlSources(const TessStageInfo& control, const LinkResult& capture);

// The evaluation stage as a vertex shader. With a control stage, it reads
// that stage's patch records; without one, the captured vertex records and
// the default levels.
std::string tessEvaluationVertexShader(const TessStageInfo& evaluation, const TessControlSources* control,
                                       const LinkResult* capture, std::string& log);

// The evaluation vertex function (`msl`, from tessEvaluationVertexShader) as
// a Metal post-tessellation vertex function for triangle or quad patches:
// the patch number and gl_TessCoord come from [[patch_id]] and
// [[position_in_patch]] instead of the domain point buffer, so draws can use
// Metal's tessellator. Empty when the function does not have the expected form.
std::string postTessellationVertexMsl(const std::string& msl, const std::string& entryPoint, bool quads);

}  // namespace wmgl::shader
