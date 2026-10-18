/* Native macOS host for WineMetalGL under SDL2 programs (OpenMW): stands in
 * for both libSDL2 (re-exporting the real one, renamed libSDL2-real.dylib)
 * and, through libwmglhost.dylib, OpenGL.framework (every gl* name, from
 * stubs.S, jumps through wmglhost_procs, which this library fills).
 * SDL_GL_* follow src/wine/unix/unix_wgl.c: a CAMetalLayer on SDL's content
 * view, a WineMetalGL window surface, contexts made current on it, and swaps
 * through wmgl_surface_flush/swap.
 *
 * WMGLHOST_CORE: the core to load (default: next to this library).
 * WMGLHOST_TRACE=1: log the SDL_GL calls.
 * WMGLHOST_DUMP=dir: save a frame every WMGLHOST_DUMP_EVERY (300) swaps. */
#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#include <dlfcn.h>
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
    void (*surface_flush)(wmgl_surface *, unsigned int, int, int, int);
    int (*surface_swap)(wmgl_surface *);
    wmgl_context *(*context_create)(int, wmgl_context *, const int *, int *);
    void (*context_destroy)(wmgl_context *);
    int (*make_current)(wmgl_context *, wmgl_surface *, wmgl_surface *);
    void *(*get_proc_address)(const char *);
    void (*log_set_host)(const char *, const char *);
} core;

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
    LOAD(surface_flush, "wmgl_surface_flush");
    LOAD(surface_swap, "wmgl_surface_swap");
    LOAD(context_create, "wmgl_context_create");
    LOAD(context_destroy, "wmgl_context_destroy");
    LOAD(make_current, "wmgl_make_current");
    LOAD(get_proc_address, "wmgl_get_proc_address");
#undef LOAD
    *(void **)&core.log_set_host = dlsym(handle, "wmgl_log_set_host");
    if (core.log_set_host) core.log_set_host(getprogname(), "native macOS (wmglhost)");
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
    TRACE("loaded %s, %u GL names", path, (unsigned int)NAME_COUNT);
}

/* SDL2, declared here: no SDL headers needed. */
typedef struct SDL_Window SDL_Window;
typedef void *SDL_GLContext;
typedef struct { unsigned char major, minor, patch; } SDL_version;
typedef struct
{
    SDL_version version;
    int subsystem;
    union { struct { NSWindow *__unsafe_unretained window; } cocoa; unsigned char dummy[64]; } info;
} SDL_SysWMinfo;
extern int SDL_GetWindowWMInfo(SDL_Window *window, SDL_SysWMinfo *info);
extern void SDL_GL_GetDrawableSize(SDL_Window *window, int *w, int *h);
extern int SDL_SetError(const char *fmt, ...);

enum
{
    SDL_GL_RED_SIZE, SDL_GL_GREEN_SIZE, SDL_GL_BLUE_SIZE, SDL_GL_ALPHA_SIZE, SDL_GL_BUFFER_SIZE,
    SDL_GL_DOUBLEBUFFER, SDL_GL_DEPTH_SIZE, SDL_GL_STENCIL_SIZE, SDL_GL_ACCUM_RED_SIZE,
    SDL_GL_ACCUM_GREEN_SIZE, SDL_GL_ACCUM_BLUE_SIZE, SDL_GL_ACCUM_ALPHA_SIZE, SDL_GL_STEREO,
    SDL_GL_MULTISAMPLEBUFFERS, SDL_GL_MULTISAMPLESAMPLES, SDL_GL_ACCELERATED_VISUAL,
    SDL_GL_RETAINED_BACKING, SDL_GL_CONTEXT_MAJOR_VERSION, SDL_GL_CONTEXT_MINOR_VERSION,
    SDL_GL_CONTEXT_EGL, SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_PROFILE_MASK,
    SDL_GL_SHARE_WITH_CURRENT_CONTEXT, SDL_GL_FRAMEBUFFER_SRGB_CAPABLE, SDL_GL_ATTR_COUNT = 32
};

static int attrs[SDL_GL_ATTR_COUNT] = {
    [SDL_GL_RED_SIZE] = 3, [SDL_GL_GREEN_SIZE] = 3, [SDL_GL_BLUE_SIZE] = 2,
    [SDL_GL_DOUBLEBUFFER] = 1, [SDL_GL_DEPTH_SIZE] = 16, [SDL_GL_ACCELERATED_VISUAL] = 1,
};

