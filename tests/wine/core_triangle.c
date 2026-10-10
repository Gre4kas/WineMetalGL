/* First-light check for the WineMetalGL core: a GL 3.3 core context clears,
 * draws one triangle and reads the result back.
 * Prints WMGL_FIRST_LIGHT_OK when the centre is the triangle colour and the
 * corners are the clear colour. */
#include <windows.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/wglext.h>
#include <stdio.h>
#include <string.h>

#define F(type, name) static type p_##name;
#define FUNCS(X) X(PFNGLGENBUFFERSPROC, glGenBuffers) X(PFNGLBINDBUFFERPROC, glBindBuffer) \
    X(PFNGLBUFFERDATAPROC, glBufferData) X(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays) \
    X(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray) X(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray) \
    X(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer) X(PFNGLCREATESHADERPROC, glCreateShader) \
    X(PFNGLSHADERSOURCEPROC, glShaderSource) X(PFNGLCOMPILESHADERPROC, glCompileShader) \
    X(PFNGLCREATEPROGRAMPROC, glCreateProgram) X(PFNGLATTACHSHADERPROC, glAttachShader) \
    X(PFNGLLINKPROGRAMPROC, glLinkProgram) X(PFNGLUSEPROGRAMPROC, glUseProgram) \
    X(PFNGLGETPROGRAMIVPROC, glGetProgramiv) X(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog) \
    X(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation) X(PFNGLUNIFORM4FPROC, glUniform4f)
FUNCS(F)
#define L(type, name) p_##name = (type)wglGetProcAddress(#name); if (!p_##name) { printf("missing %s\n", #name); return 1; }

static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcA(h, m, w, l); }

int main(void) {
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = proc; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "wmgl"; wc.style = CS_OWNDC;
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowA("wmgl", "wmgl", WS_OVERLAPPEDWINDOW, 0, 0, 400, 300, NULL, NULL, wc.hInstance, NULL);
    ShowWindow(hwnd, SW_SHOW);
    HDC dc = GetDC(hwnd);
    PIXELFORMATDESCRIPTOR pfd = {sizeof(pfd), 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 32};
    pfd.cDepthBits = 24;
    SetPixelFormat(dc, ChoosePixelFormat(dc, &pfd), &pfd);
    HGLRC legacy = wglCreateContext(dc);
    if (!legacy || !wglMakeCurrent(dc, legacy)) { printf("legacy context failed %lu\n", GetLastError()); return 1; }
    PFNWGLCREATECONTEXTATTRIBSARBPROC create = (PFNWGLCREATECONTEXTATTRIBSARBPROC)wglGetProcAddress("wglCreateContextAttribsARB");
    int attribs[] = {WGL_CONTEXT_MAJOR_VERSION_ARB, 3, WGL_CONTEXT_MINOR_VERSION_ARB, 3,
                     WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB, 0};
    HGLRC context = create ? create(dc, NULL, attribs) : NULL;
    if (!context) { printf("core context failed\n"); return 1; }
    wglMakeCurrent(dc, context);
    wglDeleteContext(legacy);
    FUNCS(L)
    printf("GL_VERSION %s\nGL_RENDERER %s\n", glGetString(GL_VERSION), glGetString(GL_RENDERER));
    RECT r; GetClientRect(hwnd, &r);
    const int w = r.right - r.left, h = r.bottom - r.top;
    glViewport(0, 0, w, h);

    static const char *vs = "#version 330 core\nlayout(location = 0) in vec2 pos;\nvoid main() { gl_Position = vec4(pos, 0.0, 1.0); }\n";
    static const char *fs = "#version 330 core\nuniform vec4 color;\nout vec4 frag;\nvoid main() { frag = color; }\n";
    GLuint v = p_glCreateShader(GL_VERTEX_SHADER), f = p_glCreateShader(GL_FRAGMENT_SHADER), prog = p_glCreateProgram();
    p_glShaderSource(v, 1, &vs, NULL); p_glCompileShader(v);
    p_glShaderSource(f, 1, &fs, NULL); p_glCompileShader(f);
    p_glAttachShader(prog, v); p_glAttachShader(prog, f); p_glLinkProgram(prog);
    GLint ok = 0; p_glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) { char log[1024]; p_glGetProgramInfoLog(prog, sizeof(log), NULL, log); printf("link failed: %s\n", log); return 1; }
    p_glUseProgram(prog);
    p_glUniform4f(p_glGetUniformLocation(prog, "color"), 1.0f, 0.5f, 0.0f, 1.0f);
    /* Triangle covering the centre, pointing up (tests orientation too). */
    static const float tri[] = {-0.5f, -0.5f, 0.5f, -0.5f, 0.0f, 0.75f};
    GLuint vao, vbo;
    p_glGenVertexArrays(1, &vao); p_glBindVertexArray(vao);
    p_glGenBuffers(1, &vbo); p_glBindBuffer(GL_ARRAY_BUFFER, vbo);
    p_glBufferData(GL_ARRAY_BUFFER, sizeof(tri), tri, GL_STATIC_DRAW);
    p_glEnableVertexAttribArray(0);
    p_glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
    glClearColor(0.0f, 0.25f, 0.5f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    unsigned char centre[4] = {0}, corner[4] = {0}, top[4] = {0}, apex[4] = {0};
    glReadPixels(w / 2, h / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, centre);
    glReadPixels(1, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, corner);
    /* Just below the apex (GL y up) is inside; the same distance from the bottom edge is outside. */
    glReadPixels(w / 2, (int)(h * 0.80), 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, apex);
    glReadPixels(w / 2, (int)(h * 0.10), 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, top);
    SwapBuffers(dc);
    /* Swap interval 0 and a few more frames, as conformance harnesses do. */
    {
        PFNWGLSWAPINTERVALEXTPROC swapInterval = (PFNWGLSWAPINTERVALEXTPROC)wglGetProcAddress("wglSwapIntervalEXT");
        int frame;
        if (swapInterval) swapInterval(0);
        for (frame = 0; frame < 3; ++frame) {
            glClear(GL_COLOR_BUFFER_BIT);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            SwapBuffers(dc);
        }
        printf("swap-interval-0 frames done\n");
    }
    GLenum err = glGetError();
    printf("size %dx%d centre %u,%u,%u,%u corner %u,%u,%u,%u near-apex %u,%u,%u,%u below-base %u,%u,%u,%u error 0x%x\n", w, h,
           centre[0], centre[1], centre[2], centre[3], corner[0], corner[1], corner[2], corner[3],
           apex[0], apex[1], apex[2], apex[3], top[0], top[1], top[2], top[3], err);
    int good = centre[0] == 255 && centre[1] >= 127 && centre[1] <= 128 && centre[2] == 0 &&
               corner[0] == 0 && corner[1] >= 63 && corner[1] <= 64 && corner[2] >= 127 && corner[2] <= 128 &&
               apex[0] == 255 && top[0] == 0 && err == GL_NO_ERROR;
    printf("%s\n", good ? "WMGL_FIRST_LIGHT_OK" : "WMGL_FIRST_LIGHT_FAIL");
    Sleep(300);
    return good ? 0 : 2;
}
