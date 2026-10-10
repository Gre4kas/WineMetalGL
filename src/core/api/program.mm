// Shader and program objects, the program interface, and uniforms
// (GL 3.3 sections 2.11-2.12, 6.1.15).

#include <algorithm>
#include <cstring>
#include <string>

#include "core/context.h"
#include "core/program.h"
#include "core/util/log.h"
#include "gl_api.gen.h"

namespace wmgl {

Stage stageFromGL(GLenum type) {
    switch (type) {
    case GL_VERTEX_SHADER: return Stage::Vertex;
    case GL_TESS_CONTROL_SHADER: return Stage::TessControl;
    case GL_TESS_EVALUATION_SHADER: return Stage::TessEval;
    case GL_GEOMETRY_SHADER: return Stage::Geometry;
    case GL_FRAGMENT_SHADER: return Stage::Fragment;
    default: return Stage::Compute;
    }
}

GLenum stageToGL(Stage stage) {
    switch (stage) {
    case Stage::Vertex: return GL_VERTEX_SHADER;
    case Stage::TessControl: return GL_TESS_CONTROL_SHADER;
    case Stage::TessEval: return GL_TESS_EVALUATION_SHADER;
    case Stage::Geometry: return GL_GEOMETRY_SHADER;
    case Stage::Fragment: return GL_FRAGMENT_SHADER;
    default: return GL_COMPUTE_SHADER;
    }
}

namespace api {
namespace {

constexpr GLint kMaxCombinedTextureUnits = 48;

void copyString(const std::string& s, GLsizei bufSize, GLsizei* length, GLchar* out) {
    if (bufSize <= 0 || !out) {
        if (length) *length = 0;
        return;
    }
    const GLsizei n = std::min<GLsizei>(bufSize - 1, static_cast<GLsizei>(s.size()));
    std::memcpy(out, s.data(), static_cast<size_t>(n));
    out[n] = 0;
    if (length) *length = n;
}

// Shader or program lookups with GL's error rules: a name that is neither
// is INVALID_VALUE; a name of the other kind is INVALID_OPERATION.
Shader* shaderOrError(Context& ctx, GLuint name) {
    ShareLock lock(ctx);
    if (Shader* s = ctx.share().shader(name)) return s;
    ctx.setError(ctx.share().program(name) ? GL_INVALID_OPERATION : GL_INVALID_VALUE);
    return nullptr;
}

Program* programOrError(Context& ctx, GLuint name) {
    ShareLock lock(ctx);
    if (Program* p = ctx.share().program(name)) return p;
    ctx.setError(ctx.share().shader(name) ? GL_INVALID_OPERATION : GL_INVALID_VALUE);
    return nullptr;
}

Program* linkedProgramOrError(Context& ctx, GLuint name) {
    Program* p = programOrError(ctx, name);
    if (p && (!p->linkStatus || !p->linked)) {
        ctx.setError(GL_INVALID_OPERATION);
        return nullptr;
    }
    return p;
}

void maybeDeleteShader(Context& ctx, Shader* shader) {
    if (shader->deletePending && shader->attachCount == 0) {
        ShareLock lock(ctx);
        ctx.share().shaders.erase(shader->name);
    }
}

void maybeDeleteProgram(Context& ctx, Program* program) {
    if (!program->deletePending || program->useCount > 0) return;
    for (RefPtr<Shader>& s : program->attached) {
        --s->attachCount;
        maybeDeleteShader(ctx, s.get());
    }
    program->attached.clear();
    ShareLock lock(ctx);
    ctx.share().programs.erase(program->name);
}

// --- uniform type information

struct TypeInfo {
    char base;      // 'f', 'i', 'u', 'b', 's' (sampler), 'd'
    int columns;
    int rows;
};

TypeInfo typeInfo(GLenum type) {
    switch (type) {
    case GL_FLOAT: return {'f', 1, 1};
    case GL_FLOAT_VEC2: return {'f', 1, 2};
    case GL_FLOAT_VEC3: return {'f', 1, 3};
    case GL_FLOAT_VEC4: return {'f', 1, 4};
    case GL_INT: return {'i', 1, 1};
    case GL_INT_VEC2: return {'i', 1, 2};
    case GL_INT_VEC3: return {'i', 1, 3};
    case GL_INT_VEC4: return {'i', 1, 4};
    case GL_UNSIGNED_INT: return {'u', 1, 1};
    case GL_UNSIGNED_INT_VEC2: return {'u', 1, 2};
    case GL_UNSIGNED_INT_VEC3: return {'u', 1, 3};
    case GL_UNSIGNED_INT_VEC4: return {'u', 1, 4};
    case GL_BOOL: return {'b', 1, 1};
    case GL_BOOL_VEC2: return {'b', 1, 2};
    case GL_BOOL_VEC3: return {'b', 1, 3};
    case GL_BOOL_VEC4: return {'b', 1, 4};
    case GL_FLOAT_MAT2: return {'f', 2, 2};
    case GL_FLOAT_MAT3: return {'f', 3, 3};
    case GL_FLOAT_MAT4: return {'f', 4, 4};
    case GL_FLOAT_MAT2x3: return {'f', 2, 3};
    case GL_FLOAT_MAT2x4: return {'f', 2, 4};
    case GL_FLOAT_MAT3x2: return {'f', 3, 2};
    case GL_FLOAT_MAT3x4: return {'f', 3, 4};
    case GL_FLOAT_MAT4x2: return {'f', 4, 2};
    case GL_FLOAT_MAT4x3: return {'f', 4, 3};
    case GL_DOUBLE: return {'d', 1, 1};
    default: return {shader::glTypeIsSampler(type) ? 's' : 'f', 1, 1};
    }
}

// Resolves a uniform location to (uniform, element).
const shader::UniformInfo* uniformAt(Program& p, GLint location, int& element) {
    if (!p.linked || location < 0 || static_cast<size_t>(location) >= p.linked->link.locations.size()) return nullptr;
    const auto& [index, e] = p.linked->link.locations[static_cast<size_t>(location)];
    element = e;
    return &p.linked->link.uniforms[static_cast<size_t>(index)];
}

// Writes `count` elements of uniform data from `values` (kind f/i/u, `components` per element).
template <class T>
void setUniform(Context& ctx, Program* p, GLint location, GLsizei count, char kind, int components, const T* values) {
    if (location == -1) return;
    if (!p || !p->linked) return ctx.setError(GL_INVALID_OPERATION);
    if (count < 0) return ctx.setError(GL_INVALID_VALUE);
    int element = 0;
    const shader::UniformInfo* u = uniformAt(*p, location, element);
    if (!u) return ctx.setError(GL_INVALID_OPERATION);
    const TypeInfo info = typeInfo(u->type);
    if (info.columns != 1 || info.rows != components) return ctx.setError(GL_INVALID_OPERATION);
    if (count > 1 && u->size == 1) return ctx.setError(GL_INVALID_OPERATION);
    if (info.base == 's') {
        if (kind != 'i' || components != 1) return ctx.setError(GL_INVALID_OPERATION);
    } else if (info.base == 'b') {
        // bools accept f, i and u variants
    } else if (info.base != kind) {
        return ctx.setError(GL_INVALID_OPERATION);
    }
    const GLsizei n = std::min<GLsizei>(count, u->size - element);
    if (info.base == 's') {
        for (GLsizei i = 0; i < n; ++i) {
            const GLint unit = static_cast<GLint>(values[i]);
            if (unit < 0 || unit >= kMaxCombinedTextureUnits) return ctx.setError(GL_INVALID_VALUE);
        }
        for (GLsizei i = 0; i < n; ++i) p->samplerUnits[static_cast<size_t>(u->samplerSlot + element + i)] = static_cast<GLint>(values[i]);
        ctx.markDirty(kDirtyTextures);
        return;
    }
    const size_t stride = u->arrayStride ? static_cast<size_t>(u->arrayStride) : 16;
    for (GLsizei i = 0; i < n; ++i) {
        uint8_t* dst = p->uniformData.data() + u->offset + (element + i) * stride;
        for (int c = 0; c < components; ++c) {
            const T v = values[i * components + c];
            if (info.base == 'b') {
                const uint32_t b = v != T(0) ? 1u : 0u;
                std::memcpy(dst + c * 4, &b, 4);
            } else if (info.base == 'f') {
                const float f = static_cast<float>(v);
                std::memcpy(dst + c * 4, &f, 4);
            } else {
                std::memcpy(dst + c * 4, &v, 4);
            }
        }
    }
    ++p->uniformVersion;
    ctx.markDirty(kDirtyUniforms);
}

void setUniformMatrix(Context& ctx, Program* p, GLint location, GLsizei count, GLboolean transpose, int columns, int rows,
                      const GLfloat* values) {
    if (location == -1) return;
    if (!p || !p->linked) return ctx.setError(GL_INVALID_OPERATION);
    if (count < 0) return ctx.setError(GL_INVALID_VALUE);
    int element = 0;
    const shader::UniformInfo* u = uniformAt(*p, location, element);
    if (!u) return ctx.setError(GL_INVALID_OPERATION);
    const TypeInfo info = typeInfo(u->type);
    if (info.base != 'f' || info.columns != columns || info.rows != rows) return ctx.setError(GL_INVALID_OPERATION);
    if (count > 1 && u->size == 1) return ctx.setError(GL_INVALID_OPERATION);
    const GLsizei n = std::min<GLsizei>(count, u->size - element);
    const size_t matrixStride = u->matrixStride ? static_cast<size_t>(u->matrixStride) : 16;
    const size_t arrayStride = u->arrayStride ? static_cast<size_t>(u->arrayStride) : matrixStride * static_cast<size_t>(columns);
    for (GLsizei i = 0; i < n; ++i) {
        const GLfloat* m = values + i * columns * rows;
        uint8_t* dst = p->uniformData.data() + u->offset + (element + i) * arrayStride;
        for (int c = 0; c < columns; ++c)
            for (int r = 0; r < rows; ++r) {
                const float v = transpose ? m[r * columns + c] : m[c * rows + r];
                if (u->rowMajor) std::memcpy(dst + r * matrixStride + c * 4, &v, 4);
                else std::memcpy(dst + c * matrixStride + r * 4, &v, 4);
            }
    }
    ++p->uniformVersion;
    ctx.markDirty(kDirtyUniforms);
}

Program* current(Context& ctx) { return ctx.state.program.get(); }

GLint locationForName(const shader::LinkResult& link, const std::string& name) {
    for (const shader::UniformInfo& u : link.uniforms) {
        if (u.blockIndex >= 0 || u.location < 0) continue;
        if (u.name == name) return u.location;
        if (u.size >= 1 && u.name.size() > 3 && u.name.compare(u.name.size() - 3, 3, "[0]") == 0) {
            const std::string base = u.name.substr(0, u.name.size() - 3);
            if (name == base) return u.location;
            if (name.size() > base.size() + 2 && name.compare(0, base.size(), base) == 0 && name[base.size()] == '[' &&
                name.back() == ']') {
                const std::string digits = name.substr(base.size() + 1, name.size() - base.size() - 2);
                if (digits.empty() || !std::all_of(digits.begin(), digits.end(), ::isdigit)) continue;
                const long index = std::strtol(digits.c_str(), nullptr, 10);
                if (index < u.size) return u.location + static_cast<GLint>(index);
                return -1;
            }
        }
    }
    return -1;
}

}  // namespace

// ------------------------------------------------------------ shaders

GLuint CreateShader(Context& ctx, GLenum type) {
    switch (type) {
    case GL_VERTEX_SHADER: case GL_FRAGMENT_SHADER: break;
    case GL_GEOMETRY_SHADER: if (ctx.version() >= 32) break; [[fallthrough]];
    case GL_TESS_CONTROL_SHADER: case GL_TESS_EVALUATION_SHADER:
        if (type != GL_GEOMETRY_SHADER && ctx.version() >= 40) break;
        [[fallthrough]];
    case GL_COMPUTE_SHADER:
        if (type == GL_COMPUTE_SHADER && ctx.version() >= 43) break;
        [[fallthrough]];
    default: ctx.setError(GL_INVALID_ENUM); return 0;
    }
    ShareLock lock(ctx);
    const GLuint name = ctx.share().createShaderProgramName();
    ctx.share().shaders[name] = makeRef<Shader>(name, type);
    return name;
}

void ShaderSource(Context& ctx, GLuint shader, GLsizei count, const GLchar* const* strings, const GLint* lengths) {
    if (count < 0) return ctx.setError(GL_INVALID_VALUE);
    Shader* s = shaderOrError(ctx, shader);
    if (!s) return;
    std::string source;
    for (GLsizei i = 0; i < count; ++i) {
        if (!strings || !strings[i]) continue;
        if (lengths && lengths[i] >= 0) source.append(strings[i], static_cast<size_t>(lengths[i]));
        else source.append(strings[i]);
    }
    s->source = std::move(source);
}

void CompileShader(Context& ctx, GLuint shader) {
    Shader* s = shaderOrError(ctx, shader);
    if (!s) return;
    shader::CompileResult result = shader::compile(s->stage, s->source);
    s->compiled = true;
    s->compileStatus = result.ok;
    s->infoLog = result.log;
    if (!result.ok) WMGL_INFO("shader %u failed to compile:\n%s", shader, result.log.c_str());
}

void DeleteShader(Context& ctx, GLuint shader) {
    if (!shader) return;
    Shader* s = shaderOrError(ctx, shader);
    if (!s) return;
    s->deletePending = true;
    maybeDeleteShader(ctx, s);
}

GLboolean IsShader(Context& ctx, GLuint shader) {
    ShareLock lock(ctx);
    return ctx.share().shader(shader) ? GL_TRUE : GL_FALSE;
}

void GetShaderiv(Context& ctx, GLuint shader, GLenum pname, GLint* params) {
    Shader* s = shaderOrError(ctx, shader);
    if (!s || !params) return;
    switch (pname) {
    case GL_SHADER_TYPE: *params = static_cast<GLint>(s->type); return;
    case GL_DELETE_STATUS: *params = s->deletePending; return;
    case GL_COMPILE_STATUS: *params = s->compileStatus; return;
    case GL_INFO_LOG_LENGTH: *params = s->infoLog.empty() ? 0 : static_cast<GLint>(s->infoLog.size() + 1); return;
    case GL_SHADER_SOURCE_LENGTH: *params = s->source.empty() ? 0 : static_cast<GLint>(s->source.size() + 1); return;
    default: ctx.setError(GL_INVALID_ENUM);
    }
}

void GetShaderInfoLog(Context& ctx, GLuint shader, GLsizei bufSize, GLsizei* length, GLchar* infoLog) {
    if (bufSize < 0) return ctx.setError(GL_INVALID_VALUE);
    Shader* s = shaderOrError(ctx, shader);
    if (s) copyString(s->infoLog, bufSize, length, infoLog);
}

void GetShaderSource(Context& ctx, GLuint shader, GLsizei bufSize, GLsizei* length, GLchar* source) {
    if (bufSize < 0) return ctx.setError(GL_INVALID_VALUE);
    Shader* s = shaderOrError(ctx, shader);
    if (s) copyString(s->source, bufSize, length, source);
}

void ReleaseShaderCompiler(Context& ctx) {}

void GetShaderPrecisionFormat(Context& ctx, GLenum shadertype, GLenum precisiontype, GLint* range, GLint* precision) {
    if (shadertype != GL_VERTEX_SHADER && shadertype != GL_FRAGMENT_SHADER) return ctx.setError(GL_INVALID_ENUM);
    const bool isInt = precisiontype == GL_LOW_INT || precisiontype == GL_MEDIUM_INT || precisiontype == GL_HIGH_INT;
    const bool isFloat = precisiontype == GL_LOW_FLOAT || precisiontype == GL_MEDIUM_FLOAT || precisiontype == GL_HIGH_FLOAT;
    if (!isInt && !isFloat) return ctx.setError(GL_INVALID_ENUM);
    if (range) { range[0] = isInt ? 31 : 127; range[1] = isInt ? 30 : 127; }
    if (precision) *precision = isInt ? 0 : 23;
}

// ----------------------------------------------------------- programs

GLuint CreateProgram(Context& ctx) {
    ShareLock lock(ctx);
    const GLuint name = ctx.share().createShaderProgramName();
    ctx.share().programs[name] = makeRef<Program>(name);
    return name;
}

void DeleteProgram(Context& ctx, GLuint program) {
    if (!program) return;
    Program* p = programOrError(ctx, program);
    if (!p) return;
    p->deletePending = true;
    maybeDeleteProgram(ctx, p);
}

GLboolean IsProgram(Context& ctx, GLuint program) {
    ShareLock lock(ctx);
    return ctx.share().program(program) ? GL_TRUE : GL_FALSE;
}

void AttachShader(Context& ctx, GLuint program, GLuint shader) {
    Program* p = programOrError(ctx, program);
    if (!p) return;
    Shader* s = shaderOrError(ctx, shader);
    if (!s) return;
    for (const RefPtr<Shader>& a : p->attached)
        if (a.get() == s) return ctx.setError(GL_INVALID_OPERATION);
    p->attached.push_back(RefPtr<Shader>(s));
    ++s->attachCount;
}

void DetachShader(Context& ctx, GLuint program, GLuint shader) {
    Program* p = programOrError(ctx, program);
    if (!p) return;
    Shader* s = shaderOrError(ctx, shader);
    if (!s) return;
    auto it = std::find_if(p->attached.begin(), p->attached.end(), [&](const RefPtr<Shader>& a) { return a.get() == s; });
    if (it == p->attached.end()) return ctx.setError(GL_INVALID_OPERATION);
    RefPtr<Shader> keep = *it;
    p->attached.erase(it);
    --s->attachCount;
    maybeDeleteShader(ctx, s);
}

void GetAttachedShaders(Context& ctx, GLuint program, GLsizei maxCount, GLsizei* count, GLuint* shaders) {
    if (maxCount < 0) return ctx.setError(GL_INVALID_VALUE);
    Program* p = programOrError(ctx, program);
    if (!p) return;
    GLsizei n = 0;
    for (const RefPtr<Shader>& s : p->attached) {
        if (n >= maxCount) break;
        if (shaders) shaders[n] = s->name;
        ++n;
    }
    if (count) *count = n;
}

void BindAttribLocation(Context& ctx, GLuint program, GLuint index, const GLchar* name) {
    if (index >= static_cast<GLuint>(VertexArray::kMaxAttribs)) return ctx.setError(GL_INVALID_VALUE);
    Program* p = programOrError(ctx, program);
    if (!p || !name) return;
    if (std::strncmp(name, "gl_", 3) == 0) return ctx.setError(GL_INVALID_OPERATION);
    p->attribBindings[name] = index;
}

void BindFragDataLocationIndexed(Context& ctx, GLuint program, GLuint colorNumber, GLuint index, const GLchar* name) {
    if (index > 1) return ctx.setError(GL_INVALID_VALUE);
    if (colorNumber >= (index ? 1u : static_cast<GLuint>(ContextState::kMaxDrawBuffers))) return ctx.setError(GL_INVALID_VALUE);
    Program* p = programOrError(ctx, program);
    if (!p || !name) return;
    if (std::strncmp(name, "gl_", 3) == 0) return ctx.setError(GL_INVALID_OPERATION);
    p->fragDataBindings[name] = {colorNumber, index};
}

void BindFragDataLocation(Context& ctx, GLuint program, GLuint color, const GLchar* name) {
    BindFragDataLocationIndexed(ctx, program, color, 0, name);
}

void LinkProgram(Context& ctx, GLuint program) {
    Program* p = programOrError(ctx, program);
    if (!p) return;
    const TransformFeedback& xfb = *ctx.state.transformFeedback;
    if (xfb.active && xfb.program.get() == p) return ctx.setError(GL_INVALID_OPERATION);
    shader::LinkInput input;
    std::string log;
    bool ok = true;
    for (const RefPtr<Shader>& s : p->attached) {
        if (!s->compileStatus) {
            log += "error: attached shader " + std::to_string(s->name) + " is not compiled\n";
            ok = false;
        }
        input.shaders.push_back({s->stage, s->source});
    }
    input.attribBindings = p->attribBindings;
    input.fragDataBindings = p->fragDataBindings;
    input.transformFeedbackVaryings = p->transformFeedbackVaryings;
    input.transformFeedbackMode = p->transformFeedbackMode;
    input.separable = p->separable;
    p->linkAttempted = true;
    if (ok && input.shaders.empty()) {
        log += "error: no shaders attached\n";
        ok = false;
    }
    shader::LinkResult result;
    if (ok) {
        result = shader::link(input);
        ok = result.ok;
        log += result.log;
    }
    p->linkStatus = ok;
    p->infoLog = log;
    p->validateStatus = false;
    if (!ok) {
        WMGL_INFO("program %u failed to link:\n%s", program, log.c_str());
        // A failed relink of the current program keeps its old executable in use.
        if (p->useCount == 0) p->linked.reset();
        return;
    }
    auto linked = std::make_shared<LinkedProgram>();
    linked->link = std::move(result);
    linked->uid = nextLinkedProgramId();
    p->uniformData.assign((linked->link.defaultBlockSize + 15) & ~15u, 0);
    p->uniformBlockBindings.assign(linked->link.blocks.size(), 0);
    p->samplerUnits.assign(linked->link.samplers.size(), 0);
    p->linked = std::move(linked);
    ++p->uniformVersion;
    if (ctx.state.program.get() == p) ctx.markDirty(kDirtyProgram | kDirtyUniforms | kDirtyTextures | kDirtyVertexInput);
}

void UseProgram(Context& ctx, GLuint program) {
    const TransformFeedback& xfb = *ctx.state.transformFeedback;
    if (xfb.active && !xfb.paused) return ctx.setError(GL_INVALID_OPERATION);
    RefPtr<Program> next;
    if (program) {
        Program* p = programOrError(ctx, program);
        if (!p) return;
        if (!p->linkStatus) return ctx.setError(GL_INVALID_OPERATION);
        next = RefPtr<Program>(p);
    }
    RefPtr<Program> previous = ctx.state.program;
    if (previous == next) return;
    if (next) ++next->useCount;
    ctx.state.program = next;
    if (previous) {
        --previous->useCount;
        maybeDeleteProgram(ctx, previous.get());
    }
    ctx.markDirty(kDirtyProgram | kDirtyUniforms | kDirtyTextures | kDirtyVertexInput);
}

void ValidateProgram(Context& ctx, GLuint program) {
    Program* p = programOrError(ctx, program);
    if (!p) return;
    p->validateStatus = p->linkStatus && p->linked;
    if (p->validateStatus) {
        // Samplers of different types may not use the same texture unit.
        const auto& samplers = p->linked->link.samplers;
        for (size_t i = 0; i < samplers.size(); ++i)
            for (size_t j = i + 1; j < samplers.size(); ++j)
                if (p->samplerUnits[i] == p->samplerUnits[j] && samplers[i].textureTarget != samplers[j].textureTarget) {
                    p->validateStatus = false;
                    p->infoLog += "validation: samplers of different types share texture unit " +
                                  std::to_string(p->samplerUnits[i]) + "\n";
                }
    }
}

void GetProgramiv(Context& ctx, GLuint program, GLenum pname, GLint* params) {
    Program* p = programOrError(ctx, program);
    if (!p || !params) return;
    const shader::LinkResult* link = p->linked && p->linkStatus ? &p->linked->link : nullptr;
    auto maxLength = [](const auto& list) {
        GLint n = 0;
        for (const auto& item : list) n = std::max<GLint>(n, static_cast<GLint>(item.name.size() + 1));
        return n;
    };
    switch (pname) {
    case GL_DELETE_STATUS: *params = p->deletePending; return;
    case GL_LINK_STATUS: *params = p->linkStatus; return;
    case GL_VALIDATE_STATUS: *params = p->validateStatus; return;
    case GL_INFO_LOG_LENGTH: *params = p->infoLog.empty() ? 0 : static_cast<GLint>(p->infoLog.size() + 1); return;
    case GL_ATTACHED_SHADERS: *params = static_cast<GLint>(p->attached.size()); return;
    case GL_ACTIVE_ATTRIBUTES: *params = link ? static_cast<GLint>(link->attribs.size()) : 0; return;
    case GL_ACTIVE_ATTRIBUTE_MAX_LENGTH: *params = link ? maxLength(link->attribs) : 0; return;
    case GL_ACTIVE_UNIFORMS: *params = link ? static_cast<GLint>(link->uniforms.size()) : 0; return;
    case GL_ACTIVE_UNIFORM_MAX_LENGTH: *params = link ? maxLength(link->uniforms) : 0; return;
    case GL_ACTIVE_UNIFORM_BLOCKS: *params = link ? static_cast<GLint>(link->blocks.size()) : 0; return;
    case GL_ACTIVE_UNIFORM_BLOCK_MAX_NAME_LENGTH: *params = link ? maxLength(link->blocks) : 0; return;
    case GL_TRANSFORM_FEEDBACK_BUFFER_MODE: *params = static_cast<GLint>(p->transformFeedbackMode); return;
    case GL_TRANSFORM_FEEDBACK_VARYINGS: *params = link ? static_cast<GLint>(link->transformFeedback.size()) : 0; return;
    case GL_TRANSFORM_FEEDBACK_VARYING_MAX_LENGTH: *params = link ? maxLength(link->transformFeedback) : 0; return;
    case GL_GEOMETRY_VERTICES_OUT: case GL_GEOMETRY_INPUT_TYPE: case GL_GEOMETRY_OUTPUT_TYPE:
        if (!link || !link->stages[static_cast<int>(shader::Stage::Geometry)].present) return ctx.setError(GL_INVALID_OPERATION);
        *params = pname == GL_GEOMETRY_VERTICES_OUT ? link->geometryVerticesOut
                  : pname == GL_GEOMETRY_INPUT_TYPE ? static_cast<GLint>(link->geometryInputType)
                                                    : static_cast<GLint>(link->geometryOutputType);
        return;
    case GL_PROGRAM_BINARY_RETRIEVABLE_HINT:
        if (ctx.version() >= 41) { *params = p->binaryRetrievableHint; return; }
        break;
    case GL_PROGRAM_SEPARABLE:
        if (ctx.version() >= 41) { *params = p->separable; return; }
        break;
    default: break;
    }
    ctx.setError(GL_INVALID_ENUM);
}

void GetProgramInfoLog(Context& ctx, GLuint program, GLsizei bufSize, GLsizei* length, GLchar* infoLog) {
    if (bufSize < 0) return ctx.setError(GL_INVALID_VALUE);
    Program* p = programOrError(ctx, program);
    if (p) copyString(p->infoLog, bufSize, length, infoLog);
}

GLint GetAttribLocation(Context& ctx, GLuint program, const GLchar* name) {
    Program* p = linkedProgramOrError(ctx, program);
    if (!p || !name || std::strncmp(name, "gl_", 3) == 0) return -1;
    for (const shader::AttribInfo& a : p->linked->link.attribs)
        if (a.name == name) return a.location;
    return -1;
}

void GetActiveAttrib(Context& ctx, GLuint program, GLuint index, GLsizei bufSize, GLsizei* length, GLint* size,
                     GLenum* type, GLchar* name) {
    if (bufSize < 0) return ctx.setError(GL_INVALID_VALUE);
    Program* p = programOrError(ctx, program);
    if (!p) return;
    if (!p->linked || !p->linkStatus || index >= p->linked->link.attribs.size()) return ctx.setError(GL_INVALID_VALUE);
    const shader::AttribInfo& a = p->linked->link.attribs[index];
    copyString(a.name, bufSize, length, name);
    if (size) *size = a.size;
    if (type) *type = a.type;
}

GLint GetFragDataLocation(Context& ctx, GLuint program, const GLchar* name) {
    Program* p = linkedProgramOrError(ctx, program);
    if (!p || !name) return -1;
    for (const shader::FragOutputInfo& f : p->linked->link.fragOutputs)
        if (f.name == name) return f.location;
    return -1;
}

GLint GetFragDataIndex(Context& ctx, GLuint program, const GLchar* name) {
    Program* p = linkedProgramOrError(ctx, program);
    if (!p || !name) return -1;
    for (const shader::FragOutputInfo& f : p->linked->link.fragOutputs)
        if (f.name == name) return f.index;
    return -1;
}

GLint GetUniformLocation(Context& ctx, GLuint program, const GLchar* name) {
    Program* p = linkedProgramOrError(ctx, program);
    if (!p || !name || std::strncmp(name, "gl_", 3) == 0) return -1;
    return locationForName(p->linked->link, name);
}

void GetActiveUniform(Context& ctx, GLuint program, GLuint index, GLsizei bufSize, GLsizei* length, GLint* size,
                      GLenum* type, GLchar* name) {
    if (bufSize < 0) return ctx.setError(GL_INVALID_VALUE);
    Program* p = programOrError(ctx, program);
    if (!p) return;
    if (!p->linked || !p->linkStatus || index >= p->linked->link.uniforms.size()) return ctx.setError(GL_INVALID_VALUE);
    const shader::UniformInfo& u = p->linked->link.uniforms[index];
    copyString(u.name, bufSize, length, name);
    if (size) *size = u.size;
    if (type) *type = u.type;
}

void GetActiveUniformName(Context& ctx, GLuint program, GLuint index, GLsizei bufSize, GLsizei* length, GLchar* name) {
    GetActiveUniform(ctx, program, index, bufSize, length, nullptr, nullptr, name);
}

void GetActiveUniformsiv(Context& ctx, GLuint program, GLsizei count, const GLuint* indices, GLenum pname, GLint* params) {
    if (count < 0) return ctx.setError(GL_INVALID_VALUE);
    Program* p = programOrError(ctx, program);
    if (!p || !indices || !params) return;
    const auto& uniforms = p->linked && p->linkStatus ? p->linked->link.uniforms : std::vector<shader::UniformInfo>();
    for (GLsizei i = 0; i < count; ++i)
        if (indices[i] >= uniforms.size()) return ctx.setError(GL_INVALID_VALUE);
    for (GLsizei i = 0; i < count; ++i) {
        const shader::UniformInfo& u = uniforms[indices[i]];
        switch (pname) {
        case GL_UNIFORM_TYPE: params[i] = static_cast<GLint>(u.type); break;
        case GL_UNIFORM_SIZE: params[i] = u.size; break;
        case GL_UNIFORM_NAME_LENGTH: params[i] = static_cast<GLint>(u.name.size() + 1); break;
        case GL_UNIFORM_BLOCK_INDEX: params[i] = u.blockIndex; break;
        case GL_UNIFORM_OFFSET: params[i] = u.blockIndex >= 0 ? u.offset : -1; break;
        case GL_UNIFORM_ARRAY_STRIDE: {
            // Arrays (named "x[0]") report their stride even with one element.
            const bool array = !u.name.empty() && u.name.back() == ']';
            params[i] = u.blockIndex >= 0 ? (array ? u.arrayStride : 0) : -1;
            break;
        }
        case GL_UNIFORM_MATRIX_STRIDE: params[i] = u.blockIndex >= 0 ? u.matrixStride : -1; break;
        case GL_UNIFORM_IS_ROW_MAJOR: params[i] = u.blockIndex >= 0 && u.rowMajor; break;
        default: return ctx.setError(GL_INVALID_ENUM);
        }
    }
}

void GetUniformIndices(Context& ctx, GLuint program, GLsizei count, const GLchar* const* names, GLuint* indices) {
    if (count < 0) return ctx.setError(GL_INVALID_VALUE);
    Program* p = programOrError(ctx, program);
    if (!p || !names || !indices) return;
    const auto* uniforms = p->linked && p->linkStatus ? &p->linked->link.uniforms : nullptr;
    for (GLsizei i = 0; i < count; ++i) {
        indices[i] = GL_INVALID_INDEX;
        if (!uniforms || !names[i]) continue;
        const std::string name = names[i];
        for (size_t u = 0; u < uniforms->size(); ++u) {
            const std::string& n = (*uniforms)[u].name;
            if (n == name || (n.size() > 3 && n.compare(n.size() - 3, 3, "[0]") == 0 && n.substr(0, n.size() - 3) == name)) {
                indices[i] = static_cast<GLuint>(u);
                break;
            }
        }
    }
}

GLuint GetUniformBlockIndex(Context& ctx, GLuint program, const GLchar* name) {
    Program* p = programOrError(ctx, program);
    if (!p || !name || !p->linked || !p->linkStatus) return GL_INVALID_INDEX;
    const auto& blocks = p->linked->link.blocks;
    for (size_t i = 0; i < blocks.size(); ++i)
        if (blocks[i].name == name) return static_cast<GLuint>(i);
    return GL_INVALID_INDEX;
}

void GetActiveUniformBlockiv(Context& ctx, GLuint program, GLuint blockIndex, GLenum pname, GLint* params) {
    Program* p = programOrError(ctx, program);
    if (!p || !params) return;
    if (!p->linked || !p->linkStatus || blockIndex >= p->linked->link.blocks.size()) return ctx.setError(GL_INVALID_VALUE);
    const shader::UniformBlockInfo& b = p->linked->link.blocks[blockIndex];
    const auto stageBit = [](shader::Stage s) { return 1u << static_cast<int>(s); };
    switch (pname) {
    case GL_UNIFORM_BLOCK_BINDING: *params = static_cast<GLint>(p->uniformBlockBindings[blockIndex]); return;
    case GL_UNIFORM_BLOCK_DATA_SIZE: *params = b.dataSize; return;
    case GL_UNIFORM_BLOCK_NAME_LENGTH: *params = static_cast<GLint>(b.name.size() + 1); return;
    case GL_UNIFORM_BLOCK_ACTIVE_UNIFORMS: *params = static_cast<GLint>(b.activeUniforms.size()); return;
    case GL_UNIFORM_BLOCK_ACTIVE_UNIFORM_INDICES:
        for (size_t i = 0; i < b.activeUniforms.size(); ++i) params[i] = b.activeUniforms[i];
        return;
    case GL_UNIFORM_BLOCK_REFERENCED_BY_VERTEX_SHADER: *params = (b.stageMask & stageBit(shader::Stage::Vertex)) != 0; return;
    case GL_UNIFORM_BLOCK_REFERENCED_BY_GEOMETRY_SHADER: *params = (b.stageMask & stageBit(shader::Stage::Geometry)) != 0; return;
    case GL_UNIFORM_BLOCK_REFERENCED_BY_FRAGMENT_SHADER: *params = (b.stageMask & stageBit(shader::Stage::Fragment)) != 0; return;
    default: break;
    }
    ctx.setError(GL_INVALID_ENUM);
}

void GetActiveUniformBlockName(Context& ctx, GLuint program, GLuint blockIndex, GLsizei bufSize, GLsizei* length,
                               GLchar* name) {
    if (bufSize < 0) return ctx.setError(GL_INVALID_VALUE);
    Program* p = programOrError(ctx, program);
    if (!p) return;
    if (!p->linked || !p->linkStatus || blockIndex >= p->linked->link.blocks.size()) return ctx.setError(GL_INVALID_VALUE);
    copyString(p->linked->link.blocks[blockIndex].name, bufSize, length, name);
}

void UniformBlockBinding(Context& ctx, GLuint program, GLuint blockIndex, GLuint binding) {
    Program* p = programOrError(ctx, program);
    if (!p) return;
    if (!p->linked || !p->linkStatus || blockIndex >= p->linked->link.blocks.size()) return ctx.setError(GL_INVALID_VALUE);
    if (binding >= static_cast<GLuint>(ContextState::kMaxUniformBufferBindings)) return ctx.setError(GL_INVALID_VALUE);
    p->uniformBlockBindings[blockIndex] = binding;
    ctx.markDirty(kDirtyUniformBuffers);
}

// ------------------------------------------------------------ uniforms

#define WMGL_UNIFORM_V(Name, T, kind, n)                                                         \
    void Uniform##Name(Context& ctx, GLint location, GLsizei count, const T* value) {           \
        setUniform(ctx, current(ctx), location, count, kind, n, value);                         \
    }                                                                                           \
    void ProgramUniform##Name(Context& ctx, GLuint program, GLint location, GLsizei count, const T* value) { \
        setUniform(ctx, linkedProgramOrError(ctx, program), location, count, kind, n, value);   \
    }
