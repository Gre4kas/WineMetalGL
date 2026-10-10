// Objects shared between contexts (wglShareLists / share context).
#pragma once

#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include "core/metal/queue.h"
#include "core/objects.h"
#include "core/util/name_table.h"

namespace wmgl {

class PipelineCache;

class ShareGroup : public RefCounted {
public:
    ShareGroup();
    ~ShareGroup() override;

    // Held while touching any shared name table or shared object state.
    std::recursive_mutex mutex;

    NameTable<Buffer> buffers;
    NameTable<Texture> textures;
    NameTable<Renderbuffer> renderbuffers;
    NameTable<Sampler> samplers;

    // Shaders and programs share one name space (GL 3.3 section 2.11.1).
    std::unordered_map<GLuint, RefPtr<Shader>> shaders;
    std::unordered_map<GLuint, RefPtr<Program>> programs;
    GLuint createShaderProgramName();
    Shader* shader(GLuint name) const;
    Program* program(GLuint name) const;

    std::unordered_set<Sync*> syncs;

    Queue queue;
    PipelineCache& pipelines() { return *pipelines_; }

private:
    GLuint nextShaderProgram_ = 1;
    PipelineCache* pipelines_;
};

}  // namespace wmgl
