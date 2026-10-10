// winemac.drv-facing ABI (include/winemetalgl/driver.h).

#include "winemetalgl/driver.h"

#include <cstdlib>
#include <cstring>

#include "core/context.h"
#include "core/metal/device.h"
#include "core/proc_table.h"
#include "core/surface.h"
#include "core/util/log.h"

#define WMGL_EXPORT extern "C" __attribute__((visibility("default")))

using namespace wmgl;

namespace {

// WGL_ARB_create_context / _profile / _robustness / ARB_create_context_no_error.
constexpr int kMajorVersion = 0x2091, kMinorVersion = 0x2092, kLayerPlane = 0x2093, kFlags = 0x2094,
              kProfileMask = 0x9126, kResetStrategy = 0x8256, kNoError = 0x31B3, kReleaseBehavior = 0x2097;
constexpr int kDebugBit = 0x1, kForwardCompatibleBit = 0x2, kRobustAccessBit = 0x4;
constexpr int kCoreBit = 0x1, kCompatBit = 0x2, kES2Bit = 0x4;
constexpr int kLoseContextOnReset = 0x8252;

void maxVersion(int& major, int& minor) {
    major = 3;
    minor = 3;
    if (const char* level = std::getenv("WINEMETALGL_MAX_FEATURE_LEVEL")) {
        int a = 0, b = 0;
        if (std::sscanf(level, "%d.%d", &a, &b) == 2 && (a < 3 || (a == 3 && b <= 3))) {
            major = a;
            minor = b;
        }
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
        WMGL_ERR("driver ABI mismatch: winemac %u, core %u", abi_version, WMGL_DRIVER_ABI_VERSION);
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
    }
}

WMGL_EXPORT int wmgl_surface_swap(wmgl_surface* handle) {
    Surface* surface = asSurface(handle);
    if (!surface) return 0;
    Context* ctx = currentContext();
    if (ctx && ctx->drawSurface() == surface) return surface->swap(ctx->stream()) ? 1 : 0;
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
    if (profileGiven && (profile & kES2Bit)) {
        if (error) *error = WMGL_ERROR_INVALID_PROFILE;
        return nullptr;
    }
    if (profileGiven && !(profile & (kCoreBit | kCompatBit))) {
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
    const bool wantsCore = (requestedMajor > 3 || (requestedMajor == 3 && requestedMinor >= 2)) && (profile & kCoreBit);
    config.coreProfile = wantsCore;
    config.forwardCompatible = (flags & kForwardCompatibleBit) && requestedMajor >= 3;
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
    static std::string renderer = std::string(Device::get().name()) + " (WineMetalGL)";
    return renderer.c_str();
}

WMGL_EXPORT unsigned int wmgl_video_memory_mb(void) {
    return static_cast<unsigned>(Device::get().caps().workingSetBytes >> 20);
}

WMGL_EXPORT void wmgl_max_versions(int* coreMajor, int* coreMinor, int* compatMajor, int* compatMinor) {
    int major, minor;
    maxVersion(major, minor);
    if (coreMajor) *coreMajor = major;
    if (coreMinor) *coreMinor = minor;
    if (compatMajor) *compatMajor = major;
    if (compatMinor) *compatMinor = minor;
}
