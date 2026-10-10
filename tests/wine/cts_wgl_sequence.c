/* Reproduces the Khronos CTS WGL platform sequence (framework/platform/win32
 * tcuWGL.cpp): a temporary 1x1 window and legacy context to load the WGL ARB
 * entry points, then a real window with wglChoosePixelFormatARB-style
 * selection, wglCreateContextAttribsARB, swap interval 0, and per-iteration
 * clear + wglSwapLayerBuffers + message pumping. Prints a marker after each
 * step so a failure can be located.
 * Prints WMGL_CTS_SEQUENCE_OK on success. */
#include <windows.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/wglext.h>
#include <stdio.h>

#define STEP(...) do { printf(__VA_ARGS__); printf("\n"); fflush(stdout); } while (0)

static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcA(h, m, w, l); }

static HWND make_window(int w, int h) {
    static int registered;
    if (!registered) {
        WNDCLASSA wc = {0};
        wc.lpfnWndProc = proc; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "wmgl_cts"; wc.style = CS_OWNDC;
        RegisterClassA(&wc);
        registered = 1;
    }
    HWND hwnd = CreateWindowA("wmgl_cts", "wmgl_cts", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, w, h, NULL, NULL,
                              GetModuleHandleA(NULL), NULL);
    ShowWindow(hwnd, SW_SHOW);
    return hwnd;
}

static void pump(HWND hwnd) {
    MSG msg;
    while (PeekMessageA(&msg, hwnd, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
}

int main(void) {
    HMODULE gl = LoadLibraryA("opengl32.dll");
    typedef BOOL(WINAPI * SwapLayer)(HDC, UINT);
    SwapLayer swapLayerBuffers = (SwapLayer)GetProcAddress(gl, "wglSwapLayerBuffers");
    STEP("step 1: loaded opengl32, swapLayerBuffers=%p", (void*)swapLayerBuffers);

    /* Temporary context for loading extension functions. */
    HWND tmp = make_window(1, 1);
    HDC tmpDc = GetDC(tmp);
    PIXELFORMATDESCRIPTOR pfd = {sizeof(pfd), 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, PFD_TYPE_RGBA};
    pfd.iLayerType = PFD_MAIN_PLANE;
    int tmpFormat = ChoosePixelFormat(tmpDc, &pfd);
    SetPixelFormat(tmpDc, tmpFormat, &pfd);
    HGLRC tmpCtx = wglCreateContext(tmpDc);
    BOOL ok = tmpCtx && wglMakeCurrent(tmpDc, tmpCtx);
    STEP("step 2: temporary context %p current %d", (void*)tmpCtx, ok);
    PFNWGLGETPIXELFORMATATTRIBIVARBPROC getAttribiv = (PFNWGLGETPIXELFORMATATTRIBIVARBPROC)wglGetProcAddress("wglGetPixelFormatAttribivARB");
    PFNWGLGETPIXELFORMATATTRIBFVARBPROC getAttribfv = (PFNWGLGETPIXELFORMATATTRIBFVARBPROC)wglGetProcAddress("wglGetPixelFormatAttribfvARB");
    PFNWGLCHOOSEPIXELFORMATARBPROC choose = (PFNWGLCHOOSEPIXELFORMATARBPROC)wglGetProcAddress("wglChoosePixelFormatARB");
    PFNWGLCREATECONTEXTATTRIBSARBPROC create = (PFNWGLCREATECONTEXTATTRIBSARBPROC)wglGetProcAddress("wglCreateContextAttribsARB");
    PFNWGLGETEXTENSIONSSTRINGARBPROC extString = (PFNWGLGETEXTENSIONSSTRINGARBPROC)wglGetProcAddress("wglGetExtensionsStringARB");
    PFNWGLSWAPINTERVALEXTPROC swapInterval = (PFNWGLSWAPINTERVALEXTPROC)wglGetProcAddress("wglSwapIntervalEXT");
    STEP("step 3: iv=%p fv=%p choose=%p create=%p ext=%p swap=%p", (void*)getAttribiv, (void*)getAttribfv, (void*)choose,
         (void*)create, (void*)extString, (void*)swapInterval);
    wglMakeCurrent(tmpDc, NULL);
    wglDeleteContext(tmpCtx);
    ReleaseDC(tmp, tmpDc);
    DestroyWindow(tmp);
    STEP("step 4: temporary context and window destroyed");
    if (!getAttribiv || !create) return 1;

    HWND hwnd = make_window(400, 300);
    HDC dc = GetDC(hwnd);
    /* Enumerate formats like tcuWGL::choosePixelFormat. */
    int attrib = WGL_NUMBER_PIXEL_FORMATS_ARB, count = 0;
    getAttribiv(dc, 0, 0, 1, &attrib, &count);
    int chosen = -1;
    for (int i = 1; i <= count && chosen < 0; ++i) {
        int names[] = {WGL_SUPPORT_OPENGL_ARB, WGL_ACCELERATION_ARB, WGL_PIXEL_TYPE_ARB, WGL_DRAW_TO_WINDOW_ARB,
                       WGL_RED_BITS_ARB, WGL_DEPTH_BITS_ARB, WGL_STENCIL_BITS_ARB, WGL_SAMPLES_ARB};
        int values[8] = {0};
        getAttribiv(dc, i, 0, 8, names, values);
        if (values[0] && values[1] == WGL_FULL_ACCELERATION_ARB && values[2] == WGL_TYPE_RGBA_ARB && values[3]) chosen = i;
    }
    STEP("step 5: %d formats, chosen %d", count, chosen);
    PIXELFORMATDESCRIPTOR desc;
    DescribePixelFormat(dc, chosen, sizeof(desc), &desc);
    SetPixelFormat(dc, chosen, &desc);
    int attribs[] = {WGL_CONTEXT_MAJOR_VERSION_ARB, 3, WGL_CONTEXT_MINOR_VERSION_ARB, 3, WGL_CONTEXT_PROFILE_MASK_ARB,
                     WGL_CONTEXT_CORE_PROFILE_BIT_ARB, 0};
    HGLRC ctx = create(dc, NULL, attribs);
    ok = ctx && wglMakeCurrent(dc, ctx);
    STEP("step 6: context %p current %d, version %s", (void*)ctx, ok, ctx ? (const char*)glGetString(GL_VERSION) : "-");
    if (extString) STEP("step 7: WGL extensions: %s", extString(dc));
    if (swapInterval) swapInterval(0);
    for (int iteration = 0; iteration < 5; ++iteration) {
        glClearColor(0.f, 0.f, 0.f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        STEP("step 8.%d: cleared", iteration);
        if (!swapLayerBuffers(dc, WGL_SWAP_MAIN_PLANE)) STEP("swapLayerBuffers failed");
        STEP("step 9.%d: swapped", iteration);
        pump(hwnd);
        STEP("step 10.%d: events processed", iteration);
    }
    wglMakeCurrent(dc, NULL);
    wglDeleteContext(ctx);
    STEP("WMGL_CTS_SEQUENCE_OK");
    return 0;
}