/* Our window: one, as in OpenMW. */
static SDL_Window *gl_window;
static CAMetalLayer *gl_layer;
static wmgl_surface *gl_surface;
static int gl_format, gl_width, gl_height, gl_interval = 1;
static struct wmgl_pixel_format gl_pf;
static __thread SDL_Window *current_window;
static __thread SDL_GLContext current_context;

int SDL_GL_SetAttribute(int attr, int value)
{
    TRACE("SDL_GL_SetAttribute(%d, %d)", attr, value);
    if (attr < 0 || attr >= SDL_GL_ATTR_COUNT) return SDL_SetError("Unknown OpenGL attribute");
    attrs[attr] = value;
    return 0;
}

void SDL_GL_ResetAttributes(void)
{
    memset(attrs, 0, sizeof(attrs));
    attrs[SDL_GL_RED_SIZE] = attrs[SDL_GL_GREEN_SIZE] = 3;
    attrs[SDL_GL_BLUE_SIZE] = 2;
    attrs[SDL_GL_DOUBLEBUFFER] = attrs[SDL_GL_ACCELERATED_VISUAL] = 1;
    attrs[SDL_GL_DEPTH_SIZE] = 16;
}

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

static int create_surface(SDL_Window *window)
{
    SDL_SysWMinfo info;
    __block NSView *view;

    memset(&info, 0, sizeof(info));
    info.version = (SDL_version){2, 32, 10};
    if (!SDL_GetWindowWMInfo(window, &info) || !info.info.cocoa.window)
        return SDL_SetError("wmglhost: no Cocoa window");
    gl_format = choose_format();
    SDL_GL_GetDrawableSize(window, &gl_width, &gl_height);
    on_main(^{
        NSWindow *nswindow = info.info.cocoa.window;
        view = nswindow.contentView;
        view.wantsLayer = YES;
        gl_layer = [CAMetalLayer layer];
        gl_layer.device = MTLCreateSystemDefaultDevice();
        gl_layer.contentsScale = nswindow.backingScaleFactor;
        gl_layer.frame = view.layer.bounds;
        gl_layer.autoresizingMask = kCALayerWidthSizable | kCALayerHeightSizable;
        [view.layer addSublayer:gl_layer];
    });
    if (!(gl_surface = core.surface_create_window((__bridge void *)gl_layer, gl_format, gl_width, gl_height)))
        return SDL_SetError("wmglhost: wmgl_surface_create_window failed");
    gl_window = window;
    TRACE("surface %p on layer %p, %dx%d", (void *)gl_surface, (__bridge void *)gl_layer, gl_width, gl_height);
    return 0;
}

SDL_GLContext SDL_GL_CreateContext(SDL_Window *window)
{
    int list[16], n = 0, error = 0;
    wmgl_context *share = attrs[SDL_GL_SHARE_WITH_CURRENT_CONTEXT] ? current_context : NULL;
    wmgl_context *context;

    if (!gl_surface && create_surface(window) < 0) return NULL;
    if (attrs[SDL_GL_CONTEXT_MAJOR_VERSION])
    {
        list[n++] = 0x2091; list[n++] = attrs[SDL_GL_CONTEXT_MAJOR_VERSION];
        list[n++] = 0x2092; list[n++] = attrs[SDL_GL_CONTEXT_MINOR_VERSION];
    }
    if (attrs[SDL_GL_CONTEXT_PROFILE_MASK] & 3)
    {
        list[n++] = 0x9126; list[n++] = attrs[SDL_GL_CONTEXT_PROFILE_MASK] & 3;
    }
    if (attrs[SDL_GL_CONTEXT_FLAGS])
    {
        list[n++] = 0x2094; list[n++] = attrs[SDL_GL_CONTEXT_FLAGS] & 3;
    }
    list[n] = 0;
    if (!(context = core.context_create(gl_format, share, list, &error)))
    {
        SDL_SetError("wmglhost: wmgl_context_create failed (%d)", error);
        return NULL;
    }
    TRACE("SDL_GL_CreateContext -> %p (share %p)", (void *)context, (void *)share);
    if (core.make_current(context, gl_surface, gl_surface))
    {
        current_window = window;
        current_context = context;
    }
    return context;
}

