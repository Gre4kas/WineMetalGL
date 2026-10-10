#include "core/shader/compiler.h"

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <SPIRV/GlslangToSpv.h>
#include <glslang/MachineIndependent/localintermediate.h>
#include <spirv_msl.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>

#include "core/shader/glsl_upgrade.h"

namespace wmgl::shader {
namespace {

constexpr const char* kDefaultBlockName = "wmgl_DefaultUniforms";
constexpr int kDefaultBlockBinding = 1000;
constexpr unsigned kNoLocation = 0xFFF;  // glslang's TQualifier::layoutLocationEnd

void initGlslang() {
    static std::once_flag once;
    std::call_once(once, [] { glslang::InitializeProcess(); });
}

EShLanguage language(Stage stage) {
    switch (stage) {
    case Stage::Vertex: return EShLangVertex;
    case Stage::TessControl: return EShLangTessControl;
    case Stage::TessEval: return EShLangTessEvaluation;
    case Stage::Geometry: return EShLangGeometry;
    case Stage::Fragment: return EShLangFragment;
    default: return EShLangCompute;
    }
}

spv::ExecutionModel executionModel(Stage stage) {
    switch (stage) {
    case Stage::Vertex: return spv::ExecutionModelVertex;
    case Stage::TessControl: return spv::ExecutionModelTessellationControl;
    case Stage::TessEval: return spv::ExecutionModelTessellationEvaluation;
    case Stage::Geometry: return spv::ExecutionModelGeometry;
    case Stage::Fragment: return spv::ExecutionModelFragment;
    default: return spv::ExecutionModelGLCompute;
    }
}

const EShMessages kMessages = static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules);

// Configures a shader for codegen: relaxed Vulkan rules, one default block.
void configure(glslang::TShader& shader, EShLanguage lang) {
    shader.setEnvInput(glslang::EShSourceGlsl, lang, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_1);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_3);
    shader.setEnvInputVulkanRulesRelaxed();
    shader.setGlobalUniformBlockName(kDefaultBlockName);
    shader.setGlobalUniformSet(0);
    shader.setGlobalUniformBinding(kDefaultBlockBinding);
    shader.setAutoMapLocations(true);
    shader.setAutoMapBindings(true);
}

// Strips glslang's trailing "N compilation errors" noise but keeps messages.
std::string cleanLog(const char* info, const char* debug) {
    std::string log = info ? info : "";
    if (debug && *debug) log += debug;
    return log;
}

struct ParsedShader {
    std::unique_ptr<glslang::TShader> shader;
    std::string source;  // codegen source (after legacy upgrade)
};

bool parseForCodegen(Stage stage, const std::string& original, ParsedShader& out, std::string& log) {
    const EShLanguage lang = language(stage);
    UpgradeResult upgraded = upgradeLegacyGlsl(stage, original);
    if (!upgraded.ok) {
        log += upgraded.log;
        return false;
    }
    out.source = upgraded.source;
    out.shader = std::make_unique<glslang::TShader>(lang);
    const char* text = out.source.c_str();
    out.shader->setStrings(&text, 1);
    configure(*out.shader, lang);
    if (!out.shader->parse(GetDefaultResources(), 140, ENoProfile, false, false, kMessages)) {
        log += cleanLog(out.shader->getInfoLog(), out.shader->getInfoDebugLog());
        return false;
    }
    return true;
}

// ------------------------------------------------------------ GL types

GLenum glTypeOf(const spirv_cross::SPIRType& type) {
    using BT = spirv_cross::SPIRType::BaseType;
    const uint32_t n = type.vecsize, c = type.columns;
    if (type.basetype == BT::SampledImage || type.basetype == BT::Image || type.basetype == BT::Sampler) {
        const auto& image = type.image;
        const bool array = image.arrayed, ms = image.ms, shadow = image.depth;
        const BT sampled = BT::Float;  // refined below by caller via image.type
        (void)sampled;
        switch (image.dim) {
        case spv::Dim1D: return shadow ? (array ? GL_SAMPLER_1D_ARRAY_SHADOW : GL_SAMPLER_1D_SHADOW) : (array ? GL_SAMPLER_1D_ARRAY : GL_SAMPLER_1D);
        case spv::Dim2D:
            if (ms) return array ? GL_SAMPLER_2D_MULTISAMPLE_ARRAY : GL_SAMPLER_2D_MULTISAMPLE;
            return shadow ? (array ? GL_SAMPLER_2D_ARRAY_SHADOW : GL_SAMPLER_2D_SHADOW) : (array ? GL_SAMPLER_2D_ARRAY : GL_SAMPLER_2D);
        case spv::Dim3D: return GL_SAMPLER_3D;
        case spv::DimCube: return shadow ? (array ? GL_SAMPLER_CUBE_MAP_ARRAY_SHADOW : GL_SAMPLER_CUBE_SHADOW) : (array ? GL_SAMPLER_CUBE_MAP_ARRAY : GL_SAMPLER_CUBE);
        case spv::DimRect: return shadow ? GL_SAMPLER_2D_RECT_SHADOW : GL_SAMPLER_2D_RECT;
        case spv::DimBuffer: return GL_SAMPLER_BUFFER;
        default: return GL_SAMPLER_2D;
        }
    }
    auto pick = [&](GLenum s, GLenum v2, GLenum v3, GLenum v4) { return n == 1 ? s : n == 2 ? v2 : n == 3 ? v3 : v4; };
    switch (type.basetype) {
    case BT::Float:
        if (c == 1) return pick(GL_FLOAT, GL_FLOAT_VEC2, GL_FLOAT_VEC3, GL_FLOAT_VEC4);
        if (c == 2) return n == 2 ? GL_FLOAT_MAT2 : n == 3 ? GL_FLOAT_MAT2x3 : GL_FLOAT_MAT2x4;
        if (c == 3) return n == 2 ? GL_FLOAT_MAT3x2 : n == 3 ? GL_FLOAT_MAT3 : GL_FLOAT_MAT3x4;
        return n == 2 ? GL_FLOAT_MAT4x2 : n == 3 ? GL_FLOAT_MAT4x3 : GL_FLOAT_MAT4;
    case BT::Double:
        if (c == 1) return pick(GL_DOUBLE, GL_DOUBLE_VEC2, GL_DOUBLE_VEC3, GL_DOUBLE_VEC4);
        if (c == 2) return n == 2 ? GL_DOUBLE_MAT2 : n == 3 ? GL_DOUBLE_MAT2x3 : GL_DOUBLE_MAT2x4;
        if (c == 3) return n == 2 ? GL_DOUBLE_MAT3x2 : n == 3 ? GL_DOUBLE_MAT3 : GL_DOUBLE_MAT3x4;
        return n == 2 ? GL_DOUBLE_MAT4x2 : n == 3 ? GL_DOUBLE_MAT4x3 : GL_DOUBLE_MAT4;
    case BT::Int: return pick(GL_INT, GL_INT_VEC2, GL_INT_VEC3, GL_INT_VEC4);
    case BT::UInt: return pick(GL_UNSIGNED_INT, GL_UNSIGNED_INT_VEC2, GL_UNSIGNED_INT_VEC3, GL_UNSIGNED_INT_VEC4);
    case BT::Boolean: return pick(GL_BOOL, GL_BOOL_VEC2, GL_BOOL_VEC3, GL_BOOL_VEC4);
    default: return 0;
    }
}

