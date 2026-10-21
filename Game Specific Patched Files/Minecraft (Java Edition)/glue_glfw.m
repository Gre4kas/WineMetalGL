/* Native macOS host for WineMetalGL under GLFW programs (Minecraft Java
 * Edition up to 1.21.x, through LWJGL 3.3): stands in for libglfw
 * (re-exporting the real one, renamed libglfw-real.dylib) and fills
 * libwmglhost.dylib's GL stubs. Windows are created by the real GLFW without
 * a context (GLFW_NO_API); the OpenGL hints given are kept and make a
 * WineMetalGL context instead, on a CAMetalLayer over the window's content
 * view, as glue.m does for SDL3.
 *
 * WMGLHOST_CORE: the core to load (default: next to this library).
 * WMGLHOST_TRACE=1: log the context calls.
 * WMGLHOST_FRAMES=file: write each swap's time (seconds) to file.
 * WMGLHOST_SPIN=pixels: move the cursor that many pixels right every frame
 * (a camera turning at a steady rate, for reproducing motion artifacts).
 * WMGLHOST_DUMP=dir: save a frame every WMGLHOST_DUMP_EVERY (300) swaps. */
#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3native.h>
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

/* The real GLFW's functions this library overrides or calls. */
static struct
{
    void (*window_hint)(int, int);
    void (*default_window_hints)(void);
    GLFWwindow *(*create_window)(int, int, const char *, GLFWmonitor *, GLFWwindow *);
    void (*destroy_window)(GLFWwindow *);
    int (*get_window_attrib)(GLFWwindow *, int);
    GLFWcursorposfun (*set_cursor_pos_callback)(GLFWwindow *, GLFWcursorposfun);
} real;

static void *stubs; /* libwmglhost.dylib, for glfwGetProcAddress */
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
    Dl_info info;

    tracing = getenv("WMGLHOST_TRACE") && atoi(getenv("WMGLHOST_TRACE"));
    if (!path)
    {
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
    if (core.log_set_host) core.log_set_host(getprogname(), "native macOS (wmglhost, GLFW)");
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
    if (dladdr((void *)wmglhost_procs, &info)) stubs = dlopen(info.dli_fname, RTLD_NOW | RTLD_NOLOAD);
    /* glfwInit is the real GLFW's: its image has the functions overridden here. */
    void *glfw = dladdr((void *)glfwInit, &info) ? dlopen(info.dli_fname, RTLD_NOW | RTLD_NOLOAD) : NULL;
#define REAL(f, n) if (!glfw || !(*(void **)&real.f = dlsym(glfw, n))) { fprintf(stderr, "wmglhost: no real %s\n", n); abort(); }
    REAL(window_hint, "glfwWindowHint");
    REAL(default_window_hints, "glfwDefaultWindowHints");
    REAL(create_window, "glfwCreateWindow");
    REAL(destroy_window, "glfwDestroyWindow");
    REAL(get_window_attrib, "glfwGetWindowAttrib");
    REAL(set_cursor_pos_callback, "glfwSetCursorPosCallback");
#undef REAL
    TRACE("loaded %s, %u GL names", path, (unsigned int)NAME_COUNT);
}

/* The context hints, as GLFW keeps them. */
static struct
{
    int client_api, major, minor, profile, forward, debug;
    int red, green, blue, alpha, depth, stencil, samples, srgb;
} hints;

static void reset_hints(void)
{
    hints.client_api = GLFW_OPENGL_API;
    hints.major = 1;
    hints.minor = 0;
    hints.profile = GLFW_OPENGL_ANY_PROFILE;
    hints.forward = hints.debug = 0;
    hints.red = hints.green = hints.blue = hints.alpha = 8;
    hints.depth = 24;
    hints.stencil = 8;
    hints.samples = 0;
    hints.srgb = 0;
}

__attribute__((constructor)) static void init_hints(void) { reset_hints(); }

void glfwDefaultWindowHints(void)
{
    reset_hints();
    real.default_window_hints();
}

void glfwWindowHint(int hint, int value)
{
    switch (hint)
    {
    case GLFW_CLIENT_API: hints.client_api = value; return;
    case GLFW_CONTEXT_VERSION_MAJOR: hints.major = value; return;
    case GLFW_CONTEXT_VERSION_MINOR: hints.minor = value; return;
    case GLFW_OPENGL_PROFILE: hints.profile = value; return;
    case GLFW_OPENGL_FORWARD_COMPAT: hints.forward = value; return;
    case GLFW_CONTEXT_DEBUG: hints.debug = value; return;
    case GLFW_RED_BITS: hints.red = value; return;
    case GLFW_GREEN_BITS: hints.green = value; return;
    case GLFW_BLUE_BITS: hints.blue = value; return;
    case GLFW_ALPHA_BITS: hints.alpha = value; return;
    case GLFW_DEPTH_BITS: hints.depth = value; return;
    case GLFW_STENCIL_BITS: hints.stencil = value; return;
    case GLFW_SAMPLES: hints.samples = value; return;
    case GLFW_SRGB_CAPABLE: hints.srgb = value; return;
    default: real.window_hint(hint, value); return;
    }
}

