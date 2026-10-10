#include "core/shader/transform_feedback.h"

#include <set>

namespace wmgl::shader {
namespace {

bool parseName(const std::string& varying, std::string& base, int& index) {
    index = -1;
    const size_t open = varying.find('[');
    if (open == std::string::npos) {
        base = varying;
        return !base.empty();
    }
    if (varying.back() != ']' || open + 2 > varying.size() - 1) return false;
    base = varying.substr(0, open);
    const std::string digits = varying.substr(open + 1, varying.size() - open - 2);
    if (digits.empty() || digits.find_first_not_of("0123456789") != std::string::npos) return false;
    index = std::stoi(digits);
    return true;
}

GLenum scalarKind(GLenum type) {
    switch (type) {
    case GL_INT: case GL_INT_VEC2: case GL_INT_VEC3: case GL_INT_VEC4: return GL_INT;
    case GL_UNSIGNED_INT: case GL_UNSIGNED_INT_VEC2: case GL_UNSIGNED_INT_VEC3: case GL_UNSIGNED_INT_VEC4: return GL_UNSIGNED_INT;
    default: return GL_FLOAT;
    }
}

// Expressions for each 32-bit word of one element, converted to uint.
std::vector<std::string> elementWords(const std::string& e, GLenum type) {
    const GLenum kind = scalarKind(type);
    auto convert = [&](const std::string& x) {
        return kind == GL_FLOAT ? "floatBitsToUint(" + x + ")" : kind == GL_INT ? "uint(" + x + ")" : x;
    };
    std::vector<std::string> words;
    int columns = 1, rows = glTypeComponents(type);
    switch (type) {
    case GL_FLOAT_MAT2: columns = 2; rows = 2; break;
    case GL_FLOAT_MAT2x3: columns = 2; rows = 3; break;
    case GL_FLOAT_MAT2x4: columns = 2; rows = 4; break;
    case GL_FLOAT_MAT3: columns = 3; rows = 3; break;
    case GL_FLOAT_MAT3x2: columns = 3; rows = 2; break;
    case GL_FLOAT_MAT3x4: columns = 3; rows = 4; break;
    case GL_FLOAT_MAT4: columns = 4; rows = 4; break;
    case GL_FLOAT_MAT4x2: columns = 4; rows = 2; break;
    case GL_FLOAT_MAT4x3: columns = 4; rows = 3; break;
    default: break;
    }
    if (columns > 1) {
        for (int c = 0; c < columns; ++c)
            for (int r = 0; r < rows; ++r) words.push_back(convert(e + "[" + std::to_string(c) + "][" + std::to_string(r) + "]"));
    } else if (rows == 1) {
        words.push_back(convert(e));
    } else {
        for (int c = 0; c < rows; ++c) words.push_back(convert(e + "[" + std::to_string(c) + "]"));
    }
    return words;
}

}  // namespace

bool layoutTransformFeedback(const std::vector<std::string>& varyings, GLenum mode,
                             const std::vector<StageOutputVar>& outputs, LinkResult& result, std::string& log) {
    const bool separate = mode == GL_SEPARATE_ATTRIBS;
    if (separate && varyings.size() > 4) {
        log += "error: too many separate transform feedback varyings\n";
        return false;
    }
    std::set<std::string> seen;
    uint32_t interleavedWords = 0;
    for (const std::string& varying : varyings) {
        if (!seen.insert(varying).second) {
            log += "error: transform feedback varying '" + varying + "' is specified more than once\n";
            return false;
        }
        std::string base;
        int index = -1;
        if (!parseName(varying, base, index)) {
            log += "error: '" + varying + "' is not a valid transform feedback varying name\n";
            return false;
        }
        const StageOutputVar* out = nullptr;
        for (const StageOutputVar& o : outputs)
            if (o.name == base) out = &o;
        if (!out) {
            log += "error: transform feedback varying '" + varying + "' is not an output of the last vertex processing stage\n";
            return false;
        }
        if (index >= 0 && index >= out->arraySize) {
            log += "error: transform feedback varying '" + varying + "' indexes past the end of its array\n";
            return false;
        }
        TransformFeedbackVarying v;
        v.name = varying;
        v.type = out->type;
        v.size = index >= 0 || out->arraySize == 0 ? 1 : out->arraySize;
        v.components = glTypeComponents(out->type) * v.size;
        v.array = index < 0 && out->arraySize > 0;
        v.expression = index >= 0 ? base + "[" + std::to_string(index) + "]" : base;
        if (separate) {
            if (v.components > 4) {
                log += "error: transform feedback varying '" + varying + "' exceeds MAX_TRANSFORM_FEEDBACK_SEPARATE_COMPONENTS\n";
                return false;
            }
            v.buffer = static_cast<int>(result.transformFeedback.size());
            v.offset = 0;
            result.xfbStride[v.buffer] = static_cast<uint32_t>(v.components);
        } else {
            v.buffer = 0;
            v.offset = interleavedWords;
            interleavedWords += static_cast<uint32_t>(v.components);
        }
        result.transformFeedback.push_back(v);
    }
    if (!separate) {
        if (interleavedWords > 64) {
            log += "error: transform feedback varyings exceed MAX_TRANSFORM_FEEDBACK_INTERLEAVED_COMPONENTS\n";
            return false;
        }
        result.xfbStride[0] = interleavedWords;
    }
    result.xfbBufferCount = varyings.empty() ? 0 : separate ? static_cast<int>(varyings.size()) : 1;
    return true;
}

CaptureCode vertexCaptureCode(const LinkResult& layout, int glslVersion) {
    CaptureCode code;
    std::string& d = code.declarations;
    if (glslVersion < 430) d += "#extension GL_ARB_shader_storage_buffer_object : require\n";
    for (int b = 0; b < layout.xfbBufferCount; ++b)
        d += "buffer wmgl_XfbData" + std::to_string(b) + " { uint wmgl_xfb_data" + std::to_string(b) + "[]; };\n";
    d += "buffer wmgl_XfbControl {\n"
         "    uint wmgl_xfb_active, wmgl_xfb_vertex_count, wmgl_xfb_limit, wmgl_xfb_pad;\n"
         "    uint wmgl_xfb_base[4];\n"
         "    uint wmgl_xfb_stride[4];\n"
         "    uint wmgl_xfb_vertex_id[];\n"
         "};\n"
         "int wmgl_xfb_vertex_id_of() {\n"
         "    if (wmgl_xfb_active != 0u) return int(wmgl_xfb_vertex_id[gl_VertexID]);\n"
         "    return gl_VertexID;\n"
         "}\n"
         "#define gl_VertexID wmgl_xfb_vertex_id_of()\n"
         "#define main wmgl_user_main\n";
    std::string& e = code.epilogue;
    e += "\n#undef main\n#undef gl_VertexID\n"
         "void main() {\n"
         "    wmgl_user_main();\n"
         "    if (wmgl_xfb_active != 0u) {\n"
         "        uint v = uint(gl_VertexID) + uint(gl_InstanceID) * wmgl_xfb_vertex_count;\n"
         "        if (v < wmgl_xfb_limit) {\n";
    for (int b = 0; b < layout.xfbBufferCount; ++b)
        e += "            uint p" + std::to_string(b) + " = wmgl_xfb_base[" + std::to_string(b) + "] + v * wmgl_xfb_stride[" +
             std::to_string(b) + "];\n";
    for (const TransformFeedbackVarying& v : layout.transformFeedback) {
        uint32_t word = v.offset;
        for (int element = 0; element < v.size; ++element) {
            const std::string expr = v.array ? v.expression + "[" + std::to_string(element) + "]" : v.expression;
            for (const std::string& w : elementWords(expr, v.type)) {
                e += "            wmgl_xfb_data" + std::to_string(v.buffer) + "[p" + std::to_string(v.buffer) + " + " +
                     std::to_string(word++) + "u] = " + w + ";\n";
            }
        }
    }
    e += "        }\n    }\n}\n";
    return code;
}

}  // namespace wmgl::shader