GLenum samplerTypeWithComponent(GLenum base, spirv_cross::SPIRType::BaseType component) {
    using BT = spirv_cross::SPIRType::BaseType;
    if (component == BT::Int) {
        switch (base) {
        case GL_SAMPLER_1D: return GL_INT_SAMPLER_1D;
        case GL_SAMPLER_2D: return GL_INT_SAMPLER_2D;
        case GL_SAMPLER_3D: return GL_INT_SAMPLER_3D;
        case GL_SAMPLER_CUBE: return GL_INT_SAMPLER_CUBE;
        case GL_SAMPLER_1D_ARRAY: return GL_INT_SAMPLER_1D_ARRAY;
        case GL_SAMPLER_2D_ARRAY: return GL_INT_SAMPLER_2D_ARRAY;
        case GL_SAMPLER_2D_RECT: return GL_INT_SAMPLER_2D_RECT;
        case GL_SAMPLER_BUFFER: return GL_INT_SAMPLER_BUFFER;
        case GL_SAMPLER_2D_MULTISAMPLE: return GL_INT_SAMPLER_2D_MULTISAMPLE;
        case GL_SAMPLER_2D_MULTISAMPLE_ARRAY: return GL_INT_SAMPLER_2D_MULTISAMPLE_ARRAY;
        case GL_SAMPLER_CUBE_MAP_ARRAY: return GL_INT_SAMPLER_CUBE_MAP_ARRAY;
        default: return base;
        }
    }
    if (component == BT::UInt) {
        switch (base) {
        case GL_SAMPLER_1D: return GL_UNSIGNED_INT_SAMPLER_1D;
        case GL_SAMPLER_2D: return GL_UNSIGNED_INT_SAMPLER_2D;
        case GL_SAMPLER_3D: return GL_UNSIGNED_INT_SAMPLER_3D;
        case GL_SAMPLER_CUBE: return GL_UNSIGNED_INT_SAMPLER_CUBE;
        case GL_SAMPLER_1D_ARRAY: return GL_UNSIGNED_INT_SAMPLER_1D_ARRAY;
        case GL_SAMPLER_2D_ARRAY: return GL_UNSIGNED_INT_SAMPLER_2D_ARRAY;
        case GL_SAMPLER_2D_RECT: return GL_UNSIGNED_INT_SAMPLER_2D_RECT;
        case GL_SAMPLER_BUFFER: return GL_UNSIGNED_INT_SAMPLER_BUFFER;
        case GL_SAMPLER_2D_MULTISAMPLE: return GL_UNSIGNED_INT_SAMPLER_2D_MULTISAMPLE;
        case GL_SAMPLER_2D_MULTISAMPLE_ARRAY: return GL_UNSIGNED_INT_SAMPLER_2D_MULTISAMPLE_ARRAY;
        case GL_SAMPLER_CUBE_MAP_ARRAY: return GL_UNSIGNED_INT_SAMPLER_CUBE_MAP_ARRAY;
        default: return base;
        }
    }
    return base;
}

