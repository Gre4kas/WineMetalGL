/* WineMetalGL driver ABI: the C interface winemac.drv uses to drive the
 * WineMetalGL core (winemetalgl.dylib). winemac loads the library with
 * dlopen and resolves these symbols with dlsym; GL entry points are obtained
 * through wmgl_get_proc_address, never as exported symbols.
 *
 * Every function is called on the Unix side of Wine. Pointers are host
 * pointers. Contexts and surfaces are opaque handles owned by the core.
 *
 * Bump WMGL_DRIVER_ABI_VERSION whenever a struct or signature changes; the
 * core refuses to initialise for a different version.
 */
#ifndef WINEMETALGL_DRIVER_H
#define WINEMETALGL_DRIVER_H

#ifdef __cplusplus
extern "C" {
#endif

#define WMGL_DRIVER_ABI_VERSION 1

struct wmgl_pixel_format
{
    int draw_to_window;
    int draw_to_pbuffer;
    int double_buffer;
    int stereo;
    int color_bits;        /* red + green + blue */
    int red_bits, green_bits, blue_bits, alpha_bits;
    int red_shift, green_shift, blue_shift, alpha_shift;
    int depth_bits;
    int stencil_bits;
    int accum_bits;        /* total accumulation bits (compatibility profile) */
    int aux_buffers;
    int sample_buffers;
    int samples;
    int srgb_capable;
    int float_components;
    int max_pbuffer_width, max_pbuffer_height, max_pbuffer_pixels;
};

/* Context creation errors reported through *error. */
#define WMGL_ERROR_NONE             0
#define WMGL_ERROR_INVALID_VERSION  1  /* ERROR_INVALID_VERSION_ARB */
#define WMGL_ERROR_INVALID_PROFILE  2  /* ERROR_INVALID_PROFILE_ARB */
#define WMGL_ERROR_INVALID_ATTRIB   3
#define WMGL_ERROR_BAD_SHARE        4

/* Flags for wmgl_surface_flush (mirror win32u's GL_FLUSH_* bits). */
#define WMGL_FLUSH_FINISHED  0x01
#define WMGL_FLUSH_INTERVAL  0x02
#define WMGL_FLUSH_UPDATED   0x04
#define WMGL_FLUSH_PRESENT   0x08

typedef struct wmgl_context wmgl_context;
typedef struct wmgl_surface wmgl_surface;

/* Returns 0 on success. */
int wmgl_initialize(unsigned int abi_version);

/* Fills up to max formats; returns the total count. *onscreen receives the
 * number of formats (at the start of the list) usable for windows. Format
 * numbers passed back to the core are 1-based indices into this list. */
unsigned int wmgl_get_pixel_formats(struct wmgl_pixel_format *formats, unsigned int max, unsigned int *onscreen);

/* layer is a CAMetalLayer*; width/height are in device pixels. */
wmgl_surface *wmgl_surface_create_window(void *layer, int format, int width, int height);
wmgl_surface *wmgl_surface_create_pbuffer(int format, int width, int height);
void wmgl_surface_destroy(wmgl_surface *surface);
void wmgl_surface_flush(wmgl_surface *surface, unsigned int flags, int width, int height, int interval);
int wmgl_surface_swap(wmgl_surface *surface);

/* attribs: WGL_CONTEXT_* name/value pairs terminated by 0, may be NULL. */
wmgl_context *wmgl_context_create(int format, wmgl_context *share, const int *attribs, int *error);
void wmgl_context_destroy(wmgl_context *context);
/* Makes the context current on the calling thread (NULL releases). */
int wmgl_make_current(wmgl_context *context, wmgl_surface *draw, wmgl_surface *read);

void *wmgl_get_proc_address(const char *name);

/* Renderer information for WGL_WINE_query_renderer. */
const char *wmgl_renderer_string(void);
unsigned int wmgl_video_memory_mb(void);
void wmgl_max_versions(int *core_major, int *core_minor, int *compat_major, int *compat_minor);

#ifdef __cplusplus
}
#endif

#endif /* WINEMETALGL_DRIVER_H */
