// A successfully linked program executable: the compiler's interface data
// plus the Metal functions built from its MSL.
#pragma once

#import <Metal/Metal.h>

#include <mutex>

#include "core/shader/compiler.h"

namespace wmgl {

struct LinkedProgram {
    shader::LinkResult link;
    uint64_t uid = 0;  // unique per link (pipeline-cache key)

    // Metal functions, compiled on first use.
    id<MTLFunction> function(shader::Stage stage);
    bool functionFailed(shader::Stage stage) const { return failed_[static_cast<int>(stage)]; }

    bool hasStage(shader::Stage stage) const { return link.stages[static_cast<int>(stage)].present; }
    const shader::StageOutput& stage(shader::Stage stage) const { return link.stages[static_cast<int>(stage)]; }

private:
    std::mutex mutex_;
    id<MTLFunction> functions_[shader::kStageCount] = {};
    bool failed_[shader::kStageCount] = {};
};

uint64_t nextLinkedProgramId();

}  // namespace wmgl
