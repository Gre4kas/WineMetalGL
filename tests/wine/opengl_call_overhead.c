/* What one GL call costs from a Windows program, across opengl32 and the
 * Wine module into the core: a million glIsEnabled calls (each returns a
 * value, so each crosses into the core) and a million glColorMask calls (no
 * value). Then a few frames of state changes and draws, so
 * WINEMETALGL_STATS=1 prints per-frame statistics. Prints
 * WINEMETALGL_CALL_OVERHEAD_OK with ns per call; not part of the gate. */
#include <windows.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <stdio.h>

static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcA(h, m, w, l); }

static double seconds(void)
{
    LARGE_INTEGER now, frequency;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    return (double)now.QuadPart / (double)frequency.QuadPart;
}

int main(void)
{
    enum { kCalls = 1000000, kFrames = 8, kDrawsPerFrame = 200 };
    WNDCLASSA wc = {0};
    PIXELFORMATDESCRIPTOR pfd = {sizeof(pfd), 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER,
                                 PFD_TYPE_RGBA, 32};
    HWND hwnd;
    HDC dc;
    HGLRC context;
    double start, isEnabled, colorMask;
    volatile GLboolean sink = 0;
    int i, frame;

    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "wmgl";
    wc.style = CS_OWNDC;
    RegisterClassA(&wc);
    hwnd = CreateWindowA("wmgl", "wmgl", WS_OVERLAPPEDWINDOW, 0, 0, 320, 240, NULL, NULL, wc.hInstance, NULL);
    ShowWindow(hwnd, SW_SHOW);
    dc = GetDC(hwnd);
    SetPixelFormat(dc, ChoosePixelFormat(dc, &pfd), &pfd);
    if (!(context = wglCreateContext(dc)) || !wglMakeCurrent(dc, context)) { printf("no context\n"); return 1; }
    printf("%u-bit\n", (unsigned)sizeof(void *) * 8);

    for (i = 0; i < 10000; i++) sink ^= glIsEnabled(GL_BLEND);  /* warm-up */
    start = seconds();
    for (i = 0; i < kCalls; i++) sink ^= glIsEnabled(GL_BLEND);
    isEnabled = (seconds() - start) * 1e9 / kCalls;
    start = seconds();
    for (i = 0; i < kCalls; i++) glColorMask(i & 1, GL_TRUE, GL_TRUE, GL_TRUE);
    glFinish();
    colorMask = (seconds() - start) * 1e9 / kCalls;
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    printf("glIsEnabled %.1f ns/call, glColorMask %.1f ns/call\n", isEnabled, colorMask);

    /* Frames with state changes and immediate-mode draws, for the statistics. */
    for (frame = 0; frame < kFrames; frame++)
    {
        glClearColor(0.1f, 0.2f, 0.3f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        for (i = 0; i < kDrawsPerFrame; i++)
        {
            if (i & 1) glEnable(GL_BLEND); else glDisable(GL_BLEND);
            glColor3f((float)(i % 7) / 7.0f, 0.5f, 0.25f);
            glBegin(GL_TRIANGLES);
            glVertex2f(-0.5f, -0.5f);
            glVertex2f(0.5f, -0.5f);
            glVertex2f(0.0f, 0.5f);
            glEnd();
        }
        SwapBuffers(dc);
    }
    printf("WINEMETALGL_CALL_OVERHEAD_OK %.1f %.1f\n", isEnabled, colorMask);
    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(context);
    ReleaseDC(hwnd, dc);
    DestroyWindow(hwnd);
    return sink == 2;
}
