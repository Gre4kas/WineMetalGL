// Rewrites GLSL that SPIR-V codegen cannot take directly (GLSL 1.10-1.30, no
// #version, or the compatibility profile) into equivalent GLSL 1.40+.
//
// The rewrite works on tokens: comments and preprocessor directives are kept
// (so line numbers in logs stay right) and only identifiers/keywords change:
// attribute/varying become in/out, gl_FragColor/gl_FragData become declared
// outputs, and fixed-function built-ins become WineMetalGL-provided inputs
// and uniforms (fed by the compatibility profile, W22).
#pragma once

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
};

UpgradeResult upgradeLegacyGlsl(Stage stage, const std::string& source);

// Codegen parses with Vulkan GLSL rules, which reserve type names that are
// ordinary identifiers in desktop GLSL (sampler, texture2D, subpassInput...).
// escapeVulkanKeywords renames such identifiers to kEscapePrefix + name;
// unescapeName undoes it for every name the program interface reports.
inline constexpr const char* kEscapePrefix = "wmglK_";
std::string escapeVulkanKeywords(const std::string& source);
std::string unescapeName(std::string name);

}  // namespace wmgl::shader
