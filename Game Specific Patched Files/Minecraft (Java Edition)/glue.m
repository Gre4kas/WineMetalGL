/* Native macOS host for WineMetalGL under SDL3 programs (Minecraft Java
 * Edition 26.x, through LWJGL): stands in for libSDL3 (re-exporting the real
 * one, renamed libSDL3-real.dylib) and fills libwmglhost.dylib's GL stubs.
 * Like the OpenMW glue (SDL2), SDL_GL_* put a CAMetalLayer on SDL's content
 * view, make WineMetalGL contexts current on a window surface, and swap
 * through wmgl_surface_flush/swap.
 *
 * WMGLHOST_CORE: the core to load (default: next to this library).
 * WMGLHOST_TRACE=1: log the SDL_GL calls.
 * WMGLHOST_DUMP=dir: save a frame every WMGLHOST_DUMP_EVERY (300) swaps.
 * WMGLHOST_FRAMES=file: write each swap's time (seconds) to file. */
#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#include <SDL3/SDL.h>
#include <dlfcn.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "winemetalgl/driver.h"

static const char *const names[] = {
#include "names.inc"
};
#define NAME_COUNT (sizeof(names) / sizeof(names[0]))
extern void *wmglhost_procs[]; /* libwmglhost.dylib, with the GL stubs */

static struct
{
    int (*initialize)(unsigned int);
    unsigned int (*get_pixel_formats)(struct wmgl_pixel_format *, unsigned int, unsigned int *);
    wmgl_surface *(*surface_create_window)(void *, int, int, int);
    void (*surface_destroy)(wmgl_surface *);
    void (*surface_flush)(wmgl_surface *, unsigned int, int, int, int);
    int (*surface_swap)(wmgl_surface *);
    wmgl_context *(*context_create)(int, wmgl_context *, const int *, int *);
    void (*context_destroy)(wmgl_context *);
    int (*make_current)(wmgl_context *, wmgl_surface *, wmgl_surface *);
    void *(*get_proc_address)(const char *);
    void (*log_set_host)(const char *, const char *);
} core;

static void *stubs; /* libwmglhost.dylib, for SDL_GL_GetProcAddress */
static int tracing;
#define TRACE(...) do { if (tracing) { fprintf(stderr, "wmglhost: " __VA_ARGS__); fputc('\n', stderr); } } while (0)

static void missing(void)
{
    fprintf(stderr, "wmglhost: called a GL function the core does not have\n");
    abort();
}

__attribute__((constructor)) static void load_core(void)
{
    const char *path = getenv("WMGLHOST_CORE");
    char buf[4096];
    void *handle;

    tracing = getenv("WMGLHOST_TRACE") && atoi(getenv("WMGLHOST_TRACE"));
    if (!path)
    {
        Dl_info info;
        dladdr((void *)load_core, &info);
        snprintf(buf, sizeof(buf), "%s", info.dli_fname);
        char *slash = strrchr(buf, '/');
        snprintf(slash ? slash + 1 : buf, sizeof(buf) - (slash ? slash + 1 - buf : 0), "libwinemetalgl.dylib");
        path = buf;
    }
    if (!(handle = dlopen(path, RTLD_NOW | RTLD_LOCAL)))
    {
        fprintf(stderr, "wmglhost: cannot load %s: %s\n", path, dlerror());
        abort();
    }
#define LOAD(f, n) if (!(*(void **)&core.f = dlsym(handle, n))) { fprintf(stderr, "wmglhost: no %s\n", n); abort(); }
    LOAD(initialize, "wmgl_initialize");
    LOAD(get_pixel_formats, "wmgl_get_pixel_formats");
    LOAD(surface_create_window, "wmgl_surface_create_window");
    LOAD(surface_destroy, "wmgl_surface_destroy");
    LOAD(surface_flush, "wmgl_surface_flush");
    LOAD(surface_swap, "wmgl_surface_swap");
    LOAD(context_create, "wmgl_context_create");
    LOAD(context_destroy, "wmgl_context_destroy");
    LOAD(make_current, "wmgl_make_current");
    LOAD(get_proc_address, "wmgl_get_proc_address");
#undef LOAD
    *(void **)&core.log_set_host = dlsym(handle, "wmgl_log_set_host");
    if (core.log_set_host) core.log_set_host(getprogname(), "native macOS (wmglhost, SDL3)");
    if (core.initialize(WMGL_DRIVER_ABI_VERSION))
    {
        fprintf(stderr, "wmglhost: wmgl_initialize failed\n");
        abort();
    }
    for (unsigned int i = 0; i < NAME_COUNT; i++)
    {
        wmglhost_procs[i] = core.get_proc_address(names[i]);
        if (!wmglhost_procs[i]) wmglhost_procs[i] = (void *)missing;
    }
    Dl_info info;
    if (dladdr((void *)wmglhost_procs, &info)) stubs = dlopen(info.dli_fname, RTLD_NOW | RTLD_NOLOAD);
    TRACE("loaded %s, %u GL names", path, (unsigned int)NAME_COUNT);
}

