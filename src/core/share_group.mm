#include "core/share_group.h"

#include "core/metal/pipeline_cache.h"

namespace wmgl {

ShareGroup::ShareGroup() : pipelines_(new PipelineCache()) {}

ShareGroup::~ShareGroup() {
    queue.waitIdle();
    delete pipelines_;
}

GLuint ShareGroup::createShaderProgramName() {
    while (shaders.count(nextShaderProgram_) || programs.count(nextShaderProgram_) || nextShaderProgram_ == 0)
        ++nextShaderProgram_;
    return nextShaderProgram_++;
}

Shader* ShareGroup::shader(GLuint name) const {
    auto it = shaders.find(name);
    return it == shaders.end() ? nullptr : it->second.get();
}

Program* ShareGroup::program(GLuint name) const {
    auto it = programs.find(name);
    return it == programs.end() ? nullptr : it->second.get();
}

}  // namespace wmgl
