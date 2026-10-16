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

// OpenGL ES 3.2 section 7.4.1: each stage's inputs match the outputs of the
// stage before it (by location, else by name; type and size). Arrayed
// inputs (tessellation and geometry stages) compare their element type only.
std::string esInterfaceMismatch(const ProgramPipeline& pp) {
    const Stage order[] = {Stage::Vertex, Stage::TessControl, Stage::TessEval, Stage::Geometry, Stage::Fragment};
    const Program* producer = nullptr;
    Stage producerStage = Stage::Vertex;
    for (Stage s : order) {
        const Program* p = pp.stages[static_cast<int>(s)].get();
        if (!p || !p->linked) continue;
        if (producer && producer != p) {
            const bool arrayed = s == Stage::TessControl || s == Stage::TessEval || s == Stage::Geometry;
            for (const shader::InterfaceVariableInfo& in : p->linked->link.programInputs) {
                if (in.name.rfind("gl_", 0) == 0 || in.perPatch) continue;
                // Per-vertex inputs of arrayed stages are listed as "name[0]".
                std::string name = in.name;
                if (arrayed && name.size() > 3 && name.compare(name.size() - 3, 3, "[0]") == 0) name.resize(name.size() - 3);
                const shader::InterfaceVariableInfo* match = nullptr;
                for (const shader::InterfaceVariableInfo& out : producer->linked->link.programOutputs) {
                    if (out.name.rfind("gl_", 0) == 0) continue;
                    if (in.location >= 0 && out.location >= 0 ? in.location == out.location
                                                              : in.name == out.name || name == out.name) {
                        match = &out;
                        break;
                    }
                }
                if (!match || match->type != in.type || (!arrayed && producerStage != Stage::TessControl && match->size != in.size))
                    return "error: input '" + in.name + "' does not match an output of the previous stage\n";

            }
        }
        // OpenGL ES 3.2 section 7.4.1: the stages' interfaces match exactly,
        // every output an input and every input an output, with the same
        // types (struct and block names and members in order), precisions,
        // interpolation, locations and array sizes.
        if (producer && producer != p) {
            const auto& outs = producer->linked->input.esOutputInterface;
            const auto& ins = p->linked->input.esInputInterface;
            for (const auto& [key, signature] : ins) {
                const auto o = outs.find(key);
                if (o == outs.end()) return "error: input '" + key + "' has no matching output in the previous stage\n";
                if (o->second != signature)
                    return "error: input '" + key + "' does not match its output in the previous stage (" + signature +
                           " / " + o->second + ")\n";
            }
            for (const auto& [key, signature] : outs)
                if (!ins.count(key)) return "error: output '" + key + "' has no matching input in the next stage\n";
        }
        producer = p;
        producerStage = s;
    }
    return std::string();
}

// GL 4.1 section 11.1.3.11 checks for drawing with the pipeline.
bool validate(ProgramPipeline& pp, std::string& log, bool es) {
    log.clear();
    bool any = false;
    for (const StageBit& s : kStageBits) any |= pp.stages[static_cast<int>(s.stage)] != nullptr;
    if (!any) {
        log += "error: no program is bound to the pipeline\n";
        return false;
    }
    for (const StageBit& s : kStageBits) {
        const Program* p = pp.stages[static_cast<int>(s.stage)].get();
        if (!p) continue;
        // A failed relink keeps the program's previous executable active.
        if (!p->linked) log += "error: a stage program is not linked\n";
        else if (!p->separable) log += "error: program " + std::to_string(p->name) + " is not separable\n";
    }
    // A program must be active for every stage it was linked with (GL 4.1
    // section 11.1.3.11), compute aside.
    for (const StageBit& s : kStageBits) {
        const Program* p = pp.stages[static_cast<int>(s.stage)].get();
        if (!p || !p->linked || s.stage == Stage::Compute) continue;
        for (const StageBit& other : kStageBits)
            if (other.stage != Stage::Compute && hasStage(*p, other.stage) && pp.stages[static_cast<int>(other.stage)].get() != p) {
                log += "error: program " + std::to_string(p->name) + " is active for only some of its stages\n";
                break;
            }
        if (!log.empty()) break;
    }
    if (log.empty() && es) log += esInterfaceMismatch(pp);
    // Stages from several programs are assembled when the pipeline draws
    // (shader::linkPipeline); report what cannot be.
    if (log.empty()) {
        const Program* vs = pp.stages[static_cast<int>(Stage::Vertex)].get();
        bool spans = false;
        for (Stage s : {Stage::TessControl, Stage::TessEval, Stage::Geometry})
            if (const Program* p = pp.stages[static_cast<int>(s)].get(); p && p != vs) spans = true;
        // Tessellation or geometry stages need a vertex stage (GL 4.1 section 11.1.3.11).
        if (spans && !vs) log += "error: tessellation and geometry stages need a vertex shader\n";
        else if (spans) {
            if (std::shared_ptr<LinkedProgram> e = pipelineExecutable(pp); !e->pipelineLink || !e->pipelineLink->ok)
                log += e->pipelineLink ? e->pipelineLink->log : std::string("error: the pipeline stages cannot be combined\n");
        }
    }
    return log.empty();
}

}  // namespace

bool validateProgramPipeline(ProgramPipeline& pp, bool es) {
    pp.validateStatus = validate(pp, pp.infoLog, es);
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
    resetSubroutineSelections(ctx);
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
    if (ProgramPipeline* pp = pipelineOrError(ctx, pipeline)) validateProgramPipeline(*pp, ctx.isEs());
}

void GetProgramPipelineInfoLog(Context& ctx, GLuint pipeline, GLsizei bufSize, GLsizei* length, GLchar* infoLog) {
    if (bufSize < 0) return ctx.setError(GL_INVALID_VALUE);
    // Not a pipeline name: INVALID_VALUE here (GL 4.1 section 6.1.12).
    if (!ctx.programPipelines.isGenerated(pipeline)) return ctx.setError(GL_INVALID_VALUE);
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