#define ATTR_COUNT 32
static int attrs[ATTR_COUNT];

static void reset_attributes(void)
{
    memset(attrs, 0, sizeof(attrs));
    attrs[SDL_GL_RED_SIZE] = attrs[SDL_GL_GREEN_SIZE] = attrs[SDL_GL_BLUE_SIZE] = 8;
    attrs[SDL_GL_DOUBLEBUFFER] = 1;
    attrs[SDL_GL_DEPTH_SIZE] = 16;
    attrs[SDL_GL_ACCELERATED_VISUAL] = -1;
}

__attribute__((constructor)) static void init_attributes(void) { reset_attributes(); }

/* Each window a context is made current on gets a layer and a surface:
 * Minecraft creates a window to probe the backend, then its own. */
struct window
{
    SDL_Window *window;
    CAMetalLayer *layer;
    wmgl_surface *surface;
    int width, height;
};
static struct window windows[8];
static int gl_format, gl_interval = 1;
static struct wmgl_pixel_format gl_pf;
static __thread SDL_Window *current_window;
static __thread SDL_GLContext current_context;

bool SDL_GL_SetAttribute(SDL_GLAttr attr, int value)
{
    TRACE("SDL_GL_SetAttribute(%d, %d)", attr, value);
    if ((int)attr < 0 || attr >= ATTR_COUNT) return SDL_SetError("Unknown OpenGL attribute");
    attrs[attr] = value;
    return true;
}

void SDL_GL_ResetAttributes(void) { reset_attributes(); }

/* The first onscreen format with at least the requested bits, preferring
 * exact sample counts. */
static int choose_format(void)
{
    unsigned int onscreen = 0, count = core.get_pixel_formats(NULL, 0, &onscreen);
    struct wmgl_pixel_format *formats = calloc(count, sizeof(*formats));
    int best = 0, want_samples = attrs[SDL_GL_MULTISAMPLEBUFFERS] ? attrs[SDL_GL_MULTISAMPLESAMPLES] : 0;

    core.get_pixel_formats(formats, count, &onscreen);
    for (unsigned int i = 0; i < onscreen && !best; i++)
    {
        const struct wmgl_pixel_format *f = &formats[i];
        if (!f->draw_to_window || !f->double_buffer || f->float_components) continue;
        if (f->red_bits < attrs[SDL_GL_RED_SIZE] || f->green_bits < attrs[SDL_GL_GREEN_SIZE] ||
            f->blue_bits < attrs[SDL_GL_BLUE_SIZE] || f->alpha_bits < attrs[SDL_GL_ALPHA_SIZE] ||
            f->depth_bits < attrs[SDL_GL_DEPTH_SIZE] || f->stencil_bits < attrs[SDL_GL_STENCIL_SIZE])
            continue;
        if ((f->samples > 1 ? f->samples : 0) != want_samples) continue;
        if (attrs[SDL_GL_FRAMEBUFFER_SRGB_CAPABLE] && !f->srgb_capable) continue;
        best = i + 1;
    }
    if (!best) best = 1;
    gl_pf = formats[best - 1];
    free(formats);
    TRACE("pixel format %d: rgba %d%d%d%d depth %d stencil %d samples %d srgb %d", best, gl_pf.red_bits,
          gl_pf.green_bits, gl_pf.blue_bits, gl_pf.alpha_bits, gl_pf.depth_bits, gl_pf.stencil_bits,
          gl_pf.samples, gl_pf.srgb_capable);
    return best;
}

static void on_main(dispatch_block_t block)
{
    if ([NSThread isMainThread]) block();
    else dispatch_sync(dispatch_get_main_queue(), block);
}

