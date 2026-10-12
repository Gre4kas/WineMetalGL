#include "core/program.h"

#include <algorithm>
#include <atomic>

#include "core/metal/device.h"
#include "core/objects.h"
#include "core/shader/border.h"
#include "core/util/log.h"

namespace wmgl {

uint64_t nextLinkedProgramId() {
    static std::atomic<uint64_t> next{1};
    return next++;
}

namespace {

id<MTLFunction> compileFunction(const std::string& msl, const std::string& entryPoint, int stage) {
    MTLCompileOptions* options = [MTLCompileOptions new];
    options.languageVersion = MTLLanguageVersion3_1;  // texture atomics
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

// A vertex function without the [[point_size]] output. The member stays
// (the code writing it is unchanged) as an unread varying.
std::string withoutPointSize(std::string msl) {
    const std::string from = "[[point_size]]", to = "[[user(wmgl_point_size)]]";
    for (size_t at = msl.find(from); at != std::string::npos; at = msl.find(from, at + to.size())) msl.replace(at, from.size(), to);
    return msl;
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

namespace {

// How GL_COLOR_LOGIC_OP treats an attachment format: unsigned normalized and
// integer formats take the operation; float, signed normalized and missing
// attachments do not.
shader::LogicOpTarget logicOpTarget(MTLPixelFormat format) {
    shader::LogicOpTarget t;
    auto set = [&](char kind, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
        t.kind = kind;
        t.bits[0] = r;
        t.bits[1] = g;
        t.bits[2] = b;
        t.bits[3] = a;
    };
    switch (format) {
    case MTLPixelFormatR8Unorm: case MTLPixelFormatRG8Unorm: case MTLPixelFormatRGBA8Unorm:
    case MTLPixelFormatRGBA8Unorm_sRGB: case MTLPixelFormatBGRA8Unorm: case MTLPixelFormatBGRA8Unorm_sRGB:
        set('n', 8, 8, 8, 8); break;
    case MTLPixelFormatR16Unorm: case MTLPixelFormatRG16Unorm: case MTLPixelFormatRGBA16Unorm: set('n', 16, 16, 16, 16); break;
    case MTLPixelFormatRGB10A2Unorm: case MTLPixelFormatBGR10A2Unorm: set('n', 10, 10, 10, 2); break;
    case MTLPixelFormatB5G6R5Unorm: set('n', 5, 6, 5, 0); break;
    case MTLPixelFormatA1BGR5Unorm: set('n', 5, 5, 5, 1); break;
    case MTLPixelFormatABGR4Unorm: set('n', 4, 4, 4, 4); break;
    case MTLPixelFormatR8Uint: case MTLPixelFormatRG8Uint: case MTLPixelFormatRGBA8Uint: set('u', 8, 8, 8, 8); break;
    case MTLPixelFormatR16Uint: case MTLPixelFormatRG16Uint: case MTLPixelFormatRGBA16Uint: set('u', 16, 16, 16, 16); break;
    case MTLPixelFormatR32Uint: case MTLPixelFormatRG32Uint: case MTLPixelFormatRGBA32Uint: set('u', 32, 32, 32, 32); break;
    case MTLPixelFormatRGB10A2Uint: set('u', 10, 10, 10, 2); break;
    case MTLPixelFormatR8Sint: case MTLPixelFormatRG8Sint: case MTLPixelFormatRGBA8Sint: set('i', 8, 8, 8, 8); break;
    case MTLPixelFormatR16Sint: case MTLPixelFormatRG16Sint: case MTLPixelFormatRGBA16Sint: set('i', 16, 16, 16, 16); break;
    case MTLPixelFormatR32Sint: case MTLPixelFormatRG32Sint: case MTLPixelFormatRGBA32Sint: set('i', 32, 32, 32, 32); break;
    default: break;
    }
    return t;
}

}  // namespace

bool logicOpApplies(MTLPixelFormat format) { return logicOpTarget(format).kind != 0; }

int LinkedProgram::outputFlags() const {
    int flags = outputFlags_.load(std::memory_order_relaxed);
    if (flags >= 0) return flags;
    const std::string& vertex = drawingVertexMsl();
    flags = (vertex.find("[[viewport_array_index]]") != std::string::npos ? kWritesViewport : 0) |
            (vertex.find("[[render_target_array_index]]") != std::string::npos ? kWritesLayer : 0);
    int culls = 0;
    while (culls < 8 && vertex.find("[[user(cull" + std::to_string(culls) + ")]]") != std::string::npos) ++culls;
    flags |= culls << kCullShift;
    if (pipeline) {
        const auto& fs = stageExecutable[static_cast<int>(shader::Stage::Fragment)];
        if (fs && fs->writesSampleMask()) flags |= kWritesSampleMask;
    } else {
        const shader::StageOutput& output = link.stages[static_cast<int>(shader::Stage::Fragment)];
        if (output.present && output.msl.find("[[sample_mask]]") != std::string::npos) flags |= kWritesSampleMask;
    }
    outputFlags_.store(flags, std::memory_order_relaxed);
    return flags;
}

bool LinkedProgram::writesSampleMask() { return (outputFlags() & kWritesSampleMask) != 0; }

id<MTLFunction> LinkedProgram::fragmentVariant(bool sampleMask, unsigned logicOp, const uint16_t colorFormats[8],
                                               bool sampleRate, bool ignoreSampleMask, unsigned cull, uint16_t borders) {
    if (pipeline) {
        const auto& fs = stageExecutable[static_cast<int>(shader::Stage::Fragment)];
        return fs ? fs->fragmentVariant(sampleMask, logicOp, colorFormats, sampleRate, ignoreSampleMask, cull, borders) : nil;
    }
    std::string key = std::string(sampleMask ? "m" : "-") + (sampleRate ? "s" : "-") + (ignoreSampleMask ? "i" : "-") +
                      std::to_string(logicOp) + "c" + std::to_string(cull) + "b" + std::to_string(borders);
    if (logicOp)
        for (int i = 0; i < 8; ++i) key += "/" + std::to_string(colorFormats[i]);
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto it = fragmentVariants_.find(key); it != fragmentVariants_.end()) return it->second;
    const shader::StageOutput& output = link.stages[static_cast<int>(shader::Stage::Fragment)];
    std::string msl = output.present ? output.msl : std::string();
    if (!msl.empty() && borders) msl = shader::mslWithBorders(msl, shader::borderVariantDefinitions(borders));
    if (!msl.empty() && cull) msl = shader::fragmentWithCullDistances(msl, static_cast<int>(cull));
    if (!msl.empty() && logicOp) {
        shader::LogicOpTarget targets[8];
        for (int i = 0; i < 8; ++i) targets[i] = logicOpTarget(static_cast<MTLPixelFormat>(colorFormats[i]));
        msl = shader::fragmentWithLogicOp(msl, logicOp - 1, targets);
    }
    if (!msl.empty() && sampleMask) msl = shader::fragmentWithSampleMask(msl);
    if (!msl.empty() && sampleRate) msl = shader::fragmentAtSampleRate(msl);
    if (!msl.empty() && ignoreSampleMask) msl = shader::fragmentIgnoringSampleMask(msl);
    id<MTLFunction> function =
        msl.empty() ? nil : compileFunction(msl, output.entryPoint, static_cast<int>(shader::Stage::Fragment));
    fragmentVariants_[key] = function;
    return function;
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

id<MTLFunction> LinkedProgram::vertexWithoutPointSize(bool geometryPass) {
    if (pipeline) {
        const auto& vs = stageExecutable[static_cast<int>(shader::Stage::Vertex)];
        return vs ? vs->vertexWithoutPointSize(geometryPass) : nil;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const int v = geometryPass ? 1 : 0;
    if (noPointSize_[v] || noPointSizeFailed_[v]) return noPointSize_[v];
    const shader::StageOutput& output = geometryPass ? link.geometry.passthrough : link.stages[static_cast<int>(shader::Stage::Vertex)];
    if (output.msl.empty()) return nil;
    noPointSize_[v] = compileFunction(withoutPointSize(output.msl), output.entryPoint, 0);
    noPointSizeFailed_[v] = noPointSize_[v] == nil;
    return noPointSize_[v];
}

id<MTLFunction> LinkedProgram::vertexWithBorders(bool rasterization, bool pointSize, uint16_t borders) {
    const int vs = static_cast<int>(shader::Stage::Vertex);
    if (pipeline) return stageExecutable[vs] ? stageExecutable[vs]->vertexWithBorders(rasterization, pointSize, borders) : nil;
    const std::string key = std::string(rasterization ? "r" : "-") + (pointSize ? "p" : "-") + std::to_string(borders);
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto it = borderVertex_.find(key); it != borderVertex_.end()) return it->second;
    const shader::StageOutput& output = link.stages[vs];
    std::string msl = rasterization ? output.msl : output.mslNoRaster;
    if (!msl.empty() && rasterization && !pointSize) msl = withoutPointSize(msl);
    if (!msl.empty()) msl = shader::mslWithBorders(msl, shader::borderVariantDefinitions(borders));
    id<MTLFunction> function = msl.empty() ? nil : compileFunction(msl, output.entryPoint, vs);
    borderVertex_[key] = function;
    return function;
}

namespace {

// `threads`: the work group size a compute program dispatches (0: any); the
// compiler then fits the kernel to it (GL guarantees 1024 invocations).
id<MTLComputePipelineState> computePipelineFor(const shader::StageOutput& output, int stage, NSUInteger threads = 0,
                                               uint16_t borders = 0) {
    if (output.msl.empty()) return nil;
    id<MTLFunction> function =
        compileFunction(borders ? shader::mslWithBorders(output.msl, shader::borderVariantDefinitions(borders)) : output.msl,
                        output.entryPoint, stage);
    if (!function) return nil;
    NSError* error = nil;
    MTLComputePipelineDescriptor* desc = [[MTLComputePipelineDescriptor alloc] init];
    desc.computeFunction = function;
    if (threads) desc.maxTotalThreadsPerThreadgroup = threads;
    id<MTLComputePipelineState> state = [Device::get().mtl() newComputePipelineStateWithDescriptor:desc
                                                                                            options:MTLPipelineOptionNone
                                                                                         reflection:nil
                                                                                              error:&error];
    if (!state) WMGL_ERR("compute pipeline creation failed: %s", error.localizedDescription.UTF8String);
    else if (threads && state.maxTotalThreadsPerThreadgroup < threads)
        WMGL_ERR("compute pipeline fits %lu threads per group, the program needs %lu",
                 static_cast<unsigned long>(state.maxTotalThreadsPerThreadgroup), static_cast<unsigned long>(threads));
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
    const NSUInteger threads = NSUInteger(link.computeLocalSize[0]) * link.computeLocalSize[1] * link.computeLocalSize[2];
    if (link.stages[cs].present) compute_ = computePipelineFor(link.stages[cs], cs, threads);
    computeFailed_ = compute_ == nil;
    return compute_;
}

id<MTLComputePipelineState> LinkedProgram::kernelWithBorders(const shader::StageOutput& output, int stage, uint16_t borders) {
    if (pipeline && stage == static_cast<int>(shader::Stage::Compute)) {
        const auto& cs = stageExecutable[stage];
        return cs ? cs->kernelWithBorders(cs->link.stages[stage], stage, borders) : nil;
    }
    const std::string key = std::to_string(reinterpret_cast<uintptr_t>(&output)) + "/" + std::to_string(borders);
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto it = borderKernels_.find(key); it != borderKernels_.end()) return it->second;
    const NSUInteger threads = stage == static_cast<int>(shader::Stage::Compute) && &output == &link.stages[stage]
                                   ? NSUInteger(link.computeLocalSize[0]) * link.computeLocalSize[1] * link.computeLocalSize[2]
                                   : 0;
    id<MTLComputePipelineState> state = computePipelineFor(output, stage, threads, borders);
    borderKernels_[key] = state;
    return state;
}

id<MTLComputePipelineState> LinkedProgram::tessControlKernel() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (tessKernel_ || tessKernelFailed_) return tessKernel_;
    if (link.tess.active && link.tess.hasControl)
        tessKernel_ = computePipelineFor(link.tess.control, static_cast<int>(shader::Stage::TessControl));
    tessKernelFailed_ = tessKernel_ == nil;
    return tessKernel_;
}

std::shared_ptr<LinkedProgram> LinkedProgram::tessEvaluation() {
    using shader::Stage;
    std::lock_guard<std::mutex> lock(mutex_);
    if (tessEvaluation_ || !link.tess.active || !link.tess.evaluation) return tessEvaluation_;
    const shader::LinkResult& e = *link.tess.evaluation;
    auto out = std::make_shared<LinkedProgram>();
    out->uid = nextLinkedProgramId();
    if (pipeline) {
        tessEvaluation_ = pipelineLink ? spanningTessEvaluation(std::move(out)) : pipelineTessEvaluation(e, std::move(out));
        return tessEvaluation_;
    }
    shader::LinkResult& l = out->link;
    l.ok = true;
    l.stages[static_cast<int>(Stage::Vertex)] = e.stages[static_cast<int>(Stage::Vertex)];
    l.stages[static_cast<int>(Stage::Fragment)] = link.stages[static_cast<int>(Stage::Fragment)];
    l.geometry = e.geometry;
    l.geometryInputType = e.geometryInputType;
    l.geometryOutputType = e.geometryOutputType;
    l.geometryVerticesOut = e.geometryVerticesOut;
    l.transformFeedback = e.transformFeedback;
    std::copy(std::begin(e.xfbStride), std::end(e.xfbStride), std::begin(l.xfbStride));
    l.xfbBufferCount = e.xfbBufferCount;
    l.xfbVertexCapture = e.xfbVertexCapture;
    l.fragOutputs = link.fragOutputs;
    l.uniforms = link.uniforms;
    l.locations = link.locations;
    l.defaultBlockSize = link.defaultBlockSize;
    l.fixedFunctionBlock = link.fixedFunctionBlock;
    // Storage blocks: this program's (its block bindings apply).
    l.storageBlocks = link.storageBlocks;
    shader::remapStorageArgs(l.stages[static_cast<int>(Stage::Vertex)], e, link);
    if (l.geometry.active) shader::remapStorageArgs(l.geometry.kernel, e, link);
    // This program's blocks and samplers, bound in the evaluation executable's
    // vertex and compute (geometry kernel) stages where it uses them.
    const int vs = static_cast<int>(Stage::Vertex), fs = static_cast<int>(Stage::Fragment), cs = static_cast<int>(Stage::Compute);
    l.blocks = link.blocks;
    for (shader::UniformBlockInfo& b : l.blocks) {
        for (int s = 0; s < shader::kStageCount; ++s)
            if (s != fs) b.stageSlot[s] = -1;
        for (const shader::UniformBlockInfo& other : e.blocks)
            if (other.name == b.name) {
                b.stageSlot[vs] = other.stageSlot[vs];
                b.stageSlot[cs] = other.stageSlot[cs];
            }
    }
    l.samplers = link.samplers;
    for (shader::SamplerSlotInfo& slot : l.samplers)
        for (int s = 0; s < shader::kStageCount; ++s)
            if (s != fs) slot.stageTexture[s] = -1;
    for (const shader::UniformInfo& u : link.uniforms) {
        if (u.samplerSlot < 0) continue;
        for (const shader::UniformInfo& other : e.uniforms)
            if (other.name == u.name && other.samplerSlot >= 0)
                for (GLint i = 0; i < std::min(u.size, other.size); ++i) {
                    shader::SamplerSlotInfo& slot = l.samplers[static_cast<size_t>(u.samplerSlot + i)];
                    const shader::SamplerSlotInfo& from = e.samplers[static_cast<size_t>(other.samplerSlot + i)];
                    slot.stageTexture[vs] = from.stageTexture[vs];
                    slot.stageTexture[cs] = from.stageTexture[cs];
                }
    }
    // Images and atomic counter buffers the same way.
    l.images = link.images;
    for (shader::ImageSlotInfo& slot : l.images)
        for (int s = 0; s < shader::kStageCount; ++s)
            if (s != fs) slot.stageTexture[s] = -1;
    for (const shader::UniformInfo& u : link.uniforms) {
        if (u.imageSlot < 0) continue;
        for (const shader::UniformInfo& other : e.uniforms)
            if (other.name == u.name && other.imageSlot >= 0)
                for (GLint i = 0; i < std::min(u.size, other.size); ++i) {
                    shader::ImageSlotInfo& slot = l.images[static_cast<size_t>(u.imageSlot + i)];
                    const shader::ImageSlotInfo& from = e.images[static_cast<size_t>(other.imageSlot + i)];
                    slot.stageTexture[vs] = from.stageTexture[vs];
                    slot.stageTexture[cs] = from.stageTexture[cs];
                }
    }
    l.atomicBuffers = link.atomicBuffers;
    for (shader::AtomicCounterBufferInfo& b : l.atomicBuffers) {
        for (int s = 0; s < shader::kStageCount; ++s)
            if (s != fs) b.stageSlot[s] = -1;
        for (const shader::AtomicCounterBufferInfo& other : e.atomicBuffers)
            if (other.binding == b.binding) {
                b.stageSlot[vs] = other.stageSlot[vs];
                b.stageSlot[cs] = other.stageSlot[cs];
            }
    }
    tessEvaluation_ = out;
    return out;
}

// A program pipeline's evaluation executable: the vertex program's
// evaluation stage (and geometry emulation) with the pipeline's fragment
// stage, each stage's state from its program.
std::shared_ptr<LinkedProgram> LinkedProgram::pipelineTessEvaluation(const shader::LinkResult& e,
                                                                     std::shared_ptr<LinkedProgram> out) {
    using shader::Stage;
    const int vs = static_cast<int>(Stage::Vertex), fs = static_cast<int>(Stage::Fragment), cs = static_cast<int>(Stage::Compute);
    auto inner = std::make_shared<LinkedProgram>();
    inner->uid = nextLinkedProgramId();
    inner->link = e;
    Program* vertexOwner = stageOwner[vs];
    out->pipeline = true;
    out->stageOwner[vs] = vertexOwner;
    out->stageOwner[cs] = vertexOwner;  // the geometry kernel
    out->stageOwner[fs] = stageOwner[fs];
    out->stageExecutable[vs] = inner;
    out->stageExecutable[fs] = stageExecutable[fs];
    shader::LinkResult& l = out->link;
    l.ok = true;
    l.stages[vs] = e.stages[vs];
    l.stages[fs] = link.stages[fs];
    l.geometry = e.geometry;
    l.transformFeedback = e.transformFeedback;
    std::copy(std::begin(e.xfbStride), std::end(e.xfbStride), std::begin(l.xfbStride));
    l.xfbBufferCount = e.xfbBufferCount;
    l.xfbVertexCapture = e.xfbVertexCapture;
    l.fragOutputs = link.fragOutputs;
    l.blocks = link.blocks;
    out->blockOwner = blockOwner;
    for (size_t b = 0; b < l.blocks.size(); ++b) {
        shader::UniformBlockInfo& block = l.blocks[b];
        for (int s = 0; s < shader::kStageCount; ++s)
            if (s != fs) block.stageSlot[s] = -1;
        if (blockOwner[b].first != vertexOwner) continue;
        for (const shader::UniformBlockInfo& other : e.blocks)
            if (other.name == block.name) {
                block.stageSlot[vs] = other.stageSlot[vs];
                block.stageSlot[cs] = other.stageSlot[cs];
            }
    }
    l.samplers = link.samplers;
    out->samplerOwner = samplerOwner;
    const std::vector<shader::UniformInfo>& ownUniforms =
        vertexOwner && vertexOwner->linked ? vertexOwner->linked->link.uniforms : std::vector<shader::UniformInfo>();
    for (size_t i = 0; i < l.samplers.size(); ++i) {
        shader::SamplerSlotInfo& slot = l.samplers[i];
        for (int s = 0; s < shader::kStageCount; ++s)
            if (s != fs) slot.stageTexture[s] = -1;
        if (samplerOwner[i].first != vertexOwner) continue;
        // The vertex program's sampler uniform and element of this slot, by name in the evaluation link.
        const int own = samplerOwner[i].second;
        for (const shader::UniformInfo& u : ownUniforms) {
            if (u.samplerSlot < 0 || own < u.samplerSlot || own >= u.samplerSlot + u.size) continue;
            for (const shader::UniformInfo& other : e.uniforms)
                if (other.name == u.name && other.samplerSlot >= 0 && own - u.samplerSlot < other.size) {
                    const shader::SamplerSlotInfo& from = e.samplers[static_cast<size_t>(other.samplerSlot + own - u.samplerSlot)];
                    slot.stageTexture[vs] = from.stageTexture[vs];
                    slot.stageTexture[cs] = from.stageTexture[cs];
                }
        }
    }
    l.images = link.images;
    out->imageOwner = imageOwner;
    for (size_t i = 0; i < l.images.size(); ++i) {
        shader::ImageSlotInfo& slot = l.images[i];
        for (int s = 0; s < shader::kStageCount; ++s)
            if (s != fs) slot.stageTexture[s] = -1;
        if (imageOwner[i].first != vertexOwner) continue;
        const int own = imageOwner[i].second;
        for (const shader::UniformInfo& u : ownUniforms) {
            if (u.imageSlot < 0 || own < u.imageSlot || own >= u.imageSlot + u.size) continue;
            for (const shader::UniformInfo& other : e.uniforms)
                if (other.name == u.name && other.imageSlot >= 0 && own - u.imageSlot < other.size) {
                    const shader::ImageSlotInfo& from = e.images[static_cast<size_t>(other.imageSlot + own - u.imageSlot)];
                    slot.stageTexture[vs] = from.stageTexture[vs];
                    slot.stageTexture[cs] = from.stageTexture[cs];
                }
        }
    }
    l.atomicBuffers = link.atomicBuffers;
    for (shader::AtomicCounterBufferInfo& b : l.atomicBuffers) {
        for (int s = 0; s < shader::kStageCount; ++s)
            if (s != fs) b.stageSlot[s] = -1;
        for (const shader::AtomicCounterBufferInfo& other : e.atomicBuffers)
            if (other.binding == b.binding) {
                b.stageSlot[vs] = other.stageSlot[vs];
                b.stageSlot[cs] = other.stageSlot[cs];
            }
    }
    return out;
}

namespace {

// Appends `owner`'s blocks, samplers, images and atomic counter buffers to
// `out`, with only stage `keep`'s slots (-1: none) of the owner's own link
// plus the slots of the pipeline pieces it owns.
void appendOwnerResources(LinkedProgram& out, Program* owner, int keep, const std::vector<const shader::PipelinePiece*>& pieces) {
    const shader::LinkResult& l = owner->linked->link;
    shader::LinkResult own;
    own.blocks = l.blocks;
    own.uniforms = l.uniforms;
    own.samplers = l.samplers;
    own.images = l.images;
    own.atomicBuffers = l.atomicBuffers;
    for (int s = 0; s < shader::kStageCount; ++s) {
        if (s == keep) continue;
        for (shader::UniformBlockInfo& b : own.blocks) b.stageSlot[s] = -1;
        for (shader::SamplerSlotInfo& slot : own.samplers) slot.stageTexture[s] = -1;
        for (shader::ImageSlotInfo& slot : own.images) slot.stageTexture[s] = -1;
        for (shader::AtomicCounterBufferInfo& b : own.atomicBuffers) b.stageSlot[s] = -1;
    }
    for (const shader::PipelinePiece* piece : pieces) shader::copyStageSlots(*piece->slots, piece->from, own, piece->to);
    for (size_t b = 0; b < own.blocks.size(); ++b) {
        out.link.blocks.push_back(own.blocks[b]);
        out.blockOwner.emplace_back(owner, static_cast<int>(b));
    }
    for (size_t i = 0; i < own.samplers.size(); ++i) {
        out.link.samplers.push_back(own.samplers[i]);
        out.samplerOwner.emplace_back(owner, static_cast<int>(i));
    }
    for (size_t i = 0; i < own.images.size(); ++i) {
        out.link.images.push_back(own.images[i]);
        out.imageOwner.emplace_back(owner, static_cast<int>(i));
    }
    for (const shader::AtomicCounterBufferInfo& b : own.atomicBuffers) out.link.atomicBuffers.push_back(b);
}

// The distinct programs of `stages` (in stage order).
std::vector<Program*> distinctOwners(Program* const stages[shader::kStageCount]) {
    std::vector<Program*> owners;
    for (int s = 0; s < shader::kStageCount; ++s)
        if (stages[s] && std::find(owners.begin(), owners.end(), stages[s]) == owners.end()) owners.push_back(stages[s]);
    return owners;
}

}  // namespace

// The evaluation executable of a pipeline whose stages span programs: the
// evaluation stage (and the geometry emulation after it) assembled by
// shader::linkPipeline, with the pipeline's fragment stage; every stage's
// state comes from its own program.
std::shared_ptr<LinkedProgram> LinkedProgram::spanningTessEvaluation(std::shared_ptr<LinkedProgram> out) {
    using shader::Stage;
    const int vs = static_cast<int>(Stage::Vertex), fs = static_cast<int>(Stage::Fragment);
    const shader::LinkResult& e = *link.tess.evaluation;
    auto inner = std::make_shared<LinkedProgram>();
    inner->uid = nextLinkedProgramId();
    inner->link = e;
    out->pipeline = true;
    std::vector<const shader::PipelinePiece*> pieces;
    for (const shader::PipelinePiece& piece : pipelineLink->pieces)
        if (piece.evaluation) {
            out->stageOwner[piece.to] = stageOwner[piece.owner];
            pieces.push_back(&piece);
        }
    out->stageOwner[fs] = stageOwner[fs];
    out->stageExecutable[vs] = inner;
    out->stageExecutable[fs] = stageExecutable[fs];
    shader::LinkResult& l = out->link;
    l.ok = true;
    l.stages[vs] = e.stages[vs];
    l.stages[fs] = link.stages[fs];
    l.geometry = e.geometry;
    l.transformFeedback = e.transformFeedback;
    std::copy(std::begin(e.xfbStride), std::end(e.xfbStride), std::begin(l.xfbStride));
    l.xfbBufferCount = e.xfbBufferCount;
    l.xfbVertexCapture = e.xfbVertexCapture;
    l.fragOutputs = link.fragOutputs;
    for (Program* owner : distinctOwners(out->stageOwner)) {
        std::vector<const shader::PipelinePiece*> owned;
        for (const shader::PipelinePiece* piece : pieces)
            if (stageOwner[piece->owner] == owner) owned.push_back(piece);
        appendOwnerResources(*out, owner, out->stageOwner[fs] == owner ? fs : -1, owned);
    }
    return out;
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
        // Tessellation is emulated with the vertex program (api/pipeline.mm validation).
        if (out->stageOwner[static_cast<int>(Stage::TessEval)] == v) out->link.tess = l.tess;
        // Geometry shaders are emulated with the vertex program (api/pipeline.mm validation).
        if (out->stageOwner[gs] == v) {
            out->link.geometry = l.geometry;
            out->link.geometryInputType = l.geometryInputType;
            out->link.geometryOutputType = l.geometryOutputType;
            out->link.geometryVerticesOut = l.geometryVerticesOut;
        }
    }
    // Vertex processing stages from several programs: their emulations are
    // assembled from each stage's program.
    const int tcs = static_cast<int>(Stage::TessControl), tes = static_cast<int>(Stage::TessEval);
    if (Program* v = out->stageOwner[vs];
        v && ((out->stageOwner[tcs] && out->stageOwner[tcs] != v) || (out->stageOwner[tes] && out->stageOwner[tes] != v) ||
              (out->stageOwner[gs] && out->stageOwner[gs] != v))) {
        shader::PipelineStageInput stages[kStages];
        for (int s : {vs, tcs, tes, gs})
            if (Program* p = out->stageOwner[s]) stages[s] = {&p->linked->input, &p->linked->link};
        auto assembled = std::make_shared<shader::PipelineLinkResult>(shader::linkPipeline(stages));
        out->pipelineLink = assembled;
        if (!assembled->ok) {
            WMGL_WARN("program pipeline stages cannot be assembled:\n%s", assembled->log.c_str());
        } else {
            const shader::LinkResult& a = assembled->link;
            auto inner = std::make_shared<LinkedProgram>();
            inner->uid = nextLinkedProgramId();
            inner->link = a;
            inner->link.ok = true;
            out->stageExecutable[vs] = inner;
            out->link.stages[vs] = a.stages[vs];
            out->link.geometry = a.geometry;
            out->link.geometryInputType = a.geometryInputType;
            out->link.geometryOutputType = a.geometryOutputType;
            out->link.geometryVerticesOut = a.geometryVerticesOut;
            out->link.tess = a.tess;
            out->link.transformFeedback = a.transformFeedback;
            std::copy(std::begin(a.xfbStride), std::end(a.xfbStride), std::begin(out->link.xfbStride));
            out->link.xfbBufferCount = a.xfbBufferCount;
            out->link.xfbVertexCapture = false;
            for (const shader::PipelinePiece& piece : assembled->pieces)
                if (!piece.evaluation) out->stageOwner[piece.to] = out->stageOwner[piece.owner];
            if (Program* f = out->stageOwner[fs]) out->link.fragOutputs = f->linked->link.fragOutputs;
            for (Program* owner : distinctOwners(out->stageOwner)) {
                std::vector<const shader::PipelinePiece*> owned;
                for (const shader::PipelinePiece& piece : assembled->pieces)
                    if (!piece.evaluation && out->stageOwner[piece.owner] == owner) owned.push_back(&piece);
                appendOwnerResources(*out, owner, out->stageOwner[fs] == owner ? fs : -1, owned);
            }
            pp.executable = out;
            return out;
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
        for (size_t i = 0; i < l.images.size(); ++i) {
            shader::ImageSlotInfo slot = l.images[i];
            for (int t = 0; t < kStages; ++t)
                if (!owns(p, t)) slot.stageTexture[t] = -1;
            out->link.images.push_back(slot);
            out->imageOwner.emplace_back(p, static_cast<int>(i));
        }
        // Atomic counter buffers read the context's binding points: no owner.
        for (shader::AtomicCounterBufferInfo buffer : l.atomicBuffers) {
            for (int t = 0; t < kStages; ++t)
                if (!owns(p, t)) buffer.stageSlot[t] = -1;
            out->link.atomicBuffers.push_back(buffer);
        }
    }
    pp.executable = out;
    return out;
}

}  // namespace wmgl
