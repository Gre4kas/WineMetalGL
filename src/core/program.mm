#include "core/program.h"

#include <atomic>

#include "core/metal/device.h"
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
    std::lock_guard<std::mutex> lock(mutex_);
    if (functions_[index] || failed_[index]) return functions_[index];
    const shader::StageOutput& output = link.stages[index];
    if (!output.present) return nil;
    functions_[index] = compileFunction(output.msl, output.entryPoint, index);
    failed_[index] = functions_[index] == nil;
    return functions_[index];
}

id<MTLFunction> LinkedProgram::vertexNoRaster() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (vertexNoRaster_ || vertexNoRasterFailed_) return vertexNoRaster_;
    const shader::StageOutput& output = link.stages[static_cast<int>(shader::Stage::Vertex)];
    if (!output.present || output.mslNoRaster.empty()) return nil;
    vertexNoRaster_ = compileFunction(output.mslNoRaster, output.entryPoint, 0);
    vertexNoRasterFailed_ = vertexNoRaster_ == nil;
    return vertexNoRaster_;
}

id<MTLFunction> LinkedProgram::geometryPassthrough() {
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
    std::lock_guard<std::mutex> lock(mutex_);
    if (kernel_ || kernelFailed_) return kernel_;
    if (link.geometry.active) kernel_ = computePipelineFor(link.geometry.kernel, static_cast<int>(shader::Stage::Geometry));
    kernelFailed_ = kernel_ == nil;
    return kernel_;
}

id<MTLComputePipelineState> LinkedProgram::computePipeline() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (compute_ || computeFailed_) return compute_;
    const int cs = static_cast<int>(shader::Stage::Compute);
    if (link.stages[cs].present) compute_ = computePipelineFor(link.stages[cs], cs);
    computeFailed_ = compute_ == nil;
    return compute_;
}

}  // namespace wmgl
