#include "core/shader/compiler.h"

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <SPIRV/GlslangToSpv.h>
#include <glslang/MachineIndependent/localintermediate.h>
#include <spirv_msl.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>

#include "core/shader/fp64.h"
#include "core/shader/glsl_upgrade.h"
#include "core/limits.h"
#include "core/shader/geometry.h"
#include "core/shader/tessellation.h"
#include "core/shader/spirv_passes.h"
#include "core/shader/subroutines.h"
#include "core/shader/transform_feedback.h"

namespace wmgl::shader {
namespace {

constexpr const char* kDefaultBlockName = "wmgl_DefaultUniforms";
constexpr int kDefaultBlockBinding = 1000;
constexpr unsigned kNoLocation = 0xFFF;  // glslang's TQualifier::layoutLocationEnd

// GLSL built-in constants (gl_Max*): glslang's defaults, with the limits
// the core reports through glGet (core/limits.h).
const TBuiltInResource* resources() {
    static const TBuiltInResource r = [] {
        TBuiltInResource x = *GetDefaultResources();
        namespace L = wmgl::limits;
        x.maxVertexAttribs = L::kMaxVertexAttribs;
        x.maxDrawBuffers = L::kMaxDrawBuffers;
        x.maxTextureImageUnits = x.maxVertexTextureImageUnits = x.maxGeometryTextureImageUnits = L::kMaxTextureUnitsPerStage;
        x.maxTessControlTextureImageUnits = x.maxTessEvaluationTextureImageUnits = L::kMaxTextureUnitsPerStage;
        x.maxCombinedTextureImageUnits = L::kMaxCombinedTextureUnits;
        x.maxVaryingComponents = 64;
        x.maxVaryingVectors = 16;
        x.maxImageUnits = L::kMaxImageUnits;
        x.maxCombinedImageUnitsAndFragmentOutputs = x.maxCombinedShaderOutputResources = L::kMaxCombinedImageUnitsAndFragmentOutputs;
        x.maxImageSamples = 0;
        x.maxVertexImageUniforms = x.maxTessControlImageUniforms = x.maxTessEvaluationImageUniforms = L::kMaxImageUniformsPerStage;
        x.maxGeometryImageUniforms = x.maxFragmentImageUniforms = x.maxComputeImageUniforms = L::kMaxImageUniformsPerStage;
        x.maxCombinedImageUniforms = L::kMaxCombinedImageUniforms;
        x.maxTessControlAtomicCounters = x.maxTessEvaluationAtomicCounters = 0;
        x.maxVertexAtomicCounters = x.maxGeometryAtomicCounters = x.maxFragmentAtomicCounters = x.maxComputeAtomicCounters =
            L::kMaxStageAtomicCounters;
        x.maxCombinedAtomicCounters = L::kMaxStageAtomicCounters;
        x.maxTessControlAtomicCounterBuffers = x.maxTessEvaluationAtomicCounterBuffers = 0;
        x.maxVertexAtomicCounterBuffers = L::kMaxVertexAtomicCounterBuffers;
        x.maxGeometryAtomicCounterBuffers = x.maxFragmentAtomicCounterBuffers = x.maxComputeAtomicCounterBuffers =
            L::kMaxStageAtomicCounterBuffers;
        x.maxCombinedAtomicCounterBuffers = L::kMaxStageAtomicCounterBuffers;
        x.maxAtomicCounterBindings = L::kMaxAtomicCounterBufferBindings;
        x.maxAtomicCounterBufferSize = L::kMaxAtomicCounterBufferSize;
        x.maxViewports = L::kMaxViewports;
        x.maxPatchVertices = L::kMaxPatchVertices;
        x.maxTessGenLevel = L::kMaxTessGenLevel;
        x.maxSamples = 4;
        return x;
    }();
    return &r;
}

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

// MSL has no rectangle textures: rectangle images become 2D images (the
// driver samples them with unnormalized-coordinate samplers and texelFetch
// maps to read()).
std::vector<uint32_t> rectImagesAs2D(std::vector<uint32_t> spirv) {
    constexpr uint32_t kOpTypeImage = 25, kDimRect = 4, kDim2D = 1;
    for (size_t i = 5; i < spirv.size();) {
        const uint32_t count = spirv[i] >> 16, opcode = spirv[i] & 0xffff;
        if (count == 0) break;
        if (opcode == kOpTypeImage && count > 3 && spirv[i + 3] == kDimRect) spirv[i + 3] = kDim2D;
        i += count;
    }
    return spirv;
}

// A vertex shader without outputs becomes a void function, which Metal only
// accepts with rasterization disabled: give it a position output (GL leaves
// the position undefined when the shader does not write it).
void ensureVertexOutput(std::string& msl) {
    const size_t fn = msl.find("vertex void main0(");
    if (fn == std::string::npos) return;
    std::string head = msl.substr(0, fn) + "struct main0_out\n{\n    float4 gl_Position [[position]];\n};\n\n";
    std::string main = msl.substr(fn);
    main.replace(0, std::string("vertex void main0(").size(), "vertex main0_out main0(");
    const size_t open = main.find('{');
    main.insert(open + 1, "\n    main0_out out = {};");
    for (size_t at = main.find("return;"); at != std::string::npos; at = main.find("return;", at + 11))
        main.replace(at, 7, "return out;");
    const size_t close = main.rfind('}');
    main.insert(close, "    return out;\n");
    msl = head + main;
}

// Every vertex function writes [[point_size]] (Metal leaves it undefined
// otherwise). GL 3.3: the size is the shader's gl_PointSize when
// PROGRAM_POINT_SIZE is enabled, else glPointSize; both come from the driver
// uniforms in buffer kSlotDriverUniforms (float pointSize, uint programPointSize, uint clipFlags, uint clipEnables).
void addPointSize(std::string& msl) {
    const size_t fn = msl.find("vertex main0_out main0(");
    const size_t structAt = msl.find("struct main0_out\n{\n");
    if (fn == std::string::npos || structAt == std::string::npos || structAt > fn) return;
    const bool writes = msl.find("[[point_size]]") != std::string::npos;
    const std::string driver =
        "struct wmgl_DriverUniforms\n{\n    float pointSize;\n    uint programPointSize;\n    uint clipFlags;\n    uint clipEnables;\n};\n\n";
    std::string patched = msl.substr(0, structAt) + driver + msl.substr(structAt, fn - structAt);
    if (!writes) {
        const size_t body = patched.find("struct main0_out\n{\n") + std::string("struct main0_out\n{\n").size();
        patched.insert(body, "    float gl_PointSize [[point_size]];\n");
    }
    std::string main = msl.substr(fn);
    const size_t open = main.find('(');
    const bool noParameters = main[open + 1] == ')';
    main.insert(open + 1, "constant wmgl_DriverUniforms& wmgl_driver [[buffer(" + std::to_string(kSlotDriverUniforms) + ")]]" +
                              (noParameters ? "" : ", "));
    std::string assign = writes ? "out.gl_PointSize = wmgl_driver.programPointSize != 0u ? out.gl_PointSize : wmgl_driver.pointSize; "
                                : "out.gl_PointSize = wmgl_driver.pointSize; ";
    // Clip distances of disabled planes (GL_CLIP_DISTANCEi) do not clip.
    const std::string clipDecl = "float gl_ClipDistance [[clip_distance]] [";
    if (const size_t clipAt = patched.find(clipDecl); clipAt != std::string::npos) {
        const int planes = std::atoi(patched.c_str() + clipAt + clipDecl.size());
        assign += "for (uint wmgl_i = 0u; wmgl_i < " + std::to_string(planes) +
                  "u; ++wmgl_i) if ((wmgl_driver.clipEnables & (1u << wmgl_i)) == 0u) out.gl_ClipDistance[wmgl_i] = 0.0; ";
    }
    for (size_t at = main.find("return out;"); at != std::string::npos; at = main.find("return out;", at + assign.size() + 11))
        main.insert(at, assign);
    // GL_ARB_clip_control: clipFlags bit 0 is ZERO_TO_ONE depth (no depth
    // remap), bit 1 the UPPER_LEFT origin (no Y flip).
    const std::pair<std::string, std::string> clip[] = {
        {"out.gl_Position.z = (out.gl_Position.z + out.gl_Position.w) * 0.5;",
         "if ((wmgl_driver.clipFlags & 1u) == 0u) out.gl_Position.z = (out.gl_Position.z + out.gl_Position.w) * 0.5;"},
        {"out.gl_Position.y = -(out.gl_Position.y);",
         "if ((wmgl_driver.clipFlags & 2u) == 0u) out.gl_Position.y = -(out.gl_Position.y);"},
    };
    for (const auto& [from, to] : clip)
        for (size_t at = main.find(from); at != std::string::npos; at = main.find(from, at + to.size())) main.replace(at, from.size(), to);
    msl = patched + main;
}

// Fragment stage: GL's gl_PointCoord origin is POINT_SPRITE_COORD_ORIGIN
// (upper left by default); Metal's point_coord origin is the top of Metal's
// framebuffer, which is GL's bottom row (GL row 0 = Metal row 0). The driver
// passes a flip flag as word 0 of buffer kSlotDriverUniforms (word 1 is the
// sample mask, fragmentWithSampleMask).
bool patchPointCoord(std::string& msl) {
    const std::string param = "float2 gl_PointCoord [[point_coord]]";
    const size_t at = msl.find(param);
    const size_t fn = msl.find("fragment ");
    if (at == std::string::npos || fn == std::string::npos || at < fn) return false;
    msl.replace(at, param.size(), "float2 wmgl_point_coord [[point_coord]], constant uint* wmgl_fs_driver [[buffer(" +
                                      std::to_string(kSlotDriverUniforms) + ")]]");
    const size_t body = msl.find('{', at);
    msl.insert(body + 1, "\n    float2 gl_PointCoord = wmgl_fs_driver[0] != 0u ? float2(wmgl_point_coord.x, 1.0 - wmgl_point_coord.y) "
                         ": wmgl_point_coord;");
    return true;
}

}  // namespace

std::string fragmentWithLogicOp(const std::string& msl, unsigned op, const LogicOpTarget targets[8]) {
    const std::string signature = "fragment main0_out main0(", member = "struct main0_out\n{\n";
    const size_t structAt = msl.find(member);
    const size_t fnAt = msl.find(signature);
    if (structAt == std::string::npos || fnAt == std::string::npos || structAt > fnAt) return std::string();
    // The colour outputs: "    <type> <name> [[color(N)]];" (dual-source outputs carry ", index(1)").
    struct Output { std::string type, name; int location; };
    std::vector<Output> outputs;
    {
        const size_t end = msl.find("};", structAt);
        std::istringstream lines(msl.substr(structAt, end - structAt));
        std::string line;
        while (std::getline(lines, line)) {
            const size_t attr = line.find(" [[color(");
            if (attr == std::string::npos || line.find("index(") != std::string::npos) continue;
            std::istringstream words(line.substr(0, attr));
            Output o;
            words >> o.type >> o.name;
            o.location = std::atoi(line.c_str() + attr + 9);
            if (!o.name.empty() && o.location >= 0 && o.location < 8) outputs.push_back(o);
        }
    }
    static const char* const kOps[16] = {
        "uint4(0u)", "s & d", "s & ~d", "s", "~s & d", "d", "s ^ d", "s | d",
        "~(s | d)", "~(s ^ d)", "~d", "s | ~d", "~s", "~s | d", "~(s & d)", "uint4(0xffffffffu)"};
    std::string helpers = "static inline uint4 wmgl_logic_op(uint4 s, uint4 d) { return " + std::string(kOps[op & 15]) +
                          "; }\n"
                          "static inline float4 wmgl_logic_unorm(float4 s, float4 d, float4 m) {\n"
                          "    uint4 r = wmgl_logic_op(uint4(rint(saturate(s) * m)), uint4(rint(saturate(d) * m)));\n"
                          "    return float4(r & uint4(m)) / max(m, float4(1.0));\n}\n"
                          "static inline uint4 wmgl_logic_uint(uint4 s, uint4 d, uint4 mask) { return wmgl_logic_op(s, d) & mask; }\n"
                          "static inline int4 wmgl_logic_int(int4 s, int4 d, uint4 shift) {\n"
                          "    return as_type<int4>(wmgl_logic_op(as_type<uint4>(s), as_type<uint4>(d)) << shift) >> int4(shift);\n}\n\n";
    std::string params, assign;
    for (const Output& o : outputs) {
        const LogicOpTarget& t = targets[o.location];
        if (!t.kind) continue;
        const char base = o.type.rfind("float", 0) == 0 ? 'n' : o.type.rfind("uint", 0) == 0 ? 'u' : o.type.rfind("int", 0) == 0 ? 'i' : 0;
        if (base != t.kind) continue;  // an output of the wrong kind for its attachment is undefined in GL
        const int n = std::isdigit(static_cast<unsigned char>(o.type.back())) ? o.type.back() - '0' : 1;
        const std::string vec = base == 'n' ? "float4" : base == 'u' ? "uint4" : "int4";
        const std::string scalar = base == 'n' ? "0.0" : base == 'u' ? "0u" : "0";
        const std::string dst = "wmgl_dst" + std::to_string(o.location);
        params += vec + " " + dst + " [[color(" + std::to_string(o.location) + ")]], ";
        std::string source = "out." + o.name;
        if (n < 4) {
            source = vec + "(" + source;
            for (int c = n; c < 4; ++c) source += ", " + scalar;
            source += ")";
        }
        std::string extra;
        for (int c = 0; c < 4; ++c) {
            const unsigned bits = t.bits[c];
            if (c) extra += ", ";
            if (base == 'n') extra += std::to_string(bits ? (1u << bits) - 1 : 0) + ".0";
            else if (base == 'u') extra += std::to_string(bits >= 32 ? 0xffffffffu : (1u << bits) - 1) + "u";
            else extra += std::to_string(bits >= 32 ? 0u : 32u - bits) + "u";
        }
        const std::string fn = base == 'n' ? "wmgl_logic_unorm" : base == 'u' ? "wmgl_logic_uint" : "wmgl_logic_int";
        std::string value = fn + "(" + source + ", " + dst + ", " + vec + "(" + extra + "))";
        static const char* const kSwizzle[5] = {"", ".x", ".xy", ".xyz", ""};
        if (n < 4) value = "(" + value + ")" + kSwizzle[n];
        assign += "out." + o.name + " = " + value + "; ";
    }
    if (assign.empty()) return msl;
    std::string out = msl;
    out.insert(fnAt, helpers);
    const size_t fn = out.find(signature);
    const size_t open = fn + signature.size() - 1;
    if (out[open + 1] == ')') params.resize(params.size() - 2);
    out.insert(open + 1, params);
    for (size_t at = out.find("return out;", fn); at != std::string::npos; at = out.find("return out;", at + assign.size() + 11))
        out.insert(at, assign);
    return out;
}

namespace {

// A fragment function without outputs (depth/stencil only): returns a struct
// holding just the sample mask.
std::string voidFragmentWithSampleMask(const std::string& msl) {
    const std::string signature = "fragment void main0(";
    const size_t fn = msl.find(signature);
    if (fn == std::string::npos) return std::string();
    const size_t bodyOpen = msl.find('{', fn);
    if (bodyOpen == std::string::npos) return std::string();
    size_t bodyClose = std::string::npos;
    int depth = 0;
    for (size_t i = bodyOpen; i < msl.size(); ++i) {
        if (msl[i] == '{') ++depth;
        else if (msl[i] == '}' && --depth == 0) {
            bodyClose = i;
            break;
        }
    }
    if (bodyClose == std::string::npos) return std::string();
    const std::string ret = "return wmgl_mask_out{wmgl_fs_driver[1]};";
    std::string body = msl.substr(bodyOpen, bodyClose - bodyOpen);
    for (size_t at = body.find("return;"); at != std::string::npos; at = body.find("return;", at + ret.size()))
        body.replace(at, 7, ret);
    std::string params = msl.substr(fn + signature.size(), bodyOpen - fn - signature.size());
    const bool hasDriver = params.find("wmgl_fs_driver [[buffer(") != std::string::npos;
    std::string head = "struct wmgl_mask_out\n{\n    uint wmgl_sample_mask [[sample_mask]];\n};\n\n"
                       "fragment wmgl_mask_out main0(";
    if (!hasDriver)
        head += "constant uint* wmgl_fs_driver [[buffer(" + std::to_string(kSlotDriverUniforms) + ")]]" +
                (params.compare(0, 1, ")") == 0 ? "" : ", ");
    return msl.substr(0, fn) + head + params + body + "    " + ret + "\n" + msl.substr(bodyClose);
}

}  // namespace

std::string fragmentAtSampleRate(const std::string& msl) {
    std::string out = msl;
    // Interpolated (floating-point, not flat) inputs move to the sample.
    const size_t start = out.find("struct main0_in\n{");
    const size_t end = start == std::string::npos ? std::string::npos : out.find("};", start);
    if (start != std::string::npos && end != std::string::npos) {
        std::string body = out.substr(start, end - start), moved;
        std::istringstream lines(body);
        std::string line;
        while (std::getline(lines, line)) {
            const size_t first = line.find_first_not_of(' ');
            const bool floating = first != std::string::npos &&
                                  (line.compare(first, 5, "float") == 0 || line.compare(first, 4, "half") == 0);
            const size_t user = line.find("[[user(");
            if (floating && user != std::string::npos && line.find(", flat]]") == std::string::npos) {
                const size_t close = line.find(")", user);
                const size_t attrEnd = line.find("]]", user);
                if (close != std::string::npos && attrEnd != std::string::npos) {
                    const bool noPerspective = line.find("no_perspective", close) < attrEnd;
                    line = line.substr(0, close + 1) + (noPerspective ? ", sample_no_perspective" : ", sample_perspective") +
                           line.substr(attrEnd);
                }
            }
            moved += line + "\n";
        }
        out.replace(start, end - start, moved);
    }
    if (out.find("[[sample_id]]") != std::string::npos) return out;
    for (const char* signature : {"fragment main0_out main0(", "fragment void main0("}) {
        const size_t fn = out.find(signature);
        if (fn == std::string::npos) continue;
        const size_t open = fn + std::strlen(signature) - 1;
        const bool noParameters = out[open + 1] == ')';
        out.insert(open + 1, std::string("uint wmgl_sample_rate [[sample_id]]") + (noParameters ? "" : ", "));
        return out;
    }
    return std::string();
}

std::string fragmentIgnoringSampleMask(const std::string& msl) {
    const size_t structAt = msl.find("struct main0_out\n{");
    const size_t structEnd = structAt == std::string::npos ? std::string::npos : msl.find("};", structAt);
    const size_t attribute = msl.find(" [[sample_mask]]", structAt);
    if (structAt == std::string::npos || attribute == std::string::npos || attribute > structEnd) return std::string();
    const size_t nameStart = msl.rfind(' ', attribute - 1) + 1;
    const std::string assign = "out." + msl.substr(nameStart, attribute - nameStart) + " = 0xffffffffu; ";
    std::string out = msl;
    const size_t fn = out.find("fragment main0_out main0(");
    if (fn == std::string::npos) return std::string();
    for (size_t at = out.find("return out;", fn); at != std::string::npos; at = out.find("return out;", at + assign.size() + 11))
        out.insert(at, assign);
    return out;
}

std::string fragmentWithSampleMask(const std::string& msl) {
    const std::string signature = "fragment main0_out main0(", member = "struct main0_out\n{\n";
    const size_t structAt = msl.find(member);
    if (msl.find("[[sample_mask]]") != std::string::npos) return std::string();
    if (structAt == std::string::npos || msl.find(signature) == std::string::npos || structAt > msl.find(signature))
        return voidFragmentWithSampleMask(msl);
    std::string out = msl;
    out.insert(structAt + member.size(), "    uint wmgl_sample_mask [[sample_mask]];\n");
    const size_t fn = out.find(signature);
    if (out.find("wmgl_fs_driver [[buffer(") == std::string::npos) {
        const size_t open = fn + signature.size() - 1;
        const bool noParameters = out[open + 1] == ')';
        out.insert(open + 1, "constant uint* wmgl_fs_driver [[buffer(" + std::to_string(kSlotDriverUniforms) + ")]]" +
                                 (noParameters ? "" : ", "));
    }
    const std::string assign = "out.wmgl_sample_mask = wmgl_fs_driver[1]; ";
    for (size_t at = out.find("return out;", fn); at != std::string::npos; at = out.find("return out;", at + assign.size() + 11))
        out.insert(at, assign);
    return out;
}

namespace {

// Fragment stage: whether any input is flat (explicitly, or as an integer,
// which Metal never interpolates); flat inputs take the provoking vertex.
bool hasFlatInputs(const std::string& msl) {
    const size_t start = msl.find("struct main0_in\n{");
    if (start == std::string::npos) return false;
    const size_t end = msl.find("};", start);
    std::istringstream lines(msl.substr(start, end - start));
    std::string line;
    while (std::getline(lines, line)) {
        const size_t first = line.find_first_not_of(' ');
        if (first == std::string::npos) continue;
        if (line.find(", flat]]") != std::string::npos || line.compare(first, 3, "int") == 0 ||
            line.compare(first, 4, "uint") == 0 || line.compare(first, 5, "short") == 0 || line.compare(first, 6, "ushort") == 0)
            return true;
    }
    return false;
}

// Vertex stage: glslang maps gl_InstanceID to Metal's [[instance_id]], which
// counts from the draw's base instance; GL's gl_InstanceID does not
// (GL_ARB_base_instance).
void instanceIdFromZero(std::string& msl) {
    const std::string param = "uint gl_InstanceIndex [[instance_id]]";
    const size_t at = msl.find(param);
    if (at == std::string::npos) return;
    msl.replace(at, param.size(), "uint wmgl_instance_index [[instance_id]], uint wmgl_base_instance [[base_instance]]");
    const size_t body = msl.find('{', at);
    msl.insert(body + 1, "\n    uint gl_InstanceIndex = wmgl_instance_index - wmgl_base_instance;");
}

// Separable programs (GL_ARB_separate_shader_objects) are linked alone, so
// their inter-stage varyings cannot be numbered against the other stages.
// Varyings without an explicit location get a location derived from their
// name (the block name for blocks), so separately linked stages match by
// name as GL requires. Metal matches [[user(locnN)]] names, so the large
// numbers are fine; 64 locations per name leave room for arrays. They are
// only ever set in SPIR-V: GLSL locations stop at 4095 (see
// stripHashedLocations).
constexpr uint32_t kHashedLocationBase = 1024;

uint32_t nameLocation(const std::string& name) {
    uint32_t h = 2166136261u;
    for (char c : name) h = (h ^ static_cast<uint8_t>(c)) * 16777619u;
    return kHashedLocationBase + (h % 1000000u) * 64u;
}

// Removes hashed varying locations from GLSL (SPIRV-Cross output of a
// separable stage): "layout(location = 5389312) out vec4 c;" becomes
// "out vec4 c;" and "layout(location = 5389312, component = 1)" keeps the
// other qualifiers. The stage is then hashed again by name.
std::string stripHashedLocations(const std::string& glsl) {
    std::string out;
    size_t i = 0;
    for (size_t at = glsl.find("layout("); at != std::string::npos; at = glsl.find("layout(", i)) {
        const size_t close = glsl.find(')', at);
        if (close == std::string::npos) break;
        std::vector<std::string> kept;
        bool stripped = false;
        std::istringstream parts(glsl.substr(at + 7, close - at - 7));
        std::string part;
        while (std::getline(parts, part, ',')) {
            const size_t first = part.find_first_not_of(' ');
            const size_t eq = part.find('=');
            if (first != std::string::npos && part.compare(first, 8, "location") == 0 && eq != std::string::npos &&
                std::strtoul(part.c_str() + eq + 1, nullptr, 10) >= kHashedLocationBase) {
                stripped = true;
                continue;
            }
            kept.push_back(part.substr(first == std::string::npos ? 0 : first));
        }
        out.append(glsl, i, at - i);
        i = close + 1;
        if (!stripped) {
            out.append(glsl, at, close + 1 - at);
            continue;
        }
        if (kept.empty()) {
            if (i < glsl.size() && glsl[i] == ' ') ++i;
            continue;
        }
        out += "layout(";
        for (size_t k = 0; k < kept.size(); ++k) out += (k ? ", " : "") + kept[k];
        out += ")";
    }
    out.append(glsl, i, std::string::npos);
    return out;
}

void hashVaryingLocations(std::vector<uint32_t>& spirv, Stage stage, const std::set<std::string>& explicitNames) {
    if (spirv.size() < 5) return;
    std::map<uint32_t, std::string> names;
    std::set<uint32_t> blockTypes;                       // structs decorated Block
    std::map<uint32_t, uint32_t> pointee, arrayElement;  // pointer -> type, array -> element
    std::map<uint32_t, uint32_t> storage;                // variable -> storage class
    std::map<uint32_t, uint32_t> varType;                // variable -> pointer type
    std::set<uint32_t> builtins;
    std::vector<size_t> locationWords;                   // OpDecorate Location: index of the target word
    for (size_t i = 5; i < spirv.size();) {
        const uint32_t count = spirv[i] >> 16, op = spirv[i] & 0xffff;
        if (!count) break;
        switch (op) {
        case 5:  // OpName
            names[spirv[i + 1]] = reinterpret_cast<const char*>(&spirv[i + 2]);
            break;
        case 32: pointee[spirv[i + 1]] = spirv[i + 3]; break;  // OpTypePointer
        case 28: arrayElement[spirv[i + 1]] = spirv[i + 2]; break;  // OpTypeArray
        case 29: arrayElement[spirv[i + 1]] = spirv[i + 2]; break;  // OpTypeRuntimeArray
        case 59:  // OpVariable
            varType[spirv[i + 2]] = spirv[i + 1];
            storage[spirv[i + 2]] = spirv[i + 3];
            break;
        case 71:  // OpDecorate
            if (spirv[i + 2] == 11) builtins.insert(spirv[i + 1]);
            if (spirv[i + 2] == 2) blockTypes.insert(spirv[i + 1]);
            if (spirv[i + 2] == 30) locationWords.push_back(i);
            break;
        default: break;
        }
        i += count;
    }
    for (size_t at : locationWords) {
        const uint32_t var = spirv[at + 1];
        auto s = storage.find(var);
        if (s == storage.end() || builtins.count(var)) continue;
        const bool input = s->second == 1, output = s->second == 3;
        if (!(input && stage != Stage::Vertex) && !(output && stage != Stage::Fragment)) continue;
        // The variable's name, or its block's name (GL matches blocks by
        // block name, not instance name).
        uint32_t type = pointee[varType[var]];
        while (arrayElement.count(type)) type = arrayElement[type];
        std::string name = names[var];
        if (blockTypes.count(type) || name.empty() || name.compare(0, 3, "gl_") == 0) name = names[type];
        name = unescapeName(name);
        if (name.empty() || name.compare(0, 3, "gl_") == 0 || explicitNames.count(name)) continue;
        spirv[at + 3] = nameLocation(name);
    }
}

// Strips glslang's trailing "N compilation errors" noise but keeps messages.
std::string cleanLog(const char* info, const char* debug) {
    std::string log = info ? info : "";
    if (debug && *debug) log += debug;
    return log;
}

struct ParsedShader {
    std::unique_ptr<glslang::TShader> shader;
    std::string source;    // codegen source (after legacy upgrade)
    std::string epilogue;  // transform feedback capture code (second string)
};

// Inserts `text` after the #version line and the #extension lines that
// directly follow it, then restores the original line numbering.
std::string insertAfterDirectives(const std::string& source, const std::string& text, int glslVersion) {
    size_t at = 0;
    int line = 1;
    bool sawVersion = false;
    while (at < source.size()) {
        size_t end = source.find('\n', at);
        if (end == std::string::npos) end = source.size();
        const std::string l = source.substr(at, end - at);
        const size_t first = l.find_first_not_of(" \t\r");
        const bool blank = first == std::string::npos;
        const bool version = !blank && l.compare(first, 8, "#version") == 0;
        const bool extension = !blank && l.compare(first, 10, "#extension") == 0;
        if (!blank && !version && !extension) break;
        if (version) sawVersion = true;
        if (!sawVersion && !blank) break;
        at = end < source.size() ? end + 1 : end;
        ++line;
    }
    // GLSL 3.30+: "#line N" numbers the next line N; earlier versions N + 1.
    const int lineValue = glslVersion >= 330 ? line : line - 1;
    return source.substr(0, at) + text + "#line " + std::to_string(lineValue) + "\n" + source.substr(at);
}

std::string raiseComputeVersion(Stage stage, const std::string& source);

// Whether a shader object defines main() (one of several objects per stage
// may only hold helper functions). Comments and directives are skipped.
bool definesMain(const std::string& source) {
    const size_t n = source.size();
    // Skips whitespace and comments from j; returns the next position.
    auto skip = [&](size_t j, bool* newline) {
        while (j < n) {
            if (source[j] == '\n' && newline) *newline = true;
            if (std::isspace(static_cast<unsigned char>(source[j]))) {
                ++j;
            } else if (source.compare(j, 2, "//") == 0) {
                const size_t end = source.find('\n', j);
                j = end == std::string::npos ? n : end;
            } else if (source.compare(j, 2, "/*") == 0) {
                const size_t end = source.find("*/", j + 2);
                j = end == std::string::npos ? n : end + 2;
            } else {
                break;
            }
        }
        return j;
    };
    size_t i = 0;
    int depth = 0;
    bool lineStart = true;
    while ((i = skip(i, &lineStart)) < n) {
        const char c = source[i];
        if (c == '#' && lineStart) {
            while (i < n && source[i] != '\n') i += source[i] == '\\' && i + 1 < n ? 2 : 1;
            continue;
        }
        lineStart = false;
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            size_t end = i;
            while (end < n && (std::isalnum(static_cast<unsigned char>(source[end])) || source[end] == '_')) ++end;
            const bool isMain = depth == 0 && end - i == 4 && source.compare(i, 4, "main") == 0;
            i = end;
            if (!isMain) continue;
            size_t j = skip(i, nullptr);
            if (j >= n || source[j] != '(') continue;
            j = source.find(')', j);
            if (j == std::string::npos) return false;
            j = skip(j + 1, nullptr);
            if (j < n && source[j] == '{') return true;
            continue;
        }
        depth += c == '{' ? 1 : c == '}' ? -1 : 0;
        ++i;
    }
    return false;
}

bool parseForCodegen(Stage stage, const std::string& original, ParsedShader& out, std::string& log,
                     const CaptureCode* capture = nullptr, const UpgradeOptions& upgrade = {}) {
    const EShLanguage lang = language(stage);
    const SubroutineLowering subroutines = lowerSubroutines(stage, original);
    if (!subroutines.ok) {
        log += subroutines.log;
        return false;
    }
    UpgradeResult upgraded =
        upgradeLegacyGlsl(stage, lowerBuiltinUniforms(stage, raiseComputeVersion(stage, subroutines.source)), upgrade);
    if (!upgraded.ok) {
        log += upgraded.log;
        return false;
    }
    out.source = escapeVulkanKeywords(upgraded.source);
    if (capture) {
        // Buffer blocks need a profile (GLSL 1.50+). Upgraded 1.40 code has no legacy built-ins left, so 1.50 core takes it.
        int version = parseGlslVersion(out.source).number;
        if (version < 150) {
            const size_t at = out.source.find("#version");
            if (at != std::string::npos) {
                const size_t end = out.source.find('\n', at);
                out.source.replace(at, end == std::string::npos ? std::string::npos : end - at, "#version 150 core");
                version = 150;
            }
        }
        out.source = insertAfterDirectives(out.source, capture->declarations, version);
        out.epilogue = capture->epilogue;
    }
    out.shader = std::make_unique<glslang::TShader>(lang);
    const char* texts[2] = {out.source.c_str(), out.epilogue.c_str()};
    out.shader->setStrings(texts, capture ? 2 : 1);
    configure(*out.shader, lang);
    // Validity was decided by compile()'s GL-rules parse; codegen tolerates
    // what desktop drivers accept (e.g. 'defined' produced by macro expansion).
    if (!out.shader->parse(resources(), 140, ENoProfile, false, false,
                           static_cast<EShMessages>(kMessages | EShMsgRelaxedErrors))) {
        log += unescapeName(cleanLog(out.shader->getInfoLog(), out.shader->getInfoDebugLog()));
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

// The GL internal format of an image format layout qualifier (0: none).
GLenum imageFormatOf(spv::ImageFormat format) {
    switch (format) {
    case spv::ImageFormatRgba32f: return GL_RGBA32F;
    case spv::ImageFormatRgba16f: return GL_RGBA16F;
    case spv::ImageFormatR32f: return GL_R32F;
    case spv::ImageFormatRgba8: return GL_RGBA8;
    case spv::ImageFormatRgba8Snorm: return GL_RGBA8_SNORM;
    case spv::ImageFormatRg32f: return GL_RG32F;
    case spv::ImageFormatRg16f: return GL_RG16F;
    case spv::ImageFormatR11fG11fB10f: return GL_R11F_G11F_B10F;
    case spv::ImageFormatR16f: return GL_R16F;
    case spv::ImageFormatRgba16: return GL_RGBA16;
    case spv::ImageFormatRgb10A2: return GL_RGB10_A2;
    case spv::ImageFormatRg16: return GL_RG16;
    case spv::ImageFormatRg8: return GL_RG8;
    case spv::ImageFormatR16: return GL_R16;
    case spv::ImageFormatR8: return GL_R8;
    case spv::ImageFormatRgba16Snorm: return GL_RGBA16_SNORM;
    case spv::ImageFormatRg16Snorm: return GL_RG16_SNORM;
    case spv::ImageFormatRg8Snorm: return GL_RG8_SNORM;
    case spv::ImageFormatR16Snorm: return GL_R16_SNORM;
    case spv::ImageFormatR8Snorm: return GL_R8_SNORM;
    case spv::ImageFormatRgba32i: return GL_RGBA32I;
    case spv::ImageFormatRgba16i: return GL_RGBA16I;
    case spv::ImageFormatRgba8i: return GL_RGBA8I;
    case spv::ImageFormatR32i: return GL_R32I;
    case spv::ImageFormatRg32i: return GL_RG32I;
    case spv::ImageFormatRg16i: return GL_RG16I;
    case spv::ImageFormatRg8i: return GL_RG8I;
    case spv::ImageFormatR16i: return GL_R16I;
    case spv::ImageFormatR8i: return GL_R8I;
    case spv::ImageFormatRgba32ui: return GL_RGBA32UI;
    case spv::ImageFormatRgba16ui: return GL_RGBA16UI;
    case spv::ImageFormatRgba8ui: return GL_RGBA8UI;
    case spv::ImageFormatR32ui: return GL_R32UI;
    case spv::ImageFormatRgb10a2ui: return GL_RGB10_A2UI;
    case spv::ImageFormatRg32ui: return GL_RG32UI;
    case spv::ImageFormatRg16ui: return GL_RG16UI;
    case spv::ImageFormatRg8ui: return GL_RG8UI;
    case spv::ImageFormatR16ui: return GL_R16UI;
    case spv::ImageFormatR8ui: return GL_R8UI;
    default: return 0;
    }
}

// The GL type of a storage image (GL 4.2).
GLenum imageTypeOf(const spirv_cross::SPIRType& base, spirv_cross::SPIRType::BaseType component) {
    using BT = spirv_cross::SPIRType::BaseType;
    const auto& image = base.image;
    const int kind = component == BT::Int ? 1 : component == BT::UInt ? 2 : 0;
    static const GLenum kTypes[3][11] = {
        {GL_IMAGE_1D, GL_IMAGE_2D, GL_IMAGE_3D, GL_IMAGE_2D_RECT, GL_IMAGE_CUBE, GL_IMAGE_BUFFER, GL_IMAGE_1D_ARRAY,
         GL_IMAGE_2D_ARRAY, GL_IMAGE_CUBE_MAP_ARRAY, GL_IMAGE_2D_MULTISAMPLE, GL_IMAGE_2D_MULTISAMPLE_ARRAY},
        {GL_INT_IMAGE_1D, GL_INT_IMAGE_2D, GL_INT_IMAGE_3D, GL_INT_IMAGE_2D_RECT, GL_INT_IMAGE_CUBE, GL_INT_IMAGE_BUFFER,
         GL_INT_IMAGE_1D_ARRAY, GL_INT_IMAGE_2D_ARRAY, GL_INT_IMAGE_CUBE_MAP_ARRAY, GL_INT_IMAGE_2D_MULTISAMPLE,
         GL_INT_IMAGE_2D_MULTISAMPLE_ARRAY},
        {GL_UNSIGNED_INT_IMAGE_1D, GL_UNSIGNED_INT_IMAGE_2D, GL_UNSIGNED_INT_IMAGE_3D, GL_UNSIGNED_INT_IMAGE_2D_RECT,
         GL_UNSIGNED_INT_IMAGE_CUBE, GL_UNSIGNED_INT_IMAGE_BUFFER, GL_UNSIGNED_INT_IMAGE_1D_ARRAY,
         GL_UNSIGNED_INT_IMAGE_2D_ARRAY, GL_UNSIGNED_INT_IMAGE_CUBE_MAP_ARRAY, GL_UNSIGNED_INT_IMAGE_2D_MULTISAMPLE,
         GL_UNSIGNED_INT_IMAGE_2D_MULTISAMPLE_ARRAY}};
    int index = 1;
    switch (image.dim) {
    case spv::Dim1D: index = image.arrayed ? 6 : 0; break;
    case spv::Dim2D: index = image.ms ? (image.arrayed ? 10 : 9) : (image.arrayed ? 7 : 1); break;
    case spv::Dim3D: index = 2; break;
    case spv::DimRect: index = 3; break;
    case spv::DimCube: index = image.arrayed ? 8 : 4; break;
    case spv::DimBuffer: index = 5; break;
    default: break;
    }
    return kTypes[kind][index];
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
        const std::string name = prefix + unescapeName(m.getFieldName().c_str());
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
    const std::string memberName = compiler.get_member_name(parent.self, index);
    // A sampler member of a struct uniform is hoisted out of the struct by
    // glslang (it is reported as a sampler uniform); its place holder, named
    // "/*member*/", is not a GL uniform.
    if (memberName.compare(0, 2, "/*") == 0) return;
    std::string name = prefix + unescapeName(memberName);
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

int atomicCounterBlockBinding(const std::string& blockName) {
    // SPIRV-Cross prefixes reserved gl_ names when it writes GLSL (geometry kernels).
    static const std::string kName = "gl_AtomicCounterBlock_", kFixup = "_RESERVED_IDENTIFIER_FIXUP_";
    std::string name = blockName.rfind(kFixup, 0) == 0 ? blockName.substr(kFixup.size()) : blockName;
    if (name.rfind(kName, 0) != 0 || name.size() == kName.size()) return -1;
    const std::string digits = name.substr(kName.size());
    if (digits.find_first_not_of("0123456789") != std::string::npos) return -1;
    return std::atoi(digits.c_str());
}

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
    default: return glTypeIsSampler(type) || glTypeIsImage(type) ? 1 : 0;
    }
}

int conventionalAttribLocation(const std::string& name) {
    static const char* const kNames[kConventionalAttribCount] = {
        "wmgl_Vertex", "wmgl_Normal", "wmgl_Color", "wmgl_SecondaryColor", "wmgl_FogCoord",
        "wmgl_MultiTexCoord0", "wmgl_MultiTexCoord1", "wmgl_MultiTexCoord2", "wmgl_MultiTexCoord3",
        "wmgl_MultiTexCoord4", "wmgl_MultiTexCoord5", "wmgl_MultiTexCoord6", "wmgl_MultiTexCoord7",
        "wmgl_LinePartner", "wmgl_LineSide"};
    if (name.rfind("wmgl_", 0) != 0) return -1;
    for (int i = 0; i < kConventionalAttribCount; ++i)
        if (name == kNames[i]) return kConventionalAttribFirst + i;
    return -1;
}

int glTypeLocations(GLenum type) {
    switch (type) {
    case GL_FLOAT_MAT2: case GL_FLOAT_MAT2x3: case GL_FLOAT_MAT2x4: return 2;
    case GL_FLOAT_MAT3: case GL_FLOAT_MAT3x2: case GL_FLOAT_MAT3x4: return 3;
    case GL_FLOAT_MAT4: case GL_FLOAT_MAT4x2: case GL_FLOAT_MAT4x3: return 4;
    // Vertex inputs of double types take one location per column too (GLSL
    // 4.60 section 4.4.1); dvec3/dvec4 columns count twice against
    // MAX_VERTEX_ATTRIBS (glTypeAttribSlots).
    case GL_DOUBLE_MAT2: case GL_DOUBLE_MAT2x3: case GL_DOUBLE_MAT2x4: return 2;
    case GL_DOUBLE_MAT3: case GL_DOUBLE_MAT3x2: case GL_DOUBLE_MAT3x4: return 3;
    case GL_DOUBLE_MAT4: case GL_DOUBLE_MAT4x2: case GL_DOUBLE_MAT4x3: return 4;
    default: return 1;
    }
}

int glTypeAttribSlots(GLenum type) {
    const int columns = glTypeColumns(type);
    const bool wide = glTypeIsDouble(type) && glTypeComponents(type) / columns > 2;
    return glTypeLocations(type) * (wide ? 2 : 1);
}

int glTypeColumns(GLenum type) {
    switch (type) {
    case GL_FLOAT_MAT2: case GL_FLOAT_MAT2x3: case GL_FLOAT_MAT2x4: case GL_DOUBLE_MAT2: case GL_DOUBLE_MAT2x3:
    case GL_DOUBLE_MAT2x4: return 2;
    case GL_FLOAT_MAT3: case GL_FLOAT_MAT3x2: case GL_FLOAT_MAT3x4: case GL_DOUBLE_MAT3: case GL_DOUBLE_MAT3x2:
    case GL_DOUBLE_MAT3x4: return 3;
    case GL_FLOAT_MAT4: case GL_FLOAT_MAT4x2: case GL_FLOAT_MAT4x3: case GL_DOUBLE_MAT4: case GL_DOUBLE_MAT4x2:
    case GL_DOUBLE_MAT4x3: return 4;
    default: return 1;
    }
}

bool glTypeIsDouble(GLenum type) {
    switch (type) {
    case GL_DOUBLE: case GL_DOUBLE_VEC2: case GL_DOUBLE_VEC3: case GL_DOUBLE_VEC4: case GL_DOUBLE_MAT2: case GL_DOUBLE_MAT3:
    case GL_DOUBLE_MAT4: case GL_DOUBLE_MAT2x3: case GL_DOUBLE_MAT2x4: case GL_DOUBLE_MAT3x2: case GL_DOUBLE_MAT3x4:
    case GL_DOUBLE_MAT4x2: case GL_DOUBLE_MAT4x3: return true;
    default: return false;
    }
}

int glTypeWords(GLenum type) { return glTypeComponents(type) * (glTypeIsDouble(type) ? 2 : 1); }

bool glTypeIsSampler(GLenum type) { return textureTargetForSampler(type) >= 0; }

bool glTypeIsImage(GLenum type) {
    return (type >= GL_IMAGE_1D && type <= GL_UNSIGNED_INT_IMAGE_2D_MULTISAMPLE_ARRAY);
}

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

namespace {

// GL_ARB_compute_shader makes compute shaders available to any GLSL version;
// glslang only accepts them from 4.20, so such sources are compiled as 4.30.
std::string raiseComputeVersion(Stage stage, const std::string& source) {
    if (stage != Stage::Compute || parseGlslVersion(source).number >= 420) return source;
    if (source.find("GL_ARB_compute_shader") == std::string::npos) return source;
    const size_t at = source.find("#version");
    if (at == std::string::npos) return "#version 430\n" + source;
    const size_t end = source.find('\n', at);
    std::string raised = source;
    raised.replace(at, end == std::string::npos ? std::string::npos : end - at, "#version 430");
    return raised;
}

// layout(binding) on a uniform block must leave every element of the block
// within GL_MAX_UNIFORM_BUFFER_BINDINGS (a compile-time error in GLSL).
// glslang checks opaque bindings itself but has no limit for blocks.
std::string checkBlockBindings(glslang::TShader& shader) {
    const glslang::TIntermediate* intermediate = shader.getIntermediate();
    glslang::TIntermAggregate* root =
        intermediate && intermediate->getTreeRoot() ? intermediate->getTreeRoot()->getAsAggregate() : nullptr;
    if (!root) return {};
    for (TIntermNode* node : root->getSequence()) {
        glslang::TIntermAggregate* objects = node->getAsAggregate();
        if (!objects || objects->getOp() != glslang::EOpLinkerObjects) continue;
        for (TIntermNode* object : objects->getSequence()) {
            glslang::TIntermSymbol* symbol = object->getAsSymbolNode();
            if (!symbol) continue;
            const glslang::TType& type = symbol->getType();
            const glslang::TQualifier& q = type.getQualifier();
            if (type.getBasicType() != glslang::EbtBlock || q.storage != glslang::EvqUniform || !q.hasBinding()) continue;
            const int count = type.isSizedArray() ? type.getCumulativeArraySize() : 1;
            if (static_cast<int64_t>(q.layoutBinding) + count > wmgl::limits::kMaxUniformBufferBindings)
                return "ERROR: uniform block '" + unescapeName(type.getTypeName().c_str()) +
                       "': binding is not less than GL_MAX_UNIFORM_BUFFER_BINDINGS\n";
        }
    }
    return {};
}

}  // namespace

CompileResult compile(Stage stage, const std::string& original) {
    initGlslang();
    CompileResult result;
    // Subroutines are lowered first: glslang does not implement them.
    const SubroutineLowering subroutines = lowerSubroutines(stage, original);
    if (!subroutines.ok) {
        result.log = subroutines.log;
        return result;
    }
    const std::string source = lowerBuiltinUniforms(stage, raiseComputeVersion(stage, subroutines.source));
    // Status and log come from a plain GL parse, which accepts every GLSL
    // version and profile with GL's own rules.
    const EShLanguage lang = language(stage);
    glslang::TShader shader(lang);
    const char* text = source.c_str();
    shader.setStrings(&text, 1);
    shader.setEnvInput(glslang::EShSourceGlsl, lang, glslang::EShClientNone, 0);
    shader.setEnvClient(glslang::EShClientNone, glslang::EShTargetClientVersion(0));
    shader.setEnvTarget(glslang::EShTargetNone, glslang::EShTargetLanguageVersion(0));
    result.ok = shader.parse(resources(), 110, ENoProfile, false, false, EShMsgDefault);
    result.log = cleanLog(shader.getInfoLog(), shader.getInfoDebugLog());
    if (!result.ok && result.log.find("cannot use in preprocessor expression when expanded from macros") != std::string::npos) {
        // 'defined' produced by macro expansion is undefined behaviour in GLSL;
        // desktop drivers evaluate it. Accept the shader if that is the only problem.
        glslang::TShader relaxed(lang);
        relaxed.setStrings(&text, 1);
        relaxed.setEnvInput(glslang::EShSourceGlsl, lang, glslang::EShClientNone, 0);
        relaxed.setEnvClient(glslang::EShClientNone, glslang::EShTargetClientVersion(0));
        relaxed.setEnvTarget(glslang::EShTargetNone, glslang::EShTargetLanguageVersion(0));
        if (relaxed.parse(resources(), 110, ENoProfile, false, false, EShMsgRelaxedErrors)) {
            const std::string relaxedLog = cleanLog(relaxed.getInfoLog(), relaxed.getInfoDebugLog());
            if (relaxedLog.find("ERROR") == std::string::npos) {
                result.ok = true;
                result.log = relaxedLog;
            }
        }
    }
    if (result.ok) {
        const std::string bindings = checkBlockBindings(shader);
        if (!bindings.empty()) {
            result.ok = false;
            result.log += bindings;
        }
    }
    if (result.ok) {
        const std::string reserved = checkReservedWords(source);
        if (!reserved.empty()) {
            result.ok = false;
            result.log += reserved;
        }
    }
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

namespace {

GLenum glTypeOfAst(const glslang::TType& t) {
    const int n = t.getVectorSize();
    auto pick = [&](GLenum s, GLenum v2, GLenum v3, GLenum v4) { return n <= 1 ? s : n == 2 ? v2 : n == 3 ? v3 : v4; };
    if (t.isMatrix()) {
        const int c = t.getMatrixCols(), r = t.getMatrixRows();
        static const GLenum kMat[3][3] = {{GL_FLOAT_MAT2, GL_FLOAT_MAT2x3, GL_FLOAT_MAT2x4},
                                          {GL_FLOAT_MAT3x2, GL_FLOAT_MAT3, GL_FLOAT_MAT3x4},
                                          {GL_FLOAT_MAT4x2, GL_FLOAT_MAT4x3, GL_FLOAT_MAT4}};
        static const GLenum kDMat[3][3] = {{GL_DOUBLE_MAT2, GL_DOUBLE_MAT2x3, GL_DOUBLE_MAT2x4},
                                           {GL_DOUBLE_MAT3x2, GL_DOUBLE_MAT3, GL_DOUBLE_MAT3x4},
                                           {GL_DOUBLE_MAT4x2, GL_DOUBLE_MAT4x3, GL_DOUBLE_MAT4}};
        return t.getBasicType() == glslang::EbtDouble ? kDMat[c - 2][r - 2] : kMat[c - 2][r - 2];
    }
    switch (t.getBasicType()) {
    case glslang::EbtDouble: return pick(GL_DOUBLE, GL_DOUBLE_VEC2, GL_DOUBLE_VEC3, GL_DOUBLE_VEC4);
    case glslang::EbtInt: return pick(GL_INT, GL_INT_VEC2, GL_INT_VEC3, GL_INT_VEC4);
    case glslang::EbtUint: return pick(GL_UNSIGNED_INT, GL_UNSIGNED_INT_VEC2, GL_UNSIGNED_INT_VEC3, GL_UNSIGNED_INT_VEC4);
    case glslang::EbtBool: return pick(GL_BOOL, GL_BOOL_VEC2, GL_BOOL_VEC3, GL_BOOL_VEC4);
    default: return pick(GL_FLOAT, GL_FLOAT_VEC2, GL_FLOAT_VEC3, GL_FLOAT_VEC4);
    }
}

// The outputs a stage can capture: its user outputs plus the built-in
// gl_Position, gl_PointSize and gl_ClipDistance.
std::vector<StageOutputVar> captureableOutputs(glslang::TIntermediate* intermediate) {
    std::vector<StageOutputVar> outputs;
    bool clipDistance = false;
    glslang::TIntermAggregate* root = intermediate && intermediate->getTreeRoot() ? intermediate->getTreeRoot()->getAsAggregate() : nullptr;
    if (root) {
        for (TIntermNode* node : root->getSequence()) {
            glslang::TIntermAggregate* objects = node->getAsAggregate();
            if (!objects || objects->getOp() != glslang::EOpLinkerObjects) continue;
            for (TIntermNode* object : objects->getSequence()) {
                glslang::TIntermSymbol* symbol = object->getAsSymbolNode();
                if (!symbol || symbol->getQualifier().storage != glslang::EvqVaryingOut) continue;
                const glslang::TType& type = symbol->getType();
                if (type.getBasicType() == glslang::EbtBlock) {
                    const std::string blockName = unescapeName(type.getTypeName().c_str());
                    const bool anonymous = glslang::IsAnonymous(symbol->getName());
                    for (const glslang::TTypeLoc& member : *type.getStruct()) {
                        const glslang::TType& m = *member.type;
                        const std::string field = unescapeName(m.getFieldName().c_str());
                        if (field == "gl_ClipDistance" && m.isSizedArray()) {
                            outputs.push_back({"gl_ClipDistance", GL_FLOAT, m.getOuterArraySize()});
                            clipDistance = true;
                        }
                        if (blockName == "gl_PerVertex" || field.rfind("gl_", 0) == 0 || type.isArray()) continue;
                        // User blocks: members are named "Block.member" (GL 3.3 section 2.15);
                        // members of a block without an instance name also by their own name.
                        const std::string expression = anonymous ? m.getFieldName().c_str()
                                                                 : std::string(symbol->getName().c_str()) + "." + m.getFieldName().c_str();
                        outputs.push_back({blockName + "." + field, glTypeOfAst(m), m.isSizedArray() ? m.getOuterArraySize() : 0,
                                           expression});
                        if (anonymous)
                            outputs.push_back({field, glTypeOfAst(m), m.isSizedArray() ? m.getOuterArraySize() : 0, expression,
                                               blockName + "." + field});
                    }
                    continue;
                }
                const std::string codegenName = symbol->getName().c_str();
                const std::string name = unescapeName(codegenName);
                if (name.rfind("gl_", 0) == 0) continue;
                // Lowered legacy varyings are captured by their built-in names (gl_FrontColor, gl_TexCoord, ...).
                static const char* const kLegacy[] = {"wmgl_FrontColor", "wmgl_BackColor", "wmgl_FrontSecondaryColor",
                                                      "wmgl_BackSecondaryColor", "wmgl_TexCoord", "wmgl_FogFragCoord"};
                const bool legacy = std::find(std::begin(kLegacy), std::end(kLegacy), name) != std::end(kLegacy);
                // The capture code reads the codegen name (escaped keywords keep their prefix there).
                outputs.push_back({legacy ? "gl_" + name.substr(5) : name, glTypeOfAst(type),
                                   type.isSizedArray() ? type.getOuterArraySize() : 0,
                                   legacy || codegenName != name ? codegenName : std::string()});
            }
        }
    }
    outputs.push_back({"gl_Position", GL_FLOAT_VEC4, 0});
    outputs.push_back({"gl_PointSize", GL_FLOAT, 0});
    if (!clipDistance) outputs.push_back({"gl_ClipDistance", GL_FLOAT, 8});
    return outputs;
}

}  // namespace

namespace {

// Uniform initializers (GLSL 1.20+). The codegen parse (Vulkan rules) drops
// them, so they come from a GL-rules parse of each shader: name -> the
// initializer's components, flattened (array elements, matrix columns).
struct UniformInitializer {
    std::vector<double> values;
};

std::map<std::string, UniformInitializer> uniformInitializers(const std::vector<ShaderSource>& shaders) {
    std::map<std::string, UniformInitializer> out;
    for (const ShaderSource& shader : shaders) {
        if (shader.source.find('=') == std::string::npos) continue;
        const SubroutineLowering lowered = lowerSubroutines(shader.stage, shader.source);
        const std::string source = raiseComputeVersion(shader.stage, lowered.source);
        const EShLanguage lang = language(shader.stage);
        glslang::TShader parsed(lang);
        const char* text = source.c_str();
        parsed.setStrings(&text, 1);
        parsed.setEnvInput(glslang::EShSourceGlsl, lang, glslang::EShClientNone, 0);
        parsed.setEnvClient(glslang::EShClientNone, glslang::EShTargetClientVersion(0));
        parsed.setEnvTarget(glslang::EShTargetNone, glslang::EShTargetLanguageVersion(0));
        if (!parsed.parse(resources(), 110, ENoProfile, false, false, EShMsgRelaxedErrors)) continue;
        glslang::TIntermediate* intermediate = parsed.getIntermediate();
        glslang::TIntermAggregate* root = intermediate && intermediate->getTreeRoot() ? intermediate->getTreeRoot()->getAsAggregate() : nullptr;
        if (!root) continue;
        for (TIntermNode* node : root->getSequence()) {
            glslang::TIntermAggregate* objects = node->getAsAggregate();
            if (!objects || objects->getOp() != glslang::EOpLinkerObjects) continue;
            for (TIntermNode* object : objects->getSequence()) {
                glslang::TIntermSymbol* symbol = object->getAsSymbolNode();
                if (!symbol || symbol->getQualifier().storage != glslang::EvqUniform) continue;
                const glslang::TConstUnionArray& values = symbol->getConstArray();
                if (values.empty() || symbol->getType().isStruct() || symbol->getBasicType() == glslang::EbtSampler) continue;
                UniformInitializer init;
                for (int i = 0; i < values.size(); ++i) {
                    const glslang::TConstUnion& v = values[i];
                    switch (v.getType()) {
                    case glslang::EbtInt: init.values.push_back(v.getIConst()); break;
                    case glslang::EbtUint: init.values.push_back(v.getUConst()); break;
                    case glslang::EbtBool: init.values.push_back(v.getBConst() ? 1.0 : 0.0); break;
                    default: init.values.push_back(v.getDConst()); break;
                    }
                }
                out[unescapeName(symbol->getName().c_str())] = init;
            }
        }
    }
    return out;
}

// Writes the initializers into the default block image of `result`.
void applyUniformInitializers(const std::vector<ShaderSource>& shaders, LinkResult& result) {
    result.defaultBlockInit.assign((result.defaultBlockSize + 15) & ~15u, 0);
    const std::map<std::string, UniformInitializer> inits = uniformInitializers(shaders);
    if (inits.empty()) return;
    for (const UniformInfo& u : result.uniforms) {
        if (u.blockIndex >= 0 || u.samplerSlot >= 0 || u.imageSlot >= 0 || u.atomicBufferIndex >= 0 || u.offset < 0) continue;
        auto it = inits.find(arrayBaseName(u.name));
        if (it == inits.end()) continue;
        const std::vector<double>& values = it->second.values;
        const int components = glTypeComponents(u.type);
        int columns = 1;
        switch (u.type) {
        case GL_FLOAT_MAT2: case GL_FLOAT_MAT2x3: case GL_FLOAT_MAT2x4: case GL_DOUBLE_MAT2: case GL_DOUBLE_MAT2x3:
        case GL_DOUBLE_MAT2x4: columns = 2; break;
        case GL_FLOAT_MAT3: case GL_FLOAT_MAT3x2: case GL_FLOAT_MAT3x4: case GL_DOUBLE_MAT3: case GL_DOUBLE_MAT3x2:
        case GL_DOUBLE_MAT3x4: columns = 3; break;
        case GL_FLOAT_MAT4: case GL_FLOAT_MAT4x2: case GL_FLOAT_MAT4x3: case GL_DOUBLE_MAT4: case GL_DOUBLE_MAT4x2:
        case GL_DOUBLE_MAT4x3: columns = 4; break;
        default: break;
        }
        const int rows = components / columns;
        const bool isDouble = glTypeWords(u.type) == 2 * components;
        const char kind = isDouble ? 'd'
                          : (u.type == GL_INT || u.type == GL_INT_VEC2 || u.type == GL_INT_VEC3 || u.type == GL_INT_VEC4) ? 'i'
                          : (u.type == GL_UNSIGNED_INT || u.type == GL_UNSIGNED_INT_VEC2 || u.type == GL_UNSIGNED_INT_VEC3 ||
                             u.type == GL_UNSIGNED_INT_VEC4) ? 'u'
                          : (u.type == GL_BOOL || u.type == GL_BOOL_VEC2 || u.type == GL_BOOL_VEC3 || u.type == GL_BOOL_VEC4) ? 'b'
                                                                                                                        : 'f';
        const size_t scalar = isDouble ? 8 : 4;
        const size_t matrixStride = u.matrixStride ? static_cast<size_t>(u.matrixStride) : 16;
        const size_t stride = u.arrayStride ? static_cast<size_t>(u.arrayStride) : 0;
        for (GLint e = 0; e < u.size; ++e)
            for (int c = 0; c < components; ++c) {
                const size_t index = static_cast<size_t>(e) * static_cast<size_t>(components) + static_cast<size_t>(c);
                if (index >= values.size()) break;
                const size_t at = static_cast<size_t>(u.offset) + static_cast<size_t>(e) * stride +
                                  (columns > 1 ? static_cast<size_t>(c / rows) * matrixStride + static_cast<size_t>(c % rows) * scalar
                                               : static_cast<size_t>(c) * scalar);
                if (at + scalar > result.defaultBlockInit.size()) continue;
                uint8_t* dst = result.defaultBlockInit.data() + at;
                const double v = values[index];
                if (kind == 'd') std::memcpy(dst, &v, 8);
                else if (kind == 'f') { const float f = static_cast<float>(v); std::memcpy(dst, &f, 4); }
                else if (kind == 'i') { const int32_t i = static_cast<int32_t>(v); std::memcpy(dst, &i, 4); }
                else { const uint32_t i = static_cast<uint32_t>(v); std::memcpy(dst, &i, 4); }
            }
    }
}

}  // namespace

// Moves the wmgl_FixedFunction block and its members after the
// application's blocks and uniforms (LinkResult::fixedFunctionBlock).
static void hideFixedFunctionBlock(LinkResult& r) {
    int ff = -1;
    for (size_t b = 0; b < r.blocks.size(); ++b)
        if (r.blocks[b].name == kFixedFunctionBlockName) ff = static_cast<int>(b);
    if (ff < 0) return;
    std::vector<int> blockMap(r.blocks.size()), uniformMap(r.uniforms.size());
    std::vector<UniformBlockInfo> blocks;
    for (size_t b = 0; b < r.blocks.size(); ++b) {
        if (static_cast<int>(b) == ff) continue;
        blockMap[b] = static_cast<int>(blocks.size());
        blocks.push_back(std::move(r.blocks[b]));
    }
    blockMap[static_cast<size_t>(ff)] = static_cast<int>(blocks.size());
    blocks.push_back(std::move(r.blocks[static_cast<size_t>(ff)]));
    std::vector<UniformInfo> uniforms;
    for (int pass = 0; pass < 2; ++pass)
        for (size_t u = 0; u < r.uniforms.size(); ++u) {
            if ((r.uniforms[u].blockIndex == ff) != (pass == 1)) continue;
            uniformMap[u] = static_cast<int>(uniforms.size());
            uniforms.push_back(r.uniforms[u]);
        }
    for (UniformInfo& u : uniforms)
        if (u.blockIndex >= 0) u.blockIndex = blockMap[static_cast<size_t>(u.blockIndex)];
    for (UniformBlockInfo& b : blocks)
        for (int& u : b.activeUniforms) u = uniformMap[static_cast<size_t>(u)];
    for (AtomicCounterBufferInfo& b : r.atomicBuffers)
        for (int& u : b.activeUniforms) u = uniformMap[static_cast<size_t>(u)];
    for (auto& location : r.locations) location.first = uniformMap[static_cast<size_t>(location.first)];
    r.blocks = std::move(blocks);
    r.uniforms = std::move(uniforms);
    r.fixedFunctionBlock = static_cast<int>(r.blocks.size() - 1);
}

// `geometry`/`geometryCapture` (optional) receive the parsed geometry stage
// and the layout of the vertex outputs it reads (see geometry.h).
// What link() needs to build the tessellation emulation: the parsed control
// and evaluation stages, the layout of the vertex outputs they read, and the
// geometry stage as GLSL (linked again after the evaluation stage).
struct TessellationLink {
    TessStageInfo control, evaluation;
    bool hasControl = false;
    LinkResult capture;
    std::string geometryGlsl;
};

static LinkResult linkImpl(const LinkInput& input, const CaptureCode* capture, GeometryShaderInfo* geometry = nullptr,
                           LinkResult* geometryCapture = nullptr, TessellationLink* tessellation = nullptr) {
    initGlslang();
    LinkResult result;
    std::vector<ParsedShader> parsed(input.shaders.size());
    bool stagePresent[kStageCount] = {};
    // Legacy built-in varyings the fragment stage reads from the vertex stage.
    UpgradeOptions vertexUpgrade;
    bool hasGeometry = false;
    for (const ShaderSource& shader : input.shaders) hasGeometry |= shader.stage == Stage::Geometry;
    if (!hasGeometry)
        for (const ShaderSource& shader : input.shaders)
            if (shader.stage == Stage::Fragment)
                for (const std::string& v : upgradeLegacyGlsl(Stage::Fragment, shader.source).builtinVaryingsRead)
                    vertexUpgrade.provideVaryings.insert(v);
    for (size_t i = 0; i < input.shaders.size(); ++i) {
        const bool captureHere = capture && input.shaders[i].stage == Stage::Vertex && definesMain(input.shaders[i].source);
        if (!parseForCodegen(input.shaders[i].stage, input.shaders[i].source, parsed[i], result.log,
                             captureHere ? capture : nullptr,
                             input.shaders[i].stage == Stage::Vertex ? vertexUpgrade : UpgradeOptions()))
            return result;
        stagePresent[static_cast<int>(input.shaders[i].stage)] = true;
    }
    if (stagePresent[static_cast<int>(Stage::Compute)]) {
        for (int s = 0; s < kStageCount; ++s)
            if (stagePresent[s] && s != static_cast<int>(Stage::Compute)) {
                result.log += "error: a compute shader cannot be linked with other shader stages\n";
                return result;
            }
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
        if ((stagePresent[static_cast<int>(Stage::TessControl)] || stagePresent[static_cast<int>(Stage::TessEval)]) &&
            !stagePresent[static_cast<int>(Stage::Vertex)]) {
            result.log += "error: tessellation shaders require a vertex shader\n";
            return result;
        }
    }
    // A control shader without an evaluation shader cannot draw anything
    // (GL_PATCHES needs both; transform feedback cannot capture patches).
    if (stagePresent[static_cast<int>(Stage::TessControl)] && !stagePresent[static_cast<int>(Stage::TessEval)] &&
        !input.separable) {
        result.log += "error: a tessellation control shader requires a tessellation evaluation shader\n";
        return result;
    }

    glslang::TProgram program;
    for (auto& p : parsed) program.addShader(p.shader.get());
    if (!program.link(kMessages)) {
        result.log += unescapeName(cleanLog(program.getInfoLog(), program.getInfoDebugLog()));
        return result;
    }
    if (stagePresent[static_cast<int>(Stage::Compute)]) {
        const glslang::TIntermediate* cs = program.getIntermediate(EShLangCompute);
        if (!cs->isLocalSizeSet()) {
            result.log += "error: compute shader has no local_size layout qualifier\n";
            return result;
        }
        for (int d = 0; d < 3; ++d) result.computeLocalSize[d] = cs->getLocalSize(d);
    }
    const bool tessellated = stagePresent[static_cast<int>(Stage::TessEval)];
    if (tessellated) {
        // The tessellation layout (merged over the stage's shader objects).
        TessellationEmulation& t = result.tess;
        const glslang::TIntermediate* tes = program.getIntermediate(EShLangTessEvaluation);
        t.mode = tes->getInputPrimitive() == glslang::ElgQuads      ? GL_QUADS
                 : tes->getInputPrimitive() == glslang::ElgIsolines ? GL_ISOLINES
                                                                    : GL_TRIANGLES;
        t.spacing = tes->getVertexSpacing() == glslang::EvsFractionalEven  ? GL_FRACTIONAL_EVEN
                    : tes->getVertexSpacing() == glslang::EvsFractionalOdd ? GL_FRACTIONAL_ODD
                                                                           : GL_EQUAL;
        t.order = tes->getVertexOrder() == glslang::EvoCw ? GL_CW : GL_CCW;
        t.pointMode = tes->getPointMode();
        if (const glslang::TIntermediate* tcs = program.getIntermediate(EShLangTessControl)) {
            t.hasControl = true;
            t.verticesOut = tcs->getVertices();
        }
    } else if (stagePresent[static_cast<int>(Stage::TessControl)]) {
        // A separable control-only program: its output vertex count is queryable.
        if (const glslang::TIntermediate* tcs = program.getIntermediate(EShLangTessControl))
            result.tess.verticesOut = tcs->getVertices();
    }
    // Transform feedback varyings are validated against the last vertex
    // processing stage (geometry if present, else tessellation evaluation,
    // else vertex).
    // A separable control-only program captures nothing: its varyings would
    // apply if it ended a pipeline, which the control stage cannot, so desktop
    // GL accepts them without checking (unlike OpenGL ES).
    const bool controlOnly = input.separable && stagePresent[static_cast<int>(Stage::TessControl)] && !tessellated &&
                             !stagePresent[static_cast<int>(Stage::Geometry)] && !stagePresent[static_cast<int>(Stage::Vertex)];
    if (!capture && !input.transformFeedbackVaryings.empty() && !controlOnly) {
        const bool geometry = stagePresent[static_cast<int>(Stage::Geometry)];
        if (!geometry && !tessellated && !stagePresent[static_cast<int>(Stage::Vertex)]) {
            result.log += "error: transform feedback varyings need a vertex or geometry shader\n";
            return result;
        }
        glslang::TIntermediate* last = program.getIntermediate(geometry      ? EShLangGeometry
                                                               : tessellated ? EShLangTessEvaluation
                                                                             : EShLangVertex);
        if (!layoutTransformFeedback(input.transformFeedbackVaryings, input.transformFeedbackMode,
                                     captureableOutputs(last), result, result.log))
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
        else liveBlocks.insert(unescapeName(topLevelName(block.name)));
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
    // Separable programs: varyings declared with layout(location) keep it
    // (reflection reports only the program's own inputs and outputs).
    std::set<std::string> explicitVaryings;
    std::map<std::string, int> explicitBindings;  // "block:<name>" or opaque uniform name -> binding
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
                const bool varying = (q.storage == glslang::EvqVaryingIn && s != static_cast<int>(Stage::Vertex)) ||
                                     (q.storage == glslang::EvqVaryingOut && s != static_cast<int>(Stage::Fragment));
                if (varying && q.hasLocation())
                    explicitVaryings.insert(unescapeName(type.getBasicType() == glslang::EbtBlock ? type.getTypeName().c_str()
                                                                                                    : symbol->getName().c_str()));
                if (q.storage == glslang::EvqUniform && q.hasBinding()) {
                    // layout(binding = N) (GL 4.2): the initial binding of blocks, samplers and images.
                    if (type.getBasicType() == glslang::EbtBlock)
                        explicitBindings["block:" + unescapeName(type.getTypeName().c_str())] = static_cast<int>(q.layoutBinding);
                    else if (type.getBasicType() == glslang::EbtSampler)
                        explicitBindings[unescapeName(symbol->getName().c_str())] = static_cast<int>(q.layoutBinding);
                }
                if (q.storage == glslang::EvqUniform) {
                    if (type.getBasicType() == glslang::EbtBlock) {
                        const bool anonymous = glslang::IsAnonymous(symbol->getName());
                        const std::string blockName = unescapeName(type.getTypeName().c_str());
                        collectBoolMembers(type, anonymous ? "" : blockName + ".", boolTypes);
                        if (blockName != kDefaultBlockName) {
                            auto [it, inserted] = blockAnonymous.emplace(blockName, anonymous);
                            if (!inserted && it->second != anonymous && interfaceError.empty())
                                interfaceError = "error: uniform block '" + blockName +
                                                 "' has an instance name in one stage but not in another\n";
                            if (anonymous)
                                for (const glslang::TTypeLoc& member : *type.getStruct())
                                    claimGlobal(unescapeName(member.type->getFieldName().c_str()), blockName);
                        } else {
                            // Loose uniforms gathered into the default block.
                            for (const glslang::TTypeLoc& member : *type.getStruct())
                                claimGlobal(unescapeName(member.type->getFieldName().c_str()), "");
                        }
                    } else {
                        collectBoolMembers(type, unescapeName(symbol->getName().c_str()), boolTypes, true);
                        claimGlobal(unescapeName(symbol->getName().c_str()), "");
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
        if (u.index == defaultBlockReflectionIndex && defaultBlockReflectionIndex >= 0) liveDefault.insert(unescapeName(topLevelName(u.name)));
        else if (u.index < 0) liveSamplers.insert(unescapeName(topLevelName(u.name)));
    }

    // Collect attributes/outputs from this pre-mapIO reflection (see below),
    // then assign locations for codegen.
    struct PipeVar { std::string name; GLenum type; int size; unsigned location; uint32_t stages; int index; bool array = false; };
    std::vector<PipeVar> pipeInputs, pipeOutputs;
    for (int i = 0; i < program.getNumPipeInputs(); ++i) {
        const auto& v = program.getPipeInput(i);
        pipeInputs.push_back({unescapeName(v.name), static_cast<GLenum>(v.glDefineType), v.size, v.layoutLocation(),
                              static_cast<uint32_t>(v.stages), 0, v.getType() && v.getType()->isArray()});
    }
    for (int i = 0; i < program.getNumPipeOutputs(); ++i) {
        const auto& v = program.getPipeOutput(i);
        const glslang::TType* type = v.getType();
        const int index = type && type->getQualifier().hasIndex() ? static_cast<int>(type->getQualifier().layoutIndex) : 0;
        pipeOutputs.push_back({unescapeName(v.name), static_cast<GLenum>(v.glDefineType), v.size, v.layoutLocation(),
                               static_cast<uint32_t>(v.stages), index});
    }
    for (const PipeVar& v : pipeInputs)
        if (v.location != kNoLocation && !(v.stages & EShLangVertexMask)) explicitVaryings.insert(v.name);
    for (const PipeVar& v : pipeOutputs)
        if (v.location != kNoLocation && !(v.stages & EShLangFragmentMask)) explicitVaryings.insert(v.name);
    // The GLSL I/O mapper matches varyings between stages by name (GL rules);
    // the default (Vulkan) mapper numbers each stage's interface in
    // declaration order, which swaps varyings declared in different orders.
    std::unique_ptr<glslang::TIoMapper> ioMapper(glslang::GetGlslIoMapper());
    std::unique_ptr<glslang::TIoMapResolver> resolver;
    for (int s = 0; s < kStageCount && !resolver; ++s)
        if (stagePresent[s]) resolver.reset(program.getGlslIoResolver(language(static_cast<Stage>(s))));
    if (!program.mapIO(resolver.get(), ioMapper.get())) {
        result.log += unescapeName(cleanLog(program.getInfoLog(), program.getInfoDebugLog()));
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
        if (input.separable && !input.internal)
            hashVaryingLocations(result.stages[s].spirv, static_cast<Stage>(s), explicitVaryings);
    }
    // SPIRV-Cross GLSL of a linked stage (the emulations rebuild stages from it).
    auto stageGlsl = [&](Stage stage, std::string& glsl) {
        try {
            spirv_cross::CompilerGLSL glslOut(result.stages[static_cast<int>(stage)].spirv);
            spirv_cross::CompilerGLSL::Options options = glslOut.get_common_options();
            options.version = 450;
            options.es = false;
            options.vulkan_semantics = false;
            // Row-major matrices load as in the geometry path (doubles need the plain form).
            options.enable_row_major_load_workaround = false;
            glslOut.set_common_options(options);
            glsl = glslOut.compile();
            return true;
        } catch (const std::exception& error) {
            result.log += std::string("error: shader translation failed: ") + error.what() + "\n";
            return false;
        }
    };
    // Separable programs without a vertex stage link without the emulation
    // (a program pipeline would have to assemble it from several programs).
    if (tessellation && tessellated && stagePresent[static_cast<int>(Stage::Vertex)]) {
        TessellationLink& t = *tessellation;
        std::string glsl;
        t.hasControl = result.stages[static_cast<int>(Stage::TessControl)].present;
        if (t.hasControl) {
            if (!stageGlsl(Stage::TessControl, glsl) || !parseTessellationGlsl(Stage::TessControl, glsl, t.control, result.log))
                return result;
        }
        if (!stageGlsl(Stage::TessEval, glsl) || !parseTessellationGlsl(Stage::TessEval, glsl, t.evaluation, result.log))
            return result;
        if (result.stages[static_cast<int>(Stage::Geometry)].present) {
            if (!stageGlsl(Stage::Geometry, t.geometryGlsl)) return result;
            // The geometry shader layout, for the program queries.
            GeometryShaderInfo info;
            if (!parseGeometryGlsl(t.geometryGlsl, info, result.log)) return result;
            result.geometryInputType = info.inputType;
            result.geometryOutputType = info.outputType;
            result.geometryVerticesOut = info.maxVertices;
        }
        if (!layoutTransformFeedback(tessellationCaptureVaryings(t.hasControl ? t.control : t.evaluation), GL_INTERLEAVED_ATTRIBS,
                                     captureableOutputs(program.getIntermediate(EShLangVertex)), t.capture, result.log,
                                     UINT32_MAX))
            return result;
    } else if (geometry && result.stages[static_cast<int>(Stage::Geometry)].present) {
        std::string glsl;
        try {
            spirv_cross::CompilerGLSL glslOut(result.stages[static_cast<int>(Stage::Geometry)].spirv);
            spirv_cross::CompilerGLSL::Options options = glslOut.get_common_options();
            options.version = 450;
            options.es = false;
            options.vulkan_semantics = false;
            // The GLSL is compiled again by glslang, not by a GL driver: no
            // row-major load workaround (it has no double-matrix overloads).
            options.enable_row_major_load_workaround = false;
            glslOut.set_common_options(options);
            glsl = glslOut.compile();
        } catch (const std::exception& error) {
            result.log += std::string("error: geometry shader translation failed: ") + error.what() + "\n";
            return result;
        }
        if (!parseGeometryGlsl(glsl, *geometry, result.log)) return result;
        if (!stagePresent[static_cast<int>(Stage::Vertex)] && !input.separable) {
            result.log += "error: a geometry shader requires a vertex shader\n";
            return result;
        }
        // A separable geometry-only program links; its emulation needs the
        // vertex stage and is not built (program pipelines then report it).
        if (stagePresent[static_cast<int>(Stage::Vertex)] &&
            !layoutTransformFeedback(geometryCaptureVaryings(*geometry), GL_INTERLEAVED_ATTRIBS,
                                     captureableOutputs(program.getIntermediate(EShLangVertex)), *geometryCapture, result.log,
                                     UINT32_MAX))
            return result;
        result.geometryInputType = geometry->inputType;
        result.geometryOutputType = geometry->outputType;
        result.geometryVerticesOut = geometry->maxVertices;
        result.geometryInvocations = geometry->invocations;
    }

    // Program-wide interface, built from the first stage that declares each resource.
    // Default uniform block members.
    std::map<std::string, int> blockIndexByName;
    std::map<std::string, int> samplerUniformByName, imageUniformByName;
    for (int s = 0; s < kStageCount; ++s) {
        if (!result.stages[s].present) continue;
        spirv_cross::Compiler compiler(result.stages[s].spirv);
        const spirv_cross::ShaderResources resources = compiler.get_shader_resources();
        for (const auto& ubo : resources.uniform_buffers) {
            const spirv_cross::SPIRType& type = compiler.get_type(ubo.base_type_id);
            const std::string blockName = unescapeName(compiler.get_name(ubo.base_type_id));
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
                    // An array of blocks binds consecutive points from its layout(binding).
                    if (auto b = explicitBindings.find("block:" + blockName); b != explicitBindings.end())
                        block.explicitBinding = b->second + static_cast<GLint>(element);
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
            const std::string name = unescapeName(compiler.get_name(image.id));
            if (!liveSamplers.count(topLevelName(name))) continue;
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
                if (auto b = explicitBindings.find(name); b != explicitBindings.end()) u.explicitBinding = b->second;
                samplerUniformByName[name] = static_cast<int>(result.uniforms.size());
                result.uniforms.push_back(u);
            } else {
                result.uniforms[it->second].stageMask |= 1u << s;
            }
        }
        // Images (GL 4.2 image load/store): uniforms whose values are image units.
        for (const auto& image : resources.storage_images) {
            const spirv_cross::SPIRType& type = compiler.get_type(image.type_id);
            const spirv_cross::SPIRType& base = compiler.get_type(image.base_type_id);
            const std::string name = unescapeName(compiler.get_name(image.id));
            if (!liveSamplers.count(topLevelName(name))) continue;
            const GLenum glType = imageTypeOf(base, compiler.get_type(base.image.type).basetype);
            const uint32_t count = type.array.empty() ? 1 : std::max(1u, type.array.back());
            auto it = imageUniformByName.find(name);
            if (it != imageUniformByName.end()) {
                result.uniforms[it->second].stageMask |= 1u << s;
                continue;
            }
            UniformInfo u;
            u.name = type.array.empty() ? name : name + "[0]";
            u.type = glType;
            u.size = static_cast<GLint>(count);
            u.imageSlot = static_cast<int>(result.images.size());
            u.stageMask = 1u << s;
            if (auto b = explicitBindings.find(name); b != explicitBindings.end()) u.explicitBinding = b->second;
            for (uint32_t e = 0; e < count; ++e) {
                ImageSlotInfo slot;
                slot.type = glType;
                slot.format = imageFormatOf(base.image.format);
                result.images.push_back(slot);
            }
            imageUniformByName[name] = static_cast<int>(result.uniforms.size());
            result.uniforms.push_back(u);
        }
        // Atomic counters (GL 4.2): the storage blocks glslang made of them.
        for (const auto& ssbo : resources.storage_buffers) {
            const int binding = atomicCounterBlockBinding(compiler.get_name(ssbo.base_type_id));
            if (binding < 0) continue;
            auto buffer = std::find_if(result.atomicBuffers.begin(), result.atomicBuffers.end(),
                                       [&](const AtomicCounterBufferInfo& b) { return b.binding == static_cast<GLuint>(binding); });
            if (buffer == result.atomicBuffers.end()) {
                AtomicCounterBufferInfo info;
                info.binding = static_cast<GLuint>(binding);
                result.atomicBuffers.push_back(info);
                buffer = result.atomicBuffers.end() - 1;
            }
            const int bufferIndex = static_cast<int>(buffer - result.atomicBuffers.begin());
            buffer->stageMask |= 1u << s;
            const spirv_cross::SPIRType& type = compiler.get_type(ssbo.base_type_id);
            for (uint32_t m = 0; m < type.member_types.size(); ++m) {
                const spirv_cross::SPIRType& memberType = compiler.get_type(type.member_types[m]);
                const std::string name = unescapeName(compiler.get_member_name(type.self, m));
                const uint32_t offset = compiler.type_struct_member_offset(type, m);
                const uint32_t count = memberType.array.empty() ? 1 : std::max(1u, memberType.array.back());
                buffer->dataSize = std::max<GLint>(buffer->dataSize, static_cast<GLint>(offset + 4 * count));
                const std::string glName = memberType.array.empty() ? name : name + "[0]";
                auto it = std::find_if(result.uniforms.begin(), result.uniforms.end(),
                                       [&](const UniformInfo& u) { return u.name == glName && u.atomicBufferIndex >= 0; });
                if (it != result.uniforms.end()) {
                    it->stageMask |= 1u << s;
                    continue;
                }
                UniformInfo u;
                u.name = glName;
                u.type = GL_UNSIGNED_INT_ATOMIC_COUNTER;
                u.size = static_cast<GLint>(count);
                u.offset = static_cast<GLint>(offset);
                u.arrayStride = memberType.array.empty() ? 0 : 4;
                u.atomicBufferIndex = bufferIndex;
                u.stageMask = 1u << s;
                buffer->activeUniforms.push_back(static_cast<int>(result.uniforms.size()));
                result.uniforms.push_back(u);
            }
        }
    }

    // Shader subroutines (shader/subroutines.h): each stage's functions and
    // subroutine uniforms, whose selections live in hidden default-block
    // uniforms that are not part of the GL interface.
    for (const ShaderSource& shader : input.shaders) {
        const SubroutineLowering lowered = lowerSubroutines(shader.stage, shader.source);
        SubroutineStageInfo& info = result.subroutines[static_cast<int>(shader.stage)];
        for (const std::string& type : lowered.info.types)
            if (std::find(info.types.begin(), info.types.end(), type) == info.types.end()) info.types.push_back(type);
        for (const auto& f : lowered.info.functions)
            if (std::none_of(info.functions.begin(), info.functions.end(), [&](const auto& g) { return g.name == f.name; }))
                info.functions.push_back(f);
        for (const auto& u : lowered.info.uniforms)
            if (std::none_of(info.uniforms.begin(), info.uniforms.end(), [&](const auto& v) { return v.name == u.name; }))
                info.uniforms.push_back(u);
    }
    {
        std::vector<UniformInfo> kept;
        std::vector<int> map(result.uniforms.size(), -1);
        for (size_t i = 0; i < result.uniforms.size(); ++i) {
            const UniformInfo& u = result.uniforms[i];
            if (u.blockIndex < 0 && u.name.rfind(kBuiltinUniformPrefix, 0) == 0) {
                // Hidden built-in uniforms (lowerBuiltinUniforms).
                const std::string name = u.name.substr(std::string(kBuiltinUniformPrefix).size());
                if (name == "DepthRange.near") result.builtinUniforms.depthRange[0] = static_cast<int>(u.offset);
                if (name == "DepthRange.far") result.builtinUniforms.depthRange[1] = static_cast<int>(u.offset);
                if (name == "DepthRange.diff") result.builtinUniforms.depthRange[2] = static_cast<int>(u.offset);
                if (name == "NumSamples") result.builtinUniforms.numSamples = static_cast<int>(u.offset);
                continue;
            }
            if (u.blockIndex < 0 && isSubroutineSelectionUniform(u.name)) {
                for (int st = 0; st < kStageCount; ++st)
                    for (auto& su : result.subroutines[st].uniforms)
                        if (arrayBaseName(u.name) == subroutineSelectionUniform(static_cast<Stage>(st), su.name)) {
                            su.selectionOffset = u.offset;
                            su.selectionStride = u.arrayStride ? u.arrayStride : 16;
                        }
                continue;
            }
            map[i] = static_cast<int>(kept.size());
            kept.push_back(u);
        }
        if (kept.size() != result.uniforms.size()) {
            for (UniformBlockInfo& b : result.blocks)
                for (int& u : b.activeUniforms) u = map[static_cast<size_t>(u)];
            for (AtomicCounterBufferInfo& b : result.atomicBuffers)
                for (int& u : b.activeUniforms) u = map[static_cast<size_t>(u)];
            for (auto& [name, index] : samplerUniformByName) index = map[static_cast<size_t>(index)];
            for (auto& [name, index] : imageUniformByName) index = map[static_cast<size_t>(index)];
            result.uniforms = std::move(kept);
        }
    }
    for (SubroutineStageInfo& info : result.subroutines) {
        // Only uniforms the stage uses are active; their locations: explicit
        // ones first, then the lowest free ones.
        info.uniforms.erase(std::remove_if(info.uniforms.begin(), info.uniforms.end(),
                                           [](const SubroutineStageInfo::Uniform& u) { return u.selectionOffset < 0; }),
                            info.uniforms.end());
        std::set<GLint> usedLocations;
        for (const auto& u : info.uniforms)
            for (int e = 0; u.location >= 0 && e < u.elements(); ++e) usedLocations.insert(u.location + e);
        for (auto& u : info.uniforms) {
            if (u.location >= 0) continue;
            GLint location = 0;
            for (;; ++location) {
                bool free = true;
                for (int e = 0; e < u.elements(); ++e) free = free && !usedLocations.count(location + e);
                if (free) break;
            }
            u.location = location;
            for (int e = 0; e < u.elements(); ++e) usedLocations.insert(location + e);
        }
        info.locationCount = usedLocations.empty() ? 0 : *usedLocations.rbegin() + 1;
    }

    // Assign default-block and sampler uniform locations (consecutive per element).
    GLint nextLocation = 0;
    for (size_t i = 0; i < result.uniforms.size(); ++i) {
        UniformInfo& u = result.uniforms[i];
        if (u.blockIndex >= 0 || u.atomicBufferIndex >= 0) continue;  // atomic counters have no location
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
        a.array = in.array;
        a.locationCount = glTypeLocations(a.type) * a.size;
        a.location = in.location != kNoLocation ? static_cast<GLint>(in.location) : -1;
        // Lowered legacy attributes (gl_Vertex, ...) read the conventional arrays.
        const int conventional = conventionalAttribLocation(a.name);
        if (conventional >= 0) {
            a.location = conventional;
            a.conventional = true;
        }
        result.attribs.push_back(a);
    }
    // Explicit locations from source were already applied and take
    // precedence over glBindAttribLocation; mark them used. Then
    // glBindAttribLocation, then the lowest free locations.
    std::vector<int> needsLocation;
    for (size_t i = 0; i < result.attribs.size(); ++i) {
        AttribInfo& a = result.attribs[i];
        if (a.conventional) continue;
        if (a.location < 0) {
            auto bound = input.attribBindings.find(a.name);
            if (bound == input.attribBindings.end()) {
                needsLocation.push_back(static_cast<int>(i));
                continue;
            }
            a.location = static_cast<GLint>(bound->second);
        }
        for (int l = 0; l < a.locationCount && a.location + l < 64; ++l) usedLocations[a.location + l] = true;
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
    int attribSlots = 0;
    for (const AttribInfo& a : result.attribs) {
        if (!a.conventional && a.location + a.locationCount > 16) {
            result.log += "error: vertex attribute '" + a.name + "' uses a location beyond GL_MAX_VERTEX_ATTRIBS\n";
            return result;
        }
        if (!a.conventional) attribSlots += glTypeAttribSlots(a.type) * a.size;
    }
    // dvec3 and dvec4 columns count twice (GL_ARB_vertex_attrib_64bit).
    if (attribSlots > 16) {
        result.log += "error: vertex attributes exceed GL_MAX_VERTEX_ATTRIBS\n";
        return result;
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
            f.index = out.index;  // layout(index = N) (GL_ARB_blend_func_extended)
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

    // Metal translation per stage. The vertex stage is translated twice: the
    // second variant returns void, as Metal requires when rasterization is
    // disabled (GL_RASTERIZER_DISCARD, transform feedback capture passes).
    // The geometry stage has no Metal equivalent; link() emulates it.
    for (int s = 0; s < kStageCount; ++s)
    for (int variant = 0; variant < (s == static_cast<int>(Stage::Vertex) ? 2 : 1); ++variant) {
        StageOutput& stage = result.stages[s];
        if (!stage.present || s == static_cast<int>(Stage::Geometry) || s == static_cast<int>(Stage::TessControl) ||
            s == static_cast<int>(Stage::TessEval))
            continue;
        const bool noRaster = variant == 1;
        try {
            // Doubles become soft-float code (fp64.h); no-op without them.
            std::vector<uint32_t> spirv = cubeStorageImagesAs2DArrays(lowerGatherOffsets(rectImagesAs2D(stage.spirv)));
            if (!lowerFp64(spirv, result.log)) return result;
            spirv_cross::CompilerMSL msl(std::move(spirv));
            spirv_cross::CompilerMSL::Options mslOptions;
            mslOptions.disable_rasterization = noRaster;
            mslOptions.platform = spirv_cross::CompilerMSL::Options::macOS;
            mslOptions.set_msl_version(3, 1);      // texture atomics (macOS 14+; the floor is macOS 15)
            mslOptions.native_texture_atomics = true;
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
                const std::string blockName = unescapeName(msl.get_name(ubo.base_type_id));
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
            // Transform feedback capture and geometry emulation buffers
            // (see transform_feedback.h, geometry.h).
            static const std::map<std::string, uint32_t> kInternalBuffers = {
                {"wmgl_XfbControl", kSlotXfbControl},      {"wmgl_GsInput", kSlotFirstXfbBuffer},
                {"wmgl_GsOutput", kSlotFirstXfbBuffer + 1}, {"wmgl_GsIndices", kSlotFirstXfbBuffer + 2},
                {"wmgl_GsControl", kSlotFirstXfbBuffer + 3}, {"wmgl_GsVertices", kSlotFirstXfbBuffer},
                {"wmgl_TcsInput", kSlotFirstXfbBuffer}, {"wmgl_TcsOutput", kSlotFirstXfbBuffer + 1},
                {"wmgl_TcsControl", kSlotFirstXfbBuffer + 2}, {"wmgl_TesVertices", kSlotTessVertices},
                {"wmgl_TesPatches", kSlotTessPatches}, {"wmgl_TesControl", kSlotTessControl},
            };
            uint32_t nextAtomicSlot = kSlotFirstAtomicCounterBuffer;
            for (const auto& ssbo : resources.storage_buffers) {
                const std::string blockName = msl.get_name(ssbo.base_type_id);
                spirv_cross::MSLResourceBinding b;
                b.stage = model;
                msl.set_decoration(ssbo.id, spv::DecorationBinding, uniqueBinding++);
                b.desc_set = msl.get_decoration(ssbo.id, spv::DecorationDescriptorSet);
                b.binding = msl.get_decoration(ssbo.id, spv::DecorationBinding);
                const int counterBinding = atomicCounterBlockBinding(blockName);
                if (auto internal = kInternalBuffers.find(blockName); internal != kInternalBuffers.end()) b.msl_buffer = internal->second;
                else if (blockName.rfind("wmgl_XfbData", 0) == 0) b.msl_buffer = kSlotFirstXfbBuffer + std::stoul(blockName.substr(12));
                else if (counterBinding >= 0) {
                    const bool vertexStage = s == static_cast<int>(Stage::Vertex);
                    if (!vertexStage && s != static_cast<int>(Stage::Fragment) && s != static_cast<int>(Stage::Compute)) {
                        result.log += "error: atomic counters are not supported in this shader stage\n";
                        return result;
                    }
                    const uint32_t slotEnd = vertexStage ? kSlotVertexAtomicCounterBuffer + kMaxVertexAtomicCounterBuffers
                                                         : kSlotFirstAtomicCounterBuffer + kMaxStageAtomicCounterBuffers;
                    if (vertexStage && nextAtomicSlot < kSlotVertexAtomicCounterBuffer) nextAtomicSlot = kSlotVertexAtomicCounterBuffer;
                    if (nextAtomicSlot >= slotEnd) {
                        result.log += "error: too many atomic counter buffers in one stage\n";
                        return result;
                    }
                    b.msl_buffer = nextAtomicSlot++;
                    for (AtomicCounterBufferInfo& buffer : result.atomicBuffers)
                        if (buffer.binding == static_cast<GLuint>(counterBinding)) buffer.stageSlot[s] = static_cast<int>(b.msl_buffer);
                } else {
                    result.log += "error: shader storage blocks are not supported\n";
                    return result;
                }
                msl.add_msl_resource_binding(b);
            }
            // Samplers.
            uint32_t nextTexture = 0;
            for (const auto& image : resources.sampled_images) {
                const std::string name = unescapeName(msl.get_name(image.id));
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
            // Images: textures kSlotFirstImage.. (read_write textures).
            uint32_t nextImage = kSlotFirstImage;
            for (const auto& image : resources.storage_images) {
                const std::string name = unescapeName(msl.get_name(image.id));
                const spirv_cross::SPIRType& type = msl.get_type(image.type_id);
                const uint32_t count = type.array.empty() ? 1 : std::max(1u, type.array.back());
                msl.set_decoration(image.id, spv::DecorationBinding, uniqueBinding++);
                spirv_cross::MSLResourceBinding b;
                b.stage = model;
                b.desc_set = msl.get_decoration(image.id, spv::DecorationDescriptorSet);
                b.binding = msl.get_decoration(image.id, spv::DecorationBinding);
                b.count = count;
                b.msl_texture = nextImage;
                msl.add_msl_resource_binding(b);
                if (auto it = imageUniformByName.find(name); it != imageUniformByName.end()) {
                    const int first = result.uniforms[it->second].imageSlot;
                    for (uint32_t e = 0; e < count; ++e) result.images[first + e].stageTexture[s] = static_cast<int>(nextImage + e);
                }
                nextImage += count;
                if (nextImage > kSlotFirstImage + kMaxStageImages) {
                    result.log += "error: too many images in one stage\n";
                    return result;
                }
            }
            // Vertex inputs: GL locations.
            if (s == static_cast<int>(Stage::Vertex)) {
                if (noRaster) stage.vertexInputs.clear();  // rebuilt identically below
                // 64-bit inputs come split into one Metal input per 16 bytes
                // ("<name>_wmgl_<column>_<part>", fp64.h): part 0 of column
                // k reads at GL location + k, part 1 at a free Metal index.
                std::set<GLint> usedAttributes;
                for (const AttribInfo& a : result.attribs)
                    for (int l = 0; l < a.locationCount; ++l) usedAttributes.insert(a.location + l);
                GLint nextFree = 30;
                for (const auto& in : resources.stage_inputs) {
                    std::string split = unescapeName(msl.get_name(in.id));
                    const size_t marker = split.rfind("_wmgl_");
                    if (marker == std::string::npos || split.find('_', marker + 6) == std::string::npos) continue;
                    const int column = std::atoi(split.c_str() + marker + 6);
                    const int part = std::atoi(split.c_str() + split.find('_', marker + 6) + 1);
                    const std::string base = split.substr(0, marker);
                    auto it = std::find_if(result.attribs.begin(), result.attribs.end(),
                                           [&](const AttribInfo& a) { return a.name == base; });
                    if (it == result.attribs.end()) continue;
                    const spirv_cross::SPIRType& type = msl.get_type(in.type_id);
                    const GLint attrib = it->location + column;
                    GLint metal = attrib;
                    if (part == 1) {
                        while (nextFree >= 0 && usedAttributes.count(nextFree)) --nextFree;
                        if (nextFree < 0) {
                            result.log += "error: too many 64-bit vertex attribute components\n";
                            return result;
                        }
                        metal = nextFree--;
                    }
                    msl.set_decoration(in.id, spv::DecorationLocation, static_cast<uint32_t>(metal));
                    stage.vertexInputs.push_back({metal, static_cast<int>(type.vecsize), 'd', attrib, part});
                }
                for (const auto& in : resources.stage_inputs) {
                    const std::string name = unescapeName(msl.get_name(in.id));
                    if (name.rfind("_wmgl_") != std::string::npos && name.find('_', name.rfind("_wmgl_") + 6) != std::string::npos)
                        continue;
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
                    const std::string name = unescapeName(msl.get_name(out.id));
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
            if (noRaster) {
                stage.mslNoRaster = finishFp64Msl(msl.compile());
                if (const char* v = std::getenv("WINEMETALGL_DUMP_SHADERS"); v && v[0] == '1')
                    std::fprintf(stderr, "winemetalgl: MSL vertex stage without rasterization:\n%s\n", stage.mslNoRaster.c_str());
                instanceIdFromZero(stage.mslNoRaster);
                continue;
            }
            stage.msl = finishFp64Msl(msl.compile());
            if (s == static_cast<int>(Stage::Vertex)) {
                ensureVertexOutput(stage.msl);
                addPointSize(stage.msl);
                instanceIdFromZero(stage.msl);
            }
            if (s == static_cast<int>(Stage::Fragment)) {
                stage.usesPointCoord = patchPointCoord(stage.msl);
                stage.flatInputs = hasFlatInputs(stage.msl);
            }
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

    hideFixedFunctionBlock(result);
    applyUniformInitializers(input.shaders, result);
    result.ok = true;
    return result;
}

namespace {

// Copies stage `from`'s uniform block and sampler slots of `source` (matched
// by name) into stage `to` of `result`.
void copyStageSlots(const LinkResult& source, int from, LinkResult& result, int to) {
    for (UniformBlockInfo& block : result.blocks)
        for (const UniformBlockInfo& other : source.blocks)
            if (other.name == block.name) block.stageSlot[to] = other.stageSlot[from];
    for (AtomicCounterBufferInfo& buffer : result.atomicBuffers)
        for (const AtomicCounterBufferInfo& other : source.atomicBuffers)
            if (other.binding == buffer.binding) buffer.stageSlot[to] = other.stageSlot[from];
    for (const UniformInfo& u : result.uniforms) {
        if (u.imageSlot < 0) continue;
        for (const UniformInfo& other : source.uniforms)
            if (other.name == u.name && other.imageSlot >= 0)
                for (GLint e = 0; e < std::min(u.size, other.size); ++e)
                    result.images[u.imageSlot + e].stageTexture[to] = source.images[other.imageSlot + e].stageTexture[from];
    }
    for (const UniformInfo& u : result.uniforms) {
        if (u.samplerSlot < 0) continue;
        for (const UniformInfo& other : source.uniforms)
            if (other.name == u.name && other.samplerSlot >= 0)
                for (GLint e = 0; e < std::min(u.size, other.size); ++e)
                    result.samplers[u.samplerSlot + e].stageTexture[to] = source.samplers[other.samplerSlot + e].stageTexture[from];
    }
}

// Relinks the program with capture code in its vertex shader and makes that
// the program's vertex stage.
bool useVertexCapture(const LinkInput& input, const LinkResult& layout, LinkResult& result) {
    int version = 140;
    for (const ShaderSource& shader : input.shaders)
        if (shader.stage == Stage::Vertex) version = std::max(version, parseGlslVersion(shader.source).number);
    const CaptureCode code = vertexCaptureCode(layout, version);
    LinkInput again = input;
    again.transformFeedbackVaryings.clear();
    LinkResult captured = linkImpl(again, &code);
    if (!captured.ok) {
        result.log += "error: internal error building vertex output capture:\n" + captured.log;
        return false;
    }
    const int vs = static_cast<int>(Stage::Vertex);
    result.stages[vs] = captured.stages[vs];
    copyStageSlots(captured, vs, result, vs);
    return true;
}

bool buildGeometryEmulation(const LinkInput& input, const GeometryShaderInfo& info, const LinkResult& captureLayout,
                            LinkResult& result) {
    if (!useVertexCapture(input, captureLayout, result)) return false;
    const GeometrySources sources = geometrySources(info, captureLayout);
    GeometryEmulation& g = result.geometry;
    g.inputType = info.inputType;
    g.outputType = info.outputType;
    g.verticesIn = info.verticesIn;
    g.maxVertices = info.maxVertices;
    g.invocations = info.invocations;
    g.inputStride = captureLayout.xfbStride[0];
    g.outputStride = sources.outputStride;
    g.outputs = sources.fields;
    for (TransformFeedbackVarying& v : result.transformFeedback) {
        if (v.type == GL_NONE) continue;
        const size_t open = v.name.find('[');
        // The kernel's GLSL names anonymous block members "Block.member".
        const std::string base = v.source.empty() ? v.name.substr(0, open) : v.source;
        const int index = open == std::string::npos ? 0 : std::atoi(v.name.c_str() + open + 1);
        v.sourceOffset = TransformFeedbackVarying::kNotEmitted;
        for (const GeometryEmulation::Field& f : g.outputs)
            if (f.name == base) v.sourceOffset = f.offset + static_cast<uint32_t>(index * glTypeWords(f.type));
    }
    LinkInput kernelInput;
    kernelInput.shaders = {{Stage::Compute, sources.compute}};
    kernelInput.internal = true;
    LinkResult kernel = linkImpl(kernelInput, nullptr);
    if (!kernel.ok) {
        result.log += "error: internal error building the geometry shader kernel:\n" + kernel.log;
        return false;
    }
    const int cs = static_cast<int>(Stage::Compute);
    g.kernel = kernel.stages[cs];
    copyStageSlots(kernel, cs, result, cs);
    LinkInput passthroughInput;
    passthroughInput.separable = true;
    // A separable program's geometry outputs have locations hashed from
    // their names: the pass-through declares them without and is hashed
    // the same way.
    passthroughInput.internal = !input.separable;
    passthroughInput.shaders = {{Stage::Vertex, input.separable ? stripHashedLocations(sources.passthrough) : sources.passthrough}};
    LinkResult passthrough = linkImpl(passthroughInput, nullptr);
    if (!passthrough.ok) {
        result.log += "error: internal error building the geometry shader output stage:\n" + passthrough.log;
        return false;
    }
    g.passthrough = passthrough.stages[static_cast<int>(Stage::Vertex)];
    g.active = true;
    static const bool dump = [] {
        const char* v = std::getenv("WINEMETALGL_DUMP_SHADERS");
        return v && v[0] == '1';
    }();
    if (dump) std::fprintf(stderr, "winemetalgl: geometry kernel GLSL:\n%s\npass-through GLSL:\n%s\n", sources.compute.c_str(),
                           sources.passthrough.c_str());
    return true;
}

bool buildTessellationEmulation(const LinkInput& input, const TessellationLink& t, LinkResult& result) {
    if (!useVertexCapture(input, t.capture, result)) return false;
    TessellationEmulation& e = result.tess;
    e.inputStride = t.capture.xfbStride[0];
    static const bool dump = [] {
        const char* v = std::getenv("WINEMETALGL_DUMP_SHADERS");
        return v && v[0] == '1';
    }();
    TessControlSources control;
    if (t.hasControl) {
        control = tessControlSources(t.control, t.capture);
        e.vertexWords = control.vertexWords;
        e.patchWords = control.patchWords;
        if (dump) std::fprintf(stderr, "winemetalgl: tessellation control kernel GLSL:\n%s\n", control.kernel.c_str());
        LinkInput kernelInput;
        kernelInput.shaders = {{Stage::Compute, control.kernel}};
        kernelInput.internal = true;
        LinkResult kernel = linkImpl(kernelInput, nullptr);
        if (!kernel.ok) {
            result.log += "error: internal error building the tessellation control kernel:\n" + kernel.log;
            return false;
        }
        const int cs = static_cast<int>(Stage::Compute);
        e.control = kernel.stages[cs];
        copyStageSlots(kernel, cs, result, static_cast<int>(Stage::TessControl));
    }
    std::string log;
    const std::string vertex = tessEvaluationVertexShader(t.evaluation, t.hasControl ? &control : nullptr, &t.capture, log);
    if (vertex.empty()) {
        result.log += log;
        return false;
    }
    if (dump) std::fprintf(stderr, "winemetalgl: tessellation evaluation vertex GLSL:\n%s\n", vertex.c_str());
    LinkInput evaluationInput;
    evaluationInput.shaders = {{Stage::Vertex, vertex}};
    if (!t.geometryGlsl.empty()) evaluationInput.shaders.push_back({Stage::Geometry, t.geometryGlsl});
    // Varyings by their qualified names: the geometry stage here is GLSL
    // translated by SPIRV-Cross, which names anonymous blocks.
    for (const TransformFeedbackVarying& v : result.transformFeedback) {
        const size_t open = v.name.find('[');
        evaluationInput.transformFeedbackVaryings.push_back(
            v.source.empty() || v.source == v.name.substr(0, open) ? v.name
                                                                   : v.source + (open == std::string::npos ? "" : v.name.substr(open)));
    }
    evaluationInput.transformFeedbackMode = input.transformFeedbackMode;
    evaluationInput.internal = true;
    auto evaluation = std::make_shared<LinkResult>(link(evaluationInput));
    if (!evaluation->ok) {
        result.log += "error: internal error building the tessellation evaluation stage:\n" + evaluation->log;
        return false;
    }
    e.evaluation = std::move(evaluation);
    e.active = true;
    return true;
}

}  // namespace

LinkResult link(const LinkInput& input) {
    GeometryShaderInfo geometry;
    LinkResult geometryCapture;
    TessellationLink tessellation;
    LinkResult result = linkImpl(input, nullptr, &geometry, &geometryCapture, &tessellation);
    if (!result.ok) return result;
    if (result.stages[static_cast<int>(Stage::TessEval)].present) {
        // The evaluation executable captures transform feedback and runs the
        // geometry shader (if any); the program's vertex stage feeds patches.
        if (result.stages[static_cast<int>(Stage::Vertex)].present && !buildTessellationEmulation(input, tessellation, result))
            result.ok = false;
        return result;
    }
    if (result.stages[static_cast<int>(Stage::TessControl)].present) return result;  // separable control-only program
    if (result.stages[static_cast<int>(Stage::Geometry)].present) {
        if (!result.stages[static_cast<int>(Stage::Vertex)].present) return result;  // separable, geometry only
        // Transform feedback (if any) captures the emitted vertices; the draw
        // path reads them back from the kernel output (DrawState).
        if (!buildGeometryEmulation(input, geometry, geometryCapture, result)) result.ok = false;
        return result;
    }
    if (result.transformFeedback.empty()) return result;
    if (!useVertexCapture(input, result, result)) {
        result.ok = false;
        return result;
    }
    result.xfbVertexCapture = true;
    return result;
}

}  // namespace wmgl::shader
