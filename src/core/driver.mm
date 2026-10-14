// The driver ABI the Wine module uses (include/winemetalgl/driver.h).

#include "winemetalgl/driver.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "core/context.h"
#include "core/metal/device.h"
#include "core/proc_table.h"
#include "core/stats.h"
#include "core/surface.h"
#include "core/util/log.h"
#include "core/util/version.h"

#define WMGL_EXPORT extern "C" __attribute__((visibility("default")))

using namespace wmgl;

namespace {

// WGL_ARB_create_context / _profile / _robustness / ARB_create_context_no_error.
constexpr int kMajorVersion = 0x2091, kMinorVersion = 0x2092, kLayerPlane = 0x2093, kFlags = 0x2094,
              kProfileMask = 0x9126, kResetStrategy = 0x8256, kNoError = 0x31B3, kReleaseBehavior = 0x2097;
constexpr int kDebugBit = 0x1, kForwardCompatibleBit = 0x2, kRobustAccessBit = 0x4;
constexpr int kCoreBit = 0x1, kCompatBit = 0x2;
constexpr int kLoseContextOnReset = 0x8252;

// The highest version this core implements completely.
constexpr int kHonestMajor = 4, kHonestMinor = 6;

void honestMaxVersion(int& major, int& minor) {
    major = kHonestMajor;
    minor = kHonestMinor;
    if (const char* level = std::getenv("WINEMETALGL_MAX_FEATURE_LEVEL")) {
        int a = 0, b = 0;
        if (std::sscanf(level, "%d.%d", &a, &b) == 2 && (a < kHonestMajor || (a == kHonestMajor && b <= kHonestMinor))) {
            major = a;
            minor = b;
        }
    }
}

// The version contexts get: the honest one, unless the development override
// (devMaxVersion, core/util/version.h) raises it.
void maxVersion(int& major, int& minor) {
    honestMaxVersion(major, minor);
    const int dev = devMaxVersion();
    if (dev > major * 10 + minor) {
        major = dev / 10;
        minor = dev % 10;
    }
}

bool validVersion(int major, int minor) {
    switch (major) {
    case 1: return minor >= 0 && minor <= 5;
    case 2: return minor >= 0 && minor <= 1;
    case 3: return minor >= 0 && minor <= 3;
    case 4: return minor >= 0 && minor <= 6;
    default: return false;
    }
}

Context* asContext(wmgl_context* c) { return reinterpret_cast<Context*>(c); }
Surface* asSurface(wmgl_surface* s) { return reinterpret_cast<Surface*>(s); }

}  // namespace

WMGL_EXPORT int wmgl_initialize(unsigned int abi_version) {
    if (abi_version != WMGL_DRIVER_ABI_VERSION) {
        WMGL_ERR("driver ABI mismatch: caller %u, core %u", abi_version, WMGL_DRIVER_ABI_VERSION);
        return -1;
    }
    if (!Device::get().mtl()) return -1;
    WMGL_INFO("WineMetalGL core on %s", Device::get().name());
    return 0;
}

WMGL_EXPORT unsigned int wmgl_get_pixel_formats(struct wmgl_pixel_format* formats, unsigned int max, unsigned int* onscreen) {
    const auto& list = pixelFormats();
    for (unsigned i = 0; i < max && i < list.size(); ++i) formats[i] = list[i];
    if (onscreen) *onscreen = onscreenPixelFormatCount();
    return static_cast<unsigned>(list.size());
}

WMGL_EXPORT wmgl_surface* wmgl_surface_create_window(void* layer, int format, int width, int height) {
    auto* surface = new Surface((__bridge CAMetalLayer*)layer, format, width, height);
    surface->retain();
    WMGL_TRACE("surface %p window %dx%d format %d", surface, width, height, format);
    return reinterpret_cast<wmgl_surface*>(surface);
}

WMGL_EXPORT wmgl_surface* wmgl_surface_create_pbuffer(int format, int width, int height) {
    auto* surface = new Surface(nil, format, width, height);
    surface->retain();
    return reinterpret_cast<wmgl_surface*>(surface);
}

