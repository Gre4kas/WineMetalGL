// GL_ARB_separate_shader_objects: program pipeline objects (GL 4.1 section 7.4).
//
// A pipeline names one separable program per stage. When no program is
// current (UseProgram), draws use the bound pipeline's executable, which
// DrawState assembles from the stage programs (core/program.h).

#include <cstring>

#include "core/context.h"
#include "core/program.h"
#include "gl_api.gen.h"

namespace wmgl::api {
namespace {

using shader::Stage;

struct StageBit {
    GLbitfield bit;
    Stage stage;
    GLenum shaderType;
};
const StageBit kStageBits[] = {
    {GL_VERTEX_SHADER_BIT, Stage::Vertex, GL_VERTEX_SHADER},
    {GL_TESS_CONTROL_SHADER_BIT, Stage::TessControl, GL_TESS_CONTROL_SHADER},
    {GL_TESS_EVALUATION_SHADER_BIT, Stage::TessEval, GL_TESS_EVALUATION_SHADER},
    {GL_GEOMETRY_SHADER_BIT, Stage::Geometry, GL_GEOMETRY_SHADER},
    {GL_FRAGMENT_SHADER_BIT, Stage::Fragment, GL_FRAGMENT_SHADER},
    {GL_COMPUTE_SHADER_BIT, Stage::Compute, GL_COMPUTE_SHADER},
};

// The pipeline object for `name`, created on first use of a generated name.
ProgramPipeline* pipelineOrError(Context& ctx, GLuint name) {
    if (!ctx.programPipelines.isGenerated(name)) {
        ctx.setError(GL_INVALID_OPERATION);
        return nullptr;
    }
    ProgramPipeline* pp = ctx.programPipelines.get(name);
    if (!pp) {
        RefPtr<ProgramPipeline> created = makeRef<ProgramPipeline>(name);
        ctx.programPipelines.set(name, created);
        pp = created.get();
    }
    return pp;
}

Program* programObject(Context& ctx, GLuint name) {
    ShareLock lock(ctx);
    if (Program* p = ctx.share().program(name)) return p;
    ctx.setError(ctx.share().shader(name) ? GL_INVALID_OPERATION : GL_INVALID_VALUE);
    return nullptr;
}

bool hasStage(const Program& p, Stage stage) {
    return p.linked && p.linked->link.stages[static_cast<int>(stage)].present;
}

// GL 4.1 section 11.1.3.11 checks for drawing with the pipeline.
bool validate(ProgramPipeline& pp, std::string& log) {
    log.clear();
    for (const StageBit& s : kStageBits) {
        const Program* p = pp.stages[static_cast<int>(s.stage)].get();
        if (!p) continue;
        if (!p->linkStatus || !p->linked) log += "error: a stage program is not linked\n";
        else if (!p->separable) log += "error: program " + std::to_string(p->name) + " is not separable\n";
    }
    const Program* vs = pp.stages[static_cast<int>(Stage::Vertex)].get();
    const Program* gs = pp.stages[static_cast<int>(Stage::Geometry)].get();
    if (gs && gs != vs)
        log += "error: WineMetalGL needs the geometry shader in the same program as the vertex shader\n";
    const Program* tcs = pp.stages[static_cast<int>(Stage::TessControl)].get();
    const Program* tes = pp.stages[static_cast<int>(Stage::TessEval)].get();
    if ((tcs && tcs != vs) || (tes && tes != vs))
        log += "error: WineMetalGL needs the tessellation shaders in the same program as the vertex shader\n";
    return log.empty();
}

}  // namespace

bool validateProgramPipeline(ProgramPipeline& pp) {
    pp.validateStatus = validate(pp, pp.infoLog);
    return pp.validateStatus;
}

void GenProgramPipelines(Context& ctx, GLsizei n, GLuint* pipelines) {
    if (n < 0) return ctx.setError(GL_INVALID_VALUE);
    ctx.programPipelines.generate(n, pipelines);
}

void DeleteProgramPipelines(Context& ctx, GLsizei n, const GLuint* pipelines) {
    if (n < 0) return ctx.setError(GL_INVALID_VALUE);
    for (GLsizei i = 0; i < n; ++i) {
        if (!pipelines[i]) continue;
        RefPtr<ProgramPipeline> pp = ctx.programPipelines.remove(pipelines[i]);
        if (pp && ctx.state.programPipeline == pp) ctx.state.programPipeline.reset();
        // Programs deleted while they were its stages go now.
        for (int stage = 0; pp && stage < 6; ++stage) {
            RefPtr<Program> previous = pp->stages[stage];
            pp->setStage(stage, RefPtr<Program>());
            if (previous) maybeDeleteProgram(ctx, previous.get());
        }
    }
    ctx.markDirty(kDirtyProgram);
}

GLboolean IsProgramPipeline(Context& ctx, GLuint pipeline) {
    return ctx.programPipelines.exists(pipeline) ? GL_TRUE : GL_FALSE;
}

void BindProgramPipeline(Context& ctx, GLuint pipeline) {
    const TransformFeedback& xfb = *ctx.state.transformFeedback;
    if (xfb.active && !xfb.paused) return ctx.setError(GL_INVALID_OPERATION);
    if (!pipeline) {
        ctx.state.programPipeline.reset();
    } else {
        if (!pipelineOrError(ctx, pipeline)) return;
        ctx.state.programPipeline = ctx.programPipelines.getRef(pipeline);
    }
    ctx.markDirty(kDirtyProgram | kDirtyUniforms | kDirtyTextures | kDirtyVertexInput);
}

void UseProgramStages(Context& ctx, GLuint pipeline, GLbitfield stages, GLuint program) {
    GLbitfield known = GL_VERTEX_SHADER_BIT | GL_GEOMETRY_SHADER_BIT | GL_FRAGMENT_SHADER_BIT;
    if (ctx.computeShaders()) known |= GL_COMPUTE_SHADER_BIT;
    if (ctx.tessellationShaders()) known |= GL_TESS_CONTROL_SHADER_BIT | GL_TESS_EVALUATION_SHADER_BIT;
    if (stages != GL_ALL_SHADER_BITS && (stages & ~known)) return ctx.setError(GL_INVALID_VALUE);
    ProgramPipeline* pp = pipelineOrError(ctx, pipeline);
    if (!pp) return;
    RefPtr<Program> p;
    if (program) {
        Program* object = programObject(ctx, program);
        if (!object) return;
        if (!object->separable || !object->linkStatus) return ctx.setError(GL_INVALID_OPERATION);
        p = RefPtr<Program>(object);
    }
    for (const StageBit& s : kStageBits) {
        if (!(stages & s.bit)) continue;
        RefPtr<Program> previous = pp->stages[static_cast<int>(s.stage)];
        pp->setStage(static_cast<int>(s.stage), p && hasStage(*p, s.stage) ? p : RefPtr<Program>());
        if (previous) maybeDeleteProgram(ctx, previous.get());  // deleted while it was this stage
    }
    pp->executable.reset();
    ctx.markDirty(kDirtyProgram | kDirtyUniforms | kDirtyTextures | kDirtyVertexInput);
}

void ActiveShaderProgram(Context& ctx, GLuint pipeline, GLuint program) {
    ProgramPipeline* pp = pipelineOrError(ctx, pipeline);
    if (!pp) return;
    if (!program) {
        pp->activeProgram.reset();
        return;
    }
    Program* p = programObject(ctx, program);
    if (!p) return;
    if (!p->linkStatus) return ctx.setError(GL_INVALID_OPERATION);
    pp->activeProgram = RefPtr<Program>(p);
}

void GetProgramPipelineiv(Context& ctx, GLuint pipeline, GLenum pname, GLint* params) {
    ProgramPipeline* pp = pipelineOrError(ctx, pipeline);
    if (!pp) return;
    auto stageName = [&](Stage s) {
        const Program* p = pp->stages[static_cast<int>(s)].get();
        return static_cast<GLint>(p ? p->name : 0);
    };
    switch (pname) {
    case GL_ACTIVE_PROGRAM: *params = static_cast<GLint>(pp->activeProgram ? pp->activeProgram->name : 0); return;
    case GL_VERTEX_SHADER: *params = stageName(Stage::Vertex); return;
    case GL_GEOMETRY_SHADER: *params = stageName(Stage::Geometry); return;
    case GL_FRAGMENT_SHADER: *params = stageName(Stage::Fragment); return;
    case GL_TESS_CONTROL_SHADER: case GL_TESS_EVALUATION_SHADER:
        if (!ctx.tessellationShaders()) break;
        *params = stageName(pname == GL_TESS_CONTROL_SHADER ? Stage::TessControl : Stage::TessEval);
        return;
    case GL_COMPUTE_SHADER:
        if (!ctx.computeShaders()) break;
        *params = stageName(Stage::Compute);
        return;
    case GL_VALIDATE_STATUS: *params = pp->validateStatus; return;
    case GL_INFO_LOG_LENGTH: *params = pp->infoLog.empty() ? 0 : static_cast<GLint>(pp->infoLog.size() + 1); return;
    default: break;
    }
    ctx.setError(GL_INVALID_ENUM);
}

void ValidateProgramPipeline(Context& ctx, GLuint pipeline) {
    if (ProgramPipeline* pp = pipelineOrError(ctx, pipeline)) validateProgramPipeline(*pp);
}

void GetProgramPipelineInfoLog(Context& ctx, GLuint pipeline, GLsizei bufSize, GLsizei* length, GLchar* infoLog) {
    if (bufSize < 0) return ctx.setError(GL_INVALID_VALUE);
    ProgramPipeline* pp = pipelineOrError(ctx, pipeline);
    if (!pp) return;
    const GLsizei n = bufSize > 0 ? std::min<GLsizei>(bufSize - 1, static_cast<GLsizei>(pp->infoLog.size())) : 0;
    if (infoLog && bufSize > 0) {
        std::memcpy(infoLog, pp->infoLog.data(), static_cast<size_t>(n));
        infoLog[n] = 0;
    }
    if (length) *length = n;
}

GLuint CreateShaderProgramv(Context& ctx, GLenum type, GLsizei count, const GLchar* const* strings) {
    if (count < 0) {
        ctx.setError(GL_INVALID_VALUE);
        return 0;
    }
    const GLuint shader = CreateShader(ctx, type);
    if (!shader) return 0;
    ShaderSource(ctx, shader, count, strings, nullptr);
    CompileShader(ctx, shader);
    const GLuint program = CreateProgram(ctx);
    if (program) {
        GLint compiled = GL_FALSE;
        GetShaderiv(ctx, shader, GL_COMPILE_STATUS, &compiled);
        ProgramParameteri(ctx, program, GL_PROGRAM_SEPARABLE, GL_TRUE);
        if (compiled) {
            AttachShader(ctx, program, shader);
            LinkProgram(ctx, program);
            DetachShader(ctx, program, shader);
        } else {
            // The program's info log carries the compile log (GL 4.1 section 7.3).
            GLint length = 0;
            GetShaderiv(ctx, shader, GL_INFO_LOG_LENGTH, &length);
            std::string log(static_cast<size_t>(std::max(length, 1)), '\0');
            GetShaderInfoLog(ctx, shader, length, nullptr, log.data());
            ShareLock lock(ctx);
            if (Program* p = ctx.share().program(program)) p->infoLog = log.c_str();
        }
    }
    DeleteShader(ctx, shader);
    return program;
}

}  // namespace wmgl::api
