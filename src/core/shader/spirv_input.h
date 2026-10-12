// GL 4.6 / GL_ARB_gl_spirv: SPIR-V modules given to glShaderBinary
// (SHADER_BINARY_FORMAT_SPIR_V) are specialized by glSpecializeShader and
// translated to GLSL, which the rest of the compiler takes as it takes an
// application's source.
//
// SPIR-V matches stage interfaces by location and may carry no names; GLSL
// matches them by name. The translation names every inter-stage variable
// after its location, and every resource the module leaves unnamed after its
// location or binding, with kSpirvNamePrefix. Program queries report those
// names, and every name of a module without debug names, as empty
// (spirvDisplayName).
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "core/shader/compiler.h"

namespace wmgl::shader {

inline constexpr const char* kSpirvNamePrefix = "wmgl_spv_";

struct SpirvSpecialization {
    enum Status { kOk, kBadEntryPoint, kBadConstant, kFailed } status = kFailed;
    std::string glsl;    // kOk: the stage as GLSL
    std::string log;     // kFailed: why the module cannot be used
    bool named = false;  // the module carries debug names (OpName, OpMemberName)
    // Uniforms with a location: (location, their name in `glsl`). SPIR-V
    // stages match uniforms by location (ARB_gl_spirv), GLSL by name:
    // spirvUnifyUniforms renames them alike before linking.
    std::vector<std::pair<uint32_t, std::string>> uniformLocations;
};

// Specializes `words` for `stage` at entry point `entry` with `constants`
// (SpecId, 32-bit value).
SpirvSpecialization specializeSpirv(const std::vector<uint32_t>& words, Stage stage, const std::string& entry,
                                    const std::vector<std::pair<uint32_t, uint32_t>>& constants);

// A module's header is SPIR-V (magic number, a version WineMetalGL reads).
bool spirvHeaderValid(const std::vector<uint32_t>& words);
// Every instruction fits the module and every <id> operand is below the
// header's bound (spirv_validate.cpp): SPIRV-Cross indexes its id table with
// them unchecked. `error` says what is wrong.
bool spirvStructureValid(const std::vector<uint32_t>& words, std::string& error);

// The SPIR-V extensions a module may use (GL_SPIR_V_EXTENSIONS).
const std::vector<const char*>& spirvExtensions();

// A stage's GLSL with its inputs named to match the outputs of `previous`,
// the stage before it in the program.
std::string spirvLinkInputs(const std::string& glsl, Stage previous);

// Renames the uniforms of `glsl` whose location `names` (location -> name,
// from the stages before it) already has under another name to that name.
std::string spirvUnifyUniforms(const std::string& glsl, const std::vector<std::pair<uint32_t, std::string>>& uniforms,
                               const std::map<uint32_t, std::string>& names);

// The name a program query reports for `name` (see above).
std::string spirvDisplayName(const LinkResult& link, const std::string& name);

}  // namespace wmgl::shader