WMGL_UNIFORM_V(1fv, GLfloat, 'f', 1)
WMGL_UNIFORM_V(2fv, GLfloat, 'f', 2)
WMGL_UNIFORM_V(3fv, GLfloat, 'f', 3)
WMGL_UNIFORM_V(4fv, GLfloat, 'f', 4)
WMGL_UNIFORM_V(1iv, GLint, 'i', 1)
WMGL_UNIFORM_V(2iv, GLint, 'i', 2)
WMGL_UNIFORM_V(3iv, GLint, 'i', 3)
WMGL_UNIFORM_V(4iv, GLint, 'i', 4)
WMGL_UNIFORM_V(1uiv, GLuint, 'u', 1)
WMGL_UNIFORM_V(2uiv, GLuint, 'u', 2)
WMGL_UNIFORM_V(3uiv, GLuint, 'u', 3)
WMGL_UNIFORM_V(4uiv, GLuint, 'u', 4)
#undef WMGL_UNIFORM_V

void Uniform1f(Context& ctx, GLint l, GLfloat x) { Uniform1fv(ctx, l, 1, &x); }
void Uniform2f(Context& ctx, GLint l, GLfloat x, GLfloat y) { const GLfloat v[] = {x, y}; Uniform2fv(ctx, l, 1, v); }
void Uniform3f(Context& ctx, GLint l, GLfloat x, GLfloat y, GLfloat z) { const GLfloat v[] = {x, y, z}; Uniform3fv(ctx, l, 1, v); }
void Uniform4f(Context& ctx, GLint l, GLfloat x, GLfloat y, GLfloat z, GLfloat w) { const GLfloat v[] = {x, y, z, w}; Uniform4fv(ctx, l, 1, v); }
void Uniform1i(Context& ctx, GLint l, GLint x) { Uniform1iv(ctx, l, 1, &x); }
void Uniform2i(Context& ctx, GLint l, GLint x, GLint y) { const GLint v[] = {x, y}; Uniform2iv(ctx, l, 1, v); }
void Uniform3i(Context& ctx, GLint l, GLint x, GLint y, GLint z) { const GLint v[] = {x, y, z}; Uniform3iv(ctx, l, 1, v); }
void Uniform4i(Context& ctx, GLint l, GLint x, GLint y, GLint z, GLint w) { const GLint v[] = {x, y, z, w}; Uniform4iv(ctx, l, 1, v); }
void Uniform1ui(Context& ctx, GLint l, GLuint x) { Uniform1uiv(ctx, l, 1, &x); }
void Uniform2ui(Context& ctx, GLint l, GLuint x, GLuint y) { const GLuint v[] = {x, y}; Uniform2uiv(ctx, l, 1, v); }
void Uniform3ui(Context& ctx, GLint l, GLuint x, GLuint y, GLuint z) { const GLuint v[] = {x, y, z}; Uniform3uiv(ctx, l, 1, v); }
void Uniform4ui(Context& ctx, GLint l, GLuint x, GLuint y, GLuint z, GLuint w) { const GLuint v[] = {x, y, z, w}; Uniform4uiv(ctx, l, 1, v); }

