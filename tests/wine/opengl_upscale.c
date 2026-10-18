/* UpscaleGL (WINEMETALGL_UPSCALE) as a fullscreen program sees it, and the
 * frame time of a GPU-bound fullscreen frame with and without it. Not part
 * of the gate.
 *
 *   opengl_upscale.exe [frames]
 *
 * Checks that the screen it is shown agrees everywhere (system metrics, the
 * current mode, the largest listed mode, the monitor), that a popup window
 * covering it gets that client size, that cursor positions round-trip in its
 * coordinates, and (with UpscaleGL on) that a change to a smaller listed mode
 * and back is taken.
 * Then draws a fragment-heavy full-screen pass for [frames] frames (swap
 * interval 0) and prints
 *   BENCH upscale setting=<WINEMETALGL_UPSCALE> screen=WxH mean=<ms> fps=<n>
 * then WINEMETALGL_UPSCALE_OK.
 */
#include <windows.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WGL_CONTEXT_MAJOR_VERSION_ARB 0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB 0x2092
#define WGL_CONTEXT_PROFILE_MASK_ARB 0x9126
#define WGL_CONTEXT_CORE_PROFILE_BIT_ARB 0x00000001
typedef HGLRC (WINAPI *CreateAttribs)(HDC, HGLRC, const int *);
typedef BOOL (WINAPI *SwapInterval)(int);

static PFNGLCREATESHADERPROC pCreateShader;
static PFNGLSHADERSOURCEPROC pShaderSource;
static PFNGLCOMPILESHADERPROC pCompileShader;
static PFNGLCREATEPROGRAMPROC pCreateProgram;
static PFNGLATTACHSHADERPROC pAttachShader;
static PFNGLLINKPROGRAMPROC pLinkProgram;
static PFNGLUSEPROGRAMPROC pUseProgram;
static PFNGLGETPROGRAMIVPROC pGetProgramiv;
static PFNGLGENVERTEXARRAYSPROC pGenVertexArrays;
static PFNGLBINDVERTEXARRAYPROC pBindVertexArray;
static PFNGLGETUNIFORMLOCATIONPROC pGetUniformLocation;
static PFNGLUNIFORM1FPROC pUniform1f;

static int failures;

static void check(int ok, const char *what)
{
    if (ok) return;
    printf("FAIL %s\n", what);
    failures++;
}

static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcA(h, m, w, l); }

static int close_to(long a, long b) { return a - b <= 1 && b - a <= 1; }

static GLuint shader(GLenum type, const char *source)
{
    GLuint s = pCreateShader(type);
    pShaderSource(s, 1, &source, NULL);
    pCompileShader(s);
    return s;
}

static void check_display(int *width, int *height)
{
    DEVMODEA mode = {0}, largest = {0}, listed = {0};
    MONITORINFO monitor = {sizeof(monitor)};
    POINT origin = {0, 0};
    DWORD i;

    *width = GetSystemMetrics(SM_CXSCREEN);
    *height = GetSystemMetrics(SM_CYSCREEN);
    mode.dmSize = sizeof(mode);
    check(EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &mode), "current mode");
    check((int)mode.dmPelsWidth == *width && (int)mode.dmPelsHeight == *height, "current mode is the screen");
    for (i = 0;; i++) {
        listed.dmSize = sizeof(listed);
        if (!EnumDisplaySettingsA(NULL, i, &listed)) break;
        if (listed.dmPelsWidth * listed.dmPelsHeight > largest.dmPelsWidth * largest.dmPelsHeight) largest = listed;
    }
    check(i > 0, "listed modes");
    check((int)largest.dmPelsWidth <= *width && (int)largest.dmPelsHeight <= *height, "no listed mode beyond the screen");
    check(GetMonitorInfoA(MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY), &monitor), "monitor info");
    check(monitor.rcMonitor.right == *width && monitor.rcMonitor.bottom == *height, "monitor is the screen");
}

static void check_window(HWND hwnd, int width, int height)
{
    RECT client, window;
    POINT p;

    GetClientRect(hwnd, &client);
    GetWindowRect(hwnd, &window);
    check(client.right == width && client.bottom == height, "client area is the screen");
    check(window.left == 0 && window.top == 0 && window.right == width && window.bottom == height, "window is the screen");
    check(SetCursorPos(width / 4, height / 4), "set cursor");
    check(GetCursorPos(&p) && close_to(p.x, width / 4) && close_to(p.y, height / 4), "cursor round trip");
    p.x = width / 2;
    p.y = height / 2;
    check(ScreenToClient(hwnd, &p) && close_to(p.x, width / 2) && close_to(p.y, height / 2), "screen to client");
    check(ClientToScreen(hwnd, &p) && close_to(p.x, width / 2) && close_to(p.y, height / 2), "client to screen");
}

/* A smaller listed mode, set and the window fitted to it, then back. */
static void check_mode_change(HWND hwnd, int width, int height)
{
    DEVMODEA mode = {0};
    RECT client;
    DWORD i;

    for (i = 0;; i++) {
        mode.dmSize = sizeof(mode);
        if (!EnumDisplaySettingsA(NULL, i, &mode)) return;
        if ((int)mode.dmPelsWidth < width && (int)mode.dmPelsHeight < height && mode.dmPelsWidth >= 320) break;
    }
    mode.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT;
    check(ChangeDisplaySettingsA(&mode, CDS_FULLSCREEN) == DISP_CHANGE_SUCCESSFUL, "smaller mode");
    check(GetSystemMetrics(SM_CXSCREEN) == (int)mode.dmPelsWidth, "screen is the smaller mode");
    SetWindowPos(hwnd, NULL, 0, 0, mode.dmPelsWidth, mode.dmPelsHeight, SWP_NOZORDER);
    GetClientRect(hwnd, &client);
    check(client.right == (LONG)mode.dmPelsWidth && client.bottom == (LONG)mode.dmPelsHeight, "client area in the smaller mode");
    check(ChangeDisplaySettingsA(NULL, 0) == DISP_CHANGE_SUCCESSFUL, "mode back");
    check(GetSystemMetrics(SM_CXSCREEN) == width, "screen back");
    SetWindowPos(hwnd, NULL, 0, 0, width, height, SWP_NOZORDER);
}