/* Each GLFW window with an OpenGL context: its layer, surface and context. */
struct window
{
    GLFWwindow *window;
    CAMetalLayer *layer;
    wmgl_surface *surface;
    wmgl_context *context;
    int format, width, height, major, minor, profile, interval;
};
static struct window windows[8];
static __thread struct window *current;

static struct window *find_window(GLFWwindow *window)
{
    for (unsigned int i = 0; i < sizeof(windows) / sizeof(windows[0]); i++)
        if (window && windows[i].window == window) return &windows[i];
    return NULL;
}

/* The first onscreen format with at least the hinted bits, preferring exact
 * sample counts. */
static int choose_format(struct wmgl_pixel_format *chosen)
{
    unsigned int onscreen = 0, count = core.get_pixel_formats(NULL, 0, &onscreen);
    struct wmgl_pixel_format *formats = calloc(count, sizeof(*formats));
    int best = 0, want_samples = hints.samples > 1 ? hints.samples : 0;

    core.get_pixel_formats(formats, count, &onscreen);
    for (unsigned int i = 0; i < onscreen && !best; i++)
    {
        const struct wmgl_pixel_format *f = &formats[i];
        if (!f->draw_to_window || !f->double_buffer || f->float_components) continue;
        if (f->red_bits < hints.red || f->green_bits < hints.green || f->blue_bits < hints.blue ||
            f->alpha_bits < hints.alpha || f->depth_bits < hints.depth || f->stencil_bits < hints.stencil)
            continue;
        if ((f->samples > 1 ? f->samples : 0) != want_samples) continue;
        if (hints.srgb && !f->srgb_capable) continue;
        best = i + 1;
    }
    if (!best) best = 1;
    *chosen = formats[best - 1];
    free(formats);
    return best;
}

static void on_main(dispatch_block_t block)
{
    if ([NSThread isMainThread]) block();
    else dispatch_sync(dispatch_get_main_queue(), block);
}

GLFWwindow *glfwCreateWindow(int width, int height, const char *title, GLFWmonitor *monitor, GLFWwindow *share)
{
    struct window *w = NULL, *shared = find_window(share);
    struct wmgl_pixel_format pf;
    __block CAMetalLayer *layer;
    int list[16], n = 0, error = 0;

    if (hints.client_api == GLFW_NO_API) return real.create_window(width, height, title, monitor, share);
    for (unsigned int i = 0; i < sizeof(windows) / sizeof(windows[0]) && !w; i++)
        if (!windows[i].window) w = &windows[i];
    if (!w) return NULL;
    real.window_hint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWwindow *window = real.create_window(width, height, title, monitor, shared ? shared->window : NULL);
    real.window_hint(GLFW_CLIENT_API, GLFW_OPENGL_API);
    if (!window) return NULL;
    NSWindow *nswindow = glfwGetCocoaWindow(window);
    w->format = choose_format(&pf);
    glfwGetFramebufferSize(window, &w->width, &w->height);
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
    if (hints.major > 1 || hints.minor > 0)
    {
        list[n++] = 0x2091; list[n++] = hints.major;
        list[n++] = 0x2092; list[n++] = hints.minor;
    }
    if (hints.profile == GLFW_OPENGL_CORE_PROFILE || hints.profile == GLFW_OPENGL_COMPAT_PROFILE)
    {
        list[n++] = 0x9126; list[n++] = hints.profile == GLFW_OPENGL_CORE_PROFILE ? 1 : 2;
    }
    if (hints.forward || hints.debug)
    {
        list[n++] = 0x2094; list[n++] = (hints.forward ? 2 : 0) | (hints.debug ? 1 : 0);
    }
    list[n] = 0;
    w->surface = core.surface_create_window((__bridge void *)layer, w->format, w->width, w->height);
    w->context = w->surface ? core.context_create(w->format, shared ? shared->context : NULL, list, &error) : NULL;
    if (!w->context)
    {
        fprintf(stderr, "wmglhost: cannot create an OpenGL %d.%d context (%d)\n", hints.major, hints.minor, error);
        if (w->surface) core.surface_destroy(w->surface);
        on_main(^{ [layer removeFromSuperlayer]; });
        real.destroy_window(window);
        *w = (struct window){0};
        return NULL;
    }
    w->window = window;
    w->layer = layer;
    w->major = hints.major;
    w->minor = hints.minor;
    w->profile = hints.profile;
    w->interval = 0;
    core.surface_flush(w->surface, WMGL_FLUSH_INTERVAL, w->width, w->height, w->interval);
    TRACE("glfwCreateWindow %dx%d: OpenGL %d.%d profile 0x%x -> window %p, surface %p, context %p", width, height,
          hints.major, hints.minor, hints.profile, (void *)window, (void *)w->surface, (void *)w->context);
    return window;
}