static bool attach(struct window *w, SDL_Window *window)
{
    NSWindow *nswindow = (__bridge NSWindow *)SDL_GetPointerProperty(
        SDL_GetWindowProperties(window), SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, NULL);
    __block CAMetalLayer *layer;

    if (!nswindow) return SDL_SetError("wmglhost: no Cocoa window");
    if (!gl_format) gl_format = choose_format();
    SDL_GetWindowSizeInPixels(window, &w->width, &w->height);
    on_main(^{
        NSView *view = nswindow.contentView;
        view.wantsLayer = YES;
        layer = [CAMetalLayer layer];
        layer.device = MTLCreateSystemDefaultDevice();
        layer.contentsScale = nswindow.backingScaleFactor;
        layer.frame = view.layer.bounds;
        layer.autoresizingMask = kCALayerWidthSizable | kCALayerHeightSizable;
        [view.layer addSublayer:layer];
    });
    if (!(w->surface = core.surface_create_window((__bridge void *)layer, gl_format, w->width, w->height)))
    {
        on_main(^{ [layer removeFromSuperlayer]; });
        return SDL_SetError("wmglhost: wmgl_surface_create_window failed");
    }
    core.surface_flush(w->surface, WMGL_FLUSH_INTERVAL, w->width, w->height, gl_interval);
    w->layer = layer;
    w->window = window;
    TRACE("window %p (id %u): surface %p, %dx%d", (void *)window, SDL_GetWindowID(window), (void *)w->surface,
          w->width, w->height);
    return true;
}

static struct window *find_window(SDL_Window *window, bool create)
{
    for (unsigned int i = 0; i < SDL_arraysize(windows); i++)
        if (window && windows[i].window == window) return &windows[i];
    if (!create || !window) return NULL;
    for (unsigned int i = 0; i < SDL_arraysize(windows); i++)
        if (!windows[i].window) return attach(&windows[i], window) ? &windows[i] : NULL;
    SDL_SetError("wmglhost: too many windows");
    return NULL;
}

SDL_GLContext SDL_GL_CreateContext(SDL_Window *window)
{
    int list[16], n = 0, error = 0;
    wmgl_context *share = attrs[SDL_GL_SHARE_WITH_CURRENT_CONTEXT] ? (wmgl_context *)current_context : NULL;
    wmgl_context *context;
    struct window *w = find_window(window, true);

    if (!w) return NULL;
    if (attrs[SDL_GL_CONTEXT_MAJOR_VERSION])
    {
        list[n++] = 0x2091; list[n++] = attrs[SDL_GL_CONTEXT_MAJOR_VERSION];
        list[n++] = 0x2092; list[n++] = attrs[SDL_GL_CONTEXT_MINOR_VERSION];
    }
    if (attrs[SDL_GL_CONTEXT_PROFILE_MASK] & 3)
    {
        list[n++] = 0x9126; list[n++] = attrs[SDL_GL_CONTEXT_PROFILE_MASK] & 3;
    }
    if (attrs[SDL_GL_CONTEXT_FLAGS] & 3)
    {
        list[n++] = 0x2094; list[n++] = attrs[SDL_GL_CONTEXT_FLAGS] & 3;
    }
    list[n] = 0;
    TRACE("SDL_GL_CreateContext: version %d.%d profile %d flags %d", attrs[SDL_GL_CONTEXT_MAJOR_VERSION],
          attrs[SDL_GL_CONTEXT_MINOR_VERSION], attrs[SDL_GL_CONTEXT_PROFILE_MASK], attrs[SDL_GL_CONTEXT_FLAGS]);
    if (!(context = core.context_create(gl_format, share, list, &error)))
    {
        SDL_SetError("wmglhost: wmgl_context_create failed (%d)", error);
        return NULL;
    }
    TRACE("SDL_GL_CreateContext -> %p (share %p)", (void *)context, (void *)share);
    if (core.make_current(context, w->surface, w->surface))
    {
        current_window = window;
        current_context = (SDL_GLContext)context;
    }
    return (SDL_GLContext)context;
}

bool SDL_GL_MakeCurrent(SDL_Window *window, SDL_GLContext context)
{
    TRACE("SDL_GL_MakeCurrent(%p, %p) on %p", (void *)window, (void *)context, (void *)pthread_self());
    if (!context)
    {
        core.make_current(NULL, NULL, NULL);
        current_window = NULL;
        current_context = NULL;
        return true;
    }
    struct window *w = find_window(window, true);
    if (!w) return false;
    if (!core.make_current((wmgl_context *)context, w->surface, w->surface))
        return SDL_SetError("wmglhost: make current failed");
    current_window = window;
    current_context = context;
    return true;
}