int SDL_GL_MakeCurrent(SDL_Window *window, SDL_GLContext context)
{
    TRACE("SDL_GL_MakeCurrent(%p, %p) on %p", (void *)window, context, (void *)pthread_self());
    if (!context)
    {
        core.make_current(NULL, NULL, NULL);
        current_window = NULL;
        current_context = NULL;
        return 0;
    }
    if (!core.make_current(context, gl_surface, gl_surface)) return SDL_SetError("wmglhost: make current failed");
    current_window = window;
    current_context = context;
    return 0;
}

SDL_GLContext SDL_GL_GetCurrentContext(void) { return current_context; }
SDL_Window *SDL_GL_GetCurrentWindow(void) { return current_window; }

void SDL_GL_DeleteContext(SDL_GLContext context)
{
    TRACE("SDL_GL_DeleteContext(%p)", context);
    if (!context) return;
    if (context == current_context) SDL_GL_MakeCurrent(NULL, NULL);
    core.context_destroy(context);
}

int SDL_GL_SetSwapInterval(int interval)
{
    TRACE("SDL_GL_SetSwapInterval(%d)", interval);
    gl_interval = interval;
    if (gl_surface) core.surface_flush(gl_surface, WMGL_FLUSH_INTERVAL, gl_width, gl_height, gl_interval);
    return 0;
}

int SDL_GL_GetSwapInterval(void) { return gl_interval; }

/* WMGLHOST_DUMP=dir: every WMGLHOST_DUMP_EVERY (300) frames, the back buffer
 * as dir/frame-N.ppm, read before the swap. */
static void dump_frame(void)
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
    unsigned char *pixels = malloc((size_t)gl_width * gl_height * 4);
    readPixels(0, 0, gl_width, gl_height, 0x1908 /* GL_RGBA */, 0x1401, pixels);
    bindFramebuffer(0x8CA8, read_fb);
    readBuffer(read_buffer);
    snprintf(path, sizeof(path), "%s/frame-%lu.ppm", dir, frame);
    FILE *f = fopen(path, "wb");
    if (f)
    {
        fprintf(f, "P6\n%d %d\n255\n", gl_width, gl_height);
        for (int y = gl_height - 1; y >= 0; y--)
            for (int x = 0; x < gl_width; x++) fwrite(pixels + ((size_t)y * gl_width + x) * 4, 1, 3, f);
        fclose(f);
        TRACE("dumped %s", path);
    }
    free(pixels);
}

void SDL_GL_SwapWindow(SDL_Window *window)
{
    int width, height;

    if (window != gl_window || !gl_surface) return;
    SDL_GL_GetDrawableSize(window, &width, &height);
    if (width != gl_width || height != gl_height)
    {
        TRACE("resize %dx%d -> %dx%d", gl_width, gl_height, width, height);
        gl_width = width;
        gl_height = height;
        core.surface_flush(gl_surface, WMGL_FLUSH_UPDATED, width, height, gl_interval);
    }
    dump_frame();
    core.surface_swap(gl_surface);
}

void *SDL_GL_GetProcAddress(const char *proc) { return core.get_proc_address(proc); }
int SDL_GL_LoadLibrary(const char *path) { return 0; }
void SDL_GL_UnloadLibrary(void) {}

int SDL_GL_ExtensionSupported(const char *extension)
{
    const unsigned char *(*getStringi)(unsigned int, unsigned int) = core.get_proc_address("glGetStringi");
    void (*getIntegerv)(unsigned int, int *) = core.get_proc_address("glGetIntegerv");
    int count = 0;

    if (!current_context) return 0;
    getIntegerv(0x821D /* GL_NUM_EXTENSIONS */, &count);
    for (int i = 0; i < count; i++)
        if (!strcmp((const char *)getStringi(0x1F03 /* GL_EXTENSIONS */, i), extension)) return 1;
    return 0;
}

int SDL_GL_GetAttribute(int attr, int *value)
{
    if (!value) return -1;
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
    default: *value = attr >= 0 && attr < SDL_GL_ATTR_COUNT ? attrs[attr] : 0; break;
    }
    TRACE("SDL_GL_GetAttribute(%d) = %d", attr, *value);
    return 0;
}