void glfwDestroyWindow(GLFWwindow *window)
{
    struct window *w = find_window(window);

    if (w)
    {
        CAMetalLayer *layer = w->layer;
        TRACE("glfwDestroyWindow %p", (void *)window);
        if (current == w)
        {
            core.make_current(NULL, NULL, NULL);
            current = NULL;
        }
        core.context_destroy(w->context);
        core.surface_destroy(w->surface);
        on_main(^{ [layer removeFromSuperlayer]; });
        *w = (struct window){0};
    }
    real.destroy_window(window);
}

void glfwMakeContextCurrent(GLFWwindow *window)
{
    struct window *w = find_window(window);

    TRACE("glfwMakeContextCurrent %p on %p", (void *)window, (void *)pthread_self());
    if (!w)
    {
        core.make_current(NULL, NULL, NULL);
        current = NULL;
        return;
    }
    if (core.make_current(w->context, w->surface, w->surface)) current = w;
}

GLFWwindow *glfwGetCurrentContext(void) { return current ? current->window : NULL; }

void glfwSwapInterval(int interval)
{
    TRACE("glfwSwapInterval %d", interval);
    if (!current) return;
    current->interval = interval;
    core.surface_flush(current->surface, WMGL_FLUSH_INTERVAL, current->width, current->height, interval);
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

/* WMGLHOST_SPIN: the application's cursor callback, fed a steady motion. */
static GLFWcursorposfun cursor_callback;
static double spin_x;

GLFWcursorposfun glfwSetCursorPosCallback(GLFWwindow *window, GLFWcursorposfun callback)
{
    cursor_callback = callback;
    return real.set_cursor_pos_callback(window, callback);
}

static void spin(GLFWwindow *window)
{
    static double step = -1;
    if (step < 0) step = getenv("WMGLHOST_SPIN") ? atof(getenv("WMGLHOST_SPIN")) : 0;
    if (step <= 0 || !cursor_callback) return;
    spin_x += step;
    cursor_callback(window, spin_x, 0);
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
    }
    free(pixels);
}

void glfwSwapBuffers(GLFWwindow *window)
{
    struct window *w = find_window(window);
    int width, height;

    if (!w) return;
    glfwGetFramebufferSize(window, &width, &height);
    if (width != w->width || height != w->height)
    {
        TRACE("resize %dx%d -> %dx%d", w->width, w->height, width, height);
        w->width = width;
        w->height = height;
        core.surface_flush(w->surface, WMGL_FLUSH_UPDATED, width, height, w->interval);
    }
    dump_frame(w);
    core.surface_swap(w->surface);
    log_frame();
    spin(window);
}

/* The stub when there is one, so it matches the GL library's
 * (org.lwjgl.opengl.libname, libwmglhost.dylib). */
GLFWglproc glfwGetProcAddress(const char *proc)
{
    void *address = stubs ? dlsym(stubs, proc) : NULL;
    return (GLFWglproc)(address ? address : core.get_proc_address(proc));
}

int glfwExtensionSupported(const char *extension)
{
    const unsigned char *(*getStringi)(unsigned int, unsigned int) = core.get_proc_address("glGetStringi");
    void (*getIntegerv)(unsigned int, int *) = core.get_proc_address("glGetIntegerv");
    int count = 0;

    if (!current) return GLFW_FALSE;
    getIntegerv(0x821D /* GL_NUM_EXTENSIONS */, &count);
    for (int i = 0; i < count; i++)
        if (!strcmp((const char *)getStringi(0x1F03 /* GL_EXTENSIONS */, i), extension)) return GLFW_TRUE;
    return GLFW_FALSE;
}

int glfwGetWindowAttrib(GLFWwindow *window, int attrib)
{
    struct window *w = find_window(window);

    if (w)
    {
        switch (attrib)
        {
        case GLFW_CLIENT_API: return GLFW_OPENGL_API;
        case GLFW_CONTEXT_CREATION_API: return GLFW_NATIVE_CONTEXT_API;
        case GLFW_CONTEXT_VERSION_MAJOR: return w->major;
        case GLFW_CONTEXT_VERSION_MINOR: return w->minor;
        case GLFW_OPENGL_PROFILE: return w->profile;
        default: break;
        }
    }
    return real.get_window_attrib(window, attrib);
}
