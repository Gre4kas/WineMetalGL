#include "core/program.h"

#include <algorithm>
#include <atomic>

#include "core/metal/device.h"
#include "core/objects.h"
#include "core/util/log.h"

namespace wmgl {

uint64_t nextLinkedProgramId() {
    static std::atomic<uint64_t> next{1};
    return next++;
}

namespace {

id<MTLFunction> compileFunction(const std::string& msl, const std::string& entryPoint, int stage) {
    MTLCompileOptions* options = [MTLCompileOptions new];
    options.languageVersion = MTLLanguageVersion3_0;
    options.mathMode = MTLMathModeSafe;  // GL requires IEEE behaviour for inf/nan propagation
    NSError* error = nil;
    id<MTLLibrary> library = [Device::get().mtl() newLibraryWithSource:[NSString stringWithUTF8String:msl.c_str()]
                                                               options:options
                                                                 error:&error];
    if (!library) {
        WMGL_ERR("MSL compile failed for stage %d: %s\n%s", stage, error.localizedDescription.UTF8String, msl.c_str());
        return nil;
    }
    id<MTLFunction> function = [library newFunctionWithName:[NSString stringWithUTF8String:entryPoint.c_str()]];
    if (!function) WMGL_ERR("entry point %s missing", entryPoint.c_str());
    return function;
}

}  // namespace

id<MTLFunction> LinkedProgram::function(shader::Stage stage) {
    const int index = static_cast<int>(stage);
    if (pipeline) return stageExecutable[index] ? stageExecutable[index]->function(stage) : nil;
    std::lock_guard<std::mutex> lock(mutex_);
    if (functions_[index] || failed_[index]) return functions_[index];
    const shader::StageOutput& output = link.stages[index];
    if (!output.present) return nil;
    functions_[index] = compileFunction(output.msl, output.entryPoint, index);
    failed_[index] = functions_[index] == nil;
    return functions_[index];
}

id<MTLFunction> LinkedProgram::vertexNoRaster() {
    if (pipeline) {
        const auto& vs = stageExecutable[static_cast<int>(shader::Stage::Vertex)];
        return vs ? vs->vertexNoRaster() : nil;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (vertexNoRaster_ || vertexNoRasterFailed_) return vertexNoRaster_;
    const shader::StageOutput& output = link.stages[static_cast<int>(shader::Stage::Vertex)];
    if (!output.present || output.mslNoRaster.empty()) return nil;
    vertexNoRaster_ = compileFunction(output.mslNoRaster, output.entryPoint, 0);
    vertexNoRasterFailed_ = vertexNoRaster_ == nil;
    return vertexNoRaster_;
}

id<MTLFunction> LinkedProgram::geometryPassthrough() {
    if (pipeline) {
        const auto& vs = stageExecutable[static_cast<int>(shader::Stage::Vertex)];
        return vs ? vs->geometryPassthrough() : nil;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (passthrough_ || passthroughFailed_) return passthrough_;
    const shader::StageOutput& output = link.geometry.passthrough;
    if (!link.geometry.active || output.msl.empty()) return nil;
    passthrough_ = compileFunction(output.msl, output.entryPoint, static_cast<int>(shader::Stage::Geometry));
    passthroughFailed_ = passthrough_ == nil;
    return passthrough_;
}

namespace {

id<MTLComputePipelineState> computePipelineFor(const shader::StageOutput& output, int stage) {
    if (output.msl.empty()) return nil;
    id<MTLFunction> function = compileFunction(output.msl, output.entryPoint, stage);
    if (!function) return nil;
    NSError* error = nil;
    id<MTLComputePipelineState> state = [Device::get().mtl() newComputePipelineStateWithFunction:function error:&error];
    if (!state) WMGL_ERR("compute pipeline creation failed: %s", error.localizedDescription.UTF8String);
    return state;
}

}  // namespace

id<MTLComputePipelineState> LinkedProgram::geometryKernel() {
    if (pipeline) {
        const auto& vs = stageExecutable[static_cast<int>(shader::Stage::Vertex)];
        return vs ? vs->geometryKernel() : nil;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (kernel_ || kernelFailed_) return kernel_;
    if (link.geometry.active) kernel_ = computePipelineFor(link.geometry.kernel, static_cast<int>(shader::Stage::Geometry));
    kernelFailed_ = kernel_ == nil;
    return kernel_;
}

id<MTLComputePipelineState> LinkedProgram::computePipeline() {
    if (pipeline) {
        const auto& cs = stageExecutable[static_cast<int>(shader::Stage::Compute)];
        return cs ? cs->computePipeline() : nil;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (compute_ || computeFailed_) return compute_;
    const int cs = static_cast<int>(shader::Stage::Compute);
    if (link.stages[cs].present) compute_ = computePipelineFor(link.stages[cs], cs);
    computeFailed_ = compute_ == nil;
    return compute_;
}

std::shared_ptr<LinkedProgram> pipelineExecutable(ProgramPipeline& pp) {
    using shader::Stage;
    constexpr int kStages = shader::kStageCount;
    bool current = pp.executable != nullptr;
    for (int s = 0; s < kStages; ++s) {
        const Program* p = pp.stages[s].get();
        if (pp.builtFrom[s] != (p ? p->linked : nullptr)) current = false;
    }
    if (current) return pp.executable;

    auto out = std::make_shared<LinkedProgram>();
    out->pipeline = true;
    out->uid = nextLinkedProgramId();
    std::vector<Program*> owners;
    for (int s = 0; s < kStages; ++s) {
        Program* p = pp.stages[s].get();
        pp.builtFrom[s] = p ? p->linked : nullptr;
        if (!p || !p->linked || !p->linked->link.stages[s].present) continue;
        out->link.stages[s] = p->linked->link.stages[s];
        out->stageOwner[s] = p;
        out->stageExecutable[s] = p->linked;
        if (std::find(owners.begin(), owners.end(), p) == owners.end()) owners.push_back(p);
    }
    const int vs = static_cast<int>(Stage::Vertex), fs = static_cast<int>(Stage::Fragment);
    const int gs = static_cast<int>(Stage::Geometry), cs = static_cast<int>(Stage::Compute);
    if (Program* v = out->stageOwner[vs]) {
        const shader::LinkResult& l = v->linked->link;
        out->link.attribs = l.attribs;
        out->link.transformFeedback = l.transformFeedback;
        std::copy(std::begin(l.xfbStride), std::end(l.xfbStride), std::begin(out->link.xfbStride));
        out->link.xfbBufferCount = l.xfbBufferCount;
        out->link.xfbVertexCapture = l.xfbVertexCapture;
        // Geometry shaders are emulated with the vertex program (api/pipeline.mm validation).
        if (out->stageOwner[gs] == v) {
            out->link.geometry = l.geometry;
            out->link.geometryInputType = l.geometryInputType;
            out->link.geometryOutputType = l.geometryOutputType;
            out->link.geometryVerticesOut = l.geometryVerticesOut;
        }
    }
    if (Program* f = out->stageOwner[fs]) out->link.fragOutputs = f->linked->link.fragOutputs;
    if (Program* c = out->stageOwner[cs]) std::copy(std::begin(c->linked->link.computeLocalSize),
                                                    std::end(c->linked->link.computeLocalSize),
                                                    std::begin(out->link.computeLocalSize));
    // The Stage::Compute slots belong to the compute program, or to the
    // geometry kernel of the vertex program.
    Program* computeOwner = out->stageOwner[cs] ? out->stageOwner[cs] : out->link.geometry.active ? out->stageOwner[vs] : nullptr;
    auto owns = [&](const Program* p, int stage) {
        return stage == cs ? p == computeOwner : out->stageOwner[stage] == p;
    };
    for (Program* p : owners) {
        const shader::LinkResult& l = p->linked->link;
        for (size_t b = 0; b < l.blocks.size(); ++b) {
            shader::UniformBlockInfo block = l.blocks[b];
            for (int t = 0; t < kStages; ++t)
                if (!owns(p, t)) block.stageSlot[t] = -1;
            out->link.blocks.push_back(block);
            out->blockOwner.emplace_back(p, static_cast<int>(b));
        }
        for (size_t i = 0; i < l.samplers.size(); ++i) {
            shader::SamplerSlotInfo slot = l.samplers[i];
            for (int t = 0; t < kStages; ++t)
                if (!owns(p, t)) slot.stageTexture[t] = -1;
            out->link.samplers.push_back(slot);
            out->samplerOwner.emplace_back(p, static_cast<int>(i));
        }
    }
    pp.executable = out;
    return out;
}

}  // namespace wmgl
