// Program interface queries (GL 4.3 section 7.3.1, GL_ARB_program_interface_query):
// every interface over the reflection the linker records (shader/compiler.h).
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "core/context.h"
#include "core/shader/spirv_input.h"
#include "core/program.h"
#include "gl_api.gen.h"

namespace wmgl::api {

namespace {

using shader::LinkResult;
using shader::Stage;

enum class Kind {
    Uniform, UniformBlock, AtomicCounterBuffer, Input, Output, Subroutine, SubroutineUniform, TransformFeedbackVarying,
    BufferVariable, StorageBlock, TransformFeedbackBuffer,
};

bool programInterfaceQuery(Context& ctx, const char* command) {
    if (ctx.version() >= 43 || ctx.hasExtension("GL_ARB_program_interface_query")) return true;
    ctx.unimplemented(command);
    return false;
}

// The interface named by `iface`, or false (INVALID_ENUM set).
bool interfaceKind(Context& ctx, GLenum iface, Kind& kind, Stage& stage) {
    stage = Stage::Vertex;
    switch (iface) {
    case GL_UNIFORM: kind = Kind::Uniform; return true;
    case GL_UNIFORM_BLOCK: kind = Kind::UniformBlock; return true;
    case GL_ATOMIC_COUNTER_BUFFER: kind = Kind::AtomicCounterBuffer; return true;
    case GL_PROGRAM_INPUT: kind = Kind::Input; return true;
    case GL_PROGRAM_OUTPUT: kind = Kind::Output; return true;
    case GL_TRANSFORM_FEEDBACK_VARYING: kind = Kind::TransformFeedbackVarying; return true;
    case GL_BUFFER_VARIABLE: kind = Kind::BufferVariable; return true;
    case GL_SHADER_STORAGE_BLOCK: kind = Kind::StorageBlock; return true;
    case GL_TRANSFORM_FEEDBACK_BUFFER:  // GL 4.4, GL_ARB_enhanced_layouts
        if (ctx.version() < 44 && !ctx.hasExtension("GL_ARB_enhanced_layouts")) return ctx.setError(GL_INVALID_ENUM), false;
        kind = Kind::TransformFeedbackBuffer;
        return true;
    case GL_VERTEX_SUBROUTINE: case GL_VERTEX_SUBROUTINE_UNIFORM: stage = Stage::Vertex; break;
    case GL_TESS_CONTROL_SUBROUTINE: case GL_TESS_CONTROL_SUBROUTINE_UNIFORM: stage = Stage::TessControl; break;
    case GL_TESS_EVALUATION_SUBROUTINE: case GL_TESS_EVALUATION_SUBROUTINE_UNIFORM: stage = Stage::TessEval; break;
    case GL_GEOMETRY_SUBROUTINE: case GL_GEOMETRY_SUBROUTINE_UNIFORM: stage = Stage::Geometry; break;
    case GL_FRAGMENT_SUBROUTINE: case GL_FRAGMENT_SUBROUTINE_UNIFORM: stage = Stage::Fragment; break;
    case GL_COMPUTE_SUBROUTINE: case GL_COMPUTE_SUBROUTINE_UNIFORM: stage = Stage::Compute; break;
    default: ctx.setError(GL_INVALID_ENUM); return false;
    }
    const bool uniform = iface == GL_VERTEX_SUBROUTINE_UNIFORM || iface == GL_TESS_CONTROL_SUBROUTINE_UNIFORM ||
                         iface == GL_TESS_EVALUATION_SUBROUTINE_UNIFORM || iface == GL_GEOMETRY_SUBROUTINE_UNIFORM ||
                         iface == GL_FRAGMENT_SUBROUTINE_UNIFORM || iface == GL_COMPUTE_SUBROUTINE_UNIFORM;
    kind = uniform ? Kind::SubroutineUniform : Kind::Subroutine;
    return true;
}

Program* programOrError(Context& ctx, GLuint name) {
    ShareLock lock(ctx);
    Program* p = ctx.share().program(name);
    if (!p) ctx.setError(ctx.share().shader(name) ? GL_INVALID_OPERATION : GL_INVALID_VALUE);
    return p;
}

const LinkResult* linkOf(const Program& p) { return p.linked && p.linkStatus ? &p.linked->link : nullptr; }

// GL_TRANSFORM_FEEDBACK_BUFFER resources: the buffer bindings that captured
// varyings use, in binding order.
std::vector<int> xfbBuffers(const LinkResult& link) {
    std::vector<int> buffers;
    for (const shader::TransformFeedbackVarying& v : link.transformFeedback)
        if (v.name.rfind("gl_SkipComponents", 0) != 0 && v.name != "gl_NextBuffer" &&
            std::find(buffers.begin(), buffers.end(), v.buffer) == buffers.end())
            buffers.push_back(v.buffer);
    std::sort(buffers.begin(), buffers.end());
    return buffers;
}

size_t resourceCount(const LinkResult* link, Kind kind, Stage stage) {
    if (!link) return 0;
    switch (kind) {
    case Kind::Uniform: return link->visibleUniformCount();
    case Kind::UniformBlock: return link->visibleBlockCount();
    case Kind::AtomicCounterBuffer: return link->atomicBuffers.size();
    case Kind::Input: return link->programInputs.size();
    case Kind::Output: return link->programOutputs.size();
    case Kind::Subroutine: return link->subroutines[static_cast<int>(stage)].functions.size();
    case Kind::SubroutineUniform: return link->subroutines[static_cast<int>(stage)].uniforms.size();
    case Kind::TransformFeedbackVarying: return link->transformFeedback.size();
    case Kind::BufferVariable: return link->bufferVariables.size();
    case Kind::StorageBlock: return link->storageBlocks.size();
    case Kind::TransformFeedbackBuffer: return xfbBuffers(*link).size();
    }
    return 0;
}

std::string resourceName(const LinkResult& link, Kind kind, Stage stage, size_t index) {
    switch (kind) {
    case Kind::Uniform: return link.uniforms[index].name;
    case Kind::UniformBlock: return link.blocks[index].name;
    case Kind::AtomicCounterBuffer: return {};
    case Kind::Input: return link.programInputs[index].name;
    case Kind::Output: return link.programOutputs[index].name;
    case Kind::Subroutine:  // known by its GL index (layout(index) or assigned)
        for (const auto& f : link.subroutines[static_cast<int>(stage)].functions)
            if (static_cast<size_t>(f.index) == index) return f.name;
        return {};
    case Kind::SubroutineUniform: {
        const auto& u = link.subroutines[static_cast<int>(stage)].uniforms[index];
        return u.dimensions.empty() ? u.name : u.name + "[0]";
    }
    case Kind::TransformFeedbackVarying: return link.transformFeedback[index].name;
    case Kind::BufferVariable: return link.bufferVariables[index].name;
    case Kind::StorageBlock: return link.storageBlocks[index].name;
    case Kind::TransformFeedbackBuffer: return {};
    }
    return {};
}

// GL 4.3 section 7.3.1: a name matches a resource's name exactly, or with
// "[0]" appended.
bool nameMatches(const std::string& resource, const std::string& name) {
    return resource == name || resource == name + "[0]";
}

// "base[i]" -> i for an array resource named "base[0]", else -1 (0 for "base").
long elementOf(const std::string& resource, const std::string& name) {
    if (resource == name) return 0;
    if (resource.size() < 3 || resource.compare(resource.size() - 3, 3, "[0]") != 0) return -1;
    const std::string base = resource.substr(0, resource.size() - 3);
    if (name == base) return 0;
    if (name.size() < base.size() + 3 || name.compare(0, base.size(), base) != 0 || name[base.size()] != '[' || name.back() != ']')
        return -1;
    const std::string digits = name.substr(base.size() + 1, name.size() - base.size() - 2);
    if (digits.empty() || digits.find_first_not_of("0123456789") != std::string::npos) return -1;
    if (digits.size() > 1 && digits[0] == '0') return -1;  // "a[01]" names nothing
    return std::strtol(digits.c_str(), nullptr, 10);
}

// Whether `index` names a resource (subroutines by their GL index).
bool validIndex(const LinkResult* link, Kind kind, Stage stage, GLuint index) {
    if (!link) return false;
    if (kind == Kind::Subroutine) {
        for (const auto& f : link->subroutines[static_cast<int>(stage)].functions)
            if (static_cast<GLuint>(f.index) == index) return true;
        return false;
    }
    return index < resourceCount(link, kind, stage);
}

GLint referencedBy(uint32_t stageMask, Stage stage) { return (stageMask >> static_cast<int>(stage)) & 1 ? 1 : 0; }

bool referencedProperty(GLenum prop, Stage& stage) {
    switch (prop) {
    case GL_REFERENCED_BY_VERTEX_SHADER: stage = Stage::Vertex; return true;
    case GL_REFERENCED_BY_TESS_CONTROL_SHADER: stage = Stage::TessControl; return true;
    case GL_REFERENCED_BY_TESS_EVALUATION_SHADER: stage = Stage::TessEval; return true;
    case GL_REFERENCED_BY_GEOMETRY_SHADER: stage = Stage::Geometry; return true;
    case GL_REFERENCED_BY_FRAGMENT_SHADER: stage = Stage::Fragment; return true;
    case GL_REFERENCED_BY_COMPUTE_SHADER: stage = Stage::Compute; return true;
    default: return false;
    }
}

// Writes property `prop` of resource `index` into `out`; false if the
// interface has no such property (INVALID_ENUM).
bool resourceProperty(const Program& p, const LinkResult& link, Kind kind, Stage stage, size_t index, GLenum prop,
                      std::vector<GLint>& out) {
    out.clear();
    Stage referenced;
    const bool referencedProp = referencedProperty(prop, referenced);
    if (prop == GL_NAME_LENGTH) {
        if (kind == Kind::AtomicCounterBuffer || kind == Kind::TransformFeedbackBuffer) return false;
        out.push_back(static_cast<GLint>(shader::spirvDisplayName(link, resourceName(link, kind, stage, index)).size() + 1));
        return true;
    }
    switch (kind) {
    case Kind::Uniform: {
        const shader::UniformInfo& u = link.uniforms[index];
        const bool array = !u.name.empty() && u.name.back() == ']';
        const bool inBlock = u.blockIndex >= 0, counter = u.atomicBufferIndex >= 0;
        if (referencedProp) { out.push_back(referencedBy(u.stageMask, referenced)); return true; }
        switch (prop) {
        case GL_TYPE: out.push_back(static_cast<GLint>(u.type)); return true;
        case GL_ARRAY_SIZE: out.push_back(u.size); return true;
        case GL_OFFSET: out.push_back(inBlock || counter ? u.offset : -1); return true;
        case GL_BLOCK_INDEX: out.push_back(u.blockIndex); return true;
        case GL_ARRAY_STRIDE: out.push_back(inBlock ? (array ? u.arrayStride : 0) : counter ? (array ? 4 : 0) : -1); return true;
        case GL_MATRIX_STRIDE: out.push_back(inBlock ? u.matrixStride : counter ? 0 : -1); return true;
        case GL_IS_ROW_MAJOR: out.push_back(inBlock && u.rowMajor ? 1 : 0); return true;
        case GL_ATOMIC_COUNTER_BUFFER_INDEX: out.push_back(u.atomicBufferIndex); return true;
        case GL_LOCATION: out.push_back(inBlock || counter ? -1 : u.location); return true;
        default: return false;
        }
    }
    case Kind::UniformBlock: case Kind::StorageBlock: case Kind::AtomicCounterBuffer: {
        uint32_t stageMask = 0;
        GLint binding = 0, dataSize = 0;
        const std::vector<int>* active = nullptr;
        if (kind == Kind::UniformBlock) {
            const shader::UniformBlockInfo& b = link.blocks[index];
            stageMask = b.stageMask;
            binding = index < p.uniformBlockBindings.size() ? static_cast<GLint>(p.uniformBlockBindings[index]) : 0;
            dataSize = b.dataSize;
            active = &b.activeUniforms;
        } else if (kind == Kind::StorageBlock) {
            const shader::StorageBlockInfo& b = link.storageBlocks[index];
            stageMask = b.stageMask;
            binding = index < p.storageBlockBindings.size() ? static_cast<GLint>(p.storageBlockBindings[index]) : 0;
            dataSize = b.dataSize;
            active = &b.activeVariables;
        } else {
            const shader::AtomicCounterBufferInfo& b = link.atomicBuffers[index];
            stageMask = b.stageMask;
            binding = static_cast<GLint>(b.binding);
            dataSize = b.dataSize;
            active = &b.activeUniforms;
        }
        if (referencedProp) { out.push_back(referencedBy(stageMask, referenced)); return true; }
        switch (prop) {
        case GL_BUFFER_BINDING: out.push_back(binding); return true;
        case GL_BUFFER_DATA_SIZE: out.push_back(dataSize); return true;
        case GL_NUM_ACTIVE_VARIABLES: out.push_back(static_cast<GLint>(active->size())); return true;
        case GL_ACTIVE_VARIABLES: out.assign(active->begin(), active->end()); return true;
        default: return false;
        }
    }
    case Kind::Input: case Kind::Output: {
        const shader::InterfaceVariableInfo& v = kind == Kind::Input ? link.programInputs[index] : link.programOutputs[index];
        if (referencedProp) { out.push_back(referencedBy(v.stageMask, referenced)); return true; }
        switch (prop) {
        case GL_TYPE: out.push_back(static_cast<GLint>(v.type)); return true;
        case GL_ARRAY_SIZE: out.push_back(v.size); return true;
        case GL_LOCATION: out.push_back(v.location); return true;
        case GL_LOCATION_COMPONENT: out.push_back(v.location < 0 ? 0 : v.component); return true;
        case GL_IS_PER_PATCH: out.push_back(v.perPatch ? 1 : 0); return true;
        case GL_LOCATION_INDEX:
            if (kind != Kind::Output) return false;
            out.push_back((v.stageMask >> static_cast<int>(Stage::Fragment)) & 1 ? std::max(0, v.locationIndex) : -1);
            if (v.location < 0) out.back() = -1;
            return true;
        default: return false;
        }
    }
    case Kind::Subroutine: return false;
    case Kind::SubroutineUniform: {
        const auto& u = link.subroutines[static_cast<int>(stage)].uniforms[index];
        switch (prop) {
        case GL_ARRAY_SIZE: out.push_back(u.elements()); return true;
        case GL_NUM_COMPATIBLE_SUBROUTINES: out.push_back(static_cast<GLint>(u.compatible.size())); return true;
        case GL_COMPATIBLE_SUBROUTINES: out.assign(u.compatible.begin(), u.compatible.end()); return true;
        case GL_LOCATION: out.push_back(u.location); return true;
        default: return false;
        }
    }
    case Kind::TransformFeedbackVarying: {
        const shader::TransformFeedbackVarying& v = link.transformFeedback[index];
        switch (prop) {
        case GL_TYPE: out.push_back(static_cast<GLint>(v.type)); return true;
        case GL_ARRAY_SIZE: out.push_back(v.size); return true;
        case GL_OFFSET: out.push_back(static_cast<GLint>(v.offset * 4)); return true;
        case GL_TRANSFORM_FEEDBACK_BUFFER_INDEX: {
            const std::vector<int> buffers = xfbBuffers(link);
            const auto it = std::find(buffers.begin(), buffers.end(), v.buffer);
            out.push_back(it == buffers.end() ? -1 : static_cast<GLint>(it - buffers.begin()));
            return true;
        }
        default: return false;
        }
    }
    case Kind::TransformFeedbackBuffer: {
        const int buffer = xfbBuffers(link)[index];
        std::vector<GLint> active;
        for (size_t i = 0; i < link.transformFeedback.size(); ++i) {
            const shader::TransformFeedbackVarying& v = link.transformFeedback[i];
            if (v.buffer == buffer && v.name.rfind("gl_SkipComponents", 0) != 0 && v.name != "gl_NextBuffer")
                active.push_back(static_cast<GLint>(i));
        }
        switch (prop) {
        case GL_BUFFER_BINDING: out.push_back(buffer); return true;
        case GL_NUM_ACTIVE_VARIABLES: out.push_back(static_cast<GLint>(active.size())); return true;
        case GL_ACTIVE_VARIABLES: out.assign(active.begin(), active.end()); return true;
        case GL_TRANSFORM_FEEDBACK_BUFFER_STRIDE:
            out.push_back(static_cast<GLint>(buffer >= 0 && buffer < 4 ? link.xfbStride[buffer] * 4 : 0));
            return true;
        default: return false;
        }
    }
    case Kind::BufferVariable: {
        const shader::BufferVariableInfo& v = link.bufferVariables[index];
        const bool array = !v.name.empty() && v.name.back() == ']';
        if (referencedProp) { out.push_back(referencedBy(v.stageMask, referenced)); return true; }
        switch (prop) {
        case GL_TYPE: out.push_back(static_cast<GLint>(v.type)); return true;
        case GL_ARRAY_SIZE: out.push_back(v.size); return true;
        case GL_OFFSET: out.push_back(v.offset); return true;
        case GL_BLOCK_INDEX: out.push_back(v.blockIndex); return true;
        case GL_ARRAY_STRIDE: out.push_back(array ? v.arrayStride : 0); return true;
        case GL_MATRIX_STRIDE: out.push_back(v.matrixStride); return true;
        case GL_IS_ROW_MAJOR: out.push_back(v.rowMajor ? 1 : 0); return true;
        case GL_TOP_LEVEL_ARRAY_SIZE: out.push_back(v.topLevelArraySize); return true;
        case GL_TOP_LEVEL_ARRAY_STRIDE: out.push_back(v.topLevelArrayStride); return true;
        default: return false;
        }
    }
    }
    return false;
}

// A property name of any interface (GL 4.3 table 7.2): one that the
// interface lacks is INVALID_OPERATION, an unknown name INVALID_ENUM.
bool knownProperty(GLenum prop) {
    switch (prop) {
    case GL_NAME_LENGTH: case GL_TYPE: case GL_ARRAY_SIZE: case GL_OFFSET: case GL_BLOCK_INDEX: case GL_ARRAY_STRIDE:
    case GL_MATRIX_STRIDE: case GL_IS_ROW_MAJOR: case GL_ATOMIC_COUNTER_BUFFER_INDEX: case GL_BUFFER_BINDING:
    case GL_BUFFER_DATA_SIZE: case GL_NUM_ACTIVE_VARIABLES: case GL_ACTIVE_VARIABLES: case GL_REFERENCED_BY_VERTEX_SHADER:
    case GL_REFERENCED_BY_TESS_CONTROL_SHADER: case GL_REFERENCED_BY_TESS_EVALUATION_SHADER:
    case GL_REFERENCED_BY_GEOMETRY_SHADER: case GL_REFERENCED_BY_FRAGMENT_SHADER: case GL_REFERENCED_BY_COMPUTE_SHADER:
    case GL_TOP_LEVEL_ARRAY_SIZE: case GL_TOP_LEVEL_ARRAY_STRIDE: case GL_LOCATION: case GL_LOCATION_INDEX:
    case GL_IS_PER_PATCH: case GL_NUM_COMPATIBLE_SUBROUTINES: case GL_COMPATIBLE_SUBROUTINES:
    case GL_LOCATION_COMPONENT: case GL_TRANSFORM_FEEDBACK_BUFFER_INDEX: case GL_TRANSFORM_FEEDBACK_BUFFER_STRIDE:
        return true;
    default: return false;
    }
}

}  // namespace

void GetProgramInterfaceiv(Context& ctx, GLuint program, GLenum programInterface, GLenum pname, GLint* params) {
    if (!programInterfaceQuery(ctx, "glGetProgramInterfaceiv")) return;
    Kind kind;
    Stage stage;
    if (!interfaceKind(ctx, programInterface, kind, stage)) return;
    Program* p = programOrError(ctx, program);
    if (!p || !params) return;
    const LinkResult* link = linkOf(*p);
    const size_t count = resourceCount(link, kind, stage);
    switch (pname) {
    case GL_ACTIVE_RESOURCES: *params = static_cast<GLint>(count); return;
    case GL_MAX_NAME_LENGTH: {
        if (kind == Kind::AtomicCounterBuffer || kind == Kind::TransformFeedbackBuffer) return ctx.setError(GL_INVALID_OPERATION);
        GLint n = 0;
        if (kind == Kind::Subroutine && link)
            for (const auto& f : link->subroutines[static_cast<int>(stage)].functions) n = std::max(n, static_cast<GLint>(f.name.size() + 1));
        else if (kind != Kind::Subroutine)
            for (size_t i = 0; i < count; ++i)
                n = std::max(n, static_cast<GLint>(shader::spirvDisplayName(*link, resourceName(*link, kind, stage, i)).size() + 1));
        *params = n;
        return;
    }
    case GL_MAX_NUM_ACTIVE_VARIABLES: {
        if (kind != Kind::UniformBlock && kind != Kind::StorageBlock && kind != Kind::AtomicCounterBuffer &&
            kind != Kind::TransformFeedbackBuffer)
            return ctx.setError(GL_INVALID_OPERATION);
        GLint n = 0;
        std::vector<GLint> values;
        for (size_t i = 0; i < count; ++i) {
            resourceProperty(*p, *link, kind, stage, i, GL_NUM_ACTIVE_VARIABLES, values);
            n = std::max(n, values.empty() ? 0 : values[0]);
        }
        *params = n;
        return;
    }
    case GL_MAX_NUM_COMPATIBLE_SUBROUTINES: {
        if (kind != Kind::SubroutineUniform) return ctx.setError(GL_INVALID_OPERATION);
        GLint n = 0;
        for (size_t i = 0; i < count; ++i)
            n = std::max(n, static_cast<GLint>(link->subroutines[static_cast<int>(stage)].uniforms[i].compatible.size()));
        *params = n;
        return;
    }
    default: return ctx.setError(GL_INVALID_ENUM);
    }
}

GLuint GetProgramResourceIndex(Context& ctx, GLuint program, GLenum programInterface, const GLchar* name) {
    if (!programInterfaceQuery(ctx, "glGetProgramResourceIndex")) return GL_INVALID_INDEX;
    Kind kind;
    Stage stage;
    if (!interfaceKind(ctx, programInterface, kind, stage)) return GL_INVALID_INDEX;
    if (kind == Kind::AtomicCounterBuffer || kind == Kind::TransformFeedbackBuffer) {
        ctx.setError(GL_INVALID_ENUM);
        return GL_INVALID_INDEX;
    }
    Program* p = programOrError(ctx, program);
    if (!p || !name) return GL_INVALID_INDEX;
    const LinkResult* link = linkOf(*p);
    const size_t count = resourceCount(link, kind, stage);
    // Transform feedback's gl_NextBuffer and gl_SkipComponents* are listed
    // but have no index by name (GL 4.3 section 7.3.1.1).
    if (kind == Kind::TransformFeedbackVarying &&
        (std::strcmp(name, "gl_NextBuffer") == 0 || std::strncmp(name, "gl_SkipComponents", 17) == 0))
        return GL_INVALID_INDEX;
    if (kind == Kind::Subroutine) {
        for (const auto& f : link ? link->subroutines[static_cast<int>(stage)].functions
                                  : std::vector<shader::SubroutineStageInfo::Function>())
            if (f.name == name) return static_cast<GLuint>(f.index);
        return GL_INVALID_INDEX;
    }
    for (size_t i = 0; i < count; ++i) {
        if (!nameMatches(resourceName(*link, kind, stage, i), name)) continue;
        return static_cast<GLuint>(i);
    }
    return GL_INVALID_INDEX;
}

void GetProgramResourceName(Context& ctx, GLuint program, GLenum programInterface, GLuint index, GLsizei bufSize,
                            GLsizei* length, GLchar* name) {
    if (!programInterfaceQuery(ctx, "glGetProgramResourceName")) return;
    Kind kind;
    Stage stage;
    if (!interfaceKind(ctx, programInterface, kind, stage)) return;
    if (kind == Kind::AtomicCounterBuffer || kind == Kind::TransformFeedbackBuffer) return ctx.setError(GL_INVALID_ENUM);
    Program* p = programOrError(ctx, program);
    if (!p) return;
    if (bufSize < 0) return ctx.setError(GL_INVALID_VALUE);
    const LinkResult* link = linkOf(*p);
    if (!validIndex(link, kind, stage, index)) return ctx.setError(GL_INVALID_VALUE);
    const std::string s = shader::spirvDisplayName(*link, resourceName(*link, kind, stage, index));
    if (length) *length = 0;
    if (bufSize <= 0 || !name) return;
    const GLsizei n = std::min<GLsizei>(bufSize - 1, static_cast<GLsizei>(s.size()));
    std::memcpy(name, s.data(), static_cast<size_t>(n));
    name[n] = 0;
    if (length) *length = n;
}

void GetProgramResourceiv(Context& ctx, GLuint program, GLenum programInterface, GLuint index, GLsizei propCount,
                          const GLenum* props, GLsizei count, GLsizei* length, GLint* params) {
    if (!programInterfaceQuery(ctx, "glGetProgramResourceiv")) return;
    Kind kind;
    Stage stage;
    if (!interfaceKind(ctx, programInterface, kind, stage)) return;
    Program* p = programOrError(ctx, program);
    if (!p) return;
    if (propCount <= 0 || count < 0) return ctx.setError(GL_INVALID_VALUE);
    const LinkResult* link = linkOf(*p);
    if (!validIndex(link, kind, stage, index)) return ctx.setError(GL_INVALID_VALUE);
    // Validate every property first: an unknown one writes nothing.
    std::vector<std::vector<GLint>> values(static_cast<size_t>(propCount));
    for (GLsizei i = 0; i < propCount; ++i)
        if (!props || !resourceProperty(*p, *link, kind, stage, index, props[i], values[static_cast<size_t>(i)]))
            return ctx.setError(props && knownProperty(props[i]) ? GL_INVALID_OPERATION : GL_INVALID_ENUM);
    GLsizei written = 0;
    for (const std::vector<GLint>& v : values)
        for (GLint x : v) {
            if (written >= count) break;
            if (params) params[written] = x;
            ++written;
        }
    if (length) *length = written;
}

GLint GetProgramResourceLocation(Context& ctx, GLuint program, GLenum programInterface, const GLchar* name) {
    if (!programInterfaceQuery(ctx, "glGetProgramResourceLocation")) return -1;
    Kind kind;
    Stage stage;
    if (!interfaceKind(ctx, programInterface, kind, stage)) return -1;
    if (kind != Kind::Uniform && kind != Kind::Input && kind != Kind::Output && kind != Kind::SubroutineUniform) {
        ctx.setError(GL_INVALID_ENUM);
        return -1;
    }
    Program* p = programOrError(ctx, program);
    if (!p) return -1;
    const LinkResult* link = linkOf(*p);
    if (!link) {
        ctx.setError(GL_INVALID_OPERATION);
        return -1;
    }
    if (!name || std::strncmp(name, "gl_", 3) == 0) return -1;
    if (kind == Kind::SubroutineUniform) {
        static const GLenum kShaderTypes[] = {GL_VERTEX_SHADER,   GL_TESS_CONTROL_SHADER, GL_TESS_EVALUATION_SHADER,
                                              GL_GEOMETRY_SHADER, GL_FRAGMENT_SHADER,     GL_COMPUTE_SHADER};
        return GetSubroutineUniformLocation(ctx, program, kShaderTypes[static_cast<int>(stage)], name);
    }
    const size_t count = resourceCount(link, kind, stage);
    for (size_t i = 0; i < count; ++i) {
        const long element = elementOf(resourceName(*link, kind, stage, i), name);
        if (element < 0) continue;
        std::vector<GLint> v;
        resourceProperty(*p, *link, kind, stage, i, GL_LOCATION, v);
        GLint size = 1;
        if (kind == Kind::Uniform) size = link->uniforms[i].size;
        else if (kind == Kind::Input) size = link->programInputs[i].size;
        else if (kind == Kind::Output) size = link->programOutputs[i].size;
        else size = link->subroutines[static_cast<int>(stage)].uniforms[i].elements();
        if (v.empty() || v[0] < 0 || element >= size) return -1;
        // Elements of an input or output take the type's locations each.
        const GLint step = kind == Kind::Input || kind == Kind::Output
                               ? shader::glTypeLocations(kind == Kind::Input ? link->programInputs[i].type : link->programOutputs[i].type)
                               : 1;
        return v[0] + static_cast<GLint>(element) * step;
    }
    return -1;
}

GLint GetProgramResourceLocationIndex(Context& ctx, GLuint program, GLenum programInterface, const GLchar* name) {
    if (!programInterfaceQuery(ctx, "glGetProgramResourceLocationIndex")) return -1;
    if (programInterface != GL_PROGRAM_OUTPUT) {
        ctx.setError(GL_INVALID_ENUM);
        return -1;
    }
    Program* p = programOrError(ctx, program);
    if (!p) return -1;
    const LinkResult* link = linkOf(*p);
    if (!link) {
        ctx.setError(GL_INVALID_OPERATION);
        return -1;
    }
    if (!name || std::strncmp(name, "gl_", 3) == 0) return -1;
    for (size_t i = 0; i < link->programOutputs.size(); ++i) {
        const shader::InterfaceVariableInfo& v = link->programOutputs[i];
        const long element = elementOf(v.name, name);
        if (element < 0 || element >= v.size) continue;
        if (!((v.stageMask >> static_cast<int>(Stage::Fragment)) & 1) || v.location < 0) return -1;
        return std::max(0, v.locationIndex);
    }
    return -1;
}

}  // namespace wmgl::api
