// A successfully linked program executable: the compiler's interface data
// plus the Metal functions built from its MSL.
#pragma once

#import <Metal/Metal.h>

#include <atomic>
#include <memory>
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
    std::vector<std::pair<Program*, int>> blockOwner, samplerOwner, imageOwner;

    // Metal functions, compiled on first use.
    id<MTLFunction> function(shader::Stage stage);
    // Vertex function returning void, for pipelines with rasterization disabled.
    id<MTLFunction> vertexNoRaster();
    // Fragment function variants: writing GL_SAMPLE_MASK / GL_SAMPLE_COVERAGE's
    // mask (shader::fragmentWithSampleMask), and applying logical operation
    // `logicOp` - 1 (0: none; shader::fragmentWithLogicOp) to attachments of
    // `colorFormats` (MTLPixelFormat). Nil if the stage cannot have it.
    // `cull`: the drawing vertex function's cull distance count, 0 for none
    // (shader::fragmentWithCullDistances).
    // `borders`: the clamp-to-border emulation variant (shader::borderVariantId; 0: none).
    id<MTLFunction> fragmentVariant(bool sampleMask, unsigned logicOp, const uint16_t colorFormats[8],
                                    bool sampleRate = false, bool ignoreSampleMask = false, unsigned cull = 0,
                                    uint16_t borders = 0);
    // The vertex function (with rasterization, with or without [[point_size]],
    // or without rasterization) with clamp-to-border emulation variant `borders`.
    id<MTLFunction> vertexWithBorders(bool rasterization, bool pointSize, uint16_t borders);
    // A kernel (the compute stage, or the geometry or tessellation control
    // emulation's: `output`) with clamp-to-border emulation variant `borders`.
    id<MTLComputePipelineState> kernelWithBorders(const shader::StageOutput& output, int stage, uint16_t borders);
    // The fragment stage writes gl_SampleMask.
    bool writesSampleMask();
    id<MTLFunction> fragmentSampleMask() { return fragmentVariant(true, 0, nullptr); }
    bool functionFailed(shader::Stage stage) const { return failed_[static_cast<int>(stage)]; }
    // Geometry shader emulation (link.geometry): the drawing vertex function
    // and the geometry kernel.
    id<MTLFunction> geometryPassthrough();
    // The drawing vertex function (with a geometry shader, its pass-through)
    // without the [[point_size]] output, which Metal rejects in pipelines
    // with a line or triangle input topology (set for gl_Layer and
    // gl_ViewportIndex writers).
    id<MTLFunction> vertexWithoutPointSize(bool geometryPass);
    id<MTLComputePipelineState> geometryKernel();
    // Compute programs.
    id<MTLComputePipelineState> computePipeline();
    // Tessellation emulation (link.tess): the control kernel, and the
    // executable that runs the evaluation stage (as its vertex stage, with
    // the geometry emulation and transform feedback capture) and this
    // program's fragment stage. Its uniform blocks and samplers are this
    // program's, in the same order, so the draw binds the program's state.
    id<MTLComputePipelineState> tessControlKernel();
    std::shared_ptr<LinkedProgram> tessEvaluation();
    std::shared_ptr<LinkedProgram> pipelineTessEvaluation(const shader::LinkResult& evaluation, std::shared_ptr<LinkedProgram> out);
    // A pipeline whose vertex processing stages span programs: the emulation
    // assembled from each stage's program (shader::linkPipeline), or null.
    std::shared_ptr<const shader::PipelineLinkResult> pipelineLink;
    std::shared_ptr<LinkedProgram> spanningTessEvaluation(std::shared_ptr<LinkedProgram> out);

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

    // Whether the drawing vertex function writes gl_ViewportIndex (with a
    // geometry shader, its pass-through).
    bool writesViewportIndex() const { return (outputFlags() & kWritesViewport) != 0; }
    // ... or gl_Layer: Metal then needs the pipeline's input topology class.
    bool writesLayerOrViewport() const { return (outputFlags() & (kWritesViewport | kWritesLayer)) != 0; }
    // The gl_CullDistance elements it writes (shader::addCullDistances).
    int cullDistances() const { return (outputFlags() >> kCullShift) & 15; }
    const std::string& drawingVertexMsl() const {
        return link.geometry.active ? link.geometry.passthrough.msl : stage(shader::Stage::Vertex).msl;
    }

    bool hasStage(shader::Stage stage) const { return link.stages[static_cast<int>(stage)].present; }
    const shader::StageOutput& stage(shader::Stage stage) const { return link.stages[static_cast<int>(stage)]; }

private:
    std::mutex mutex_;
    std::atomic<int> readsVertexId_{-1};
    // What the drawing vertex and fragment functions write, found once in
    // their MSL (draws ask on every call): kWrites* bits.
    enum : int { kWritesViewport = 1, kWritesLayer = 2, kWritesSampleMask = 4, kCullShift = 4 };
    mutable std::atomic<int> outputFlags_{-1};
    int outputFlags() const;
    id<MTLFunction> functions_[shader::kStageCount] = {};
    bool failed_[shader::kStageCount] = {};
    id<MTLFunction> vertexNoRaster_ = nil;
    bool vertexNoRasterFailed_ = false;
    std::unordered_map<std::string, id<MTLFunction>> fragmentVariants_;  // nil entries: failed
    std::unordered_map<std::string, id<MTLFunction>> borderVertex_;
    std::unordered_map<std::string, id<MTLComputePipelineState>> borderKernels_;
    id<MTLFunction> passthrough_ = nil;
    id<MTLFunction> noPointSize_[2] = {};
    bool noPointSizeFailed_[2] = {};
    id<MTLComputePipelineState> kernel_ = nil;
    bool passthroughFailed_ = false, kernelFailed_ = false;
    id<MTLComputePipelineState> compute_ = nil;
    bool computeFailed_ = false;
    id<MTLComputePipelineState> tessKernel_ = nil;
    bool tessKernelFailed_ = false;
    std::shared_ptr<LinkedProgram> tessEvaluation_;
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
// Subroutine selections (api/subroutine.mm): reset to the defaults (UseProgram,
// program pipeline changes), and written into `program`'s hidden selection
// uniforms before a draw.
void resetSubroutineSelections(Context& ctx);
void applySubroutineSelections(Context& ctx, Program& program);
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
