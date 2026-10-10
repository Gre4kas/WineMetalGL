#include "core/context.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "core/draw.h"
#include "core/metal/device.h"
#include "core/program.h"
#include "core/util/log.h"

namespace wmgl {
namespace {

thread_local Context* t_current = nullptr;

}  // namespace

bool g_traceCalls = [] {
    const char* value = std::getenv("WINEMETALGL_TRACE");
    return value && value[0] == '1';
}();

void traceCall(const char* command) {
    std::fprintf(stderr, "winemetalgl:call: %s\n", command);
}

Context* currentContext() { return t_current; }

void setCurrentContext(Context* context) { t_current = context; }

void noCurrentContext(const char* command) {
    logOnce(LogLevel::Warn, command, "%s called without a current context", command);
}

Context::Context(RefPtr<ShareGroup> share, const ContextConfig& config, int pixelFormat)
    : config_(config), share_(std::move(share)), stream_(share_->queue) {
    (void)pixelFormat;
    defaultVertexArray = makeRef<VertexArray>(0);
    defaultTransformFeedback = makeRef<TransformFeedback>(0);
    state.vertexArray = defaultVertexArray;
    state.transformFeedback = defaultTransformFeedback;
    for (int i = 0; i < kTexTargetCount; ++i) {
        defaultTextures[i] = makeRef<Texture>(0, textureTargetEnum(i));
        proxyTextures[i] = makeRef<Texture>(0, textureTargetEnum(i));
        for (TextureUnit& unit : state.textureUnits) unit.bound[i] = defaultTextures[i];
    }
    draw_ = std::make_unique<DrawState>(*this);
    state.debugOutput = config_.debug;  // DEBUG_OUTPUT starts enabled in debug contexts
    initStrings();
}

Context::~Context() {
    stream_.finish();
    {
        ShareLock lock(*this);
        api::releaseCurrentProgram(*this);
    }
    draw_.reset();
}

void Context::initStrings() {
    char buffer[128];
    std::snprintf(buffer, sizeof(buffer), "%d.%d.0 WineMetalGL %s", config_.major, config_.minor,
                  config_.coreProfile ? "Core Profile" : "Compatibility Profile");
    versionString_ = buffer;
    glslVersionString_ = config_.major > 3 || (config_.major == 3 && config_.minor >= 3) ? "3.30"
                         : config_.major == 3 && config_.minor == 2                       ? "1.50"
                         : config_.major == 3 && config_.minor == 1                       ? "1.40"
                         : config_.major == 3                                             ? "1.30"
                                                                                          : "1.20";
    // Only extensions this core implements completely are advertised.
    extensions_.clear();
    // GL_ARB_compatibility needs the fixed-function API, which is not implemented yet.
    extensions_.push_back("GL_ARB_ES2_compatibility");
    extensions_.push_back("GL_ARB_texture_storage");
    extensions_.push_back("GL_EXT_texture_storage");  // the same entry points (EXT aliases)
    extensions_.push_back("GL_ARB_compute_shader");
    extensions_.push_back("GL_ARB_compressed_texture_pixel_storage");
    extensions_.push_back("GL_ARB_texture_storage_multisample");
    extensions_.push_back("GL_EXT_texture_compression_s3tc");
    extensions_.push_back("GL_EXT_texture_filter_anisotropic");
    extensions_.push_back("GL_ARB_texture_filter_anisotropic");
    // Extensions core GL 3.3 subsumes. Engines written against 2.x-era
    // drivers test for them even in 3.3 contexts.
    for (const char* name : {"GL_ARB_framebuffer_object", "GL_EXT_framebuffer_object", "GL_EXT_framebuffer_blit",
                             "GL_EXT_framebuffer_multisample", "GL_EXT_packed_depth_stencil", "GL_ARB_vertex_array_object",
                             "GL_ARB_map_buffer_range", "GL_ARB_texture_rg", "GL_ARB_texture_compression_rgtc",
                             "GL_ARB_instanced_arrays", "GL_ARB_draw_instanced", "GL_ARB_draw_elements_base_vertex",
                             "GL_ARB_uniform_buffer_object", "GL_ARB_copy_buffer", "GL_ARB_sync", "GL_ARB_timer_query",
                             "GL_EXT_timer_query", "GL_ARB_occlusion_query2", "GL_ARB_sampler_objects",
                             "GL_ARB_texture_swizzle", "GL_ARB_depth_clamp", "GL_ARB_seamless_cube_map",
                             "GL_ARB_provoking_vertex", "GL_ARB_texture_multisample", "GL_ARB_blend_func_extended",
                             "GL_ARB_explicit_attrib_location", "GL_ARB_fragment_coord_conventions",
                             "GL_ARB_shader_bit_encoding", "GL_ARB_texture_rgb10_a2ui",
                             "GL_ARB_half_float_vertex", "GL_ARB_texture_buffer_object", "GL_ARB_framebuffer_sRGB",
                             "GL_EXT_framebuffer_sRGB", "GL_EXT_texture_sRGB", "GL_ARB_map_buffer_alignment"})
        extensions_.push_back(name);
    extensions_.push_back("GL_ARB_draw_buffers_blend");
    extensions_.push_back("GL_ARB_base_instance");
    extensions_.push_back("GL_ARB_clip_control");
    extensions_.push_back("GL_ARB_get_program_binary");
    extensions_.push_back("GL_ARB_buffer_storage");
    extensions_.push_back("GL_KHR_debug");
    extensions_.push_back("GL_ARB_debug_output");
    extensions_.push_back("GL_ARB_separate_shader_objects");
    extensionString_.clear();
    for (const std::string& ext : extensions_) {
        if (!extensionString_.empty()) extensionString_ += ' ';
        extensionString_ += ext;
    }
}

bool Context::hasExtension(const char* name) const {
    return std::find(extensions_.begin(), extensions_.end(), name) != extensions_.end();
}

void Context::setError(GLenum error) {
    if (error_ == GL_NO_ERROR) error_ = error;
    if (logEnabled(LogLevel::Trace)) WMGL_TRACE("GL error 0x%04x in %s", error, currentCommand);
    if (state.debugOutput) {
        const char* name = error == GL_INVALID_ENUM        ? "GL_INVALID_ENUM"
                           : error == GL_INVALID_VALUE     ? "GL_INVALID_VALUE"
                           : error == GL_INVALID_OPERATION ? "GL_INVALID_OPERATION"
                           : error == GL_OUT_OF_MEMORY     ? "GL_OUT_OF_MEMORY"
                           : error == GL_INVALID_FRAMEBUFFER_OPERATION ? "GL_INVALID_FRAMEBUFFER_OPERATION"
                                                                       : "GL error";
        char message[160];
        std::snprintf(message, sizeof(message), "%s in %s", name, currentCommand);
        debugMessage(GL_DEBUG_SOURCE_API, GL_DEBUG_TYPE_ERROR, error, GL_DEBUG_SEVERITY_HIGH, message);
    }
}

GLenum Context::takeError() {
    const GLenum error = error_;
    error_ = GL_NO_ERROR;
    return error;
}

void Context::unimplemented(const char* command) {
    logOnce(LogLevel::Warn, command, "unimplemented: %s", command);
    setError(GL_INVALID_OPERATION);
}

bool Context::debugMessageEnabled(GLenum source, GLenum type, GLuint id, GLenum severity) const {
    bool enabled = severity != GL_DEBUG_SEVERITY_LOW;
    for (const DebugRule& r : debugGroups.back().rules) {
        if ((r.source != GL_DONT_CARE && r.source != source) || (r.type != GL_DONT_CARE && r.type != type) ||
            (r.severity != GL_DONT_CARE && r.severity != severity))
            continue;
        if (!r.ids.empty() && std::find(r.ids.begin(), r.ids.end(), id) == r.ids.end()) continue;
        enabled = r.enabled;
    }
    return enabled;
}

void Context::debugMessage(GLenum source, GLenum type, GLuint id, GLenum severity, const std::string& message) {
    if (!state.debugOutput || !debugMessageEnabled(source, type, id, severity)) return;
    const std::string text = message.size() >= kMaxDebugMessageLength ? message.substr(0, kMaxDebugMessageLength - 1) : message;
    if (debugCallback) {
        debugCallback(source, type, id, severity, static_cast<GLsizei>(text.size()), text.c_str(), debugUserParam);
        return;
    }
    if (debugLog.size() < kMaxDebugLoggedMessages) debugLog.push_back({source, type, severity, id, text});
}

void Context::bindSurfaces(Surface* draw, Surface* read) {
    drawSurface_ = draw;
    readSurface_ = read;
    if (!everMadeCurrent && draw) {
        // The first MakeCurrent sets the viewport and scissor to the surface size.
        state.viewport[2] = state.scissor[2] = draw->width();
        state.viewport[3] = state.scissor[3] = draw->height();
        everMadeCurrent = true;
    }
    markDirty(kDirtyFramebuffer | kDirtyViewport | kDirtyScissor);
}

Buffer* Context::buffer(GLuint name) {
    ShareLock lock(*this);
    return share().buffers.get(name);
}

Texture* Context::texture(GLuint name) {
    ShareLock lock(*this);
    return share().textures.get(name);
}

Renderbuffer* Context::renderbuffer(GLuint name) {
    ShareLock lock(*this);
    return share().renderbuffers.get(name);
}

Sampler* Context::sampler(GLuint name) {
    ShareLock lock(*this);
    return share().samplers.get(name);
}

RefPtr<Buffer>* Context::bufferBinding(GLenum target) {
    switch (target) {
    case GL_ARRAY_BUFFER: return &state.arrayBuffer;
    case GL_ELEMENT_ARRAY_BUFFER: return &state.vertexArray->elementBuffer;
    case GL_COPY_READ_BUFFER: return &state.copyReadBuffer;
    case GL_COPY_WRITE_BUFFER: return &state.copyWriteBuffer;
    case GL_PIXEL_PACK_BUFFER: return &state.pixelPackBuffer;
    case GL_PIXEL_UNPACK_BUFFER: return &state.pixelUnpackBuffer;
    case GL_TEXTURE_BUFFER: return &state.textureBuffer;
    case GL_UNIFORM_BUFFER: return &state.uniformBuffer;
    case GL_TRANSFORM_FEEDBACK_BUFFER: return &state.transformFeedbackBuffer;
    default: break;
    }
    if (version() >= 40 || hasExtension("GL_ARB_draw_indirect"))
        if (target == GL_DRAW_INDIRECT_BUFFER) return &state.drawIndirectBuffer;
    if (computeShaders() && target == GL_DISPATCH_INDIRECT_BUFFER) return &state.dispatchIndirectBuffer;
    if (version() >= 43 && target == GL_SHADER_STORAGE_BUFFER) return &state.shaderStorageBuffer;
    if (version() >= 42 && target == GL_ATOMIC_COUNTER_BUFFER) return &state.atomicCounterBuffer;
    if (version() >= 44 && target == GL_QUERY_BUFFER) return &state.queryBuffer;
    return nullptr;
}

}  // namespace wmgl