int main(int argc, char **argv)
{
    static const char *vs = "#version 330 core\n"
                            "void main() { vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);\n"
                            "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0); }\n";
    /* About as much work a pixel as a heavy lighting pass. */
    static const char *fs = "#version 330 core\n"
                            "uniform float t; out vec4 color;\n"
                            "void main() { vec2 p = gl_FragCoord.xy * 0.01; float v = 0.0;\n"
                            "  for (int i = 0; i < 48; ++i) { v += sin(p.x * float(i) + t) * cos(p.y * float(i) - t); p = p.yx * 1.01; }\n"
                            "  color = vec4(0.5 + 0.5 * sin(v), 0.5 + 0.5 * cos(v), 0.5, 1.0); }\n";
    PIXELFORMATDESCRIPTOR pfd = {sizeof(pfd), 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 32};
    const int attribs[] = {WGL_CONTEXT_MAJOR_VERSION_ARB, 3, WGL_CONTEXT_MINOR_VERSION_ARB, 3,
                           WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB, 0};
    const int frames = argc > 1 && atoi(argv[1]) > 0 ? atoi(argv[1]) : 300, warmup = 30;
    const char *setting = getenv("WINEMETALGL_UPSCALE");
    WNDCLASSA wc = {0};
    LARGE_INTEGER freq, start, now;
    HGLRC legacy, context;
    CreateAttribs create;
    SwapInterval interval;
    GLuint program, vao;
    GLint linked = 0, t;
    int width, height, f;
    HWND hwnd;
    HDC dc;

    check_display(&width, &height);
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "wmglupscale";
    wc.style = CS_OWNDC;
    RegisterClassA(&wc);
    hwnd = CreateWindowA("wmglupscale", "wmgl upscale", WS_POPUP | WS_VISIBLE, 0, 0, width, height, NULL, NULL,
                         wc.hInstance, NULL);
    check(hwnd != NULL, "window");
    check_window(hwnd, width, height);
    /* Without UpscaleGL a mode change is a real one: not for a test. */
    if (setting && *setting && strcmp(setting, "off") && strcmp(setting, "0")) {
        check_mode_change(hwnd, width, height);
        check_window(hwnd, width, height);
    }

    dc = GetDC(hwnd);
    SetPixelFormat(dc, ChoosePixelFormat(dc, &pfd), &pfd);
    legacy = wglCreateContext(dc);
    wglMakeCurrent(dc, legacy);
    create = (CreateAttribs)wglGetProcAddress("wglCreateContextAttribsARB");
    if (!create || !(context = create(dc, NULL, attribs)) || !wglMakeCurrent(dc, context)) {
        printf("no 3.3 context\n");
        return 1;
    }
    wglDeleteContext(legacy);
#define LOAD(p, name) p = (void *)wglGetProcAddress(name)
    LOAD(pCreateShader, "glCreateShader");
    LOAD(pShaderSource, "glShaderSource");
    LOAD(pCompileShader, "glCompileShader");
    LOAD(pCreateProgram, "glCreateProgram");
    LOAD(pAttachShader, "glAttachShader");
    LOAD(pLinkProgram, "glLinkProgram");
    LOAD(pUseProgram, "glUseProgram");
    LOAD(pGetProgramiv, "glGetProgramiv");
    LOAD(pGenVertexArrays, "glGenVertexArrays");
    LOAD(pBindVertexArray, "glBindVertexArray");
    LOAD(pGetUniformLocation, "glGetUniformLocation");
    LOAD(pUniform1f, "glUniform1f");
#undef LOAD
    if ((interval = (SwapInterval)wglGetProcAddress("wglSwapIntervalEXT"))) interval(0);
    program = pCreateProgram();
    pAttachShader(program, shader(GL_VERTEX_SHADER, vs));
    pAttachShader(program, shader(GL_FRAGMENT_SHADER, fs));
    pLinkProgram(program);
    pGetProgramiv(program, GL_LINK_STATUS, &linked);
    check(linked, "program");
    pUseProgram(program);
    t = pGetUniformLocation(program, "t");
    pGenVertexArrays(1, &vao);
    pBindVertexArray(vao);
    glViewport(0, 0, width, height);

    QueryPerformanceFrequency(&freq);
    for (f = -warmup; f < frames; ++f) {
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) DispatchMessageA(&msg);
        if (f == 0) {
            glFinish();
            QueryPerformanceCounter(&start);
        }
        pUniform1f(t, (float)f * 0.01f);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        SwapBuffers(dc);
    }
    glFinish();
    QueryPerformanceCounter(&now);
    check(glGetError() == GL_NO_ERROR, "no GL error");
    {
        const double ms = (double)(now.QuadPart - start.QuadPart) * 1000.0 / (double)freq.QuadPart / frames;
        printf("BENCH upscale setting=%s screen=%dx%d mean=%.2f fps=%.1f\n", setting ? setting : "off", width, height,
               ms, 1000.0 / ms);
    }
    if (!failures) printf("WINEMETALGL_UPSCALE_OK\n");
    return failures != 0;
}