int textureTargetForSampler(GLenum type) {
    // Must match TextureTarget in core/state.h.
    switch (type) {
    case GL_SAMPLER_1D: case GL_SAMPLER_1D_SHADOW: case GL_INT_SAMPLER_1D: case GL_UNSIGNED_INT_SAMPLER_1D: return 0;
    case GL_SAMPLER_2D: case GL_SAMPLER_2D_SHADOW: case GL_INT_SAMPLER_2D: case GL_UNSIGNED_INT_SAMPLER_2D: return 1;
    case GL_SAMPLER_3D: case GL_INT_SAMPLER_3D: case GL_UNSIGNED_INT_SAMPLER_3D: return 2;
    case GL_SAMPLER_1D_ARRAY: case GL_SAMPLER_1D_ARRAY_SHADOW: case GL_INT_SAMPLER_1D_ARRAY: case GL_UNSIGNED_INT_SAMPLER_1D_ARRAY: return 3;
    case GL_SAMPLER_2D_ARRAY: case GL_SAMPLER_2D_ARRAY_SHADOW: case GL_INT_SAMPLER_2D_ARRAY: case GL_UNSIGNED_INT_SAMPLER_2D_ARRAY: return 4;
    case GL_SAMPLER_2D_RECT: case GL_SAMPLER_2D_RECT_SHADOW: case GL_INT_SAMPLER_2D_RECT: case GL_UNSIGNED_INT_SAMPLER_2D_RECT: return 5;
    case GL_SAMPLER_CUBE: case GL_SAMPLER_CUBE_SHADOW: case GL_INT_SAMPLER_CUBE: case GL_UNSIGNED_INT_SAMPLER_CUBE: return 6;
    case GL_SAMPLER_BUFFER: case GL_INT_SAMPLER_BUFFER: case GL_UNSIGNED_INT_SAMPLER_BUFFER: return 7;
    case GL_SAMPLER_2D_MULTISAMPLE: case GL_INT_SAMPLER_2D_MULTISAMPLE: case GL_UNSIGNED_INT_SAMPLER_2D_MULTISAMPLE: return 8;
    case GL_SAMPLER_2D_MULTISAMPLE_ARRAY: case GL_INT_SAMPLER_2D_MULTISAMPLE_ARRAY: case GL_UNSIGNED_INT_SAMPLER_2D_MULTISAMPLE_ARRAY: return 9;
    case GL_SAMPLER_CUBE_MAP_ARRAY: case GL_SAMPLER_CUBE_MAP_ARRAY_SHADOW: case GL_INT_SAMPLER_CUBE_MAP_ARRAY: case GL_UNSIGNED_INT_SAMPLER_CUBE_MAP_ARRAY: return 10;
    default: return -1;
    }
}

bool isShadowSampler(GLenum type) {
    switch (type) {
    case GL_SAMPLER_1D_SHADOW: case GL_SAMPLER_2D_SHADOW: case GL_SAMPLER_1D_ARRAY_SHADOW: case GL_SAMPLER_2D_ARRAY_SHADOW:
    case GL_SAMPLER_CUBE_SHADOW: case GL_SAMPLER_2D_RECT_SHADOW: case GL_SAMPLER_CUBE_MAP_ARRAY_SHADOW: return true;
    default: return false;
    }
}

// Name with every array subscript removed ("B[1].s[0].x" -> "B.s.x"), used to
// match glslang's reflection names against GL interface names.
std::string arrayBaseName(const std::string& name) {
    std::string out;
    for (size_t i = 0; i < name.size(); ++i) {
        if (name[i] == '[') {
            while (i < name.size() && name[i] != ']') ++i;
            continue;
        }
        out += name[i];
    }
    return out;
}

GLenum boolType(const glslang::TType& type) {
    switch (type.getVectorSize()) {
    case 2: return GL_BOOL_VEC2;
    case 3: return GL_BOOL_VEC3;
    case 4: return GL_BOOL_VEC4;
    default: return GL_BOOL;
    }
}

// Records the GL bool type of every bool member under `prefix` (names without
// array subscripts, see arrayBaseName). With `self`, `type` itself is named `prefix`.
void collectBoolMembers(const glslang::TType& type, const std::string& prefix, std::map<std::string, GLenum>& out,
                        bool self = false) {
    if (self) {
        if (type.getBasicType() == glslang::EbtBool) out[prefix] = boolType(type);
        else if (type.isStruct()) collectBoolMembers(type, prefix + ".", out);
        return;
    }
    if (!type.isStruct()) return;
    for (const glslang::TTypeLoc& member : *type.getStruct()) {
        const glslang::TType& m = *member.type;
        const std::string name = prefix + m.getFieldName().c_str();
        if (m.isStruct()) collectBoolMembers(m, name + ".", out);
        else if (m.getBasicType() == glslang::EbtBool) out[name] = boolType(m);
    }
}

std::string topLevelName(const std::string& name) {
    const size_t end = name.find_first_of(".[");
    return end == std::string::npos ? name : name.substr(0, end);
}

// Expands a block member (recursively) into GL uniform entries.
void expandMember(const spirv_cross::Compiler& compiler, const spirv_cross::SPIRType& parent, uint32_t index,
                  const std::string& prefix, uint32_t baseOffset, int blockIndex, std::vector<UniformInfo>& out) {
    const uint32_t typeId = parent.member_types[index];
    const spirv_cross::SPIRType& type = compiler.get_type(typeId);
    std::string name = prefix + compiler.get_member_name(parent.self, index);
    const uint32_t offset = baseOffset + compiler.type_struct_member_offset(parent, index);
    const bool isArray = !type.array.empty();
    const uint32_t arraySize = isArray ? type.array.back() : 1;
    uint32_t arrayStride = isArray ? compiler.type_struct_member_array_stride(parent, index) : 0;
    if (type.basetype == spirv_cross::SPIRType::Struct) {
        // GL lists each element of an array of structs separately.
        const spirv_cross::SPIRType& structType = compiler.get_type(type.self);
        for (uint32_t element = 0; element < (isArray ? std::max(1u, arraySize) : 1u); ++element) {
            std::string elementName = isArray ? name + "[" + std::to_string(element) + "]" : name;
            for (uint32_t m = 0; m < structType.member_types.size(); ++m)
                expandMember(compiler, structType, m, elementName + ".", offset + element * arrayStride, blockIndex, out);
        }
        return;
    }
    if (isArray && arrayStride == 0) {
        // Some arrays come without an ArrayStride value; blocks use std140 rules,
        // where every array element starts on a 16-byte boundary (a matrix
        // element spans one 16-byte vector per column, or per row if row-major).
        const bool rowMajor = compiler.has_member_decoration(parent.self, index, spv::DecorationRowMajor);
        arrayStride = 16u * (type.columns > 1 ? (rowMajor ? type.vecsize : type.columns) : 1u);
    }
    UniformInfo info;
    info.name = isArray ? name + "[0]" : name;
    info.type = glTypeOf(type);
    info.size = static_cast<GLint>(std::max(1u, arraySize));
    info.blockIndex = blockIndex;
    info.offset = static_cast<GLint>(offset);
    info.arrayStride = static_cast<GLint>(arrayStride);
    if (type.columns > 1) {
        info.matrixStride = static_cast<GLint>(compiler.type_struct_member_matrix_stride(parent, index));
        info.rowMajor = compiler.has_member_decoration(parent.self, index, spv::DecorationRowMajor);
    }
    out.push_back(info);
}

}  // namespace

