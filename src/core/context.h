// An OpenGL rendering context.
#pragma once

#include <array>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

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
struct ImmediateState;
struct ListCompiler;

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
    NameTable<ProgramPipeline> programPipelines;
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
    // Recorded but not read yet (see state.h).
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
    // Compute shaders: GL 4.3 or GL_ARB_compute_shader.
    bool computeShaders() const { return version() >= 43 || hasExtension("GL_ARB_compute_shader"); }
    // Tessellation shaders: GL 4.0 or GL_ARB_tessellation_shader.
    bool tessellationShaders() const { return version() >= 40 || hasExtension("GL_ARB_tessellation_shader"); }
    // Sample shading: GL 4.0 or GL_ARB_sample_shading.
    bool sampleShading() const { return version() >= 40 || hasExtension("GL_ARB_sample_shading"); }

    // Occlusion-query slot of the current pass (see core/query.h).
    struct {
        uint64_t pass = 0, epoch = 0;
        bool valid = false;
    } occlusionSlot;
    uint64_t occlusionEpoch = 1;

    // Debug output (KHR_debug / ARB_debug_output, core/api/debug.mm).
    GLDEBUGPROC debugCallback = nullptr;
    const void* debugUserParam = nullptr;
    struct DebugRule {
        GLenum source, type, severity;  // GL_DONT_CARE matches any
        std::vector<GLuint> ids;        // empty: any id
        bool enabled;
    };
    struct DebugGroup {
        GLenum source;
        GLuint id;
        std::string message;
        std::vector<DebugRule> rules;  // applied in order over the default (all but LOW enabled)
    };
    struct DebugLogEntry {
        GLenum source, type, severity;
        GLuint id;
        std::string message;
    };
    std::vector<DebugGroup> debugGroups{DebugGroup{GL_DEBUG_SOURCE_APPLICATION, 0, "", {}}};
    std::deque<DebugLogEntry> debugLog;
    static constexpr size_t kMaxDebugLoggedMessages = 64, kMaxDebugMessageLength = 1024, kMaxDebugGroupDepth = 64;
    static constexpr GLsizei kMaxLabelLength = 256;
    bool debugMessageEnabled(GLenum source, GLenum type, GLuint id, GLenum severity) const;
    // Name of the GL command being executed (set by the generated entry points).
    const char* currentCommand = "";

    // Compatibility profile (W22): the packed wmgl_FixedFunction block
    // (compat/fixed_function.h), immediate mode (compat/immediate.h) and
    // display list compilation (compat/display_list.h).
    struct FixedFunctionBlockCache {
        uint64_t version = 0, generation = 0;
        float pointSize = -1, fade = -1, lineWidth = -1;
        GLfloat viewport[4] = {};
        std::vector<uint8_t> data;
        // The matrices the block's matrix section was computed from.
        bool matricesValid = false;
        Mat4 modelview, projection, modelviewInverse, texture[FixedState::kTextureUnits];
    } fixedFunctionBlockCache;
    // glPushAttrib / glPushClientAttrib (api/fixed_attrib.mm): the mask and
    // a copy of the state it covers.
    struct AttribFrame {
        GLbitfield mask = 0;
        ContextState state;
        RefPtr<VertexArray> vertexArray;
        std::array<VertexAttrib, VertexArray::kAttribSlots> attribs;
    };
    std::vector<AttribFrame> attribStack, clientAttribStack;
    // glBitmap / glDrawPixels textures by content (api/fixed_raster.mm):
    // applications draw the same glyphs and images every frame.
    std::unordered_map<uint64_t, RefPtr<Texture>> pixelRectangleCache;
    std::unique_ptr<ImmediateState> immediate;
    std::unique_ptr<ListCompiler> listCompiler;  // non-null between glNewList and glEndList
    int listCallDepth = 0;

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

// Scoped share-group lock (every GL command runs under it).
struct ShareLock {
    explicit ShareLock(Context& ctx) : lock(ctx.share().mutex) { ++depth; }
    ~ShareLock() { --depth; }
    std::lock_guard<std::recursive_mutex> lock;
    static thread_local int depth;  // share-group locks this thread holds
};

// Releases the calling thread's share-group lock around a GPU wait that
// touches no shared state (fence and query waits, glFinish), so the other
// contexts of the share group keep running meanwhile.
class ShareUnlock {
public:
    explicit ShareUnlock(Context& ctx) : mutex_(ctx.share().mutex), depth_(ShareLock::depth) {
        for (int i = 0; i < depth_; ++i) mutex_.unlock();
    }
    ~ShareUnlock() {
        for (int i = 0; i < depth_; ++i) mutex_.lock();
    }
    ShareUnlock(const ShareUnlock&) = delete;
    ShareUnlock& operator=(const ShareUnlock&) = delete;

private:
    std::recursive_mutex& mutex_;
    int depth_;
};

// Waits for `serial` (this context's work is flushed first) without the
// share-group lock, so the other contexts of the share group keep running.
// Shared object state may change meanwhile: callers re-validate it after.
void waitForGpuUnlocked(Context& ctx, uint64_t serial);
// glFinish-style: flushes, then waits for everything submitted, unlocked.
void finishUnlocked(Context& ctx);

}  // namespace wmgl
