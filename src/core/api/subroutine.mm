// Shader subroutines (GL 4.0 section 7.9, GL_ARB_shader_subroutine). The
// compiler lowers them (shader/subroutines.h); this is the program interface
// and the per-stage selection state, which DrawState writes into the hidden
// selection uniforms before each draw (applySubroutineSelections).

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "core/context.h"
#include "core/program.h"
#include "gl_api.gen.h"

namespace wmgl {

using shader::Stage;
using shader::SubroutineStageInfo;

namespace {

bool subroutines(Context& ctx, const char* command) {
    if (ctx.version() >= 40 || ctx.hasExtension("GL_ARB_shader_subroutine")) return true;
    ctx.unimplemented(command);
    return false;
}

// The stage of a shader type, or false (INVALID_ENUM set).
bool stageOf(Context& ctx, GLenum type, Stage& stage) {
    switch (type) {
    case GL_VERTEX_SHADER: stage = Stage::Vertex; return true;
    case GL_FRAGMENT_SHADER: stage = Stage::Fragment; return true;
    case GL_GEOMETRY_SHADER: stage = Stage::Geometry; return true;
    case GL_TESS_CONTROL_SHADER: case GL_TESS_EVALUATION_SHADER:
        if (!ctx.tessellationShaders()) break;
        stage = type == GL_TESS_CONTROL_SHADER ? Stage::TessControl : Stage::TessEval;
        return true;
    case GL_COMPUTE_SHADER:
        if (!ctx.computeShaders()) break;
        stage = Stage::Compute;
        return true;
    default: break;
    }
    ctx.setError(GL_INVALID_ENUM);
    return false;
}

// A program's subroutine interface for glGet*Subroutine* (errors set).
const SubroutineStageInfo* programStage(Context& ctx, GLuint name, GLenum type, Stage& stage) {
    if (!stageOf(ctx, type, stage)) return nullptr;
    Program* p = nullptr;
    {
        ShareLock lock(ctx);
        p = ctx.share().program(name);
        if (!p) {
            ctx.setError(ctx.share().shader(name) ? GL_INVALID_OPERATION : GL_INVALID_VALUE);
            return nullptr;
        }
    }
    if (!p->linkStatus || !p->linked) {
        ctx.setError(GL_INVALID_OPERATION);
        return nullptr;
    }
    return &p->linked->link.subroutines[static_cast<int>(stage)];
}

void copyName(const std::string& s, GLsizei bufSize, GLsizei* length, GLchar* out) {
    if (bufSize <= 0 || !out) {
        if (length) *length = 0;
        return;
    }
    const GLsizei n = std::min<GLsizei>(bufSize - 1, static_cast<GLsizei>(s.size()));
    std::memcpy(out, s.data(), static_cast<size_t>(n));
    out[n] = 0;
    if (length) *length = n;
}

std::string uniformName(const SubroutineStageInfo::Uniform& u) { return u.dimensions.empty() ? u.name : u.name + "[0]"; }

// The executable whose stage `stage` the context draws with: the current
// program, else the bound pipeline's stage program.
Program* stageProgram(Context& ctx, Stage stage) {
    if (Program* p = ctx.state.program.get()) return p;
    if (ProgramPipeline* pp = ctx.state.programPipeline.get()) return pp->stages[static_cast<int>(stage)].get();
    return nullptr;
}

}  // namespace

void resetSubroutineSelections(Context& ctx) {
    for (auto& selection : ctx.subroutineSelections) selection = Context::SubroutineSelection();
}

void applySubroutineSelections(Context& ctx, Program& program) {
    if (!program.linked) return;
    const shader::LinkResult& link = program.linked->link;
    for (int s = 0; s < shader::kStageCount; ++s) {
        const SubroutineStageInfo& info = link.subroutines[s];
        if (info.uniforms.empty()) continue;
        // The selections apply to the program drawing this stage; another
        // program's (or none) means the defaults.
        Context::SubroutineSelection& selection = ctx.subroutineSelections[s];
        const bool own = selection.programUid == program.linked->uid;
        for (const SubroutineStageInfo::Uniform& u : info.uniforms) {
            for (int e = 0; e < u.elements(); ++e) {
                const size_t location = static_cast<size_t>(u.location + e);
                const GLuint index = own && location < selection.indices.size() ? selection.indices[location]
                                                                                 : static_cast<GLuint>(u.compatible[0]);
                const size_t offset = static_cast<size_t>(u.selectionOffset) + static_cast<size_t>(e) * static_cast<size_t>(u.selectionStride);
                if (offset + 4 > program.uniformData.size()) continue;
                int32_t current = 0;
                std::memcpy(&current, program.uniformData.data() + offset, 4);
                if (current == static_cast<int32_t>(index)) continue;
                const int32_t value = static_cast<int32_t>(index);
                std::memcpy(program.uniformData.data() + offset, &value, 4);
                ++program.uniformVersion;
            }
        }
    }
}

namespace api {

GLint GetSubroutineUniformLocation(Context& ctx, GLuint program, GLenum shadertype, const GLchar* name) {
    if (!subroutines(ctx, "glGetSubroutineUniformLocation")) return -1;
    Stage stage;
    const SubroutineStageInfo* info = programStage(ctx, program, shadertype, stage);
    if (!info || !name) return -1;
    const std::string n = name;
    for (const SubroutineStageInfo::Uniform& u : info->uniforms) {
        if (n == u.name) return u.location;
        if (n.compare(0, u.name.size(), u.name) != 0 || u.dimensions.empty()) continue;
        // An element "name[i][j]..." (row-major over the dimensions); omitted
        // trailing subscripts are 0 ("name", "name[1]" of a 2D array).
        size_t at = u.name.size();
        int element = 0;
        bool ok = true;
        for (size_t d = 0; d < u.dimensions.size(); ++d) {
            int index = 0;
            if (at < n.size()) {
                const size_t close = n.find(']', at);
                if (n[at] != '[' || close == std::string::npos) { ok = false; break; }
                const std::string digits = n.substr(at + 1, close - at - 1);
                if (digits.empty() || digits.find_first_not_of("0123456789") != std::string::npos ||
                    (digits.size() > 1 && digits[0] == '0')) { ok = false; break; }
                index = std::atoi(digits.c_str());
                at = close + 1;
            }
            if (index >= u.dimensions[d]) { ok = false; break; }
            element = element * u.dimensions[d] + index;
        }
        if (ok && at == n.size()) return u.location + element;
    }
    return -1;
}

GLuint GetSubroutineIndex(Context& ctx, GLuint program, GLenum shadertype, const GLchar* name) {
    if (!subroutines(ctx, "glGetSubroutineIndex")) return GL_INVALID_INDEX;
    Stage stage;
    const SubroutineStageInfo* info = programStage(ctx, program, shadertype, stage);
    if (!info || !name) return GL_INVALID_INDEX;
    for (const SubroutineStageInfo::Function& f : info->functions)
        if (f.name == name) return static_cast<GLuint>(f.index);
    return GL_INVALID_INDEX;
}

void GetActiveSubroutineUniformiv(Context& ctx, GLuint program, GLenum shadertype, GLuint index, GLenum pname, GLint* values) {
    if (!subroutines(ctx, "glGetActiveSubroutineUniformiv")) return;
    Stage stage;
    const SubroutineStageInfo* info = programStage(ctx, program, shadertype, stage);
    if (!info) return;
    if (index >= info->uniforms.size()) return ctx.setError(GL_INVALID_VALUE);
    const SubroutineStageInfo::Uniform& u = info->uniforms[index];
    switch (pname) {
    case GL_NUM_COMPATIBLE_SUBROUTINES: *values = static_cast<GLint>(u.compatible.size()); return;
    case GL_COMPATIBLE_SUBROUTINES:
        for (size_t i = 0; i < u.compatible.size(); ++i) values[i] = u.compatible[i];
        return;
    case GL_UNIFORM_SIZE: *values = u.elements(); return;
    case GL_UNIFORM_NAME_LENGTH: *values = static_cast<GLint>(uniformName(u).size() + 1); return;
    default: return ctx.setError(GL_INVALID_ENUM);
    }
}

void GetActiveSubroutineUniformName(Context& ctx, GLuint program, GLenum shadertype, GLuint index, GLsizei bufSize,
                                    GLsizei* length, GLchar* name) {
    if (!subroutines(ctx, "glGetActiveSubroutineUniformName")) return;
    if (bufSize < 0) return ctx.setError(GL_INVALID_VALUE);
    Stage stage;
    const SubroutineStageInfo* info = programStage(ctx, program, shadertype, stage);
    if (!info) return;
    if (index >= info->uniforms.size()) return ctx.setError(GL_INVALID_VALUE);
    copyName(uniformName(info->uniforms[index]), bufSize, length, name);
}

void GetActiveSubroutineName(Context& ctx, GLuint program, GLenum shadertype, GLuint index, GLsizei bufSize, GLsizei* length,
                             GLchar* name) {
    if (!subroutines(ctx, "glGetActiveSubroutineName")) return;
    if (bufSize < 0) return ctx.setError(GL_INVALID_VALUE);
    Stage stage;
    const SubroutineStageInfo* info = programStage(ctx, program, shadertype, stage);
    if (!info) return;
    for (const SubroutineStageInfo::Function& f : info->functions)
        if (static_cast<GLuint>(f.index) == index) return copyName(f.name, bufSize, length, name);
    ctx.setError(GL_INVALID_VALUE);
}

void GetProgramStageiv(Context& ctx, GLuint program, GLenum shadertype, GLenum pname, GLint* values) {
    if (!subroutines(ctx, "glGetProgramStageiv")) return;
    Stage stage;
    const SubroutineStageInfo* info = programStage(ctx, program, shadertype, stage);
    if (!info) return;
    switch (pname) {
    case GL_ACTIVE_SUBROUTINE_UNIFORMS: *values = static_cast<GLint>(info->uniforms.size()); return;
    case GL_ACTIVE_SUBROUTINE_UNIFORM_LOCATIONS: *values = info->locationCount; return;
    case GL_ACTIVE_SUBROUTINES: *values = static_cast<GLint>(info->functions.size()); return;
    case GL_ACTIVE_SUBROUTINE_UNIFORM_MAX_LENGTH: {
        GLint length = 0;
        for (const auto& u : info->uniforms) length = std::max(length, static_cast<GLint>(uniformName(u).size() + 1));
        *values = length;
        return;
    }
    case GL_ACTIVE_SUBROUTINE_MAX_LENGTH: {
        GLint length = 0;
        for (const auto& f : info->functions) length = std::max(length, static_cast<GLint>(f.name.size() + 1));
        *values = length;
        return;
    }
    default: return ctx.setError(GL_INVALID_ENUM);
    }
}

void UniformSubroutinesuiv(Context& ctx, GLenum shadertype, GLsizei count, const GLuint* indices) {
    if (!subroutines(ctx, "glUniformSubroutinesuiv")) return;
    Stage stage;
    if (!stageOf(ctx, shadertype, stage)) return;
    Program* p = stageProgram(ctx, stage);
    if (!p || !p->linked) return ctx.setError(GL_INVALID_OPERATION);
    const SubroutineStageInfo& info = p->linked->link.subroutines[static_cast<int>(stage)];
    if (count != info.locationCount || count < 0) return ctx.setError(GL_INVALID_VALUE);
    // Indices must name active subroutines (INVALID_VALUE) compatible with
    // the uniform at their location (INVALID_OPERATION). Locations that no
    // active uniform uses (explicit locations leave gaps) are ignored.
    std::vector<bool> active(static_cast<size_t>(count), false);
    for (const SubroutineStageInfo::Uniform& u : info.uniforms)
        for (int e = 0; e < u.elements(); ++e)
            if (u.location + e < count) active[static_cast<size_t>(u.location + e)] = true;
    for (GLsizei i = 0; i < count; ++i) {
        if (!active[static_cast<size_t>(i)]) continue;
        const bool known = std::any_of(info.functions.begin(), info.functions.end(),
                                       [&](const SubroutineStageInfo::Function& f) { return static_cast<GLuint>(f.index) == indices[i]; });
        if (!known) return ctx.setError(GL_INVALID_VALUE);
    }
    for (const SubroutineStageInfo::Uniform& u : info.uniforms)
        for (int e = 0; e < u.elements(); ++e) {
            const GLuint index = indices[u.location + e];
            if (std::find(u.compatible.begin(), u.compatible.end(), static_cast<GLint>(index)) == u.compatible.end())
                return ctx.setError(GL_INVALID_OPERATION);
        }
    Context::SubroutineSelection& selection = ctx.subroutineSelections[static_cast<int>(stage)];
    selection.programUid = p->linked->uid;
    selection.indices.assign(indices, indices + count);
}

void GetUniformSubroutineuiv(Context& ctx, GLenum shadertype, GLint location, GLuint* params) {
    if (!subroutines(ctx, "glGetUniformSubroutineuiv")) return;
    Stage stage;
    if (!stageOf(ctx, shadertype, stage)) return;
    Program* p = stageProgram(ctx, stage);
    // The active program must have the stage.
    if (!p || !p->linked || !p->linked->link.stages[static_cast<int>(stage)].present) return ctx.setError(GL_INVALID_OPERATION);
    const SubroutineStageInfo& info = p->linked->link.subroutines[static_cast<int>(stage)];
    if (location < 0 || location >= info.locationCount) return ctx.setError(GL_INVALID_VALUE);
    const Context::SubroutineSelection& selection = ctx.subroutineSelections[static_cast<int>(stage)];
    for (const SubroutineStageInfo::Uniform& u : info.uniforms) {
        if (location < u.location || location >= u.location + u.elements()) continue;
        const bool own = selection.programUid == p->linked->uid && static_cast<size_t>(location) < selection.indices.size();
        *params = own ? selection.indices[static_cast<size_t>(location)] : static_cast<GLuint>(u.compatible[0]);
        return;
    }
    // A location no active uniform uses (explicit locations leave gaps).
    ctx.setError(GL_INVALID_VALUE);
}

}  // namespace api
}  // namespace wmgl