// ------------------------------------------------------------- helpers

int glTypeComponents(GLenum type) {
    switch (type) {
    case GL_FLOAT: case GL_INT: case GL_UNSIGNED_INT: case GL_BOOL: case GL_DOUBLE: return 1;
    case GL_FLOAT_VEC2: case GL_INT_VEC2: case GL_UNSIGNED_INT_VEC2: case GL_BOOL_VEC2: case GL_DOUBLE_VEC2: return 2;
    case GL_FLOAT_VEC3: case GL_INT_VEC3: case GL_UNSIGNED_INT_VEC3: case GL_BOOL_VEC3: case GL_DOUBLE_VEC3: return 3;
    case GL_FLOAT_VEC4: case GL_INT_VEC4: case GL_UNSIGNED_INT_VEC4: case GL_BOOL_VEC4: case GL_DOUBLE_VEC4: return 4;
    case GL_FLOAT_MAT2: case GL_DOUBLE_MAT2: return 4;
    case GL_FLOAT_MAT3: case GL_DOUBLE_MAT3: return 9;
    case GL_FLOAT_MAT4: case GL_DOUBLE_MAT4: return 16;
    case GL_FLOAT_MAT2x3: case GL_FLOAT_MAT3x2: case GL_DOUBLE_MAT2x3: case GL_DOUBLE_MAT3x2: return 6;
    case GL_FLOAT_MAT2x4: case GL_FLOAT_MAT4x2: case GL_DOUBLE_MAT2x4: case GL_DOUBLE_MAT4x2: return 8;
    case GL_FLOAT_MAT3x4: case GL_FLOAT_MAT4x3: case GL_DOUBLE_MAT3x4: case GL_DOUBLE_MAT4x3: return 12;
    default: return glTypeIsSampler(type) ? 1 : 0;
    }
}

int glTypeLocations(GLenum type) {
    switch (type) {
    case GL_FLOAT_MAT2: case GL_FLOAT_MAT2x3: case GL_FLOAT_MAT2x4: return 2;
    case GL_FLOAT_MAT3: case GL_FLOAT_MAT3x2: case GL_FLOAT_MAT3x4: return 3;
    case GL_FLOAT_MAT4: case GL_FLOAT_MAT4x2: case GL_FLOAT_MAT4x3: return 4;
    default: return 1;
    }
}

bool glTypeIsSampler(GLenum type) { return textureTargetForSampler(type) >= 0; }

const char* glTypeName(GLenum type) {
    switch (type) {
    case GL_FLOAT: return "float";
    case GL_FLOAT_VEC2: return "vec2";
    case GL_FLOAT_VEC3: return "vec3";
    case GL_FLOAT_VEC4: return "vec4";
    case GL_INT: return "int";
    case GL_UNSIGNED_INT: return "uint";
    case GL_BOOL: return "bool";
    case GL_FLOAT_MAT4: return "mat4";
    default: return "?";
    }
}

// ------------------------------------------------------------- compile

CompileResult compile(Stage stage, const std::string& source) {
    initGlslang();
    CompileResult result;
    // Status and log come from a plain GL parse, which accepts every GLSL
    // version and profile with GL's own rules.
    const EShLanguage lang = language(stage);
    glslang::TShader shader(lang);
    const char* text = source.c_str();
    shader.setStrings(&text, 1);
    shader.setEnvInput(glslang::EShSourceGlsl, lang, glslang::EShClientNone, 0);
    shader.setEnvClient(glslang::EShClientNone, glslang::EShTargetClientVersion(0));
    shader.setEnvTarget(glslang::EShTargetNone, glslang::EShTargetLanguageVersion(0));
    result.ok = shader.parse(GetDefaultResources(), 110, ENoProfile, false, false, EShMsgDefault);
    result.log = cleanLog(shader.getInfoLog(), shader.getInfoDebugLog());
    if (result.ok) {
        // Make sure the shader also translates; report problems here rather
        // than as a mysterious link failure.
        ParsedShader parsed;
        std::string log;
        if (!parseForCodegen(stage, source, parsed, log)) {
            result.ok = false;
            result.log += log;
        }
    }
    return result;
}

// ---------------------------------------------------------------- link