SDL_GLContext SDL_GL_GetCurrentContext(void) { return current_context; }
SDL_Window *SDL_GL_GetCurrentWindow(void) { return current_window; }

bool SDL_GL_DestroyContext(SDL_GLContext context)
{
    TRACE("SDL_GL_DestroyContext(%p)", (void *)context);
    if (!context) return SDL_SetError("Parameter 'context' is invalid");
    if (context == current_context) SDL_GL_MakeCurrent(NULL, NULL);
    core.context_destroy((wmgl_context *)context);
    return true;
}

bool SDL_GL_SetSwapInterval(int interval)
{
    TRACE("SDL_GL_SetSwapInterval(%d)", interval);
    gl_interval = interval;
    for (unsigned int i = 0; i < SDL_arraysize(windows); i++)
        if (windows[i].surface)
            core.surface_flush(windows[i].surface, WMGL_FLUSH_INTERVAL, windows[i].width, windows[i].height, gl_interval);
    return true;
}

bool SDL_GL_GetSwapInterval(int *interval)
{
    if (!interval) return SDL_SetError("Parameter 'interval' is invalid");
    *interval = gl_interval;
    return true;
}

/* WMGLHOST_DUMP=dir: every WMGLHOST_DUMP_EVERY (300) frames, the back buffer
 * as dir/frame-N.ppm, read before the swap. */
static void dump_frame(const struct window *w)
{
    static unsigned long frame;
    static const char *dir;
    static int every;
    void (*readPixels)(int, int, int, int, unsigned int, unsigned int, void *);
    void (*getIntegerv)(unsigned int, int *);
    void (*bindFramebuffer)(unsigned int, unsigned int);
    void (*readBuffer)(unsigned int);
    int read_fb = 0, read_buffer = 0;
    char path[4096];

    if (!frame)
    {
        dir = getenv("WMGLHOST_DUMP");
        every = getenv("WMGLHOST_DUMP_EVERY") ? atoi(getenv("WMGLHOST_DUMP_EVERY")) : 300;
        if (every <= 0) every = 300;
    }
    if (!dir || ++frame % every) return;
    readPixels = core.get_proc_address("glReadPixels");
    getIntegerv = core.get_proc_address("glGetIntegerv");
    bindFramebuffer = core.get_proc_address("glBindFramebuffer");
    readBuffer = core.get_proc_address("glReadBuffer");
    getIntegerv(0x8CAA /* GL_READ_FRAMEBUFFER_BINDING */, &read_fb);
    getIntegerv(0x0C02 /* GL_READ_BUFFER */, &read_buffer);
    bindFramebuffer(0x8CA8 /* GL_READ_FRAMEBUFFER */, 0);
    readBuffer(0x0405 /* GL_BACK */);
    unsigned char *pixels = malloc((size_t)w->width * w->height * 4);
    readPixels(0, 0, w->width, w->height, 0x1908 /* GL_RGBA */, 0x1401, pixels);
    bindFramebuffer(0x8CA8, read_fb);
    readBuffer(read_buffer);
    snprintf(path, sizeof(path), "%s/frame-%lu.ppm", dir, frame);
    FILE *f = fopen(path, "wb");
    if (f)
    {
        fprintf(f, "P6\n%d %d\n255\n", w->width, w->height);
        for (int y = w->height - 1; y >= 0; y--)
            for (int x = 0; x < w->width; x++) fwrite(pixels + ((size_t)y * w->width + x) * 4, 1, 3, f);
        fclose(f);
        TRACE("dumped %s", path);
    }
    free(pixels);
}

/* WMGLHOST_FRAMES=file: one line per swap, its time in seconds. */
static void log_frame(void)
{
    static FILE *frames;
    static int opened;
    static mach_timebase_info_data_t timebase;

    if (!opened)
    {
        const char *path = getenv("WMGLHOST_FRAMES");
        opened = 1;
        if (path && (frames = fopen(path, "w"))) setvbuf(frames, NULL, _IOLBF, 0);
        mach_timebase_info(&timebase);
    }
    if (frames) fprintf(frames, "%.6f\n", (double)mach_absolute_time() * timebase.numer / timebase.denom / 1e9);
}