WMGL_EXPORT void wmgl_surface_destroy(wmgl_surface* surface) {
    if (surface) asSurface(surface)->release();
}

WMGL_EXPORT void wmgl_surface_flush(wmgl_surface* handle, unsigned int flags, int width, int height, int interval) {
    Surface* surface = asSurface(handle);
    if (!surface) return;
    if (flags & WMGL_FLUSH_UPDATED) {
        Context* ctx = currentContext();
        surface->resize(width, height, ctx && ctx->drawSurface() == surface ? &ctx->stream() : nullptr);
    }
    if (flags & WMGL_FLUSH_INTERVAL) surface->setSwapInterval(interval);
    if (flags & WMGL_FLUSH_PRESENT) {
        Context* ctx = currentContext();
        if (ctx && ctx->drawSurface() == surface) surface->swap(ctx->stream());
        stats::endFrame();
    }
}

WMGL_EXPORT int wmgl_surface_swap(wmgl_surface* handle) {
    Surface* surface = asSurface(handle);
    if (!surface) return 0;
    Context* ctx = currentContext();
    if (ctx && ctx->drawSurface() == surface) {
        const bool presented = surface->swap(ctx->stream());
        stats::endFrame();
        return presented ? 1 : 0;
    }
    // SwapBuffers on a DC whose surface is not current here: present what it has.
    static Queue* queue = new Queue();
    CommandStream stream(*queue);
    return surface->swap(stream) ? 1 : 0;
}

WMGL_EXPORT wmgl_context* wmgl_context_create(int format, wmgl_context* share, const int* attribs, int* error) {
    if (error) *error = WMGL_ERROR_NONE;
    ContextConfig config;
    int requestedMajor = 1, requestedMinor = 0, flags = 0, profile = kCoreBit;
    bool profileGiven = false;
    for (const int* a = attribs; a && a[0]; a += 2) {
        switch (a[0]) {
        case kMajorVersion: requestedMajor = a[1]; break;
        case kMinorVersion: requestedMinor = a[1]; break;
        case kLayerPlane: break;
        case kFlags: flags = a[1]; break;
        case kProfileMask: profile = a[1]; profileGiven = true; break;
        case kResetStrategy: config.resetNotification = a[1] == kLoseContextOnReset ? GL_LOSE_CONTEXT_ON_RESET : GL_NO_RESET_NOTIFICATION; break;
        case kNoError: config.noError = a[1] != 0; break;
        case kReleaseBehavior: break;
        default:
            WMGL_WARN("unknown context attribute 0x%x", a[0]);
            if (error) *error = WMGL_ERROR_INVALID_ATTRIB;
            return nullptr;
        }
    }
    if (!validVersion(requestedMajor, requestedMinor)) {
        if (error) *error = WMGL_ERROR_INVALID_VERSION;
        return nullptr;
    }
    // WGL_ARB_create_context(_robustness): unknown flags, and the
    // forward-compatible flag before 3.0, are ERROR_INVALID_VERSION_ARB.
    if ((flags & ~(kDebugBit | kForwardCompatibleBit | kRobustAccessBit)) ||
        ((flags & kForwardCompatibleBit) && requestedMajor < 3)) {
        if (error) *error = WMGL_ERROR_INVALID_VERSION;
        return nullptr;
    }
    // The profile mask applies from 3.2 (it is ignored before): exactly one of
    // the core and compatibility bits.
    const bool profiles = requestedMajor > 3 || (requestedMajor == 3 && requestedMinor >= 2);
    if (profiles && profileGiven && profile != kCoreBit && profile != kCompatBit) {
        if (error) *error = WMGL_ERROR_INVALID_PROFILE;
        return nullptr;
    }
    int maxMajor, maxMinor;
    maxVersion(maxMajor, maxMinor);
    if (requestedMajor > maxMajor || (requestedMajor == maxMajor && requestedMinor > maxMinor)) {
        if (error) *error = WMGL_ERROR_INVALID_VERSION;
        return nullptr;
    }
    // Profiles exist from 3.2; earlier requests (and legacy wglCreateContext) get compatibility.
    // A forward-compatible 3.0 or 3.1 context drops the deprecated features:
    // the core profile is its backwards-compatible successor.
    config.coreProfile = profiles ? profile == kCoreBit : (flags & kForwardCompatibleBit) && requestedMajor >= 3;
    config.forwardCompatible = flags & kForwardCompatibleBit;
    config.debug = flags & kDebugBit;
    config.robustAccess = flags & kRobustAccessBit;
    // GL may return any later version that is backwards compatible with the request.
    config.major = maxMajor;
    config.minor = maxMinor;
    RefPtr<ShareGroup> group = share ? asContext(share)->shareRef() : makeRef<ShareGroup>();
    auto* ctx = new Context(group, config, format);
    WMGL_TRACE("context %p %d.%d %s (requested %d.%d)", ctx, config.major, config.minor,
               config.coreProfile ? "core" : "compat", requestedMajor, requestedMinor);
    return reinterpret_cast<wmgl_context*>(ctx);
}

