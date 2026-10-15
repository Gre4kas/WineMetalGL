#include "core/shader/tessellation.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <regex>
#include <set>
#include <sstream>

#include "core/shader/geometry.h"
#include "core/shader/glsl_upgrade.h"
#include "core/shader/transform_feedback.h"

namespace wmgl::shader {
namespace {

constexpr int kMaxPatchVertices = 32;  // gl_MaxPatchVertices (limits::kMaxPatchVertices)

std::string trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    const size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

bool isIdent(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

// Replaces whole-word identifiers.
std::string replaceWord(const std::string& text, const std::string& from, const std::string& to) {
    std::string out;
    size_t i = 0;
    while (i < text.size()) {
        const size_t at = text.find(from, i);
        if (at == std::string::npos) break;
        const bool before = at > 0 && isIdent(text[at - 1]);
        const bool after = at + from.size() < text.size() && isIdent(text[at + from.size()]);
        out.append(text, i, at - i);
        out += before || after ? from : to;
        i = at + from.size();
    }
    out.append(text, i, std::string::npos);
    return out;
}

// A declaration "layout(location = 1) patch flat out vec4 name[3][2]".
struct Declaration {
    std::string direction;  // "in", "out" or ""
    bool patch = false;
    int location = -1;
    std::string type, name;
    std::vector<std::string> dims;  // array dimensions as written ("" for unsized)
};

Declaration parseDeclaration(const std::string& text) {
    Declaration d;
    std::string rest = trim(text);
    if (rest.compare(0, 7, "layout(") == 0) {
        const size_t close = rest.find(')');
        std::istringstream parts(rest.substr(7, close - 7));
        std::string part;
        while (std::getline(parts, part, ',')) {
            part = trim(part);
            if (part.compare(0, 8, "location") == 0 && part.find('=') != std::string::npos)
                d.location = std::atoi(part.c_str() + part.find('=') + 1);
        }
        rest = trim(rest.substr(close + 1));
    }
    if (!rest.empty() && rest.back() == ';') rest.pop_back();
    // Array dimensions: everything from the first '[' after the name.
    std::istringstream words(rest);
    std::string word;
    std::vector<std::string> tokens;
    while (words >> word) {
        if (word == "flat" || word == "noperspective" || word == "smooth" || word == "centroid" || word == "invariant" ||
            word == "sample" || word == "highp" || word == "mediump" || word == "lowp" || word == "precise")
            continue;
        if (word == "patch") {
            d.patch = true;
            continue;
        }
        if (word == "in" || word == "out") {
            d.direction = word;
            continue;
        }
        tokens.push_back(word);
    }
    if (tokens.size() < 2) return d;
    d.type = tokens[0];
    std::string name;
    for (size_t i = 1; i < tokens.size(); ++i) name += tokens[i];
    const size_t open = name.find('[');
    d.name = name.substr(0, open);
    for (size_t at = open; at != std::string::npos && at < name.size();) {
        const size_t close = name.find(']', at);
        if (close == std::string::npos) break;
        d.dims.push_back(name.substr(at + 1, close - at - 1));
        at = name.find('[', close);
    }
    return d;
}

std::string dimsText(const std::vector<std::string>& dims, size_t from) {
    std::string out;
    for (size_t i = from; i < dims.size(); ++i) out += "[" + dims[i] + "]";
    return out;
}

// Replaces ARRAY[EXPR].gl_X with PREFIX_x[EXPR] for the gl_in / gl_out built-in arrays.
std::string renameArrayedBuiltin(const std::string& text, const std::string& array, const std::string& prefix, bool& pointSize,
                                 bool& clipDistance, bool& cullDistance) {
    std::string out;
    size_t i = 0;
    const std::string open = array + "[";
    while (true) {
        const size_t at = text.find(open, i);
        if (at == std::string::npos) break;
        if (at > 0 && isIdent(text[at - 1])) {
            out.append(text, i, at + open.size() - i);
            i = at + open.size();
            continue;
        }
        size_t j = at + open.size();
        int depth = 1;
        while (j < text.size() && depth) {
            if (text[j] == '[') ++depth;
            else if (text[j] == ']') --depth;
            ++j;
        }
        const std::string index = text.substr(at + open.size(), j - at - open.size() - 1);
        std::string member;
        if (j < text.size() && text[j] == '.') {
            size_t k = j + 1;
            while (k < text.size() && isIdent(text[k])) ++k;
            member = text.substr(j + 1, k - j - 1);
            j = k;
        }
        out.append(text, i, at - i);
        if (member == "gl_PointSize") {
            pointSize = true;
            out += prefix + "pointsize[" + index + "]";
        } else if (member == "gl_ClipDistance") {
            clipDistance = true;
            out += prefix + "clipdistance[" + index + "]";
        } else if (member == "gl_CullDistance") {
            cullDistance = true;
            out += prefix + "culldistance[" + index + "]";
        } else {
            out += prefix + "position[" + index + "]";
        }
        i = j;
    }
    out.append(text, i, std::string::npos);
    return out;
}

// The interface name of a variable: "Block.member" for block members (GL
// matches blocks by block name), else the variable name; GL names, so
// identifiers escaped for codegen are restored.
std::string varKey(const TessVar& v) {
    if (!v.structType.empty()) return unescapeName(v.name + "." + v.member);
    if (v.blockName.empty()) return unescapeName(v.name);
    return unescapeName(v.blockName + "." + (v.member.empty() ? v.name : v.member));
}

int words(GLenum type) { return glTypeWords(type); }  // two per double component

}  // namespace

bool parseTessellationGlsl(Stage stage, const std::string& glsl, TessStageInfo& info, std::string& log) {
    const bool control = stage == Stage::TessControl;
    std::vector<std::string> lines;
    {
        std::istringstream stream(glsl);
        std::string line;
        while (std::getline(stream, line)) lines.push_back(line);
    }
    size_t functionsStart = lines.size();
    int depth = 0;
    for (size_t i = 0; i + 1 < lines.size(); ++i) {
        const std::string t = trim(lines[i]);
        if (depth == 0 && !t.empty() && t.back() == ')' && trim(lines[i + 1]) == "{" && t.find('(') != std::string::npos &&
            t.compare(0, 7, "layout(") != 0) {
            functionsStart = i;
            break;
        }
        for (char c : t) depth += c == '{' ? 1 : c == '}' ? -1 : 0;
    }
    std::vector<std::string> statements;
    std::string current;
    depth = 0;
    for (size_t i = 0; i < functionsStart; ++i) {
        const std::string t = trim(lines[i]);
        if (t.empty() || t.compare(0, 8, "#version") == 0) continue;
        if (t[0] == '#') continue;  // extensions are re-declared by the generated sources as needed
        current += (current.empty() ? "" : "\n") + t;
        for (char c : t) depth += c == '{' ? 1 : c == '}' ? -1 : 0;
        if (depth == 0 && t.back() == ';') {
            statements.push_back(current);
            current.clear();
        }
    }
    // Struct types (struct-typed varyings are flattened per field).
    std::map<std::string, std::vector<Declaration>> structs;
    for (const std::string& s : statements) {
        const std::string flat = trim(s);
        if (flat.compare(0, 7, "struct ") != 0) continue;
        const size_t brace = flat.find('{'), close = flat.rfind('}');
        if (brace == std::string::npos || close == std::string::npos) continue;
        const std::string name = trim(flat.substr(7, brace - 7));
        std::istringstream members(flat.substr(brace + 1, close - brace - 1));
        std::string member;
        while (std::getline(members, member, ';'))
            if (!trim(member).empty()) structs[name].push_back(parseDeclaration(trim(member) + ";"));
    }
    for (const std::string& s : statements) {
        const std::string flat = trim(s);
        const size_t brace = flat.find('{');
        if (brace == std::string::npos) {
            // Primitive layout declarations: layout(vertices = N) out; layout(triangles, ...) in;
            if (flat.compare(0, 7, "layout(") == 0 &&
                (flat.find(") in;") != std::string::npos || flat.find(") out;") != std::string::npos)) {
                const std::string args = flat.substr(7, flat.find(')') - 7);
                std::istringstream parts(args);
                std::string part;
                while (std::getline(parts, part, ',')) {
                    part = trim(part);
                    if (part.compare(0, 8, "vertices") == 0 && part.find('=') != std::string::npos)
                        info.verticesOut = std::atoi(part.c_str() + part.find('=') + 1);
                }
                continue;
            }
            const Declaration d = parseDeclaration(flat);
            if (d.direction.empty()) {
                // "invariant gl_Position;" and other globals.
                if (flat.compare(0, 9, "invariant") == 0 || flat.compare(0, 7, "precise") == 0) {
                    if (!control) info.outputDeclarations += flat + "\n";
                    continue;
                }
                info.globals += flat + "\n";
                continue;
            }
            if (d.name.rfind("gl_", 0) == 0) {
                // Redeclared built-in arrays ("out float gl_CullDistance[N];"):
                // the evaluation stage keeps its outputs' sizes.
                if ((d.name == "gl_ClipDistance" || d.name == "gl_CullDistance") && d.direction == "out" && !control)
                    info.outputDeclarations += flat + "\n";
                continue;
            }
            const bool output = d.direction == "out";
            const bool arrayed = !d.patch && (control || !output);
            if (auto st = structs.find(d.type); st != structs.end()) {
                if (d.dims.size() > (arrayed ? 1u : 0u)) {
                    log += "error: tessellation shader " + d.direction + "put '" + d.name + "' is an array of structs\n";
                    return false;
                }
                for (const Declaration& m : st->second) {
                    TessVar v;
                    v.glslType = m.type;
                    v.type = glslTypeFromName(m.type);
                    if (!v.type || m.dims.size() > 1) {
                        log += "error: tessellation shader " + d.direction + "put '" + d.name + "' has an unsupported member\n";
                        return false;
                    }
                    v.name = d.name;
                    v.member = m.name;
                    v.structType = d.type;
                    v.arraySize = m.dims.empty() ? 0 : std::atoi(m.dims[0].c_str());
                    v.patch = d.patch;
                    v.location = d.location;
                    v.declaration = flat;
                    (output ? info.outputs : info.inputs).push_back(v);
                }
                if (!control && output) info.outputDeclarations += flat + "\n";
                continue;
            }
            const GLenum glType = glslTypeFromName(d.type);
            if (!glType) {
                log += "error: tessellation shader " + d.direction + "put '" + d.name + "' has an unsupported type\n";
                return false;
            }
            TessVar v;
            v.glslType = d.type;
            v.type = glType;
            v.name = d.name;
            v.patch = d.patch;
            v.location = d.location;
            v.declaration = flat;
            const size_t extraDim = arrayed ? 1 : 0;
            if (d.dims.size() > extraDim) v.arraySize = std::atoi(d.dims[extraDim].c_str());
            if (d.dims.size() > extraDim + 1) {
                log += "error: tessellation shader " + d.direction + "put '" + d.name + "' is an array of arrays\n";
                return false;
            }
            if (!control && output) {
                info.outputDeclarations += flat + "\n";
                v.declaration = flat;
            }
            (output ? info.outputs : info.inputs).push_back(v);
            continue;
        }
        // Blocks: header { members } tail;
        const std::string header = trim(flat.substr(0, brace));
        const size_t close = flat.rfind('}');
        const std::string body = flat.substr(brace + 1, close - brace - 1);
        std::string tail = trim(flat.substr(close + 1));
        if (!tail.empty() && tail.back() == ';') tail.pop_back();
        tail = trim(tail);
        const Declaration h = parseDeclaration(header + " wmgl_block_header;");
        std::string blockName = h.type;
        if (h.direction.empty()) {
            info.globals += flat + "\n";  // uniform / buffer blocks
            continue;
        }
        const bool output = h.direction == "out";
        std::string instance = tail;
        if (const size_t bracket = instance.find('['); bracket != std::string::npos) instance = instance.substr(0, bracket);
        std::istringstream members(body);
        std::string member;
        if (blockName == "gl_PerVertex") {
            while (std::getline(members, member, ';')) {
                const Declaration m = parseDeclaration(trim(member) + ";");
                if (m.name == "gl_ClipDistance" && !m.dims.empty())
                    (output ? info.outputClipDistances : info.inputClipDistances) = std::atoi(m.dims[0].c_str());
                if (m.name == "gl_CullDistance" && !m.dims.empty())
                    (output ? info.outputCullDistances : info.inputCullDistances) = std::atoi(m.dims[0].c_str());
            }
            // The evaluation stage's output block is a vertex shader output.
            if (!control && output) info.outputDeclarations += flat + "\n";
            continue;
        }
        if (!control && output) info.outputDeclarations += flat + "\n";
        while (std::getline(members, member, ';')) {
            member = trim(member);
            if (member.empty()) continue;
            const Declaration m = parseDeclaration(member + ";");
            const GLenum glType = glslTypeFromName(m.type);
            if (!glType) {
                log += "error: tessellation shader block member '" + m.name + "' has an unsupported type\n";
                return false;
            }
            TessVar v;
            v.glslType = m.type;
            v.type = glType;
            v.name = instance.empty() ? m.name : instance;
            v.member = instance.empty() ? "" : m.name;
            v.blockName = blockName;
            v.arraySize = m.dims.empty() ? 0 : std::atoi(m.dims[0].c_str());
            v.patch = h.patch;
            v.declaration = flat;
            if (instance.empty()) {
                // Members of an anonymous block are plain variables in the shader.
                v.member.clear();
                v.blockName = blockName;
            }
            (output ? info.outputs : info.inputs).push_back(v);
        }
    }
    if (control && info.verticesOut <= 0) {
        log += "error: tessellation control shader has no output vertex count\n";
        return false;
    }
    std::string functions;
    for (size_t i = functionsStart; i < lines.size(); ++i) functions += lines[i] + "\n";
    bool inPointSize = false, inClip = false, inCull = false, outPointSize = false, outClip = false, outCull = false;
    functions = renameArrayedBuiltin(functions, "gl_in", "wmgl_in_", inPointSize, inClip, inCull);
    if (control) functions = renameArrayedBuiltin(functions, "gl_out", "wmgl_out_", outPointSize, outClip, outCull);
    info.readsPointSize = inPointSize;
    info.readsClipDistance = inClip;
    info.readsCullDistance = inCull;
    if (inClip && !info.inputClipDistances) info.inputClipDistances = 8;
    if (inCull && !info.inputCullDistances) info.inputCullDistances = 8;
    if (control) {
        info.writesPointSize = outPointSize;
        if (outClip && !info.outputClipDistances) info.outputClipDistances = 8;
        if (!outClip) info.outputClipDistances = 0;
        if (outCull && !info.outputCullDistances) info.outputCullDistances = std::max(1, 8 - info.outputClipDistances);
        if (!outCull) info.outputCullDistances = 0;
    }
    const std::pair<const char*, const char*> renames[] = {
        {"gl_InvocationID", "wmgl_invocation_id"}, {"gl_PrimitiveID", "wmgl_primitive_id"},
        {"gl_PatchVerticesIn", "wmgl_patch_vertices_in"}, {"gl_BuiltIn_14", "wmgl_patch_vertices_in"},
        {"gl_TessLevelOuter", "wmgl_tess_outer"}, {"gl_TessLevelInner", "wmgl_tess_inner"},
        {"gl_TessCoord", "wmgl_tess_coord"},
    };
    for (const auto& [from, to] : renames) functions = replaceWord(functions, from, to);
    const size_t mainAt = functions.find("void main()");
    if (mainAt == std::string::npos) {
        log += "error: tessellation shader has no main()\n";
        return false;
    }
    functions.replace(mainAt, 11, "void wmgl_tess_main()");
    info.functions = functions;
    return true;
}

std::vector<std::string> tessellationCaptureVaryings(const TessStageInfo& first) {
    std::vector<std::string> varyings = {"gl_Position"};
    if (first.readsPointSize) varyings.push_back("gl_PointSize");
    if (first.readsClipDistance) varyings.push_back("gl_ClipDistance");
    if (first.readsCullDistance) varyings.push_back("gl_CullDistance");
    for (const TessVar& v : first.inputs)
        if (!v.patch) varyings.push_back(varKey(v));
    return varyings;
}

namespace {

// Declarations of the per-vertex variables `vars` (or the per-patch ones) as
// arrays of `vertices` elements (0: not arrayed), with `storage` ("shared ").
std::string declareVars(const std::vector<TessVar>& vars, bool patch, int vertices, const std::string& storage,
                        const std::string& structPrefix) {
    std::string out;
    std::set<std::string> declaredBlocks;
    const std::string arrayed = vertices ? "[" + std::to_string(vertices) + "]" : "";
    for (const TessVar& v : vars) {
        if (v.patch != patch) continue;
        if (v.member.empty()) {
            out += storage + v.glslType + " " + v.name + arrayed + (v.arraySize ? "[" + std::to_string(v.arraySize) + "]" : "") +
                   ";\n";
            continue;
        }
        if (!declaredBlocks.insert(v.name).second) continue;
        if (!v.structType.empty()) {
            out += storage + v.structType + " " + v.name + arrayed + ";\n";
            continue;
        }
        const std::string structName = structPrefix + v.blockName;
        out += "struct " + structName + " {";
        for (const TessVar& m : vars)
            if (m.name == v.name && m.patch == patch)
                out += " " + m.glslType + " " + m.member + (m.arraySize ? "[" + std::to_string(m.arraySize) + "]" : "") + ";";
        out += " };\n" + storage + structName + " " + v.name + arrayed + ";\n";
    }
    return out;
}

// The GLSL access path of element `e` of `v` (vertex `vertex` if arrayed).
std::string access(const TessVar& v, const std::string& vertex, int e) {
    std::string path = v.name + (vertex.empty() ? "" : "[" + vertex + "]");
    if (!v.member.empty()) path += "." + v.member;
    if (v.arraySize) path += "[" + std::to_string(e) + "]";
    return path;
}

const char* kindOf(GLenum type) {
    switch (type) {
    case GL_INT: case GL_INT_VEC2: case GL_INT_VEC3: case GL_INT_VEC4: return "int";
    case GL_UNSIGNED_INT: case GL_UNSIGNED_INT_VEC2: case GL_UNSIGNED_INT_VEC3: case GL_UNSIGNED_INT_VEC4: return "uint";
    default: return "float";
    }
}

bool identifierChar(char ch) { return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_'; }

// Whether every use of identifier `name` in `code` indexes it (name[...]).
bool alwaysIndexed(const std::string& code, const std::string& name) {
    for (size_t at = code.find(name); at != std::string::npos; at = code.find(name, at + 1)) {
        if ((at && (identifierChar(code[at - 1]) || code[at - 1] == '.')) ||
            (at + name.size() < code.size() && identifierChar(code[at + name.size()])))
            continue;
        size_t next = at + name.size();
        while (next < code.size() && (code[next] == ' ' || code[next] == '\t' || code[next] == '\n')) ++next;
        if (next >= code.size() || code[next] != '[') return false;
    }
    return true;
}

// `code` with each name[index] as fn(int(index)) (alwaysIndexed holds).
std::string callInsteadOfIndex(const std::string& code, const std::string& name, const std::string& fn) {
    std::string out;
    size_t from = 0;
    for (size_t at = code.find(name); at != std::string::npos; at = code.find(name, at + 1)) {
        if ((at && (identifierChar(code[at - 1]) || code[at - 1] == '.')) ||
            (at + name.size() < code.size() && identifierChar(code[at + name.size()])))
            continue;
        size_t open = code.find('[', at + name.size());
        int depth = 0;
        size_t close = open;
        for (; close < code.size(); ++close) {
            if (code[close] == '[') ++depth;
            else if (code[close] == ']' && --depth == 0) break;
        }
        if (close >= code.size()) return code;  // unbalanced: unchanged
        out += code.substr(from, at - from) + fn + "(int(" + code.substr(open + 1, close - open - 1) + "))";
        from = close + 1;
        at = close;
    }
    return out + code.substr(from);
}

}  // namespace

TessControlSources tessControlSources(const TessStageInfo& info, const LinkResult& capture) {
    TessControlSources out;
    const int n = info.verticesOut;
    out.verticesOut = n;
    const std::string ns = std::to_string(n), maxIn = std::to_string(kMaxPatchVertices);
    auto captured = [&](const std::string& varying) -> const TransformFeedbackVarying* {
        for (const TransformFeedbackVarying& v : capture.transformFeedback)
            if (v.name == varying) return &v;
        return nullptr;
    };

    // Record layout.
    auto add = [](std::vector<TessRecordField>& fields, uint32_t& size, const std::string& key, GLenum type, int elements,
                  int location) {
        fields.push_back({key, type, elements, size, location});
        size += static_cast<uint32_t>(words(type) * elements);
    };
    add(out.vertexFields, out.vertexWords, "gl_Position", GL_FLOAT_VEC4, 1, -1);
    add(out.vertexFields, out.vertexWords, "gl_PointSize", GL_FLOAT, 1, -1);
    if (info.outputClipDistances) add(out.vertexFields, out.vertexWords, "gl_ClipDistance", GL_FLOAT, info.outputClipDistances, -1);
    if (info.outputCullDistances) add(out.vertexFields, out.vertexWords, "gl_CullDistance", GL_FLOAT, info.outputCullDistances, -1);
    const size_t builtinFields = out.vertexFields.size();
    out.patchWords = 6;  // outer levels, inner levels
    for (const TessVar& v : info.outputs) {
        const std::string key = varKey(v);
        if (v.patch) add(out.patchFields, out.patchWords, key, v.type, std::max(1, v.arraySize), v.location);
        else add(out.vertexFields, out.vertexWords, key, v.type, std::max(1, v.arraySize), v.location);
    }
    const uint32_t record = static_cast<uint32_t>(n) * out.vertexWords + out.patchWords;

    // Inputs the code only ever indexes are loaded from the captured vertex
    // records where they are read, instead of copied into 32-element arrays
    // first: dynamically indexed arrays live on the stack (4240 bytes a
    // thread for Foundation's terrain, issue #12), loads from the read-only
    // records do not. WINEMETALGL_TCS_LAZY_INPUTS=0 copies them all.
    static const bool lazyInputs = [] {
        const char* v = std::getenv("WINEMETALGL_TCS_LAZY_INPUTS");
        return !v || v[0] != '0';
    }();
    std::string functions = info.functions, lazyLoads;
    std::set<std::string> lazy;
    const std::string stride = std::to_string(capture.xfbStride[0]);
    auto makeLazy = [&](const std::string& name, const std::string& glslType, GLenum type, const TransformFeedbackVarying* p) {
        if (!lazyInputs || !p || !alwaysIndexed(functions, name)) return;
        const std::string fn = "wmgl_lazy_" + name;
        lazyLoads += glslType + " " + fn + "(int v) {\n    uint b = (wmgl_patch_g * wmgl_tcs_patch_vertices + min(uint(v), "
                     "wmgl_tcs_patch_vertices - 1u)) * " + stride + "u;\n    return " +
                     glslLoadValue(glslType, type, "wmgl_tcs_in", "b + " + std::to_string(p->offset) + "u") + ";\n}\n";
        functions = callInsteadOfIndex(functions, name, fn);
        lazy.insert(name);
    };
    makeLazy("wmgl_in_position", "vec4", GL_FLOAT_VEC4, captured("gl_Position"));
    for (const TessVar& v : info.inputs)
        if (!v.patch && v.member.empty() && !v.arraySize) makeLazy(v.name, v.glslType, v.type, captured(varKey(v)));
    std::vector<TessVar> arrayedInputs;
    for (const TessVar& v : info.inputs)
        if (!lazy.count(v.name)) arrayedInputs.push_back(v);
    std::string& c = out.kernel;
    c += "#version 450\nlayout(local_size_x = " + ns + ") in;\n";
    c += info.globals;
    c += declareVars(arrayedInputs, false, kMaxPatchVertices, "", "wmgl_tcs_in_");
    c += declareVars(info.outputs, false, n, "shared ", "wmgl_tcs_out_");
    c += declareVars(info.outputs, true, 0, "shared ", "wmgl_tcs_patch_");
    if (!lazy.count("wmgl_in_position")) c += "vec4 wmgl_in_position[" + maxIn + "];\n";
    if (info.readsPointSize) c += "float wmgl_in_pointsize[" + maxIn + "];\n";
    if (info.readsClipDistance)
        c += "float wmgl_in_clipdistance[" + maxIn + "][" + std::to_string(info.inputClipDistances) + "];\n";
    if (info.readsCullDistance)
        c += "float wmgl_in_culldistance[" + maxIn + "][" + std::to_string(info.inputCullDistances) + "];\n";
    c += "shared vec4 wmgl_out_position[" + ns + "];\nshared float wmgl_out_pointsize[" + ns + "];\n";
    if (info.outputClipDistances)
        c += "shared float wmgl_out_clipdistance[" + ns + "][" + std::to_string(info.outputClipDistances) + "];\n";
    if (info.outputCullDistances)
        c += "shared float wmgl_out_culldistance[" + ns + "][" + std::to_string(info.outputCullDistances) + "];\n";
    c += "shared float wmgl_tess_outer[4];\nshared float wmgl_tess_inner[2];\n"
         "int wmgl_invocation_id, wmgl_primitive_id, wmgl_patch_vertices_in;\n"
         "layout(std430) readonly buffer wmgl_TcsInput { uint wmgl_tcs_in[]; };\n"
         "layout(std430) writeonly buffer wmgl_TcsOutput { uint wmgl_tcs_out[]; };\n"
         "layout(std430) readonly buffer wmgl_TcsControl { uint wmgl_tcs_patch_vertices, wmgl_tcs_patch_count, "
         "wmgl_tcs_patches_per_instance, wmgl_tcs_pad; };\n"
         "uint wmgl_patch_g;\n";
    c += lazyLoads;
    c += functions;
    c += "void main() {\n"
         "    uint wmgl_patch = gl_WorkGroupID.x;\n"
         "    wmgl_patch_g = wmgl_patch;\n"
         "    wmgl_invocation_id = int(gl_LocalInvocationID.x);\n"
         "    wmgl_patch_vertices_in = int(wmgl_tcs_patch_vertices);\n"
         "    wmgl_primitive_id = int(wmgl_patch % max(wmgl_tcs_patches_per_instance, 1u));\n"
         "    for (uint wmgl_j = 0u; wmgl_j < wmgl_tcs_patch_vertices; ++wmgl_j) {\n"
         "        uint wmgl_b = (wmgl_patch * wmgl_tcs_patch_vertices + wmgl_j) * " + std::to_string(capture.xfbStride[0]) + "u;\n";
    if (const TransformFeedbackVarying* p = lazy.count("wmgl_in_position") ? nullptr : captured("gl_Position"))
        c += "        wmgl_in_position[wmgl_j] = " +
             glslLoadValue("vec4", GL_FLOAT_VEC4, "wmgl_tcs_in", "wmgl_b + " + std::to_string(p->offset) + "u") + ";\n";
    if (const TransformFeedbackVarying* p = info.readsPointSize ? captured("gl_PointSize") : nullptr)
        c += "        wmgl_in_pointsize[wmgl_j] = " +
             glslLoadValue("float", GL_FLOAT, "wmgl_tcs_in", "wmgl_b + " + std::to_string(p->offset) + "u") + ";\n";
    if (const TransformFeedbackVarying* p = info.readsClipDistance ? captured("gl_ClipDistance") : nullptr)
        for (int k = 0; k < std::min(info.inputClipDistances, p->size); ++k)
            c += "        wmgl_in_clipdistance[wmgl_j][" + std::to_string(k) + "] = " +
                 glslLoadValue("float", GL_FLOAT, "wmgl_tcs_in", "wmgl_b + " + std::to_string(p->offset + k) + "u") + ";\n";
    if (const TransformFeedbackVarying* p = info.readsCullDistance ? captured("gl_CullDistance") : nullptr)
        for (int k = 0; k < std::min(info.inputCullDistances, p->size); ++k)
            c += "        wmgl_in_culldistance[wmgl_j][" + std::to_string(k) + "] = " +
                 glslLoadValue("float", GL_FLOAT, "wmgl_tcs_in", "wmgl_b + " + std::to_string(p->offset + k) + "u") + ";\n";
    for (const TessVar& v : arrayedInputs) {
        const TransformFeedbackVarying* p = captured(varKey(v));
        if (!p) continue;
        for (int e = 0; e < std::max(1, std::min(v.arraySize, p->size)); ++e)
            c += "        " + access(v, "wmgl_j", e) + " = " +
                 glslLoadValue(v.glslType, v.type, "wmgl_tcs_in",
                               "wmgl_b + " + std::to_string(p->offset + static_cast<uint32_t>(e * words(v.type))) + "u") +
                 ";\n";
    }
    c += "    }\n"
         "    wmgl_tess_main();\n"
         "    barrier();\n"
         "    uint wmgl_o = wmgl_patch * " + std::to_string(record) + "u + uint(wmgl_invocation_id) * " +
         std::to_string(out.vertexWords) + "u;\n";
    auto store = [&](const std::string& base, uint32_t offset, const std::string& expr, GLenum type) {
        std::string s;
        uint32_t w = offset;
        for (const std::string& word : captureWords(expr, type))
            s += "        wmgl_tcs_out[" + base + " + " + std::to_string(w++) + "u] = " + word + ";\n";
        return s;
    };
    for (const TessRecordField& f : out.vertexFields) {
        if (f.key == "gl_Position") c += store("wmgl_o", f.offset, "wmgl_out_position[wmgl_invocation_id]", GL_FLOAT_VEC4);
        else if (f.key == "gl_PointSize") c += store("wmgl_o", f.offset, "wmgl_out_pointsize[wmgl_invocation_id]", GL_FLOAT);
        else if (f.key == "gl_ClipDistance")
            for (int k = 0; k < f.elements; ++k)
                c += store("wmgl_o", f.offset + k, "wmgl_out_clipdistance[wmgl_invocation_id][" + std::to_string(k) + "]", GL_FLOAT);
        else if (f.key == "gl_CullDistance")
            for (int k = 0; k < f.elements; ++k)
                c += store("wmgl_o", f.offset + k, "wmgl_out_culldistance[wmgl_invocation_id][" + std::to_string(k) + "]", GL_FLOAT);
    }
    size_t field = 0;
    for (const TessVar& v : info.outputs) {
        if (v.patch) continue;
        const TessRecordField& f = out.vertexFields[builtinFields + field++];
        for (int e = 0; e < f.elements; ++e)
            c += store("wmgl_o", f.offset + static_cast<uint32_t>(e * words(v.type)), access(v, "wmgl_invocation_id", e), v.type);
    }
    c += "    if (wmgl_invocation_id == 0) {\n"
         "        uint wmgl_p = wmgl_patch * " + std::to_string(record) + "u + " + std::to_string(n * out.vertexWords) + "u;\n";
    for (int k = 0; k < 4; ++k) c += store("wmgl_p", static_cast<uint32_t>(k), "wmgl_tess_outer[" + std::to_string(k) + "]", GL_FLOAT);
    for (int k = 0; k < 2; ++k) c += store("wmgl_p", static_cast<uint32_t>(4 + k), "wmgl_tess_inner[" + std::to_string(k) + "]", GL_FLOAT);
    field = 0;
    for (const TessVar& v : info.outputs) {
        if (!v.patch) continue;
        const TessRecordField& f = out.patchFields[field++];
        for (int e = 0; e < f.elements; ++e)
            c += store("wmgl_p", f.offset + static_cast<uint32_t>(e * words(v.type)), access(v, "", e), v.type);
    }
    c += "    }\n}\n";
    return out;
}

std::string tessEvaluationVertexShader(const TessStageInfo& info, const TessControlSources* control, const LinkResult* capture,
                                       std::string& log) {
    const std::string maxIn = std::to_string(kMaxPatchVertices);
    std::string v;
    v += "#version 450\n";
    v += info.globals;
    v += declareVars(info.inputs, false, kMaxPatchVertices, "", "wmgl_tes_in_");
    v += declareVars(info.inputs, true, 0, "", "wmgl_tes_patch_");
    v += "vec4 wmgl_in_position[" + maxIn + "];\n";
    if (info.readsPointSize) v += "float wmgl_in_pointsize[" + maxIn + "];\n";
    if (info.readsClipDistance)
        v += "float wmgl_in_clipdistance[" + maxIn + "][" + std::to_string(info.inputClipDistances) + "];\n";
    if (info.readsCullDistance)
        v += "float wmgl_in_culldistance[" + maxIn + "][" + std::to_string(info.inputCullDistances) + "];\n";
    v += "float wmgl_tess_outer[4];\nfloat wmgl_tess_inner[2];\nvec3 wmgl_tess_coord;\n"
         "int wmgl_primitive_id, wmgl_patch_vertices_in;\n";
    v += info.outputDeclarations;
    v += "layout(std430) readonly buffer wmgl_TesVertices { uint wmgl_tes_v[]; };\n"
         "layout(std430) readonly buffer wmgl_TesPatches { uint wmgl_tes_p[]; };\n"
         "layout(std430) readonly buffer wmgl_TesControl { uint wmgl_tes_patch_vertices, wmgl_tes_patches_per_instance, "
         "wmgl_tes_pad0, wmgl_tes_pad1; float wmgl_tes_default_outer[4]; float wmgl_tes_default_inner[2]; };\n";
    v += info.functions;
    v += "void main() {\n"
         "    uint wmgl_r = uint(gl_VertexID) * 4u;\n"
         "    uint wmgl_patch = wmgl_tes_v[wmgl_r];\n"
         "    wmgl_tess_coord = vec3(uintBitsToFloat(wmgl_tes_v[wmgl_r + 1u]), uintBitsToFloat(wmgl_tes_v[wmgl_r + 2u]), "
         "uintBitsToFloat(wmgl_tes_v[wmgl_r + 3u]));\n"
         "    wmgl_primitive_id = int(wmgl_patch % max(wmgl_tes_patches_per_instance, 1u));\n";
    // Where each input comes from: the control stage's records, else the
    // captured vertex records. GL matches by name; location matches too.
    std::vector<TessRecordField> vertexFields;
    if (control) {
        vertexFields = control->vertexFields;
    } else if (capture) {
        for (const TransformFeedbackVarying& c : capture->transformFeedback)
            vertexFields.push_back({c.name, c.type, c.size, c.offset, -1});
    }
    auto find = [](const std::vector<TessRecordField>& fields, const std::string& key, int location) -> const TessRecordField* {
        for (const TessRecordField& f : fields)
            if (f.key == key) return &f;
        if (location >= 0)
            for (const TessRecordField& f : fields)
                if (f.location == location) return &f;
        return nullptr;
    };
    // With a control stage, the loads of its N output vertices are unrolled
    // (below): constant indices let Metal keep the inputs in registers, where
    // a loop leaves every input array on the stack (528 bytes per domain
    // point for Foundation's terrain, issue #12).
    size_t unrolledBody = std::string::npos;
    if (control) {
        const uint32_t n = static_cast<uint32_t>(control->verticesOut);
        const uint32_t record = n * control->vertexWords + control->patchWords;
        v += "    wmgl_patch_vertices_in = " + std::to_string(n) + ";\n"
             "    uint wmgl_pb = wmgl_patch * " + std::to_string(record) + "u;\n";
        unrolledBody = v.size();
    } else {
        const uint32_t stride = capture ? capture->xfbStride[0] : 0;
        v += "    wmgl_patch_vertices_in = int(wmgl_tes_patch_vertices);\n"
             "    for (uint wmgl_j = 0u; wmgl_j < wmgl_tes_patch_vertices; ++wmgl_j) {\n"
             "        uint wmgl_b = (wmgl_patch * wmgl_tes_patch_vertices + wmgl_j) * " + std::to_string(stride) + "u;\n";
    }
    auto load = [&](const std::string& target, const std::string& glslType, GLenum type, const std::string& base, uint32_t offset) {
        return "        " + target + " = " + glslLoadValue(glslType, type, "wmgl_tes_p", base + " + " + std::to_string(offset) + "u") +
               ";\n";
    };
    if (const TessRecordField* f = find(vertexFields, "gl_Position", -1))
        v += load("wmgl_in_position[wmgl_j]", "vec4", GL_FLOAT_VEC4, "wmgl_b", f->offset);
    if (const TessRecordField* f = info.readsPointSize ? find(vertexFields, "gl_PointSize", -1) : nullptr)
        v += load("wmgl_in_pointsize[wmgl_j]", "float", GL_FLOAT, "wmgl_b", f->offset);
    if (const TessRecordField* f = info.readsClipDistance ? find(vertexFields, "gl_ClipDistance", -1) : nullptr)
        for (int k = 0; k < std::min(info.inputClipDistances, f->elements); ++k)
            v += load("wmgl_in_clipdistance[wmgl_j][" + std::to_string(k) + "]", "float", GL_FLOAT, "wmgl_b",
                      f->offset + static_cast<uint32_t>(k));
    if (const TessRecordField* f = info.readsCullDistance ? find(vertexFields, "gl_CullDistance", -1) : nullptr)
        for (int k = 0; k < std::min(info.inputCullDistances, f->elements); ++k)
            v += load("wmgl_in_culldistance[wmgl_j][" + std::to_string(k) + "]", "float", GL_FLOAT, "wmgl_b",
                      f->offset + static_cast<uint32_t>(k));
    for (const TessVar& in : info.inputs) {
        if (in.patch) continue;
        const TessRecordField* f = find(vertexFields, varKey(in), in.location);
        if (!f) continue;  // not written by the previous stage: undefined (GL allows it while unread)
        for (int e = 0; e < std::max(1, std::min(in.arraySize, f->elements)); ++e)
            v += load(access(in, "wmgl_j", e), in.glslType, in.type, "wmgl_b", f->offset + static_cast<uint32_t>(e * words(in.type)));
    }
    if (unrolledBody != std::string::npos) {
        const std::string body = v.substr(unrolledBody);
        v.resize(unrolledBody);
        static const std::regex index("\\bwmgl_j\\b");
        for (uint32_t j = 0; j < static_cast<uint32_t>(control->verticesOut); ++j)
            v += "    {\n        uint wmgl_b = wmgl_pb + " + std::to_string(j * control->vertexWords) + "u;\n" +
                 std::regex_replace(body, index, std::to_string(j)) + "    }\n";
    } else {
        v += "    }\n";
    }
    if (control) {
        v += "    uint wmgl_ps = wmgl_pb + " + std::to_string(static_cast<uint32_t>(control->verticesOut) * control->vertexWords) +
             "u;\n";
        for (int k = 0; k < 4; ++k)
            v += "    wmgl_tess_outer[" + std::to_string(k) + "] = uintBitsToFloat(wmgl_tes_p[wmgl_ps + " + std::to_string(k) + "u]);\n";
        for (int k = 0; k < 2; ++k)
            v += "    wmgl_tess_inner[" + std::to_string(k) + "] = uintBitsToFloat(wmgl_tes_p[wmgl_ps + " + std::to_string(4 + k) +
                 "u]);\n";
        for (const TessVar& in : info.inputs) {
            if (!in.patch) continue;
            const TessRecordField* f = find(control->patchFields, varKey(in), in.location);
            if (!f) continue;  // not written by the control stage: undefined
            for (int e = 0; e < std::max(1, std::min(in.arraySize, f->elements)); ++e)
                v += "    " + access(in, "", e) + " = " +
                     glslLoadValue(in.glslType, in.type, "wmgl_tes_p",
                                   "wmgl_ps + " + std::to_string(f->offset + static_cast<uint32_t>(e * words(in.type))) + "u") +
                     ";\n";
        }
    } else {
        v += "    for (int wmgl_k = 0; wmgl_k < 4; ++wmgl_k) wmgl_tess_outer[wmgl_k] = wmgl_tes_default_outer[wmgl_k];\n"
             "    for (int wmgl_k = 0; wmgl_k < 2; ++wmgl_k) wmgl_tess_inner[wmgl_k] = wmgl_tes_default_inner[wmgl_k];\n";
    }
    v += "    wmgl_tess_main();\n}\n";
    return v;
}

std::string postTessellationVertexMsl(const std::string& msl, const std::string& entryPoint, bool quads) {
    // The entry point's parameters: the vertex number (which indexed the
    // domain points) becomes the patch number and the position in the patch.
    const std::regex entry("\\nvertex [A-Za-z0-9_]+ " + entryPoint + "\\(");
    std::smatch m;
    if (!std::regex_search(msl, m, entry)) return "";
    const size_t start = static_cast<size_t>(m.position(0)) + 1, open = start + m.length(0) - 2;
    size_t close = open + 1;
    for (int depth = 1; close < msl.size(); ++close) {
        depth += msl[close] == '(' ? 1 : msl[close] == ')' ? -1 : 0;
        if (!depth) break;
    }
    const size_t body = msl.find('{', close);
    if (close >= msl.size() || body == std::string::npos) return "";
    std::string parameters = msl.substr(open, close - open);
    const std::string vertexId = "uint gl_VertexIndex [[vertex_id]]";
    const size_t at = parameters.find(vertexId);
    if (at == std::string::npos || parameters.find("[[stage_in]]") != std::string::npos ||
        parameters.find("[[base_vertex]]") != std::string::npos)
        return "";
    parameters.replace(at, vertexId.size(),
                       std::string("uint wmgl_patch_id [[patch_id]], ") + (quads ? "float2" : "float3") +
                           " wmgl_position_in_patch [[position_in_patch]]");
    // Only main() may read the domain points: a helper that does (transform
    // feedback capture wraps main) has no [[patch_id]] to read instead.
    if (msl.find(".wmgl_tes_v[") < body) return "";
    std::string code = msl.substr(body + 1);
    // Reads of the domain point (patch, u, v, w) by the generated main().
    static const std::regex patch("_[A-Za-z0-9_]+\\.wmgl_tes_v\\[wmgl_r\\]");
    static const std::regex coord("as_type<float>\\(_[A-Za-z0-9_]+\\.wmgl_tes_v\\[wmgl_r \\+ ([123])u\\]\\)");
    code = std::regex_replace(code, patch, "wmgl_patch_id");
    std::string out;
    auto last = code.cbegin();
    for (std::sregex_iterator it(code.begin(), code.end(), coord), end; it != end; ++it) {
        out.append(last, (*it)[0].first);
        const char component = (*it)[1].str()[0];
        out += component == '1' ? "wmgl_position_in_patch.x" : component == '2' ? "wmgl_position_in_patch.y"
               : quads              ? "0.0"
                                    : "wmgl_position_in_patch.z";
        last = (*it)[0].second;
    }
    out.append(last, code.cend());
    if (out.find("wmgl_tes_v[") != std::string::npos || out.find("[[vertex_id]]") != std::string::npos) return "";
    return msl.substr(0, start) + (quads ? "[[patch(quad)]] " : "[[patch(triangle)]] ") + msl.substr(start, open - start) +
           parameters + msl.substr(close, body + 1 - close) + "\n    uint gl_VertexIndex = 0u;" + out;
}

}  // namespace wmgl::shader
