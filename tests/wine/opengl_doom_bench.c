/* Frame times of tests/wine/doom_frame.h's stand-in for a DOOM (2016) frame
 * (issue #36), through opengl32 and the Wine module: the path a game's GL
 * calls take, under Rosetta for x86_64 Wine. For A/B comparisons of cores;
 * not part of the gate.
 *
 *   opengl_doom_bench.exe [frames] [width height]
 *
 * WMGL_DOOM_SKIP leaves parts of the frame out (doom_frame.h). WMGL_DOOM_BUSY=N
 * runs N threads that spin through the run, as a game's worker threads load
 * every core. Prints the
 * CPU wall time between presents (swap interval 0): mean, median and 95th
 * percentile, frames per second, and the time spent building a frame
 * (before SwapBuffers) and per draw, then
 * WINEMETALGL_DOOM_BENCH_OK.
 */
#include <windows.h>
#include <GL/gl.h>
#include <stdio.h>
#include <stdlib.h>

#include "doom_frame.h"

#define WGL_CONTEXT_MAJOR_VERSION_ARB 0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB 0x2092
#define WGL_CONTEXT_PROFILE_MASK_ARB 0x9126
#define WGL_CONTEXT_CORE_PROFILE_BIT_ARB 0x00000001
typedef HGLRC (WINAPI *CreateAttribs)(HDC, HGLRC, const int *);
typedef BOOL (WINAPI *SwapInterval)(int);

static HMODULE opengl32;

static void *lookup(const char *name)
{
    void *p = (void *)wglGetProcAddress(name);
    /* OpenGL 1.1 commands come from opengl32's exports. */
    if (!p || p == (void *)1 || p == (void *)2 || p == (void *)3 || p == (void *)-1)
        p = (void *)GetProcAddress(opengl32, name);
    return p;
}

static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcA(h, m, w, l); }

static volatile LONG stop;

static DWORD WINAPI spin(void *arg)
{
    volatile unsigned x = 0;
    (void)arg;
    while (!stop) x = x * 1664525u + 1013904223u;
    return 0;
}

static int compare(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

int main(int argc, char **argv)
{
    PIXELFORMATDESCRIPTOR pfd = {sizeof(pfd), 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 32};
    const int attribs[] = {WGL_CONTEXT_MAJOR_VERSION_ARB, 4, WGL_CONTEXT_MINOR_VERSION_ARB, 5,
                           WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB, 0};
    const int frames = argc > 1 && atoi(argv[1]) > 0 ? atoi(argv[1]) : 300, warmup = 60;
    const int width = argc > 3 ? atoi(argv[2]) : 1920, height = argc > 3 ? atoi(argv[3]) : 1080;
    static DoomFrame frame;
    DoomGL gl;
    WNDCLASSA wc = {0};
    HWND hwnd;
    HDC dc;
    HGLRC legacy, context;
    CreateAttribs create;
    SwapInterval interval;
    LARGE_INTEGER freq, last, now, start;
    double *times = malloc(sizeof(double) * (size_t)frames), sum = 0, build = 0;
    LARGE_INTEGER built;
    const char *missing;
    int f, draws = 0;

    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "wmgldoom";
    wc.style = CS_OWNDC;
    RegisterClassA(&wc);
    hwnd = CreateWindowA("wmgldoom", "wmgl doom bench", WS_POPUP | WS_VISIBLE, 0, 0, width, height, NULL, NULL,
                         wc.hInstance, NULL);
    dc = GetDC(hwnd);
    SetPixelFormat(dc, ChoosePixelFormat(dc, &pfd), &pfd);
    legacy = wglCreateContext(dc);
    wglMakeCurrent(dc, legacy);
    create = (CreateAttribs)wglGetProcAddress("wglCreateContextAttribsARB");
    if (!create || !(context = create(dc, NULL, attribs)) || !wglMakeCurrent(dc, context)) {
        printf("no 4.5 context\n");
        return 1;
    }
    wglDeleteContext(legacy);
    opengl32 = GetModuleHandleA("opengl32.dll");
    if ((missing = doom_gl_load(&gl, lookup))) {
        printf("missing %s\n", missing);
        return 2;
    }
    if ((interval = (SwapInterval)wglGetProcAddress("wglSwapIntervalEXT"))) interval(0);
    doom_setup(&frame, &gl, width, height);

    if (getenv("WMGL_DOOM_BUSY"))
        for (f = atoi(getenv("WMGL_DOOM_BUSY")); f > 0; --f) CreateThread(NULL, 0, spin, NULL, 0, NULL);
    QueryPerformanceFrequency(&freq);
    for (f = -warmup; f < frames; ++f) {
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) DispatchMessageA(&msg);
        if (f == 0) {
            QueryPerformanceCounter(&start);
            last = start;
        }
        doom_run_frame(&frame);
        draws = frame.draws;
        QueryPerformanceCounter(&built);
        SwapBuffers(dc);
        if (f >= 0) {
            QueryPerformanceCounter(&now);
            build += (double)(built.QuadPart - last.QuadPart) * 1000.0 / (double)freq.QuadPart;
            times[f] = (double)(now.QuadPart - last.QuadPart) * 1000.0 / (double)freq.QuadPart;
            last = now;
        }
    }
    gl.Finish();
    QueryPerformanceCounter(&now);
    stop = 1;
    for (f = 0; f < frames; ++f) sum += times[f];
    qsort(times, (size_t)frames, sizeof(double), compare);
    printf("BENCH doom frames=%d draws=%d mean=%.2f p50=%.2f p95=%.2f fps=%.1f cpu=%.2f cpu_us_per_draw=%.2f error=0x%x\n",
           frames, draws, sum / frames, times[frames / 2], times[frames * 95 / 100],
           frames * (double)freq.QuadPart / (double)(now.QuadPart - start.QuadPart), build / frames,
           build / frames * 1000.0 / draws, gl.GetError());
    printf("WINEMETALGL_DOOM_BENCH_OK\n");
    return 0;
}