LinkResult link(const LinkInput& input) {
    initGlslang();
    LinkResult result;
    std::vector<ParsedShader> parsed(input.shaders.size());
    bool stagePresent[kStageCount] = {};
    for (size_t i = 0; i < input.shaders.size(); ++i) {
        if (!parseForCodegen(input.shaders[i].stage, input.shaders[i].source, parsed[i], result.log)) return result;
        stagePresent[static_cast<int>(input.shaders[i].stage)] = true;
    }
    if (!stagePresent[static_cast<int>(Stage::Compute)] && !input.separable) {
        if (!stagePresent[static_cast<int>(Stage::Vertex)] && !stagePresent[static_cast<int>(Stage::Fragment)] &&
            !stagePresent[static_cast<int>(Stage::Geometry)]) {
            result.log += "error: no shaders attached\n";
            return result;
        }
        if (stagePresent[static_cast<int>(Stage::Geometry)] && !stagePresent[static_cast<int>(Stage::Vertex)]) {
            result.log += "error: a geometry shader requires a vertex shader\n";
            return result;
        }
    }

    glslang::TProgram program;
    for (auto& p : parsed) program.addShader(p.shader.get());
    if (!program.link(kMessages)) {
        result.log += cleanLog(program.getInfoLog(), program.getInfoDebugLog());
        return result;
    }
    // Reflect before mapIO: afterwards every input/output has a location and
    // explicit layout(location) can no longer be told apart from assigned ones.
    program.buildReflection(EShReflectionDefault | EShReflectionSeparateBuffers);

    // Live top-level names and blocks, from glslang's liveness-aware reflection.
    std::set<std::string> liveDefault;
    std::set<std::string> liveBlocks;
    int defaultBlockReflectionIndex = -1;
    for (int i = 0; i < program.getNumUniformBlocks(); ++i) {
        const auto& block = program.getUniformBlock(i);
        if (block.name == kDefaultBlockName) defaultBlockReflectionIndex = i;
        else liveBlocks.insert(topLevelName(block.name));
    }
    // SPIR-V stores bools in blocks as uint; the AST keeps the GL types.
    std::map<std::string, GLenum> boolTypes;
    // Cross-stage uniform interface rules (GLSL 3.30 section 4.3.7): a block
    // is anonymous in every stage or in none, and names that anonymous blocks
    // put in the global scope must not clash with other blocks' members or
    // with default-block uniforms.
    std::map<std::string, bool> blockAnonymous;             // block name -> declared without instance name
    std::map<std::string, std::string> globalUniformOwner;  // global name -> block name ("" = default block)
    std::string interfaceError;
    auto claimGlobal = [&](const std::string& name, const std::string& owner) {
        auto [it, inserted] = globalUniformOwner.emplace(name, owner);
        if (!inserted && it->second != owner && interfaceError.empty())
            interfaceError = "error: uniform '" + name + "' is declared by both " +
                             (it->second.empty() ? std::string("the default uniform block") : "block " + it->second) + " and " +
                             (owner.empty() ? std::string("the default uniform block") : "block " + owner) + "\n";
    };
    // GL treats uniform blocks declared shared or std140 as active even when
    // nothing references them (only packed blocks may be eliminated). The
    // packing qualifier is gone in SPIR-V, so read it from the linker objects.
    for (int s = 0; s < kStageCount; ++s) {
        if (!stagePresent[s]) continue;
        glslang::TIntermediate* intermediate = program.getIntermediate(language(static_cast<Stage>(s)));
        glslang::TIntermAggregate* root = intermediate && intermediate->getTreeRoot()
                                              ? intermediate->getTreeRoot()->getAsAggregate()
                                              : nullptr;
        if (!root) continue;
        for (TIntermNode* node : root->getSequence()) {
            glslang::TIntermAggregate* objects = node->getAsAggregate();
            if (!objects || objects->getOp() != glslang::EOpLinkerObjects) continue;
            for (TIntermNode* object : objects->getSequence()) {
                glslang::TIntermSymbol* symbol = object->getAsSymbolNode();
                if (!symbol) continue;
                const glslang::TType& type = symbol->getType();
                const glslang::TQualifier& q = type.getQualifier();
                if (q.storage == glslang::EvqUniform) {
                    if (type.getBasicType() == glslang::EbtBlock) {
                        const bool anonymous = glslang::IsAnonymous(symbol->getName());
                        const std::string blockName = type.getTypeName().c_str();
                        collectBoolMembers(type, anonymous ? "" : blockName + ".", boolTypes);
                        if (blockName != kDefaultBlockName) {
                            auto [it, inserted] = blockAnonymous.emplace(blockName, anonymous);
                            if (!inserted && it->second != anonymous && interfaceError.empty())
                                interfaceError = "error: uniform block '" + blockName +
                                                 "' has an instance name in one stage but not in another\n";
                            if (anonymous)
                                for (const glslang::TTypeLoc& member : *type.getStruct())
                                    claimGlobal(member.type->getFieldName().c_str(), blockName);
                        } else {
                            // Loose uniforms gathered into the default block.
                            for (const glslang::TTypeLoc& member : *type.getStruct())
                                claimGlobal(member.type->getFieldName().c_str(), "");
                        }
                    } else {
                        collectBoolMembers(type, symbol->getName().c_str(), boolTypes, true);
                        claimGlobal(symbol->getName().c_str(), "");
                    }
                }
                if (type.getBasicType() == glslang::EbtBlock && q.storage == glslang::EvqUniform &&
                    (q.layoutPacking == glslang::ElpShared || q.layoutPacking == glslang::ElpStd140) &&
                    type.getTypeName() != kDefaultBlockName)
                    liveBlocks.insert(type.getTypeName().c_str());
            }
        }
    }
    if (!interfaceError.empty()) {
        result.log += interfaceError;
        return result;
    }
    std::set<std::string> liveSamplers;
    for (int i = 0; i < program.getNumUniformVariables(); ++i) {
        const auto& u = program.getUniform(i);
        if (u.index == defaultBlockReflectionIndex && defaultBlockReflectionIndex >= 0) liveDefault.insert(topLevelName(u.name));
        else if (u.index < 0) liveSamplers.insert(topLevelName(u.name));
    }

    // Collect attributes/outputs from this pre-mapIO reflection (see below),
    // then assign locations for codegen.
    struct PipeVar { std::string name; GLenum type; int size; unsigned location; uint32_t stages; };
    std::vector<PipeVar> pipeInputs, pipeOutputs;
    for (int i = 0; i < program.getNumPipeInputs(); ++i) {
        const auto& v = program.getPipeInput(i);
        pipeInputs.push_back({v.name, static_cast<GLenum>(v.glDefineType), v.size, v.layoutLocation(), static_cast<uint32_t>(v.stages)});
    }
    for (int i = 0; i < program.getNumPipeOutputs(); ++i) {
        const auto& v = program.getPipeOutput(i);
        pipeOutputs.push_back({v.name, static_cast<GLenum>(v.glDefineType), v.size, v.layoutLocation(), static_cast<uint32_t>(v.stages)});
    }
    if (!program.mapIO()) {
        result.log += cleanLog(program.getInfoLog(), program.getInfoDebugLog());
        return result;
    }

    // SPIR-V per stage.
    for (int s = 0; s < kStageCount; ++s) {
        if (!stagePresent[s]) continue;
        glslang::TIntermediate* intermediate = program.getIntermediate(language(static_cast<Stage>(s)));
        if (!intermediate) continue;
        glslang::SpvOptions options;
        options.disableOptimizer = true;
        options.validate = false;
        glslang::GlslangToSpv(*intermediate, result.stages[s].spirv, &options);
        result.stages[s].present = true;
    }

    // Program-wide interface, built from the first stage that declares each resource.
    // Default uniform block members.
    std::map<std::string, int> blockIndexByName;
    std::map<std::string, int> samplerUniformByName;
    for (int s = 0; s < kStageCount; ++s) {
        if (!result.stages[s].present) continue;
        spirv_cross::Compiler compiler(result.stages[s].spirv);
        const spirv_cross::ShaderResources resources = compiler.get_shader_resources();
        for (const auto& ubo : resources.uniform_buffers) {
            const spirv_cross::SPIRType& type = compiler.get_type(ubo.base_type_id);
            const std::string blockName = compiler.get_name(ubo.base_type_id);
            const uint32_t binding = compiler.get_decoration(ubo.id, spv::DecorationBinding);
            if (binding == kDefaultBlockBinding || blockName == kDefaultBlockName) {
                result.defaultBlockSize = std::max<uint32_t>(result.defaultBlockSize,
                                                             static_cast<uint32_t>(compiler.get_declared_struct_size(type)));
                std::vector<UniformInfo> members;
                for (uint32_t m = 0; m < type.member_types.size(); ++m)
                    expandMember(compiler, type, m, "", 0, -1, members);
                for (UniformInfo& u : members) {
                    if (!liveDefault.count(topLevelName(u.name))) continue;
                    if (auto b = boolTypes.find(arrayBaseName(u.name)); b != boolTypes.end()) u.type = b->second;
                    auto it = std::find_if(result.uniforms.begin(), result.uniforms.end(),
                                           [&](const UniformInfo& e) { return e.name == u.name && e.blockIndex < 0; });
                    if (it == result.uniforms.end()) {
                        u.stageMask = 1u << s;
                        result.uniforms.push_back(u);
                    } else {
                        it->stageMask |= 1u << s;
                    }
                }
                continue;
            }
            if (!liveBlocks.count(blockName)) continue;
            // The variable's type carries the instance-array dimension, not the block struct.
            const spirv_cross::SPIRType& varType = compiler.get_type(ubo.type_id);
            const uint32_t count = varType.array.empty() ? 1 : std::max(1u, varType.array.back());
            for (uint32_t element = 0; element < count; ++element) {
                const std::string name = varType.array.empty() ? blockName : blockName + "[" + std::to_string(element) + "]";
                auto it = blockIndexByName.find(name);
                int index;
                if (it == blockIndexByName.end()) {
                    index = static_cast<int>(result.blocks.size());
                    blockIndexByName[name] = index;
                    UniformBlockInfo block;
                    block.name = name;
                    block.dataSize = static_cast<GLint>(compiler.get_declared_struct_size(compiler.get_type(ubo.base_type_id)));
                    result.blocks.push_back(block);
                    std::vector<UniformInfo> members;
                    const spirv_cross::SPIRType& structType = compiler.get_type(ubo.base_type_id);
                    // Members of a block without an instance name are named without the block prefix.
                    const std::string prefix = compiler.get_name(ubo.id).empty() ? "" : blockName + ".";
                    for (uint32_t m = 0; m < structType.member_types.size(); ++m)
                        expandMember(compiler, structType, m, prefix, 0, index, members);
                    for (UniformInfo& u : members) {
                        if (auto b = boolTypes.find(arrayBaseName(u.name)); b != boolTypes.end()) u.type = b->second;
                        result.blocks[index].activeUniforms.push_back(static_cast<int>(result.uniforms.size()));
                        result.uniforms.push_back(u);
                    }
                } else {
                    index = it->second;
                }
                result.blocks[index].stageMask |= 1u << s;
                for (int u : result.blocks[index].activeUniforms) result.uniforms[u].stageMask |= 1u << s;
            }
        }
        for (const auto& image : resources.sampled_images) {
            const spirv_cross::SPIRType& type = compiler.get_type(image.type_id);
            const spirv_cross::SPIRType& base = compiler.get_type(image.base_type_id);
            const std::string name = compiler.get_name(image.id);
            if (!liveSamplers.count(name)) continue;
            const spirv_cross::SPIRType& sampled = compiler.get_type(base.image.type);
            const GLenum glType = samplerTypeWithComponent(glTypeOf(base), sampled.basetype);
            const uint32_t count = type.array.empty() ? 1 : std::max(1u, type.array.back());
            auto it = samplerUniformByName.find(name);
            if (it == samplerUniformByName.end()) {
                UniformInfo u;
                u.name = type.array.empty() ? name : name + "[0]";
                u.type = glType;
                u.size = static_cast<GLint>(count);
                u.samplerSlot = static_cast<int>(result.samplers.size());
                u.stageMask = 1u << s;
                for (uint32_t e = 0; e < count; ++e) {
                    SamplerSlotInfo slot;
                    slot.type = glType;
                    slot.textureTarget = textureTargetForSampler(glType);
                    slot.shadow = isShadowSampler(glType);
                    result.samplers.push_back(slot);
                }
                samplerUniformByName[name] = static_cast<int>(result.uniforms.size());
                result.uniforms.push_back(u);
            } else {
                result.uniforms[it->second].stageMask |= 1u << s;
            }
        }
    }

    // Assign default-block and sampler uniform locations (consecutive per element).
    GLint nextLocation = 0;
    for (size_t i = 0; i < result.uniforms.size(); ++i) {
        UniformInfo& u = result.uniforms[i];
        if (u.blockIndex >= 0) continue;
        u.location = nextLocation;
        for (GLint e = 0; e < u.size; ++e) result.locations.emplace_back(static_cast<int>(i), e);
        nextLocation += u.size;
    }

    // Vertex attributes (from reflection: active inputs only).
    std::vector<bool> usedLocations(64, false);
    for (const PipeVar& in : pipeInputs) {
        if (in.name.rfind("gl_", 0) == 0) continue;
        if (!(in.stages & EShLangVertexMask)) continue;
        AttribInfo a;
        a.name = in.name;
        a.type = in.type;
        a.size = std::max(1, in.size);
        a.locationCount = glTypeLocations(a.type) * a.size;
        a.location = in.location != kNoLocation ? static_cast<GLint>(in.location) : -1;
        result.attribs.push_back(a);
    }
    // Explicit locations from source were already applied; mark them used.
    // Then glBindAttribLocation, then the lowest free locations.
    std::vector<int> needsLocation;
    for (size_t i = 0; i < result.attribs.size(); ++i) {
        AttribInfo& a = result.attribs[i];
        if (a.location >= 0 && a.location < 64 && input.attribBindings.find(a.name) == input.attribBindings.end()) {
            for (int l = 0; l < a.locationCount && a.location + l < 64; ++l) usedLocations[a.location + l] = true;
            continue;
        }
        auto bound = input.attribBindings.find(a.name);
        if (bound != input.attribBindings.end() && a.location < 0) {
            a.location = static_cast<GLint>(bound->second);
            for (int l = 0; l < a.locationCount && a.location + l < 64; ++l) usedLocations[a.location + l] = true;
            continue;
        }
        if (a.location >= 0) continue;
        needsLocation.push_back(static_cast<int>(i));
    }
    for (int i : needsLocation) {
        AttribInfo& a = result.attribs[i];
        int location = 0;
        for (;; ++location) {
            bool free = true;
            for (int l = 0; l < a.locationCount; ++l)
                if (location + l >= 64 || usedLocations[location + l]) free = false;
            if (free || location >= 64) break;
        }
        a.location = location;
        for (int l = 0; l < a.locationCount && location + l < 64; ++l) usedLocations[location + l] = true;
    }
    for (const AttribInfo& a : result.attribs) {
        if (a.location + a.locationCount > 16) {
            result.log += "error: vertex attribute '" + a.name + "' uses a location beyond GL_MAX_VERTEX_ATTRIBS\n";
            return result;
        }
    }

    // Fragment outputs.
    for (const PipeVar& out : pipeOutputs) {
        if (out.name.rfind("gl_", 0) == 0) continue;
        if (!(out.stages & EShLangFragmentMask)) continue;
        FragOutputInfo f;
        f.name = out.name;
        f.type = out.type;
        f.size = std::max(1, out.size);
        const unsigned explicitLocation = out.location;
        auto bound = input.fragDataBindings.find(f.name);
        if (explicitLocation != kNoLocation) {
            f.location = static_cast<GLint>(explicitLocation);
        } else if (bound != input.fragDataBindings.end()) {
            f.location = static_cast<GLint>(bound->second.first);
            f.index = static_cast<GLint>(bound->second.second);
        } else {
            f.location = -1;
        }
        result.fragOutputs.push_back(f);
    }
    {
        std::vector<bool> used(16, false);
        for (const FragOutputInfo& f : result.fragOutputs)
            if (f.location >= 0)
                for (int e = 0; e < f.size && f.location + e < 16; ++e) used[f.location + e] = true;
        for (FragOutputInfo& f : result.fragOutputs) {
            if (f.location >= 0) continue;
            int location = 0;
            while (location < 16 && used[location]) ++location;
            f.location = location;
            for (int e = 0; e < f.size && location + e < 16; ++e) used[location + e] = true;
        }
    }

    // Metal translation per stage.
    for (int s = 0; s < kStageCount; ++s) {
        StageOutput& stage = result.stages[s];
        if (!stage.present) continue;
        try {
            spirv_cross::CompilerMSL msl(stage.spirv);
            spirv_cross::CompilerMSL::Options mslOptions;
            mslOptions.platform = spirv_cross::CompilerMSL::Options::macOS;
            mslOptions.set_msl_version(3, 0);
            mslOptions.enable_decoration_binding = false;
            mslOptions.pad_fragment_output_components = true;
            mslOptions.force_native_arrays = false;
            mslOptions.enable_base_index_zero = false;
            mslOptions.texture_buffer_native = true;   // samplerBuffer -> texture_buffer
            mslOptions.texture_1D_as_2D = true;        // GL 1D textures are stored as 2D (mipmaps, rendering)
            msl.set_msl_options(mslOptions);
            spirv_cross::CompilerGLSL::Options common = msl.get_common_options();
            common.vertex.flip_vert_y = true;        // GL row 0 = Metal row 0 (see docs/design/core.md)
            common.vertex.fixup_clipspace = true;    // z: [-w, w] -> [0, w]
            msl.set_common_options(common);
            const spv::ExecutionModel model = executionModel(static_cast<Stage>(s));
            const spirv_cross::ShaderResources resources = msl.get_shader_resources();

            // Uniform buffers.
            uint32_t nextBlockSlot = kSlotFirstUniformBlock;
            // glslang can give an unreferenced resource the same binding as a
            // referenced one, and SPIRV-Cross maps slots by (set, binding), so
            // every resource gets a unique binding before slots are assigned.
            uint32_t uniqueBinding = 0;
            for (const auto& ubo : resources.uniform_buffers) {
                const uint32_t binding = msl.get_decoration(ubo.id, spv::DecorationBinding);
                const std::string blockName = msl.get_name(ubo.base_type_id);
                const bool defaultBlock = binding == kDefaultBlockBinding || blockName == kDefaultBlockName;
                if (!defaultBlock) msl.set_decoration(ubo.id, spv::DecorationBinding, uniqueBinding++);
                spirv_cross::MSLResourceBinding b;
                b.stage = model;
                b.desc_set = msl.get_decoration(ubo.id, spv::DecorationDescriptorSet);
                b.binding = msl.get_decoration(ubo.id, spv::DecorationBinding);
                const spirv_cross::SPIRType& type = msl.get_type(ubo.type_id);
                b.count = type.array.empty() ? 1 : std::max(1u, type.array.back());
                if (defaultBlock) {
                    b.msl_buffer = kSlotDefaultUniforms;
                    stage.usesDefaultUniforms = true;
                } else {
                    b.msl_buffer = nextBlockSlot;
                    for (uint32_t e = 0; e < b.count; ++e) {
                        const std::string name = type.array.empty() ? blockName : blockName + "[" + std::to_string(e) + "]";
                        auto it = blockIndexByName.find(name);
                        if (it != blockIndexByName.end()) result.blocks[it->second].stageSlot[s] = static_cast<int>(nextBlockSlot + e);
                    }
                    nextBlockSlot += b.count;
                    if (nextBlockSlot > kSlotFirstUniformBlock + kMaxStageUniformBlocks) {
                        result.log += "error: too many uniform blocks in one stage\n";
                        return result;
                    }
                }
                msl.add_msl_resource_binding(b);
            }
            // Samplers.
            uint32_t nextTexture = 0;
            for (const auto& image : resources.sampled_images) {
                const std::string name = msl.get_name(image.id);
                const spirv_cross::SPIRType& type = msl.get_type(image.type_id);
                const uint32_t count = type.array.empty() ? 1 : std::max(1u, type.array.back());
                msl.set_decoration(image.id, spv::DecorationBinding, uniqueBinding++);
                spirv_cross::MSLResourceBinding b;
                b.stage = model;
                b.desc_set = msl.get_decoration(image.id, spv::DecorationDescriptorSet);
                b.binding = msl.get_decoration(image.id, spv::DecorationBinding);
                b.count = count;
                b.msl_texture = nextTexture;
                b.msl_sampler = nextTexture;
                msl.add_msl_resource_binding(b);
                auto it = samplerUniformByName.find(name);
                if (it != samplerUniformByName.end()) {
                    const int first = result.uniforms[it->second].samplerSlot;
                    for (uint32_t e = 0; e < count; ++e) result.samplers[first + e].stageTexture[s] = static_cast<int>(nextTexture + e);
                }
                nextTexture += count;
                if (nextTexture > kMaxStageSamplers) {
                    result.log += "error: too many samplers in one stage\n";
                    return result;
                }
            }
            // Vertex inputs: GL locations.
            if (s == static_cast<int>(Stage::Vertex)) {
                for (const auto& in : resources.stage_inputs) {
                    const std::string name = msl.get_name(in.id);
                    auto it = std::find_if(result.attribs.begin(), result.attribs.end(),
                                           [&](const AttribInfo& a) { return a.name == name; });
                    const spirv_cross::SPIRType& type = msl.get_type(in.type_id);
                    if (it == result.attribs.end()) continue;
                    msl.set_decoration(in.id, spv::DecorationLocation, static_cast<uint32_t>(it->location));
                    using BT = spirv_cross::SPIRType::BaseType;
                    const char kind = type.basetype == BT::Int ? 'i' : type.basetype == BT::UInt ? 'u' : 'f';
                    for (int l = 0; l < it->locationCount; ++l)
                        stage.vertexInputs.push_back({it->location + l, static_cast<int>(type.vecsize), kind});
                }
            }
            if (s == static_cast<int>(Stage::Fragment)) {
                for (const auto& out : resources.stage_outputs) {
                    const std::string name = msl.get_name(out.id);
                    auto it = std::find_if(result.fragOutputs.begin(), result.fragOutputs.end(),
                                           [&](const FragOutputInfo& f) { return f.name == name; });
                    if (it == result.fragOutputs.end()) continue;
                    msl.set_decoration(out.id, spv::DecorationLocation, static_cast<uint32_t>(it->location));
                    if (it->index) msl.set_decoration(out.id, spv::DecorationIndex, static_cast<uint32_t>(it->index));
                    const spirv_cross::SPIRType& type = msl.get_type(out.type_id);
                    using BT = spirv_cross::SPIRType::BaseType;
                    const char kind = type.basetype == BT::Int ? 'i' : type.basetype == BT::UInt ? 'u' : 'f';
                    for (int e = 0; e < it->size; ++e) stage.fragOutputs.push_back({it->location + e, it->index, kind});
                }
            }
            stage.msl = msl.compile();
            // WINEMETALGL_DUMP_SHADERS=1 prints every generated MSL stage (debugging aid).
            static const bool dump = [] {
                const char* v = std::getenv("WINEMETALGL_DUMP_SHADERS");
                return v && v[0] == '1';
            }();
            if (dump) std::fprintf(stderr, "winemetalgl: MSL stage %d:\n%s\n", s, stage.msl.c_str());
            stage.entryPoint = msl.get_cleansed_entry_point_name("main", model);
            stage.writesPointSize = stage.msl.find("[[point_size]]") != std::string::npos;
        } catch (const std::exception& error) {
            result.log += std::string("error: Metal translation failed: ") + error.what() + "\n";
            return result;
        }
    }

    result.ok = true;
    return result;
}

}  // namespace wmgl::shader
