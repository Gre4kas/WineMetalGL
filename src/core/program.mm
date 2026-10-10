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

}  // namespace wmgl
