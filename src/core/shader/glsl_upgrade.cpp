#include "core/shader/glsl_upgrade.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <sstream>

namespace wmgl::shader {
namespace {

bool isIdentStart(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
bool isIdent(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

// Compatibility built-ins that become declared variables. The declarations
// use the same names with a wmgl_ prefix; the compatibility profile (W22)
// binds the inputs to the conventional vertex arrays and fills the uniform
// block from fixed-function state.
struct BuiltinDecl {
    const char* name;
    const char* decl;   // declaration of wmgl_<name>
    int stages;         // bitmask of Stage values where it applies
};
constexpr int kVS = 1 << static_cast<int>(Stage::Vertex);
constexpr int kFS = 1 << static_cast<int>(Stage::Fragment);
constexpr int kGS = 1 << static_cast<int>(Stage::Geometry);

const BuiltinDecl kVertexInputs[] = {
    {"gl_Vertex", "in vec4 wmgl_Vertex;", kVS},
    {"gl_Normal", "in vec3 wmgl_Normal;", kVS},
    {"gl_Color", "in vec4 wmgl_Color;", kVS},
    {"gl_SecondaryColor", "in vec4 wmgl_SecondaryColor;", kVS},
    {"gl_FogCoord", "in float wmgl_FogCoord;", kVS},
    {"gl_MultiTexCoord0", "in vec4 wmgl_MultiTexCoord0;", kVS},
    {"gl_MultiTexCoord1", "in vec4 wmgl_MultiTexCoord1;", kVS},
    {"gl_MultiTexCoord2", "in vec4 wmgl_MultiTexCoord2;", kVS},
    {"gl_MultiTexCoord3", "in vec4 wmgl_MultiTexCoord3;", kVS},
    {"gl_MultiTexCoord4", "in vec4 wmgl_MultiTexCoord4;", kVS},
    {"gl_MultiTexCoord5", "in vec4 wmgl_MultiTexCoord5;", kVS},
    {"gl_MultiTexCoord6", "in vec4 wmgl_MultiTexCoord6;", kVS},
    {"gl_MultiTexCoord7", "in vec4 wmgl_MultiTexCoord7;", kVS},
};

// Varyings between vertex and fragment stages.
const BuiltinDecl kVaryings[] = {
    {"gl_FrontColor", "out vec4 wmgl_FrontColor;", kVS},
    {"gl_BackColor", "out vec4 wmgl_BackColor;", kVS},
    {"gl_FrontSecondaryColor", "out vec4 wmgl_FrontSecondaryColor;", kVS},
    {"gl_BackSecondaryColor", "out vec4 wmgl_BackSecondaryColor;", kVS},
    {"gl_FogFragCoord", "out float wmgl_FogFragCoord;", kVS},
    {"gl_ClipVertex", "vec4 wmgl_ClipVertex;", kVS},
};

const char* kFixedFunctionUniforms[] = {
    "gl_ModelViewMatrix", "gl_ProjectionMatrix", "gl_ModelViewProjectionMatrix", "gl_TextureMatrix",
    "gl_NormalMatrix", "gl_ModelViewMatrixInverse", "gl_ProjectionMatrixInverse",
    "gl_ModelViewProjectionMatrixInverse", "gl_TextureMatrixInverse", "gl_ModelViewMatrixTranspose",
    "gl_ProjectionMatrixTranspose", "gl_ModelViewProjectionMatrixTranspose", "gl_TextureMatrixTranspose",
    "gl_ModelViewMatrixInverseTranspose", "gl_ProjectionMatrixInverseTranspose",
    "gl_ModelViewProjectionMatrixInverseTranspose", "gl_TextureMatrixInverseTranspose", "gl_NormalScale",
    "gl_ClipPlane", "gl_Point", "gl_FrontMaterial", "gl_BackMaterial", "gl_LightSource", "gl_LightModel",
    "gl_FrontLightModelProduct", "gl_BackLightModelProduct", "gl_FrontLightProduct", "gl_BackLightProduct",
    "gl_TextureEnvColor", "gl_EyePlaneS", "gl_EyePlaneT", "gl_EyePlaneR", "gl_EyePlaneQ", "gl_ObjectPlaneS",
    "gl_ObjectPlaneT", "gl_ObjectPlaneR", "gl_ObjectPlaneQ", "gl_Fog",
};

}  // namespace

GlslVersion parseGlslVersion(const std::string& source) {
    GlslVersion v;
    size_t i = 0;
    const size_t n = source.size();
    // Skip whitespace and comments before #version.
    while (i < n) {
        if (std::isspace(static_cast<unsigned char>(source[i]))) { ++i; continue; }
        if (source.compare(i, 2, "//") == 0) { while (i < n && source[i] != '\n') ++i; continue; }
        if (source.compare(i, 2, "/*") == 0) {
            const size_t end = source.find("*/", i + 2);
            i = end == std::string::npos ? n : end + 2;
            continue;
        }
        break;
    }
    if (i < n && source[i] == '#') {
        size_t j = i + 1;
        while (j < n && (source[j] == ' ' || source[j] == '\t')) ++j;
        if (source.compare(j, 7, "version") == 0) {
            j += 7;
            const size_t lineEnd = source.find('\n', j);
            std::istringstream line(source.substr(j, lineEnd == std::string::npos ? std::string::npos : lineEnd - j));
            int number = 0;
            std::string profile;
            line >> number >> profile;
            if (number) v.number = number;
            v.es = profile == "es";
            v.compatibility = profile == "compatibility";
        }
    }
    if (v.number <= 130 && !v.es) v.compatibility = true;
    return v;
}

UpgradeResult upgradeLegacyGlsl(Stage stage, const std::string& source) {
    UpgradeResult result;
    const GlslVersion version = parseGlslVersion(source);
    if (!version.compatibility && version.number >= 140) {
        result.source = source;
        return result;
    }
    const int stageBit = 1 << static_cast<int>(stage);
    const bool vertex = stage == Stage::Vertex, fragment = stage == Stage::Fragment;
    const int newVersion = version.number < 140 ? 140 : version.number;

    std::map<std::string, std::string> rename;
    if (vertex) {
        rename["attribute"] = "in";
        rename["varying"] = "out";
    } else if (fragment) {
        rename["varying"] = "in";
    } else if (stage == Stage::Geometry) {
        rename["varying"] = "out";
    }

    // GLSL 1.x texture functions. Under the GLSL 1.40+ rules used for codegen
    // texture2D etc. are type names, so calls must use the generic functions.
    static const std::map<std::string, std::string> kTextureFunctions = {
        {"texture1D", "texture"}, {"texture2D", "texture"}, {"texture3D", "texture"}, {"textureCube", "texture"},
        {"texture2DRect", "texture"}, {"texture1DProj", "textureProj"}, {"texture2DProj", "textureProj"},
        {"texture3DProj", "textureProj"}, {"texture2DRectProj", "textureProj"}, {"texture1DLod", "textureLod"},
        {"texture2DLod", "textureLod"}, {"texture3DLod", "textureLod"}, {"textureCubeLod", "textureLod"},
        {"texture1DProjLod", "textureProjLod"}, {"texture2DProjLod", "textureProjLod"},
        {"texture3DProjLod", "textureProjLod"}, {"texture1DGradARB", "textureGrad"}, {"texture2DGradARB", "textureGrad"},
        {"texture3DGradARB", "textureGrad"}, {"textureCubeGradARB", "textureGrad"},
        {"shadow1D", "wmgl_shadow"}, {"shadow2D", "wmgl_shadow"}, {"shadow2DRect", "wmgl_shadow"},
        {"shadow1DProj", "wmgl_shadowProj"}, {"shadow2DProj", "wmgl_shadowProj"}, {"shadow2DRectProj", "wmgl_shadowProj"},
        {"shadow1DLod", "wmgl_shadowLod"}, {"shadow2DLod", "wmgl_shadowLod"},
        {"shadow1DProjLod", "wmgl_shadowProjLod"}, {"shadow2DProjLod", "wmgl_shadowProjLod"},
    };
    for (const auto& [from, to] : kTextureFunctions) rename[from] = to;

    std::set<std::string> used;
    std::string out;
    out.reserve(source.size() + 512);
    const size_t n = source.size();
    size_t i = 0;
    bool versionReplaced = false;
    bool lineStart = true;
    size_t insertAt = std::string::npos;  // after the leading directive block
    while (i < n) {
        const char c = source[i];
        if (c == '\n') {
            out += c;
            ++i;
            lineStart = true;
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(c))) {
            out += c;
            ++i;
            continue;
        }
        if (source.compare(i, 2, "//") == 0) {
            while (i < n && source[i] != '\n') out += source[i++];
            continue;
        }
        if (source.compare(i, 2, "/*") == 0) {
            const size_t end = source.find("*/", i + 2);
            const size_t stop = end == std::string::npos ? n : end + 2;
            out.append(source, i, stop - i);
            i = stop;
            continue;
        }
        if (c == '#' && lineStart) {
            size_t lineEnd = i;
            while (lineEnd < n && source[lineEnd] != '\n') {
                if (source[lineEnd] == '\\' && lineEnd + 1 < n && source[lineEnd + 1] == '\n') lineEnd += 2;
                else ++lineEnd;
            }
            std::string directive = source.substr(i, lineEnd - i);
            size_t k = 1;
            while (k < directive.size() && (directive[k] == ' ' || directive[k] == '\t')) ++k;
            if (!versionReplaced && directive.compare(k, 7, "version") == 0) {
                out += "#version " + std::to_string(newVersion);
                versionReplaced = true;
            } else {
                out += directive;
            }
            i = lineEnd;
            lineStart = false;
            continue;
        }
        if (insertAt == std::string::npos) {
            // First real token: declarations go before it, on their own line.
            insertAt = out.size();
        }
        lineStart = false;
        if (isIdentStart(c)) {
            size_t end = i;
            while (end < n && isIdent(source[end])) ++end;
            std::string word = source.substr(i, end - i);
            i = end;
            auto it = rename.find(word);
            if (it != rename.end()) {
                if (it->second.rfind("wmgl_shadow", 0) == 0) used.insert(it->second);
                out += it->second;
                continue;
            }
            if (word.rfind("gl_", 0) == 0) {
                if (fragment && (word == "gl_FragColor" || word == "gl_FragData")) {
                    used.insert(word);
                    out += "wmgl_" + word.substr(3);
                    continue;
                }
                if (fragment && (word == "gl_Color" || word == "gl_SecondaryColor" || word == "gl_TexCoord" ||
                                 word == "gl_FogFragCoord")) {
                    used.insert("fs:" + word);
                    out += "wmgl_" + word.substr(3);
                    continue;
                }
                bool handled = false;
                for (const BuiltinDecl& b : kVertexInputs)
                    if ((b.stages & stageBit) && word == b.name) handled = true;
                for (const BuiltinDecl& b : kVaryings)
                    if ((b.stages & stageBit) && word == b.name) handled = true;
                if (vertex && word == "gl_TexCoord") handled = true;
                for (const char* u : kFixedFunctionUniforms)
                    if (word == u) {
                        handled = true;
                        result.usesFixedFunctionState = true;
                    }
                if (handled) {
                    used.insert(word);
                    out += "wmgl_" + word.substr(3);
                    continue;
                }
            }
            if (vertex && word == "ftransform") {
                used.insert("ftransform");
                used.insert("gl_Vertex");
                result.usesFixedFunctionState = true;
                out += "wmgl_ftransform";
                continue;
            }
            out += word;
            continue;
        }
        out += c;
        ++i;
    }

    // Declarations for everything that was renamed.
    std::string decls = "\n";
    if (used.count("gl_FragColor") && used.count("gl_FragData")) {
        result.ok = false;
        result.log = "ERROR: a fragment shader cannot write both gl_FragColor and gl_FragData\n";
        return result;
    }
    if (used.count("gl_FragColor")) decls += "out vec4 wmgl_FragColor;\n";
    if (used.count("gl_FragData")) decls += "out vec4 wmgl_FragData[8];\n";
    for (const BuiltinDecl& b : kVertexInputs)
        if ((b.stages & stageBit) && used.count(b.name)) decls += std::string(b.decl) + "\n";
    for (const BuiltinDecl& b : kVaryings)
        if ((b.stages & stageBit) && used.count(b.name)) decls += std::string(b.decl) + "\n";
    if (vertex && used.count("gl_TexCoord")) decls += "out vec4 wmgl_TexCoord[8];\n";
    if (fragment) {
        if (used.count("fs:gl_Color")) decls += "in vec4 wmgl_Color;\n";
        if (used.count("fs:gl_SecondaryColor")) decls += "in vec4 wmgl_SecondaryColor;\n";
        if (used.count("fs:gl_TexCoord")) decls += "in vec4 wmgl_TexCoord[8];\n";
        if (used.count("fs:gl_FogFragCoord")) decls += "in float wmgl_FogFragCoord;\n";
    }
    // shadow*() returned vec4 in GLSL 1.x; texture() on shadow samplers returns float.
    if (used.count("wmgl_shadow"))
        decls += "vec4 wmgl_shadow(sampler1DShadow s, vec3 c) { return vec4(texture(s, c)); }\n"
                 "vec4 wmgl_shadow(sampler1DShadow s, vec3 c, float b) { return vec4(texture(s, c, b)); }\n"
                 "vec4 wmgl_shadow(sampler2DShadow s, vec3 c) { return vec4(texture(s, c)); }\n"
                 "vec4 wmgl_shadow(sampler2DShadow s, vec3 c, float b) { return vec4(texture(s, c, b)); }\n"
                 "vec4 wmgl_shadow(sampler2DRectShadow s, vec3 c) { return vec4(texture(s, c)); }\n";
    if (used.count("wmgl_shadowProj"))
        decls += "vec4 wmgl_shadowProj(sampler1DShadow s, vec4 c) { return vec4(textureProj(s, c)); }\n"
                 "vec4 wmgl_shadowProj(sampler1DShadow s, vec4 c, float b) { return vec4(textureProj(s, c, b)); }\n"
                 "vec4 wmgl_shadowProj(sampler2DShadow s, vec4 c) { return vec4(textureProj(s, c)); }\n"
                 "vec4 wmgl_shadowProj(sampler2DShadow s, vec4 c, float b) { return vec4(textureProj(s, c, b)); }\n"
                 "vec4 wmgl_shadowProj(sampler2DRectShadow s, vec4 c) { return vec4(textureProj(s, c)); }\n";
    if (used.count("wmgl_shadowLod"))
        decls += "vec4 wmgl_shadowLod(sampler1DShadow s, vec3 c, float l) { return vec4(textureLod(s, c, l)); }\n"
                 "vec4 wmgl_shadowLod(sampler2DShadow s, vec3 c, float l) { return vec4(textureLod(s, c, l)); }\n";
    if (used.count("wmgl_shadowProjLod"))
        decls += "vec4 wmgl_shadowProjLod(sampler1DShadow s, vec4 c, float l) { return vec4(textureProjLod(s, c, l)); }\n"
                 "vec4 wmgl_shadowProjLod(sampler2DShadow s, vec4 c, float l) { return vec4(textureProjLod(s, c, l)); }\n";
    if (result.usesFixedFunctionState) {
        // Fixed-function state, fed by the compatibility profile.
        decls +=
            "struct wmgl_LightSourceParameters { vec4 ambient; vec4 diffuse; vec4 specular; vec4 position;"
            " vec4 halfVector; vec3 spotDirection; float spotExponent; float spotCutoff; float spotCosCutoff;"
            " float constantAttenuation; float linearAttenuation; float quadraticAttenuation; };\n"
            "struct wmgl_MaterialParameters { vec4 emission; vec4 ambient; vec4 diffuse; vec4 specular; float shininess; };\n"
            "struct wmgl_LightModelParameters { vec4 ambient; };\n"
            "struct wmgl_LightModelProducts { vec4 sceneColor; };\n"
            "struct wmgl_LightProducts { vec4 ambient; vec4 diffuse; vec4 specular; };\n"
            "struct wmgl_FogParameters { vec4 color; float density; float start; float end; float scale; };\n"
            "struct wmgl_PointParameters { float size; float sizeMin; float sizeMax; float fadeThresholdSize;"
            " float distanceConstantAttenuation; float distanceLinearAttenuation; float distanceQuadraticAttenuation; };\n"
            "layout(std140) uniform wmgl_FixedFunction {\n"
            "  mat4 wmgl_ModelViewMatrix; mat4 wmgl_ProjectionMatrix; mat4 wmgl_ModelViewProjectionMatrix;\n"
            "  mat4 wmgl_TextureMatrix[8]; mat3 wmgl_NormalMatrix;\n"
            "  mat4 wmgl_ModelViewMatrixInverse; mat4 wmgl_ProjectionMatrixInverse; mat4 wmgl_ModelViewProjectionMatrixInverse;\n"
            "  mat4 wmgl_TextureMatrixInverse[8];\n"
            "  mat4 wmgl_ModelViewMatrixTranspose; mat4 wmgl_ProjectionMatrixTranspose; mat4 wmgl_ModelViewProjectionMatrixTranspose;\n"
            "  mat4 wmgl_TextureMatrixTranspose[8];\n"
            "  mat4 wmgl_ModelViewMatrixInverseTranspose; mat4 wmgl_ProjectionMatrixInverseTranspose;\n"
            "  mat4 wmgl_ModelViewProjectionMatrixInverseTranspose; mat4 wmgl_TextureMatrixInverseTranspose[8];\n"
            "  float wmgl_NormalScale; vec4 wmgl_ClipPlane[8]; wmgl_PointParameters wmgl_Point;\n"
            "  wmgl_MaterialParameters wmgl_FrontMaterial; wmgl_MaterialParameters wmgl_BackMaterial;\n"
            "  wmgl_LightSourceParameters wmgl_LightSource[8]; wmgl_LightModelParameters wmgl_LightModel;\n"
            "  wmgl_LightModelProducts wmgl_FrontLightModelProduct; wmgl_LightModelProducts wmgl_BackLightModelProduct;\n"
            "  wmgl_LightProducts wmgl_FrontLightProduct[8]; wmgl_LightProducts wmgl_BackLightProduct[8];\n"
            "  vec4 wmgl_TextureEnvColor[8]; vec4 wmgl_EyePlaneS[8]; vec4 wmgl_EyePlaneT[8]; vec4 wmgl_EyePlaneR[8];\n"
            "  vec4 wmgl_EyePlaneQ[8]; vec4 wmgl_ObjectPlaneS[8]; vec4 wmgl_ObjectPlaneT[8]; vec4 wmgl_ObjectPlaneR[8];\n"
            "  vec4 wmgl_ObjectPlaneQ[8]; wmgl_FogParameters wmgl_Fog;\n"
            "};\n";
        if (vertex && used.count("ftransform"))
            decls += "vec4 wmgl_ftransform() { return wmgl_ModelViewProjectionMatrix * wmgl_Vertex; }\n";
        if (vertex && used.count("ftransform") && !used.count("gl_Vertex")) decls += "in vec4 wmgl_Vertex;\n";
    }
    if (decls.size() > 1) {
        if (insertAt == std::string::npos) insertAt = out.size();
        out.insert(insertAt, decls);
        // Keep line numbers stable for error messages after the declarations.
        out.insert(insertAt + decls.size(), "#line " + std::to_string(1 + std::count(out.begin(), out.begin() + static_cast<long>(insertAt), '\n')) + "\n");
    }
    if (!versionReplaced) out = "#version " + std::to_string(newVersion) + "\n#line 1\n" + out;
    result.source = out;
    return result;
}

namespace {

const std::set<std::string>& vulkanOnlyKeywords() {
    static const std::set<std::string> words = [] {
        std::set<std::string> w = {"sampler", "samplerShadow", "subpassInput", "isubpassInput", "usubpassInput",
                                   "subpassInputMS", "isubpassInputMS", "usubpassInputMS"};
        for (const char* prefix : {"", "i", "u"})
            for (const char* dim : {"1D", "1DArray", "2D", "2DArray", "2DRect", "3D", "Cube", "CubeArray", "Buffer",
                                    "2DMS", "2DMSArray"})
                w.insert(std::string(prefix) + "texture" + dim);
        return w;
    }();
    return words;
}

}  // namespace

std::string escapeVulkanKeywords(const std::string& source) {
    const std::set<std::string>& words = vulkanOnlyKeywords();
    std::string out;
    out.reserve(source.size());
    const size_t n = source.size();
    size_t i = 0;
    while (i < n) {
        if (source.compare(i, 2, "//") == 0) {
            const size_t end = source.find('\n', i);
            const size_t stop = end == std::string::npos ? n : end;
            out.append(source, i, stop - i);
            i = stop;
            continue;
        }
        if (source.compare(i, 2, "/*") == 0) {
            const size_t end = source.find("*/", i + 2);
            const size_t stop = end == std::string::npos ? n : end + 2;
            out.append(source, i, stop - i);
            i = stop;
            continue;
        }
        if (isIdentStart(source[i])) {
            size_t end = i;
            while (end < n && isIdent(source[end])) ++end;
            const std::string word = source.substr(i, end - i);
            if (words.count(word)) out += kEscapePrefix;
            out += word;
            i = end;
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(source[i]))) {
            // Numbers can contain identifier characters (1e5, 0x1Fu); copy them whole.
            while (i < n && isIdent(source[i])) out += source[i++];
            continue;
        }
        out += source[i++];
    }
    return out;
}

std::string unescapeName(std::string name) {
    const std::string prefix = kEscapePrefix;
    for (size_t at = name.find(prefix); at != std::string::npos; at = name.find(prefix, at)) name.erase(at, prefix.size());
    return name;
}

std::string checkReservedWords(const std::string& source) {
    const GlslVersion version = parseGlslVersion(source);
    if (version.es || version.number < 130 || version.number >= 400) return "";
    static const std::set<std::string> reserved = {
        "common", "partition", "active", "asm", "class", "union", "enum", "typedef", "template", "this", "packed",
        "goto", "inline", "noinline", "volatile", "public", "static", "extern", "external", "interface", "long",
        "short", "double", "half", "fixed", "unsigned", "superp", "input", "output", "hvec2", "hvec3", "hvec4",
        "dvec2", "dvec3", "dvec4", "fvec2", "fvec3", "fvec4", "sampler3DRect", "filter", "image1D", "image2D",
        "image3D", "imageCube", "iimage1D", "iimage2D", "iimage3D", "iimageCube", "uimage1D", "uimage2D", "uimage3D",
        "uimageCube", "image1DArray", "image2DArray", "iimage1DArray", "iimage2DArray", "uimage1DArray",
        "uimage2DArray", "image1DShadow", "image2DShadow", "image1DArrayShadow", "image2DArrayShadow", "imageBuffer",
        "iimageBuffer", "uimageBuffer", "sizeof", "cast", "namespace", "using", "row_major"};
    std::string errors;
    const size_t n = source.size();
    size_t i = 0;
    int line = 1;
    bool lineStart = true;
    int layoutDepth = -1;  // parenthesis depth inside layout(...), -1 outside
    int depth = 0;
    std::string previous;
    while (i < n) {
        const char c = source[i];
        if (c == '\n') {
            ++line;
            ++i;
            lineStart = true;
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(c))) {
            ++i;
            continue;
        }
        if (source.compare(i, 2, "//") == 0) {
            while (i < n && source[i] != '\n') ++i;
            continue;
        }
        if (source.compare(i, 2, "/*") == 0) {
            const size_t end = source.find("*/", i + 2);
            const size_t stop = end == std::string::npos ? n : end + 2;
            for (size_t k = i; k < stop; ++k) line += source[k] == '\n';
            i = stop;
            continue;
        }
        if (c == '#' && lineStart) {
            while (i < n && source[i] != '\n') {
                if (source[i] == '\\' && i + 1 < n && source[i + 1] == '\n') ++line, ++i;
                ++i;
            }
            continue;
        }
        lineStart = false;
        if (isIdentStart(c)) {
            size_t end = i;
            while (end < n && isIdent(source[end])) ++end;
            const std::string word = source.substr(i, end - i);
            i = end;
            if (layoutDepth < 0 && reserved.count(word))
                errors += "ERROR: 0:" + std::to_string(line) + ": '" + word + "' : Reserved word.\n";
            previous = word;
            continue;
        }
        if (c == '(') {
            if (previous == "layout" && layoutDepth < 0) layoutDepth = depth;
            ++depth;
        } else if (c == ')') {
            --depth;
            if (depth == layoutDepth) layoutDepth = -1;
        }
        previous.clear();
        ++i;
    }
    return errors;
}

}  // namespace wmgl::shader
