// An OpenGL rendering context.
#pragma once

#include <array>
#include <memory>
#include <string>

#include "core/gl/types.h"
#include "core/metal/command_stream.h"
#include "core/objects.h"
#include "core/share_group.h"
#include "core/state.h"
#include "core/surface.h"
#include "core/util/name_table.h"

namespace wmgl {

struct ContextConfig {
    int major = 1, minor = 0;
    bool coreProfile = false;       // WGL_CONTEXT_CORE_PROFILE_BIT_ARB
    bool forwardCompatible = false;
    bool debug = false;
    bool robustAccess = false;
    bool noError = false;
    GLenum resetNotification = GL_NO_RESET_NOTIFICATION;
};

class DrawState;

class Context {
public:
    Context(RefPtr<ShareGroup> share, const ContextConfig& config, int pixelFormat);
    ~Context();

    // --- configuration
    const ContextConfig& config() const { return config_; }
    bool isCore() const { return config_.coreProfile; }
    bool isCompat() const { return !config_.coreProfile; }
    int versionMajor() const { return config_.major; }
    int versionMinor() const { return config_.minor; }
    int version() const { return config_.major * 10 + config_.minor; }

    // --- errors and diagnostics
    void setError(GLenum error);
    GLenum takeError();
    void unimplemented(const char* command);
    void debugMessage(GLenum source, GLenum type, GLuint id, GLenum severity, const std::string& message);

    // --- current surfaces (wglMakeCurrent)
    void bindSurfaces(Surface* draw, Surface* read);
    Surface* drawSurface() const { return drawSurface_.get(); }
    Surface* readSurface() const { return readSurface_.get(); }
    bool everMadeCurrent = false;

    // --- objects
    ShareGroup& share() { return *share_; }
    RefPtr<ShareGroup> shareRef() const { return share_; }
    NameTable<VertexArray> vertexArrays;
    NameTable<Framebuffer> framebuffers;
    NameTable<Query> queries;
    NameTable<TransformFeedback> transformFeedbacks;
    RefPtr<VertexArray> defaultVertexArray;
    RefPtr<TransformFeedback> defaultTransformFeedback;
    // Texture object 0 of each target, and the proxy texture of each target.
    std::array<RefPtr<Texture>, kTexTargetCount> defaultTextures;
    std::array<RefPtr<Texture>, kTexTargetCount> proxyTextures;

    // Lookup helpers that apply the share-group lock.
    Buffer* buffer(GLuint name);
    Texture* texture(GLuint name);
    Renderbuffer* renderbuffer(GLuint name);
    Sampler* sampler(GLuint name);

    ContextState state;
    uint64_t dirty = kDirtyAll;
    void markDirty(uint64_t bits) { dirty |= bits; }

    CommandStream& stream() { return stream_; }
    DrawState& draw() { return *draw_; }

    // Returns the buffer bound to a buffer target, or null for an invalid target.
    RefPtr<Buffer>* bufferBinding(GLenum target);

    // Strings returned by glGetString/glGetStringi (stable storage).
    const std::string& versionString() const { return versionString_; }
    const std::string& glslVersionString() const { return glslVersionString_; }
    const std::vector<std::string>& extensions() const { return extensions_; }
    const std::string& extensionString() const { return extensionString_; }
    bool hasExtension(const char* name) const;

    // Occlusion-query slot of the current pass (see core/query.h).
    struct {
        uint64_t pass = 0, epoch = 0;
        bool valid = false;
    } occlusionSlot;
    uint64_t occlusionEpoch = 1;

    // Debug output.
    GLDEBUGPROC debugCallback = nullptr;
    const void* debugUserParam = nullptr;

private:
    void initStrings();

    ContextConfig config_;
    RefPtr<ShareGroup> share_;
    RefPtr<Surface> drawSurface_, readSurface_;
    CommandStream stream_;
    std::unique_ptr<DrawState> draw_;
    GLenum error_ = GL_NO_ERROR;
    std::string versionString_, glslVersionString_, extensionString_;
    std::vector<std::string> extensions_;
};

Context* currentContext();
// WINEMETALGL_TRACE=1 logs every GL call (debugging aid).
extern bool g_traceCalls;
void traceCall(const char* command);
void setCurrentContext(Context* context);
void noCurrentContext(const char* command);

// Scoped share-group lock.
struct ShareLock {
    explicit ShareLock(Context& ctx) : lock(ctx.share().mutex) {}
    std::lock_guard<std::recursive_mutex> lock;
};

}  // namespace wmgl