bool SDL_GL_SwapWindow(SDL_Window *window)
{
    struct window *w = find_window(window, false);
    int width, height;

    if (!w) return SDL_SetError("wmglhost: no context was made current on this window");
    SDL_GetWindowSizeInPixels(window, &width, &height);
    if (width != w->width || height != w->height)
    {
        TRACE("resize %dx%d -> %dx%d", w->width, w->height, width, height);
        w->width = width;
        w->height = height;
        core.surface_flush(w->surface, WMGL_FLUSH_UPDATED, width, height, gl_interval);
    }
    dump_frame(w);
    core.surface_swap(w->surface);
    log_frame();
    return true;
}

/* Drops the window's surface and layer, then destroys it in the real SDL. */
void SDL_DestroyWindow(SDL_Window *window)
{
    static void (*destroy)(SDL_Window *);
    struct window *w = find_window(window, false);

    if (!destroy)
    {
        Dl_info info; /* SDL_GetError is the real SDL's */
        void *real = dladdr((void *)SDL_GetError, &info) ? dlopen(info.dli_fname, RTLD_NOW | RTLD_NOLOAD) : NULL;
        if (real) *(void **)&destroy = dlsym(real, "SDL_DestroyWindow");
    }
    if (w)
    {
        CAMetalLayer *layer = w->layer;
        TRACE("SDL_DestroyWindow(%p): surface %p", (void *)window, (void *)w->surface);
        if (current_window == window) SDL_GL_MakeCurrent(NULL, NULL);
        core.surface_destroy(w->surface);
        on_main(^{ [layer removeFromSuperlayer]; });
        *w = (struct window){0};
    }
    if (destroy) destroy(window);
}

/* The stub when there is one: Minecraft requires SDL's glGetError to be
 * the GL library's (org.lwjgl.opengl.libname, libwmglhost.dylib). */
SDL_FunctionPointer SDL_GL_GetProcAddress(const char *proc)
{
    void *address = stubs ? dlsym(stubs, proc) : NULL;
    return (SDL_FunctionPointer)(address ? address : core.get_proc_address(proc));
}
bool SDL_GL_LoadLibrary(const char *path) { return true; }
void SDL_GL_UnloadLibrary(void) {}

bool SDL_GL_ExtensionSupported(const char *extension)
{
    const unsigned char *(*getStringi)(unsigned int, unsigned int) = core.get_proc_address("glGetStringi");
    void (*getIntegerv)(unsigned int, int *) = core.get_proc_address("glGetIntegerv");
    int count = 0;

    if (!current_context) return false;
    getIntegerv(0x821D /* GL_NUM_EXTENSIONS */, &count);
    for (int i = 0; i < count; i++)
        if (!strcmp((const char *)getStringi(0x1F03 /* GL_EXTENSIONS */, i), extension)) return true;
    return false;
}

bool SDL_GL_GetAttribute(SDL_GLAttr attr, int *value)
{
    if (!value) return SDL_SetError("Parameter 'value' is invalid");
    switch (attr)
    {
    case SDL_GL_RED_SIZE: *value = gl_pf.red_bits; break;
    case SDL_GL_GREEN_SIZE: *value = gl_pf.green_bits; break;
    case SDL_GL_BLUE_SIZE: *value = gl_pf.blue_bits; break;
    case SDL_GL_ALPHA_SIZE: *value = gl_pf.alpha_bits; break;
    case SDL_GL_BUFFER_SIZE: *value = gl_pf.color_bits + gl_pf.alpha_bits; break;
    case SDL_GL_DOUBLEBUFFER: *value = gl_pf.double_buffer; break;
    case SDL_GL_DEPTH_SIZE: *value = gl_pf.depth_bits; break;
    case SDL_GL_STENCIL_SIZE: *value = gl_pf.stencil_bits; break;
    case SDL_GL_MULTISAMPLEBUFFERS: *value = gl_pf.sample_buffers; break;
    case SDL_GL_MULTISAMPLESAMPLES: *value = gl_pf.samples; break;
    case SDL_GL_ACCELERATED_VISUAL: *value = 1; break;
    case SDL_GL_FRAMEBUFFER_SRGB_CAPABLE: *value = gl_pf.srgb_capable; break;
    default: *value = (int)attr >= 0 && attr < ATTR_COUNT ? attrs[attr] : 0; break;
    }
    TRACE("SDL_GL_GetAttribute(%d) = %d", attr, *value);
    return true;
}
