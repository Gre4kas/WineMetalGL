// Rewrites GLSL that SPIR-V codegen cannot take directly (GLSL 1.10-1.30, no
// #version, or the compatibility profile) into equivalent GLSL 1.40+.
//
// The rewrite works on tokens: comments and preprocessor directives are kept
// (so line numbers in logs stay right) and only identifiers/keywords change:
// attribute/varying become in/out, gl_FragColor/gl_FragData become declared
// outputs, and fixed-function built-ins become WineMetalGL-provided inputs
// and uniforms (fed by the compatibility profile, W22).
#pragma once

#include <set>
#include <string>

#include "core/shader/compiler.h"

namespace wmgl::shader {

struct GlslVersion {
    int number = 110;            // 110 when there is no #version
    bool compatibility = false;  // "compatibility" profile (or implicit for <= 130)
    bool es = false;
};

GlslVersion parseGlslVersion(const std::string& source);

struct UpgradeResult {
    bool ok = true;
    std::string source;
    std::string log;
    bool usesFixedFunctionState = false;  // reads gl_ModelViewMatrix etc.
    // Fragment stage: the built-in varyings it reads ("FrontColor",
    // "FrontSecondaryColor", "TexCoord", "FogFragCoord"; gl_Color reads the
    // vertex stage's gl_FrontColor).
    std::set<std::string> builtinVaryingsRead;
};

struct UpgradeOptions {
    // Vertex stage: built-in varyings the next stage reads. The ones the
    // shader does not write are declared (zero) so the interfaces match.
    std::set<std::string> provideVaryings;
};

// The legacy shader runs as wmgl_user_main inside a generated main, which
// adds the compatibility profile's per-stage work:
//  - vertex: zero-initialised built-in varyings (UpgradeOptions), and the
//    user clip planes applied to gl_ClipVertex (gl_ClipDistance[0..7]);
//  - fragment: the alpha test (GL_ALPHA_TEST) on gl_FragColor/gl_FragData[0].
// Both read the wmgl_FixedFunction block (core/compat/fixed_function.h).
UpgradeResult upgradeLegacyGlsl(Stage stage, const std::string& source, const UpgradeOptions& options = {});

// Members of wmgl_FixedFunction beyond the GLSL built-ins, read by the
// generated code: wmgl_AlphaTest = (function code, reference, 0, 0), function
// code 0 = test disabled (see compat/fixed_function.h).
inline constexpr const char* kFixedFunctionBlockName = "wmgl_FixedFunction";

// Codegen parses with Vulkan GLSL rules, which reserve type names that are
// ordinary identifiers in desktop GLSL (sampler, texture2D, subpassInput...).
// escapeVulkanKeywords renames such identifiers to kEscapePrefix + name;
// unescapeName undoes it for every name the program interface reports.
inline constexpr const char* kEscapePrefix = "wmglK_";
std::string escapeVulkanKeywords(const std::string& source);
std::string unescapeName(std::string name);

// GLSL 1.30-3.30 keywords reserved for future use may not be used as
// identifiers (they remain valid as layout qualifiers). Returns compile
// errors in glslang's log format, or an empty string.
std::string checkReservedWords(const std::string& source);

// Built-in uniforms (gl_DepthRange, and gl_NumSamples in fragment shaders)
// exist only in GL-rules parses; the code generator's SPIR-V rules lack
// them. They become hidden default-block uniforms, kBuiltinUniformPrefix +
// the name without "gl_", that the draw path fills (applyBuiltinUniforms).
inline constexpr const char* kBuiltinUniformPrefix = "wmgl_bi_";
std::string lowerBuiltinUniforms(Stage stage, const std::string& source);

}  // namespace wmgl::shader
