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
    // Vertex function returning void, for pipelines with rasterization disabled.
    id<MTLFunction> vertexNoRaster();
    bool functionFailed(shader::Stage stage) const { return failed_[static_cast<int>(stage)]; }
    // Geometry shader emulation (link.geometry): the drawing vertex function
    // and the geometry kernel.
    id<MTLFunction> geometryPassthrough();
    id<MTLComputePipelineState> geometryKernel();
    // Compute programs.
    id<MTLComputePipelineState> computePipeline();

    bool hasStage(shader::Stage stage) const { return link.stages[static_cast<int>(stage)].present; }
    const shader::StageOutput& stage(shader::Stage stage) const { return link.stages[static_cast<int>(stage)]; }

private:
    std::mutex mutex_;
    id<MTLFunction> functions_[shader::kStageCount] = {};
    bool failed_[shader::kStageCount] = {};
    id<MTLFunction> vertexNoRaster_ = nil;
    bool vertexNoRasterFailed_ = false;
    id<MTLFunction> passthrough_ = nil;
    id<MTLComputePipelineState> kernel_ = nil;
    bool passthroughFailed_ = false, kernelFailed_ = false;
    id<MTLComputePipelineState> compute_ = nil;
    bool computeFailed_ = false;
};

uint64_t nextLinkedProgramId();

class Context;
namespace api {
// Drops a destroyed context's use of its current program (which may then be
// deleted, if deletion was pending).
void releaseCurrentProgram(Context& ctx);
}  // namespace api

}  // namespace wmgl
