/* Entry points of later GL versions on a 3.2 core context. HashLink's GL
 * loader (Dead Cells) refuses to start without glBindImageTexture,
 * glMemoryBarrier, glDispatchCompute and glMultiDrawElementsIndirect, which
 * Windows drivers return whatever the context's version; the 4.6 context a
 * 3.2 request gets offers them, and every entry point up to GL 4.6
 * (glSpecializeShader). Prints WINEMETALGL_LATER_ENTRY_POINTS_OK. */
#include <windows.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/wglext.h>
#include <stdio.h>

static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcA(h, m, w, l); }

int main(void) {
    static const char *const names[] = {"glBindImageTexture", "glMemoryBarrier", "glDispatchCompute",
                                        "glMultiDrawElementsIndirect"};
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = proc; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "wmgl"; wc.style = CS_OWNDC;
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowA("wmgl", "wmgl", WS_POPUP, 0, 0, 64, 64, NULL, NULL, wc.hInstance, NULL);
    HDC dc = GetDC(hwnd);
    PIXELFORMATDESCRIPTOR pfd = {sizeof(pfd), 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 32};
    pfd.cDepthBits = 24;
    pfd.cStencilBits = 8;
    if (!SetPixelFormat(dc, ChoosePixelFormat(dc, &pfd), &pfd)) return 11;
    HGLRC legacy = wglCreateContext(dc);
    if (!legacy || !wglMakeCurrent(dc, legacy)) return 12;
    PFNWGLCREATECONTEXTATTRIBSARBPROC create = (PFNWGLCREATECONTEXTATTRIBSARBPROC)wglGetProcAddress("wglCreateContextAttribsARB");
    if (!create) return 13;
    /* What SDL2 asks for when a program wants a 3.2 core profile. */
    int attribs[] = {WGL_CONTEXT_MAJOR_VERSION_ARB, 3, WGL_CONTEXT_MINOR_VERSION_ARB, 2,
                     WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB, 0};
    HGLRC core = create(dc, NULL, attribs);
    if (!core || !wglMakeCurrent(dc, core)) { printf("3.2 core context failed %lu\n", GetLastError()); return 14; }
    printf("GL_VERSION %s\n", (const char *)glGetString(GL_VERSION));
    for (int i = 0; i < 4; i++) {
        if (!wglGetProcAddress(names[i])) { printf("missing %s\n", names[i]); return 15; }
    }
    if (!wglGetProcAddress("glSpecializeShader")) { printf("missing glSpecializeShader\n"); return 17; }
    PFNGLMEMORYBARRIERPROC barrier = (PFNGLMEMORYBARRIERPROC)wglGetProcAddress("glMemoryBarrier");
    while (glGetError() != GL_NO_ERROR) {}
    barrier(GL_ALL_BARRIER_BITS);
    GLenum error = glGetError();
    if (error != GL_NO_ERROR) { printf("glMemoryBarrier error 0x%x\n", error); return 16; }
    printf("WINEMETALGL_LATER_ENTRY_POINTS_OK\n");
    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(core);
    wglDeleteContext(legacy);
    ReleaseDC(hwnd, dc);
    DestroyWindow(hwnd);
    return 0;
}