#define WMGL_UNIFORM_MATRIX(Name, c, r)                                                                       \
    void UniformMatrix##Name##fv(Context& ctx, GLint location, GLsizei count, GLboolean transpose, const GLfloat* value) { \
        setUniformMatrix(ctx, current(ctx), location, count, transpose, c, r, value);                          \
    }                                                                                                          \
    void ProgramUniformMatrix##Name##fv(Context& ctx, GLuint program, GLint location, GLsizei count, GLboolean transpose, \
                                        const GLfloat* value) {                                                \
        setUniformMatrix(ctx, linkedProgramOrError(ctx, program), location, count, transpose, c, r, value);    \
    }
WMGL_UNIFORM_MATRIX(2, 2, 2)
WMGL_UNIFORM_MATRIX(3, 3, 3)
WMGL_UNIFORM_MATRIX(4, 4, 4)
WMGL_UNIFORM_MATRIX(2x3, 2, 3)
WMGL_UNIFORM_MATRIX(2x4, 2, 4)
WMGL_UNIFORM_MATRIX(3x2, 3, 2)
WMGL_UNIFORM_MATRIX(3x4, 3, 4)
WMGL_UNIFORM_MATRIX(4x2, 4, 2)
WMGL_UNIFORM_MATRIX(4x3, 4, 3)
#undef WMGL_UNIFORM_MATRIX

