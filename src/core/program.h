// A successfully linked program executable: the compiler's interface data
// plus the Metal functions built from its MSL.
#pragma once

#import <Metal/Metal.h>

#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>

#include "core/shader/compiler.h"

namespace wmgl {

class Program;

struct LinkedProgram {
    shader::LinkResult link;
    uint64_t uid = 0;  // unique per link (pipeline-cache key)
    // What was linked (shader sources and link-time bindings): the contents
    // of the program's binary (glGetProgramBinary), which relinks from it.
    shader::LinkInput input;

    // Program pipelines (GL_ARB_separate_shader_objects): an executable
    // assembled from stage programs. Each stage's functions and state
    // (uniform values, block bindings, sampler units) come from its owner;
    // link.blocks / link.samplers entry i belongs to blockOwner[i] /
    // samplerOwner[i] (owner program, index in the owner's own lists).
    bool pipeline = false;
    Program* stageOwner[shader::kStageCount] = {};
    std::shared_ptr<LinkedProgram> stageExecutable[shader::kStageCount];
    std::vector<std::pair<Program*, int>> blockOwner, samplerOwner;

    // Metal functions, compiled on first use.
    id<MTLFunction> function(shader::Stage stage);
    // Vertex function returning void, for pipelines with rasterization disabled.
    id<MTLFunction> vertexNoRaster();
    // Fragment function variants: writing GL_SAMPLE_MASK / GL_SAMPLE_COVERAGE's
    // mask (shader::fragmentWithSampleMask), and applying logical operation
    // `logicOp` - 1 (0: none; shader::fragmentWithLogicOp) to attachments of
    // `colorFormats` (MTLPixelFormat). Nil if the stage cannot have it.
    id<MTLFunction> fragmentVariant(bool sampleMask, unsigned logicOp, const uint16_t colorFormats[8]);
    id<MTLFunction> fragmentSampleMask() { return fragmentVariant(true, 0, nullptr); }
    bool functionFailed(shader::Stage stage) const { return failed_[static_cast<int>(stage)]; }
    // Geometry shader emulation (link.geometry): the drawing vertex function
    // and the geometry kernel.
    id<MTLFunction> geometryPassthrough();
    id<MTLComputePipelineState> geometryKernel();
    // Compute programs.
    id<MTLComputePipelineState> computePipeline();

    // Whether the vertex function reads gl_VertexID (draws then keep GL's
    // vertex numbering; see DrawState::issue).
    bool readsVertexId() {
        int value = readsVertexId_.load(std::memory_order_relaxed);
        if (value < 0) {
            value = stage(shader::Stage::Vertex).msl.find("[[vertex_id]]") != std::string::npos ? 1 : 0;
            readsVertexId_.store(value, std::memory_order_relaxed);
        }
        return value != 0;
    }

    bool hasStage(shader::Stage stage) const { return link.stages[static_cast<int>(stage)].present; }
    const shader::StageOutput& stage(shader::Stage stage) const { return link.stages[static_cast<int>(stage)]; }

private:
    std::mutex mutex_;
    std::atomic<int> readsVertexId_{-1};
    id<MTLFunction> functions_[shader::kStageCount] = {};
    bool failed_[shader::kStageCount] = {};
    id<MTLFunction> vertexNoRaster_ = nil;
    bool vertexNoRasterFailed_ = false;
    std::unordered_map<std::string, id<MTLFunction>> fragmentVariants_;  // nil entries: failed
    id<MTLFunction> passthrough_ = nil;
    id<MTLComputePipelineState> kernel_ = nil;
    bool passthroughFailed_ = false, kernelFailed_ = false;
    id<MTLComputePipelineState> compute_ = nil;
    bool computeFailed_ = false;
};

uint64_t nextLinkedProgramId();
// Whether GL_COLOR_LOGIC_OP applies to an attachment of this format
// (unsigned normalized and integer formats).
bool logicOpApplies(MTLPixelFormat format);

class ProgramPipeline;
// The executable of a program pipeline's current stage programs (rebuilt
// when they change or relink).
std::shared_ptr<LinkedProgram> pipelineExecutable(ProgramPipeline& pipeline);

class Context;
namespace api {
// Updates the pipeline's VALIDATE_STATUS and info log; true if it can draw.
bool validateProgramPipeline(ProgramPipeline& pipeline);
// Drops a destroyed context's use of its current program (which may then be
// deleted, if deletion was pending).
void releaseCurrentProgram(Context& ctx);
// Deletes a program flagged by glDeleteProgram once nothing uses it (current
// program or program pipeline stage).
void maybeDeleteProgram(Context& ctx, Program* program);
// GL_ARB_get_program_binary: the one binary format ('WMG1'; see api/program.mm).
constexpr GLenum kProgramBinaryFormat = 0x574D4731;
GLsizei programBinaryLength(const Program& p);
// Components glGetUniform* writes for `location` of `program`, or -1 if the
// query is invalid (the GL error is left to glGetUniform*).
int uniformComponents(Context& ctx, GLuint program, GLint location);
}  // namespace api

}  // namespace wmgl
