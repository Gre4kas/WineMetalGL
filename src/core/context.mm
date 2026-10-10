#include "core/context.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "core/draw.h"
#include "core/metal/device.h"
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
    initStrings();
}

Context::~Context() {
    stream_.finish();
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
    if (!config_.coreProfile) extensions_.push_back("GL_ARB_compatibility");
    extensions_.push_back("GL_ARB_ES2_compatibility");
    extensions_.push_back("GL_ARB_texture_storage");
    extensions_.push_back("GL_ARB_texture_storage_multisample");
    extensions_.push_back("GL_EXT_texture_compression_s3tc");
    extensions_.push_back("GL_EXT_texture_filter_anisotropic");
    extensions_.push_back("GL_ARB_texture_filter_anisotropic");
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
    if (logEnabled(LogLevel::Trace)) WMGL_TRACE("GL error 0x%04x", error);
    if (state.debugOutput && debugCallback) {
        char message[64];
        std::snprintf(message, sizeof(message), "GL error 0x%04x", error);
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

void Context::debugMessage(GLenum source, GLenum type, GLuint id, GLenum severity, const std::string& message) {
    if (debugCallback)
        debugCallback(source, type, id, severity, static_cast<GLsizei>(message.size()), message.c_str(), debugUserParam);
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
    if (version() >= 43) {
        if (target == GL_DISPATCH_INDIRECT_BUFFER) return &state.dispatchIndirectBuffer;
        if (target == GL_SHADER_STORAGE_BUFFER) return &state.shaderStorageBuffer;
    }
    if (version() >= 42 && target == GL_ATOMIC_COUNTER_BUFFER) return &state.atomicCounterBuffer;
    if (version() >= 44 && target == GL_QUERY_BUFFER) return &state.queryBuffer;
    return nullptr;
}

}  // namespace wmgl