namespace {

template <class T>
void getUniform(Context& ctx, GLuint program, GLint location, T* params) {
    Program* p = linkedProgramOrError(ctx, program);
    if (!p || !params) return;
    int element = 0;
    const shader::UniformInfo* u = uniformAt(*p, location, element);
    if (!u) return ctx.setError(GL_INVALID_OPERATION);
    const TypeInfo info = typeInfo(u->type);
    if (info.base == 's') {
        *params = static_cast<T>(p->samplerUnits[static_cast<size_t>(u->samplerSlot + element)]);
        return;
    }
    const size_t matrixStride = u->matrixStride ? static_cast<size_t>(u->matrixStride) : 16;
    const size_t stride = u->arrayStride ? static_cast<size_t>(u->arrayStride)
                                         : (info.columns > 1 ? matrixStride * static_cast<size_t>(info.columns) : 16);
    const uint8_t* src = p->uniformData.data() + u->offset + element * stride;
    int out = 0;
    for (int c = 0; c < info.columns; ++c)
        for (int r = 0; r < info.rows; ++r) {
            const uint8_t* at = info.columns > 1 ? src + c * matrixStride + r * 4 : src + r * 4;
            if (info.base == 'f') { float v; std::memcpy(&v, at, 4); params[out++] = static_cast<T>(v); }
            else if (info.base == 'i') { int32_t v; std::memcpy(&v, at, 4); params[out++] = static_cast<T>(v); }
            else { uint32_t v; std::memcpy(&v, at, 4); params[out++] = static_cast<T>(v); }
        }
}

}  // namespace

