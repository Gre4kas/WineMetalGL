// Objects shared between contexts (wglShareLists / share context).
#pragma once

#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "core/compat/arb_programs.h"
#include "core/compat/display_list.h"
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
    // Framebuffers are container objects GL does not share, but desktop
    // drivers (Mesa, NVIDIA, AMD) do, and games rely on it: DOOM (2016) makes
    // its framebuffers on one context and draws with another.
    NameTable<Framebuffer> framebuffers;

    // Shaders and programs share one name space (GL 3.3 section 2.11.1).
    std::unordered_map<GLuint, RefPtr<Shader>> shaders;
    std::unordered_map<GLuint, RefPtr<Program>> programs;
    GLuint createShaderProgramName();
    Shader* shader(GLuint name) const;
    Program* program(GLuint name) const;

    std::unordered_set<Sync*> syncs;

    Queue queue;
    PipelineCache& pipelines() { return *pipelines_; }

    // Compatibility profile: generated fixed-function programs by state key
    // (core/compat/fixed_function.h), and display lists (compat/display_list.h).
    std::unordered_map<std::string, RefPtr<Program>> fixedFunctionPrograms;
    std::map<GLuint, RefPtr<DisplayList>> displayLists;
    // ARB assembly program objects (their own name space) and the generated
    // programs that run them (compat/arb_programs.h).
    std::unordered_map<GLuint, std::shared_ptr<compat::ArbProgramObject>> arbPrograms;
    std::unordered_map<std::string, compat::ArbLinked> arbLinkedPrograms;

private:
    GLuint nextShaderProgram_ = 1;
    PipelineCache* pipelines_;
};

}  // namespace wmgl