WMGL_EXPORT void wmgl_context_destroy(wmgl_context* handle) {
    Context* ctx = asContext(handle);
    if (!ctx) return;
    if (currentContext() == ctx) setCurrentContext(nullptr);
    delete ctx;
}

WMGL_EXPORT int wmgl_make_current(wmgl_context* handle, wmgl_surface* draw, wmgl_surface* read) {
    Context* previous = currentContext();
    Context* ctx = asContext(handle);
    // Switching contexts flushes the previous one (WGL requires an implicit glFlush).
    if (previous && previous != ctx) previous->stream().flush();
    setCurrentContext(ctx);
    if (ctx) ctx->bindSurfaces(asSurface(draw), asSurface(read ? read : draw));
    return 1;
}

WMGL_EXPORT void* wmgl_get_proc_address(const char* name) { return lookupProc(name); }

WMGL_EXPORT const char* wmgl_renderer_string(void) {
    // GL_RENDERER's string, WINEMETALGL_RENDERER included.
    return rendererString().c_str();
}

namespace wmgl::api {
extern void* (*g_bufferAllocate)(size_t);
extern void (*g_bufferRelease)(void*, size_t);
}

WMGL_EXPORT void wmgl_set_buffer_memory(void* (*allocate)(size_t), void (*release)(void*, size_t)) {
    wmgl::api::g_bufferAllocate = allocate;
    wmgl::api::g_bufferRelease = release;
}

WMGL_EXPORT void wmgl_stats_enable(int enable) { stats::g_enabled = enable || stats::g_reporting; }

WMGL_EXPORT void wmgl_stats_reset(void) { stats::frame() = stats::Frame(); }

WMGL_EXPORT int wmgl_stats_counter(const char* name, unsigned long long* value) {
    uint64_t v = 0;
    if (!stats::counter(name, v)) return 0;
    if (value) *value = v;
    return 1;
}

WMGL_EXPORT void wmgl_version(int* major, int* minor, int* patch) {
    int v[3] = {0, 0, 0};
    std::sscanf(WMGL_VERSION, "%d.%d.%d", &v[0], &v[1], &v[2]);
    if (major) *major = v[0];
    if (minor) *minor = v[1];
    if (patch) *patch = v[2];
}

WMGL_EXPORT unsigned int wmgl_video_memory_mb(void) {
    return static_cast<unsigned>(Device::get().caps().workingSetBytes >> 20);
}

WMGL_EXPORT void wmgl_max_versions(int* coreMajor, int* coreMinor, int* compatMajor, int* compatMinor) {
    // WGL_WINE_query_renderer reports what the core implements, never the
    // development override.
    int major, minor;
    honestMaxVersion(major, minor);
    if (coreMajor) *coreMajor = major;
    if (coreMinor) *coreMinor = minor;
    if (compatMajor) *compatMajor = major;
    if (compatMinor) *compatMinor = minor;
}