void GetUniformfv(Context& ctx, GLuint program, GLint location, GLfloat* params) { getUniform(ctx, program, location, params); }
void GetUniformiv(Context& ctx, GLuint program, GLint location, GLint* params) { getUniform(ctx, program, location, params); }
void GetUniformuiv(Context& ctx, GLuint program, GLint location, GLuint* params) { getUniform(ctx, program, location, params); }
void GetUniformdv(Context& ctx, GLuint program, GLint location, GLdouble* params) { getUniform(ctx, program, location, params); }

// ------------------------------------------------- transform feedback setup

void TransformFeedbackVaryings(Context& ctx, GLuint program, GLsizei count, const GLchar* const* varyings, GLenum bufferMode) {
    if (count < 0) return ctx.setError(GL_INVALID_VALUE);
    if (bufferMode != GL_INTERLEAVED_ATTRIBS && bufferMode != GL_SEPARATE_ATTRIBS) return ctx.setError(GL_INVALID_ENUM);
    if (bufferMode == GL_SEPARATE_ATTRIBS && count > TransformFeedback::kMaxBuffers) return ctx.setError(GL_INVALID_VALUE);
    Program* p = programOrError(ctx, program);
    if (!p) return;
    p->transformFeedbackVaryings.clear();
    for (GLsizei i = 0; i < count; ++i) p->transformFeedbackVaryings.emplace_back(varyings && varyings[i] ? varyings[i] : "");
    p->transformFeedbackMode = bufferMode;
}

void GetTransformFeedbackVarying(Context& ctx, GLuint program, GLuint index, GLsizei bufSize, GLsizei* length, GLsizei* size,
                                 GLenum* type, GLchar* name) {
    Program* p = programOrError(ctx, program);
    if (!p) return;
    if (!p->linked || !p->linkStatus || index >= p->linked->link.transformFeedback.size()) return ctx.setError(GL_INVALID_VALUE);
    const auto& v = p->linked->link.transformFeedback[index];
    copyString(v.name, bufSize, length, name);
    if (size) *size = v.size;
    if (type) *type = v.type;
}

}  // namespace api
}  // namespace wmgl
