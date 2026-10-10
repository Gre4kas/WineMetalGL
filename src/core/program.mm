#include "core/program.h"

#include <atomic>

#include "core/metal/device.h"
#include "core/util/log.h"

namespace wmgl {

uint64_t nextLinkedProgramId() {
    static std::atomic<uint64_t> next{1};
    return next++;
}

id<MTLFunction> LinkedProgram::function(shader::Stage stage) {
    const int index = static_cast<int>(stage);
    std::lock_guard<std::mutex> lock(mutex_);
    if (functions_[index] || failed_[index]) return functions_[index];
    const shader::StageOutput& output = link.stages[index];
    if (!output.present) return nil;
    MTLCompileOptions* options = [MTLCompileOptions new];
    options.languageVersion = MTLLanguageVersion3_0;
    options.mathMode = MTLMathModeSafe;  // GL requires IEEE behaviour for inf/nan propagation
    NSError* error = nil;
    id<MTLLibrary> library = [Device::get().mtl() newLibraryWithSource:[NSString stringWithUTF8String:output.msl.c_str()]
                                                               options:options
                                                                 error:&error];
    if (!library) {
        WMGL_ERR("MSL compile failed for stage %d: %s\n%s", index, error.localizedDescription.UTF8String,
                 output.msl.c_str());
        failed_[index] = true;
        return nil;
    }
    functions_[index] = [library newFunctionWithName:[NSString stringWithUTF8String:output.entryPoint.c_str()]];
    if (!functions_[index]) {
        WMGL_ERR("entry point %s missing", output.entryPoint.c_str());
        failed_[index] = true;
    }
    return functions_[index];
}

}  // namespace wmgl
