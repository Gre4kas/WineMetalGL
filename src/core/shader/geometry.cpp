#include "core/shader/geometry.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <regex>
#include <set>
#include <sstream>

#include "core/shader/glsl_upgrade.h"

namespace wmgl::shader {
namespace {

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

GLenum glslTypeEnum(const std::string& t) {
    static const std::map<std::string, GLenum> kTypes = {
        {"float", GL_FLOAT}, {"vec2", GL_FLOAT_VEC2}, {"vec3", GL_FLOAT_VEC3}, {"vec4", GL_FLOAT_VEC4},
        {"int", GL_INT}, {"ivec2", GL_INT_VEC2}, {"ivec3", GL_INT_VEC3}, {"ivec4", GL_INT_VEC4},
        {"uint", GL_UNSIGNED_INT}, {"uvec2", GL_UNSIGNED_INT_VEC2}, {"uvec3", GL_UNSIGNED_INT_VEC3},
        {"uvec4", GL_UNSIGNED_INT_VEC4}, {"mat2", GL_FLOAT_MAT2}, {"mat3", GL_FLOAT_MAT3}, {"mat4", GL_FLOAT_MAT4},
        {"mat2x2", GL_FLOAT_MAT2}, {"mat3x3", GL_FLOAT_MAT3}, {"mat4x4", GL_FLOAT_MAT4}, {"mat2x3", GL_FLOAT_MAT2x3},
        {"mat2x4", GL_FLOAT_MAT2x4}, {"mat3x2", GL_FLOAT_MAT3x2}, {"mat3x4", GL_FLOAT_MAT3x4},
        {"mat4x2", GL_FLOAT_MAT4x2}, {"mat4x3", GL_FLOAT_MAT4x3},
        {"double", GL_DOUBLE}, {"dvec2", GL_DOUBLE_VEC2}, {"dvec3", GL_DOUBLE_VEC3}, {"dvec4", GL_DOUBLE_VEC4},
        {"dmat2", GL_DOUBLE_MAT2}, {"dmat3", GL_DOUBLE_MAT3}, {"dmat4", GL_DOUBLE_MAT4}, {"dmat2x2", GL_DOUBLE_MAT2},
        {"dmat3x3", GL_DOUBLE_MAT3}, {"dmat4x4", GL_DOUBLE_MAT4}, {"dmat2x3", GL_DOUBLE_MAT2x3}, {"dmat2x4", GL_DOUBLE_MAT2x4},
        {"dmat3x2", GL_DOUBLE_MAT3x2}, {"dmat3x4", GL_DOUBLE_MAT3x4}, {"dmat4x2", GL_DOUBLE_MAT4x2},
        {"dmat4x3", GL_DOUBLE_MAT4x3},
    };
    auto it = kTypes.find(t);
    return it == kTypes.end() ? 0 : it->second;
}

// "flat out vec4 name[3]" -> qualifiers stripped; returns direction ("in"/"out", or "").
std::string parseVariable(const std::string& decl, std::string& type, std::string& name, int& arraySize, int* innerSize = nullptr) {
    std::string d = decl;
    if (d.compare(0, 7, "layout(") == 0) d = trim(d.substr(d.find(')') + 1));
    std::istringstream words(d);
    std::string word, direction;
    std::vector<std::string> rest;
    while (words >> word) {
        if (word == "flat" || word == "noperspective" || word == "smooth" || word == "centroid" || word == "invariant" ||
            word == "sample" || word == "highp" || word == "mediump" || word == "lowp")
            continue;
        if (word == "in" || word == "out") {
            direction = word;
            continue;
        }
        rest.push_back(word);
    }
    arraySize = 0;
    if (rest.size() != 2) return direction;
    type = rest[0];
    name = rest[1];
    if (!name.empty() && name.back() == ';') name.pop_back();
    const size_t open = name.find('[');
    if (open != std::string::npos) {
        arraySize = std::atoi(name.c_str() + open + 1);
        // A second dimension (a geometry input's array per vertex).
        const size_t second = name.find('[', open + 1);
        if (innerSize) *innerSize = second != std::string::npos ? std::atoi(name.c_str() + second + 1) : 0;
        name = name.substr(0, open);
    } else if (innerSize) {
        *innerSize = 0;
    }
    return direction;
}

// The vertex stream of a declaration "layout(..., stream = N) ..." (0 when absent).
int layoutStream(const std::string& decl) {
    if (decl.compare(0, 7, "layout(") != 0) return 0;
    std::istringstream parts(decl.substr(7, decl.find(')') - 7));
    std::string part;
    while (std::getline(parts, part, ',')) {
        part = trim(part);
        if (part.compare(0, 6, "stream") == 0 && part.find('=') != std::string::npos) return std::atoi(part.c_str() + part.find('=') + 1);
    }
    return 0;
}

// The declaration without its stream qualifier (vertex shaders have none).
std::string withoutStream(const std::string& decl) {
    std::string out = decl;
    // The word "stream" inside a layout qualifier, not in a name such as "out_stream0".
    auto qualifier = [&](size_t from) {
        for (size_t at = out.find("stream", from); at != std::string::npos; at = out.find("stream", at + 1)) {
            const bool word = (at == 0 || !(std::isalnum(static_cast<unsigned char>(out[at - 1])) || out[at - 1] == '_')) &&
                              (at + 6 >= out.size() || !(std::isalnum(static_cast<unsigned char>(out[at + 6])) || out[at + 6] == '_'));
            const size_t layout = out.rfind("layout(", at);
            if (word && layout != std::string::npos && out.find(')', layout) > at) return at;
        }
        return std::string::npos;
    };
    for (size_t at = qualifier(0); at != std::string::npos; at = qualifier(at)) {
        size_t end = at + 6;
        while (end < out.size() && (out[end] == ' ' || out[end] == '=' || std::isdigit(static_cast<unsigned char>(out[end])))) ++end;
        size_t start = at;
        // The comma joining it to the other qualifiers.
        while (start > 0 && out[start - 1] == ' ') --start;
        if (start > 0 && out[start - 1] == ',') --start;
        else if (end < out.size() && out[end] == ',') ++end;
        out.erase(start, end - start);
        at = start;
    }
    // "layout()" left empty.
    for (size_t at = out.find("layout("); at != std::string::npos; at = out.find("layout(", at + 1)) {
        const size_t close = out.find(')', at);
        if (close != std::string::npos && trim(out.substr(at + 7, close - at - 7)).empty()) {
            size_t end = close + 1;
            while (end < out.size() && out[end] == ' ') ++end;
            out.erase(at, end - at);
        }
    }
    return out;
}

int verticesForInput(GLenum type) {
    switch (type) {
    case GL_POINTS: return 1;
    case GL_LINES: return 2;
    case GL_LINES_ADJACENCY: return 4;
    case GL_TRIANGLES: return 3;
    case GL_TRIANGLES_ADJACENCY: return 6;
    default: return 0;
    }
}

// Converts words at `index` expressions into a value of `type`.
std::string loadValue(const std::string& type, GLenum glType, const std::string& buffer, const std::string& base) {
    const int n = glTypeComponents(glType);
    if (glTypeIsDouble(glType)) {
        // Two words per component, low word first (packDouble2x32).
        std::string args;
        for (int c = 0; c < n; ++c) {
            if (!args.empty()) args += ", ";
            args += "packDouble2x32(uvec2(" + buffer + "[" + base + " + " + std::to_string(2 * c) + "u], " + buffer + "[" + base +
                    " + " + std::to_string(2 * c + 1) + "u]))";
        }
        return type + "(" + args + ")";
    }
    const char* kind = glType == GL_INT || glType == GL_INT_VEC2 || glType == GL_INT_VEC3 || glType == GL_INT_VEC4 ? "int"
                       : glType == GL_UNSIGNED_INT || glType == GL_UNSIGNED_INT_VEC2 || glType == GL_UNSIGNED_INT_VEC3 ||
                               glType == GL_UNSIGNED_INT_VEC4
                           ? "uint"
                           : "float";
    std::string args;
    for (int c = 0; c < n; ++c) {
        const std::string word = buffer + "[" + base + " + " + std::to_string(c) + "u]";
        if (!args.empty()) args += ", ";
        args += std::string(kind) == "float" ? "uintBitsToFloat(" + word + ")" : std::string(kind) == "int" ? "int(" + word + ")" : word;
    }
    return type + "(" + args + ")";
}

// Replaces gl_in[EXPR].gl_X with replacement[EXPR].
std::string renameGlIn(const std::string& text, GeometryShaderInfo& info) {
    std::string out;
    size_t i = 0;
    while (true) {
        const size_t at = text.find("gl_in[", i);
        if (at == std::string::npos || (at > 0 && isIdent(text[at - 1]))) {
            if (at == std::string::npos) break;
            out.append(text, i, at + 6 - i);
            i = at + 6;
            continue;
        }
        size_t j = at + 6;
        int depth = 1;
        while (j < text.size() && depth) {
            if (text[j] == '[') ++depth;
            else if (text[j] == ']') --depth;
            ++j;
        }
        const std::string index = text.substr(at + 6, j - at - 7);
        std::string member;
        if (j < text.size() && text[j] == '.') {
            size_t k = j + 1;
            while (k < text.size() && isIdent(text[k])) ++k;
            member = text.substr(j + 1, k - j - 1);
            j = k;
        }
        out.append(text, i, at - i);
        if (member == "gl_PointSize") {
            info.readsPointSize = true;
            out += "wmgl_in_pointsize[" + index + "]";
        } else if (member == "gl_ClipDistance") {
            info.readsClipDistance = true;
            out += "wmgl_in_clipdistance[" + index + "]";
        } else if (member == "gl_CullDistance") {
            info.readsCullDistance = true;
            out += "wmgl_in_culldistance[" + index + "]";
        } else {
            out += "wmgl_in_position[" + index + "]";
        }
        i = j;
    }
    out.append(text, i, std::string::npos);
    return out;
}

// The varying name the capture pass records an input under: GL names, so
// identifiers escaped for codegen (escapeVulkanKeywords) are restored.
std::string captureName(const GeometryVar& v) {
    return unescapeName((v.member.empty() ? v.name : v.blockName + "." + v.member) + v.path);
}

// A struct's fields: (type, name, array size).
struct StructField { std::string type, name; int arraySize = 0; };
using StructTable = std::map<std::string, std::vector<StructField>>;

// The fields of a struct definition's body ("float a; vec3 b[2]; ...").
std::vector<StructField> structFields(const std::string& body) {
    std::vector<StructField> fields;
    std::istringstream members(body);
    std::string member;
    while (std::getline(members, member, ';')) {
        member = trim(member);
        if (member.empty()) continue;
        StructField f;
        parseVariable(member, f.type, f.name, f.arraySize);
        if (!f.name.empty()) fields.push_back(f);
    }
    return fields;
}

// Appends the leaves (non-struct fields) of a `type` value at `path`:
// (type, path, array size). False when a field's type is not supported.
bool structLeaves(const StructTable& structs, const std::string& type, const std::string& path,
                  std::vector<StructField>& leaves, int depth = 0) {
    const auto it = structs.find(type);
    if (it == structs.end() || depth > 8) return false;
    for (const StructField& f : it->second) {
        if (structs.count(f.type)) {
            for (int e = 0; e < std::max(1, f.arraySize); ++e)
                if (!structLeaves(structs, f.type, path + "." + f.name + (f.arraySize ? "[" + std::to_string(e) + "]" : ""),
                                  leaves, depth + 1))
                    return false;
        } else if (glslTypeEnum(f.type)) {
            leaves.push_back({f.type, path + "." + f.name, f.arraySize});
        } else {
            return false;
        }
    }
    return true;
}

// One GeometryVar per leaf of `base`, a variable or block member of struct
// type `structType` (an array of `structArray` elements, or 0).
bool addStructLeaves(const StructTable& structs, const GeometryVar& base, const std::string& structType, int structArray,
                     std::vector<GeometryVar>& out) {
    std::vector<StructField> leaves;
    for (int e = 0; e < std::max(1, structArray); ++e)
        if (!structLeaves(structs, structType, structArray ? "[" + std::to_string(e) + "]" : "", leaves)) return false;
    for (const StructField& leaf : leaves) {
        GeometryVar v = base;
        v.glslType = leaf.type;
        v.type = glslTypeEnum(leaf.type);
        v.arraySize = leaf.arraySize;
        v.path = leaf.name;
        v.structType = structType;
        v.structArray = structArray;
        out.push_back(v);
    }
    return true;
}

}  // namespace

bool parseGeometryGlsl(const std::string& glsl, GeometryShaderInfo& info, std::string& log) {
    std::vector<std::string> lines;
    {
        std::istringstream stream(glsl);
        std::string line;
        while (std::getline(stream, line)) lines.push_back(line);
    }
    // Globals end where the first function definition starts.
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
    // Global statements (blocks are gathered whole).
    bool inputTyped = false, outputTyped = false;  // GL_POINTS is 0
    StructTable structs;
    std::vector<std::string> statements;
    std::string current;
    depth = 0;
    for (size_t i = 0; i < functionsStart; ++i) {
        const std::string t = trim(lines[i]);
        if (t.empty() || t.compare(0, 8, "#version") == 0) continue;
        if (t[0] == '#') {
            if (depth == 0 && current.empty()) statements.push_back(t);
            continue;
        }
        current += (current.empty() ? "" : "\n") + t;
        for (char c : t) depth += c == '{' ? 1 : c == '}' ? -1 : 0;
        if (depth == 0 && t.back() == ';') {
            statements.push_back(current);
            current.clear();
        }
    }
    for (const std::string& s : statements) {
        const std::string flat = trim(s);
        if (flat[0] == '#') {
            continue;  // extensions are re-declared by the generated sources as needed
        }
        const size_t brace = flat.find('{');
        if (brace == std::string::npos) {
            // Primitive layout declarations.
            if (flat.compare(0, 7, "layout(") == 0 && (flat.find(") in;") != std::string::npos || flat.find(") out;") != std::string::npos)) {
                const std::string args = flat.substr(7, flat.find(')') - 7);
                const bool input = flat.find(") in;") != std::string::npos;
                std::istringstream parts(args);
                std::string part;
                while (std::getline(parts, part, ',')) {
                    part = trim(part);
                    if (input) {
                        if (part == "points") info.inputType = GL_POINTS, inputTyped = true;
                        else if (part == "lines") info.inputType = GL_LINES, inputTyped = true;
                        else if (part == "lines_adjacency") info.inputType = GL_LINES_ADJACENCY, inputTyped = true;
                        else if (part == "triangles") info.inputType = GL_TRIANGLES, inputTyped = true;
                        else if (part == "triangles_adjacency") info.inputType = GL_TRIANGLES_ADJACENCY, inputTyped = true;
                        else if (part.compare(0, 11, "invocations") == 0) info.invocations = std::max(1, std::atoi(part.c_str() + part.find('=') + 1));
                    } else {
                        if (part == "points") info.outputType = GL_POINTS, outputTyped = true;
                        else if (part == "line_strip") info.outputType = GL_LINE_STRIP, outputTyped = true;
                        else if (part == "triangle_strip") info.outputType = GL_TRIANGLE_STRIP, outputTyped = true;
                        else if (part.compare(0, 12, "max_vertices") == 0) info.maxVertices = std::atoi(part.c_str() + part.find('=') + 1);
                    }
                }
                continue;
            }
            std::string type, name;
            int arraySize = 0, innerSize = 0;
            const std::string direction = parseVariable(flat, type, name, arraySize, &innerSize);
            if (direction.empty()) {
                // "invariant gl_Position;" (wined3d) names a built-in output the
                // kernel, a compute shader, does not have.
                static const std::regex kBuiltinQualifier(R"(^\s*(invariant|precise)\s+gl_\w+(\s*,\s*gl_\w+)*\s*;?\s*$)");
                if (!std::regex_match(flat, kBuiltinQualifier)) info.globals += flat + "\n";
                continue;
            }
            const GLenum glType = glslTypeEnum(type);
            if (!glType && structs.count(type) && name.rfind("gl_", 0) != 0) {
                // Struct-typed variables are stored field by field. Inputs are
                // arrays of vertices: their own array size is the second dimension.
                GeometryVar base{type, 0, name, "", "", 0, flat};
                if (direction == "out") base.stream = layoutStream(flat);
                if (!addStructLeaves(structs, base, type, direction == "out" ? arraySize : innerSize,
                                     direction == "in" ? info.inputs : info.outputs)) {
                    log += "error: geometry shader " + direction + "put '" + name + "' has an unsupported type\n";
                    return false;
                }
                continue;
            }
            if (!glType) {
                log += "error: geometry shader " + direction + "put '" + name + "' has an unsupported type\n";
                return false;
            }
            if (name.rfind("gl_", 0) == 0) {
                // Redeclared built-in arrays: "out float gl_ClipDistance[N];".
                if (direction == "out" && name == "gl_ClipDistance") info.outputClipDistances = arraySize;
                if (direction == "out" && name == "gl_CullDistance") info.outputCullDistances = arraySize;
                continue;
            }
            // Inputs are arrays of vertices: their own array size is the second dimension.
            GeometryVar v{type, glType, name, "", "", direction == "out" ? arraySize : innerSize, flat};
            if (direction == "out") v.stream = layoutStream(flat);
            (direction == "in" ? info.inputs : info.outputs).push_back(v);
            continue;
        }
        // Blocks: header { members } tail;
        const std::string header = trim(flat.substr(0, brace));
        const size_t close = flat.rfind('}');
        const std::string body = flat.substr(brace + 1, close - brace - 1);
        std::string tail = trim(flat.substr(close + 1));
        if (!tail.empty() && tail.back() == ';') tail.pop_back();
        tail = trim(tail);
        std::string headerNoLayout = header.compare(0, 7, "layout(") == 0 ? trim(header.substr(header.find(')') + 1)) : header;
        std::istringstream hw(headerNoLayout);
        std::string direction, blockName;
        hw >> direction >> blockName;
        if (direction == "struct") {
            structs[blockName] = structFields(body);
            info.structs += flat + "\n";
        }
        if (direction != "in" && direction != "out") {
            info.globals += flat + "\n";  // uniform / buffer blocks
            continue;
        }
        std::string instance = tail;
        const size_t bracket = instance.find('[');
        // Input blocks are arrays of vertices; an output block may be an array of blocks.
        const int instanceSize = bracket != std::string::npos && direction == "out" ? std::atoi(instance.c_str() + bracket + 1) : 0;
        if (bracket != std::string::npos) instance = instance.substr(0, bracket);
        std::istringstream members(body);
        std::string member;
        if (blockName == "gl_PerVertex") {
            while (std::getline(members, member, ';')) {
                std::string type, name;
                int arraySize = 0;
                parseVariable(trim(member), type, name, arraySize);
                if (name == "gl_ClipDistance") (direction == "in" ? info.inputClipDistances : info.outputClipDistances) = arraySize;
                if (name == "gl_CullDistance") (direction == "in" ? info.inputCullDistances : info.outputCullDistances) = arraySize;
            }
            continue;
        }
        while (std::getline(members, member, ';')) {
            member = trim(member);
            if (member.empty()) continue;
            std::string type, name;
            int arraySize = 0;
            parseVariable(member, type, name, arraySize);
            const GLenum glType = glslTypeEnum(type);
            if (!glType && !structs.count(type)) {
                log += "error: geometry shader block member '" + name + "' has an unsupported type\n";
                return false;
            }
            GeometryVar v{type, glType, instance.empty() ? name : instance, instance.empty() ? "" : name, blockName,
                          arraySize, ""};
            if (direction == "out") {
                v.declaration = flat;  // whole block, emitted once by the pass-through
                v.stream = layoutStream(member) ? layoutStream(member) : layoutStream(header);
                v.instanceSize = instance.empty() ? 0 : instanceSize;
            }
            if (!glType) {
                // A struct-typed member, stored field by field.
                v.arraySize = 0;
                if (!addStructLeaves(structs, v, type, arraySize, direction == "in" ? info.inputs : info.outputs)) {
                    log += "error: geometry shader block member '" + name + "' has an unsupported type\n";
                    return false;
                }
                continue;
            }
            (direction == "in" ? info.inputs : info.outputs).push_back(v);
        }
    }
    info.verticesIn = verticesForInput(info.inputType);
    if (!inputTyped || !outputTyped || info.maxVertices < 0) {
        log += "error: geometry shader layout is incomplete\n";
        return false;
    }
    // Function bodies with built-ins renamed.
    std::string functions;
    for (size_t i = functionsStart; i < lines.size(); ++i) functions += lines[i] + "\n";
    functions = renameGlIn(functions, info);
    info.writesPointSize = functions.find("gl_PointSize") != std::string::npos;
    info.writesLayer = functions.find("gl_Layer") != std::string::npos;
    info.writesViewportIndex = functions.find("gl_ViewportIndex") != std::string::npos;
    info.writesPrimitiveId = functions.find("gl_PrimitiveID") != std::string::npos &&
                             replaceWord(functions, "gl_PrimitiveIDIn", "").find("gl_PrimitiveID") != std::string::npos;
    if (functions.find("gl_ClipDistance") != std::string::npos && !info.outputClipDistances) info.outputClipDistances = 8;
    if (info.readsClipDistance && !info.inputClipDistances) info.inputClipDistances = 8;
    // gl_PerVertex declares gl_CullDistance whether or not the shader writes
    // it: unwritten distances must not reach the cull test.
    if (functions.find("gl_CullDistance") == std::string::npos) info.outputCullDistances = 0;
    else if (!info.outputCullDistances) info.outputCullDistances = std::max(1, 8 - info.outputClipDistances);
    if (info.readsCullDistance && !info.inputCullDistances) info.inputCullDistances = 8;
    const std::pair<const char*, const char*> renames[] = {
        {"gl_PrimitiveIDIn", "wmgl_primitive_id_in"}, {"gl_PrimitiveID", "wmgl_out_primitive_id"},
        {"gl_Position", "wmgl_out_position"}, {"gl_PointSize", "wmgl_out_pointsize"},
        {"gl_ClipDistance", "wmgl_out_clipdistance"}, {"gl_CullDistance", "wmgl_out_culldistance"}, {"gl_Layer", "wmgl_out_layer"},
        {"gl_ViewportIndex", "wmgl_out_viewport"}, {"gl_InvocationID", "wmgl_invocation_id"},
        {"EmitVertex", "wmgl_EmitVertex"}, {"EndPrimitive", "wmgl_EndPrimitive"},
        {"EmitStreamVertex", "wmgl_EmitStreamVertex"}, {"EndStreamPrimitive", "wmgl_EndStreamPrimitive"},
    };
    info.streams = functions.find("EmitStreamVertex") != std::string::npos;
    for (const GeometryVar& v : info.outputs) info.streams |= v.stream != 0;
    for (const auto& [from, to] : renames) functions = replaceWord(functions, from, to);
    // The shader's main becomes a function the kernel calls.
    const size_t mainAt = functions.find("void main()");
    if (mainAt == std::string::npos) {
        log += "error: geometry shader has no main()\n";
        return false;
    }
    functions.replace(mainAt, 11, "void wmgl_gs_main()");
    info.functions = functions;
    return true;
}

std::vector<std::string> geometryCaptureVaryings(const GeometryShaderInfo& info) {
    std::vector<std::string> varyings = {"gl_Position"};
    if (info.readsPointSize) varyings.push_back("gl_PointSize");
    if (info.readsClipDistance) varyings.push_back("gl_ClipDistance");
    if (info.readsCullDistance) varyings.push_back("gl_CullDistance");
    for (const GeometryVar& v : info.inputs) varyings.push_back(captureName(v));
    return varyings;
}

GeometrySources geometrySources(const GeometryShaderInfo& info, const LinkResult& capture) {
    GeometrySources out;
    // max_vertices = 0 emits nothing (the draw is skipped); the code keeps one-vertex arrays.
    const int n = info.verticesIn, maxV = std::max(1, info.maxVertices);
    const bool strips = info.outputType != GL_POINTS;
    const std::string inStride = std::to_string(capture.xfbStride[0]);
    auto captured = [&](const std::string& varying) -> const TransformFeedbackVarying* {
        for (const TransformFeedbackVarying& v : capture.transformFeedback)
            if (v.name == varying) return &v;
        return nullptr;
    };

    // Output vertex record: position, optional built-ins, then user outputs.
    struct Field { std::string expr; GLenum type; std::string glslType; uint32_t offset; };
    std::vector<Field> fields;
    uint32_t words = 0;
    auto addField = [&](const std::string& expr, GLenum type, const std::string& glslType) {
        fields.push_back({expr, type, glslType, words});
        words += static_cast<uint32_t>(glTypeWords(type));
    };
    // Arrays are stored element by element; `out.fields` names the whole array.
    auto addVar = [&](const std::string& glName, const std::string& expr, GLenum type, const std::string& glslType, int arraySize) {
        out.fields.push_back({glName, type, std::max(1, arraySize), words});
        if (arraySize)
            for (int e = 0; e < arraySize; ++e) addField(expr + "[" + std::to_string(e) + "]", type, glslType);
        else
            addField(expr, type, glslType);
    };
    addVar("gl_Position", "wmgl_out_position", GL_FLOAT_VEC4, "vec4", 0);
    if (info.writesPointSize) addVar("gl_PointSize", "wmgl_out_pointsize", GL_FLOAT, "float", 0);
    if (info.outputClipDistances) addVar("gl_ClipDistance", "wmgl_out_clipdistance", GL_FLOAT, "float", info.outputClipDistances);
    if (info.outputCullDistances) addVar("gl_CullDistance", "wmgl_out_culldistance", GL_FLOAT, "float", info.outputCullDistances);
    if (info.writesLayer) addVar("gl_Layer", "wmgl_out_layer", GL_INT, "int", 0);
    if (info.writesViewportIndex) addVar("gl_ViewportIndex", "wmgl_out_viewport", GL_INT, "int", 0);
    for (const GeometryVar& v : info.outputs) {
        if (!v.instanceSize) {
            addVar((v.member.empty() ? v.name : v.blockName + "." + v.member) + v.path,
                   (v.member.empty() ? v.name : v.name + "." + v.member) + v.path, v.type, v.glslType, v.arraySize);
            continue;
        }
        // An array of blocks: each element's member ("Block[k].member").
        for (int k = 0; k < v.instanceSize; ++k) {
            const std::string element = "[" + std::to_string(k) + "].";
            addVar(v.blockName + element + v.member + v.path, v.name + element + v.member + v.path, v.type, v.glslType,
                   v.arraySize);
            out.fields.back().stream = v.stream;
        }
    }
    // Vertex streams: the record ends with the stream it was emitted to.
    if (info.streams) out.streamOffset = words++;
    out.outputStride = words;
    for (GeometryEmulation::Field& f : out.fields)
        for (const GeometryVar& v : info.outputs)
            if (f.name == (v.member.empty() ? v.name : v.blockName + "." + v.member) + v.path) f.stream = v.stream;
    const std::string stride = std::to_string(words), maxS = std::to_string(maxV), region = std::to_string(2 * maxV);

    // ---- compute kernel
    std::string& c = out.compute;
    c += "#version 450\nlayout(local_size_x = 64) in;\n";
    c += info.globals;
    // A block member's declaration in the struct standing in for its block
    // (a struct-typed member's fields are declared once, as the struct).
    auto memberDeclarations = [](const std::vector<GeometryVar>& vars, const std::string& instance) {
        std::string d;
        std::set<std::string> declared;
        for (const GeometryVar& m : vars) {
            if (m.name != instance || !declared.insert(m.member).second) continue;
            if (!m.path.empty())
                d += " " + m.structType + " " + m.member + (m.structArray ? "[" + std::to_string(m.structArray) + "]" : "") + ";";
            else
                d += " " + m.glslType + " " + m.member + (m.arraySize ? "[" + std::to_string(m.arraySize) + "]" : "") + ";";
        }
        return d;
    };
    std::set<std::string> declaredInputs;
    for (const GeometryVar& v : info.inputs) {
        if (!declaredInputs.insert(v.name).second) continue;
        if (v.member.empty()) {
            const std::string type = v.path.empty() ? v.glslType : v.structType;
            const int array = v.path.empty() ? v.arraySize : v.structArray;
            c += type + " " + v.name + "[" + std::to_string(n) + "]" + (array ? "[" + std::to_string(array) + "]" : "") + ";\n";
        } else {
            const std::string structName = "wmgl_in_" + v.blockName;
            c += "struct " + structName + " {" + memberDeclarations(info.inputs, v.name) + " };\n" + structName + " " + v.name + "[" +
                 std::to_string(n) + "];\n";
        }
    }
    std::set<std::string> declaredOutputs;
    for (const GeometryVar& v : info.outputs) {
        if (!declaredOutputs.insert(v.name).second) continue;
        if (v.member.empty()) {
            const std::string type = v.path.empty() ? v.glslType : v.structType;
            const int array = v.path.empty() ? v.arraySize : v.structArray;
            c += type + " " + v.name + (array ? "[" + std::to_string(array) + "]" : "") + ";\n";
        } else {
            c += "struct wmgl_out_" + v.blockName + " {" + memberDeclarations(info.outputs, v.name) + " };\nwmgl_out_" +
                 v.blockName + " " + v.name + (v.instanceSize ? "[" + std::to_string(v.instanceSize) + "]" : "") + ";\n";
        }
    }
    c += "vec4 wmgl_in_position[" + std::to_string(n) + "];\n";
    if (info.readsPointSize) c += "float wmgl_in_pointsize[" + std::to_string(n) + "];\n";
    if (info.readsClipDistance)
        c += "float wmgl_in_clipdistance[" + std::to_string(n) + "][" + std::to_string(info.inputClipDistances) + "];\n";
    if (info.readsCullDistance)
        c += "float wmgl_in_culldistance[" + std::to_string(n) + "][" + std::to_string(info.inputCullDistances) + "];\n";
    c += "vec4 wmgl_out_position;\nfloat wmgl_out_pointsize;\nint wmgl_out_layer;\nint wmgl_out_viewport;\nint wmgl_out_primitive_id;\n"
         "int wmgl_primitive_id_in;\nint wmgl_invocation_id;\n";
    if (info.outputClipDistances) c += "float wmgl_out_clipdistance[" + std::to_string(info.outputClipDistances) + "];\n";
    if (info.outputCullDistances) c += "float wmgl_out_culldistance[" + std::to_string(info.outputCullDistances) + "];\n";
    c += "buffer wmgl_GsInput { uint wmgl_gs_in[]; };\n"
         "buffer wmgl_GsOutput { uint wmgl_gs_out[]; };\n"
         "buffer wmgl_GsIndices { uint wmgl_gs_indices[]; };\n"
         "buffer wmgl_GsControl { uint wmgl_gs_prim_count, wmgl_gs_prims_per_instance, wmgl_gs_prim_base, wmgl_gs_pad; };\n"
         "uint wmgl_gs_thread, wmgl_gs_emitted, wmgl_gs_cursor;\n";
    c += "void wmgl_EmitStreamVertex(int wmgl_stream) {\n"
         "    if (wmgl_gs_emitted >= " + maxS + "u) return;\n"
         "    uint wmgl_slot = wmgl_gs_thread * " + maxS + "u + wmgl_gs_emitted;\n"
         "    uint wmgl_o = wmgl_slot * " + stride + "u;\n";
    for (const Field& f : fields) {
        uint32_t w = f.offset;
        for (const std::string& word : captureWords(f.expr, f.type))
            c += "    wmgl_gs_out[wmgl_o + " + std::to_string(w++) + "u] = " + word + ";\n";
    }
    if (info.streams) c += "    wmgl_gs_out[wmgl_o + " + std::to_string(out.streamOffset) + "u] = uint(wmgl_stream);\n";
    c += "    wmgl_gs_indices[wmgl_gs_thread * " + region + "u + wmgl_gs_cursor++] = wmgl_slot;\n";
    c += "    wmgl_gs_emitted++;\n}\n";
    c += "void wmgl_EmitVertex() { wmgl_EmitStreamVertex(0); }\n";
    c += "void wmgl_EndPrimitive();\n"
         "void wmgl_EndStreamPrimitive(int wmgl_stream) { wmgl_EndPrimitive(); }\n";
    c += "void wmgl_EndPrimitive() {\n";
    if (strips)
        c += "    if (wmgl_gs_cursor > 0u && wmgl_gs_indices[wmgl_gs_thread * " + region + "u + wmgl_gs_cursor - 1u] != 0xFFFFFFFFu)\n"
             "        wmgl_gs_indices[wmgl_gs_thread * " + region + "u + wmgl_gs_cursor++] = 0xFFFFFFFFu;\n";
    c += "}\n";
    c += info.functions;
    // One thread per (input primitive, invocation); its output slots and
    // index region are per thread, so invocations of a primitive emit in
    // invocation order.
    const std::string invocations = std::to_string(info.invocations);
    c += "void main() {\n"
         "    wmgl_gs_thread = gl_GlobalInvocationID.x;\n"
         "    if (wmgl_gs_thread >= wmgl_gs_prim_count * " + invocations + "u) return;\n"
         "    uint wmgl_gs_prim = wmgl_gs_thread / " + invocations + "u;\n"
         "    wmgl_invocation_id = int(wmgl_gs_thread % " + invocations + "u);\n"
         "    wmgl_out_viewport = 0;\n    wmgl_out_layer = 0;\n"
         "    wmgl_gs_emitted = 0u;\n    wmgl_gs_cursor = 0u;\n"
         "    for (uint wmgl_j = 0u; wmgl_j < " + std::to_string(n) + "u; ++wmgl_j) {\n"
         "        uint wmgl_b = (wmgl_gs_prim * " + std::to_string(n) + "u + wmgl_j) * " + inStride + "u;\n";
    if (const TransformFeedbackVarying* p = captured("gl_Position"))
        c += "        wmgl_in_position[wmgl_j] = " + loadValue("vec4", GL_FLOAT_VEC4, "wmgl_gs_in", "wmgl_b + " + std::to_string(p->offset) + "u") + ";\n";
    if (const TransformFeedbackVarying* p = info.readsPointSize ? captured("gl_PointSize") : nullptr)
        c += "        wmgl_in_pointsize[wmgl_j] = " + loadValue("float", GL_FLOAT, "wmgl_gs_in", "wmgl_b + " + std::to_string(p->offset) + "u") + ";\n";
    if (const TransformFeedbackVarying* p = info.readsClipDistance ? captured("gl_ClipDistance") : nullptr)
        for (int k = 0; k < std::min(info.inputClipDistances, p->size); ++k)
            c += "        wmgl_in_clipdistance[wmgl_j][" + std::to_string(k) + "] = " +
                 loadValue("float", GL_FLOAT, "wmgl_gs_in", "wmgl_b + " + std::to_string(p->offset + k) + "u") + ";\n";
    if (const TransformFeedbackVarying* p = info.readsCullDistance ? captured("gl_CullDistance") : nullptr)
        for (int k = 0; k < std::min(info.inputCullDistances, p->size); ++k)
            c += "        wmgl_in_culldistance[wmgl_j][" + std::to_string(k) + "] = " +
                 loadValue("float", GL_FLOAT, "wmgl_gs_in", "wmgl_b + " + std::to_string(p->offset + k) + "u") + ";\n";
    for (const GeometryVar& v : info.inputs) {
        const TransformFeedbackVarying* p = captured(captureName(v));
        if (!p) continue;
        const std::string target = (v.member.empty() ? v.name + "[wmgl_j]" : v.name + "[wmgl_j]." + v.member) + v.path;
        if (!v.arraySize) {
            c += "        " + target + " = " + loadValue(v.glslType, v.type, "wmgl_gs_in", "wmgl_b + " + std::to_string(p->offset) + "u") +
                 ";\n";
            continue;
        }
        // Array members of input blocks were captured element by element.
        for (int e = 0; e < std::min(v.arraySize, p->size); ++e)
            c += "        " + target + "[" + std::to_string(e) + "] = " +
                 loadValue(v.glslType, v.type, "wmgl_gs_in",
                           "wmgl_b + " + std::to_string(p->offset + static_cast<uint32_t>(e * glTypeWords(v.type))) + "u") +
                 ";\n";
    }
    c += "    }\n"
         "    wmgl_primitive_id_in = int(wmgl_gs_prim_base + wmgl_gs_prim % wmgl_gs_prims_per_instance);\n"
         "    wmgl_gs_main();\n";
    c += "    for (; wmgl_gs_cursor < " + region + "u; ++wmgl_gs_cursor) wmgl_gs_indices[wmgl_gs_thread * " + region +
         "u + wmgl_gs_cursor] = 0xFFFFFFFFu;\n";
    if (!strips)
        // Points are drawn without the index list: unused slots get a
        // position outside the clip volume.
        c += "    for (uint wmgl_k = wmgl_gs_emitted; wmgl_k < " + maxS + "u; ++wmgl_k) {\n"
             "        uint wmgl_o = (wmgl_gs_thread * " + maxS + "u + wmgl_k) * " + stride + "u;\n"
             "        wmgl_gs_out[wmgl_o] = floatBitsToUint(2.0); wmgl_gs_out[wmgl_o + 1u] = floatBitsToUint(2.0);\n"
             "        wmgl_gs_out[wmgl_o + 2u] = floatBitsToUint(2.0); wmgl_gs_out[wmgl_o + 3u] = floatBitsToUint(1.0);\n"
             "    }\n";
    c += "}\n";

    // ---- pass-through vertex shader
    std::string& p = out.passthrough;
    p += "#version 450\n";
    if (info.writesLayer || info.writesViewportIndex) p += "#extension GL_ARB_shader_viewport_layer_array : require\n";
    p += "buffer wmgl_GsVertices { uint wmgl_gs_v[]; };\n" + info.structs;
    std::map<std::string, bool> declared;
    for (const GeometryVar& v : info.outputs) {
        if (declared[v.declaration]) continue;
        declared[v.declaration] = true;
        p += withoutStream(v.declaration) + "\n";
    }
    if (info.outputClipDistances) p += "out float gl_ClipDistance[" + std::to_string(info.outputClipDistances) + "];\n";
    if (info.outputCullDistances) p += "out float gl_CullDistance[" + std::to_string(info.outputCullDistances) + "];\n";
    p += "void main() {\n    uint wmgl_b = uint(gl_VertexID) * " + stride + "u;\n";
    for (const Field& f : fields) {
        std::string target = f.expr;
        if (target == "wmgl_out_position") target = "gl_Position";
        else if (target == "wmgl_out_pointsize") target = "gl_PointSize";
        else if (target == "wmgl_out_layer") target = "gl_Layer";
        else if (target == "wmgl_out_viewport") target = "gl_ViewportIndex";
        else if (target.rfind("wmgl_out_clipdistance", 0) == 0) target = "gl_ClipDistance" + target.substr(21);
        else if (target.rfind("wmgl_out_culldistance", 0) == 0) target = "gl_CullDistance" + target.substr(21);
        p += "    " + target + " = " + loadValue(f.glslType, f.type, "wmgl_gs_v", "wmgl_b + " + std::to_string(f.offset) + "u") + ";\n";
    }
    // Only stream 0 is rasterized: other streams' vertices land outside the clip volume.
    if (info.streams)
        p += "    if (wmgl_gs_v[wmgl_b + " + std::to_string(out.streamOffset) + "u] != 0u) gl_Position = vec4(2.0, 2.0, 2.0, 1.0);\n";
    p += "}\n";
    return out;
}

}  // namespace wmgl::shader

namespace wmgl::shader {

// Shared with the tessellation emulation (tessellation.cpp).
GLenum glslTypeFromName(const std::string& type) { return glslTypeEnum(type); }

std::string glslLoadValue(const std::string& type, GLenum glType, const std::string& buffer, const std::string& base) {
    return loadValue(type, glType, buffer, base);
}

}  // namespace wmgl::shader
