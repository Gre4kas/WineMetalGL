/* Buffer storage for 32-bit and 64-bit programs: the context is above GL 4.3
 * with GL_ARB_buffer_storage and GL_AMD_pinned_memory (Wine limits 32-bit
 * programs to 4.3 without them). A persistent, coherent mapping takes vertex
 * writes the next draw sees without a flush or unmap, and a pinned-memory
 * buffer (GL_EXTERNAL_VIRTUAL_MEMORY_BUFFER_AMD) draws from the program's own
 * memory, again after it changes. Prints WINEMETALGL_BUFFER_STORAGE_OK and
 * WINEMETALGL_PINNED_MEMORY_OK. */
#include <windows.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <stdio.h>
#include <string.h>

#ifndef GL_EXTERNAL_VIRTUAL_MEMORY_BUFFER_AMD
#define GL_EXTERNAL_VIRTUAL_MEMORY_BUFFER_AMD 0x9160
#endif

#define FUNCS(X) \
    X(PFNGLGENBUFFERSPROC, glGenBuffers) \
    X(PFNGLBINDBUFFERPROC, glBindBuffer) \
    X(PFNGLBUFFERDATAPROC, glBufferData) \
    X(PFNGLBUFFERSTORAGEPROC, glBufferStorage) \
    X(PFNGLMAPBUFFERRANGEPROC, glMapBufferRange) \
    X(PFNGLGETSTRINGIPROC, glGetStringi) \
    X(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays) \
    X(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray) \
    X(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer) \
    X(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray) \
    X(PFNGLCREATESHADERPROC, glCreateShader) \
    X(PFNGLSHADERSOURCEPROC, glShaderSource) \
    X(PFNGLCOMPILESHADERPROC, glCompileShader) \
    X(PFNGLCREATEPROGRAMPROC, glCreateProgram) \
    X(PFNGLATTACHSHADERPROC, glAttachShader) \
    X(PFNGLLINKPROGRAMPROC, glLinkProgram) \
    X(PFNGLGETPROGRAMIVPROC, glGetProgramiv) \
    X(PFNGLUSEPROGRAMPROC, glUseProgram) \
    X(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation) \
    X(PFNGLUNIFORM4FPROC, glUniform4f) \
    X(PFNGLGENFRAMEBUFFERSPROC, glGenFramebuffers) \
    X(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer) \
    X(PFNGLGENRENDERBUFFERSPROC, glGenRenderbuffers) \
    X(PFNGLBINDRENDERBUFFERPROC, glBindRenderbuffer) \
    X(PFNGLRENDERBUFFERSTORAGEPROC, glRenderbufferStorage) \
    X(PFNGLFRAMEBUFFERRENDERBUFFERPROC, glFramebufferRenderbuffer)

#define DECLARE(type, name) static type p_##name;
FUNCS(DECLARE)

static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcA(h, m, w, l); }

static int has_extension(const char *name) {
    GLint count = 0, i;
    glGetIntegerv(GL_NUM_EXTENSIONS, &count);
    for (i = 0; i < count; i++)
        if (!strcmp((const char *)p_glGetStringi(GL_EXTENSIONS, i), name)) return 1;
    return 0;
}

/* A quad over columns [x0, x1) of the 64x64 target, as a 4-vertex strip. */
static void quad(float *v, int x0, int x1) {
    const float l = x0 / 32.0f - 1.0f, r = x1 / 32.0f - 1.0f;
    const float q[8] = {l, -1.0f, r, -1.0f, l, 1.0f, r, 1.0f};
    memcpy(v, q, sizeof(q));
}

static int pixel_is(int x, unsigned int want) {
    unsigned int got = 0;
    glReadPixels(x, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, &got);
    if (got != want) printf("pixel %d: 0x%08x, want 0x%08x\n", x, got, want);
    return got == want;
}

static void draw(GLint color, float r, float g, float b) {
    p_glUniform4f(color, r, g, b, 1.0f);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

int main(void) {
    static const char *vs = "#version 330 core\nlayout(location = 0) in vec2 p;\n"
                            "void main() { gl_Position = vec4(p, 0.0, 1.0); }\n";
    static const char *fs = "#version 330 core\nuniform vec4 color;\nout vec4 o;\nvoid main() { o = color; }\n";
    const GLbitfield flags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
    PIXELFORMATDESCRIPTOR pfd = {sizeof(pfd), 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER,
                                 PFD_TYPE_RGBA, 32};
    WNDCLASSA wc = {0};
    GLuint vao, buffers[2], fbo, rb, vsh, fsh, program;
    GLint linked = 0, color, major = 0, minor = 0;
    float *mapped, *pinned;
    HWND hwnd;
    HDC dc;
    HGLRC rc;

    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "wmgl";
    wc.style = CS_OWNDC;
    RegisterClassA(&wc);
    hwnd = CreateWindowA("wmgl", "wmgl", WS_POPUP, 0, 0, 64, 64, NULL, NULL, wc.hInstance, NULL);
    dc = GetDC(hwnd);
    if (!SetPixelFormat(dc, ChoosePixelFormat(dc, &pfd), &pfd)) return 11;
    rc = wglCreateContext(dc);
    if (!rc || !wglMakeCurrent(dc, rc)) return 12;
#define LOAD(type, name) if (!(p_##name = (type)wglGetProcAddress(#name))) { printf("missing %s\n", #name); return 13; }
    FUNCS(LOAD)

    glGetIntegerv(GL_MAJOR_VERSION, &major);
    glGetIntegerv(GL_MINOR_VERSION, &minor);
    printf("GL_VERSION %s (%u-bit)\n", (const char *)glGetString(GL_VERSION), (unsigned)sizeof(void *) * 8);
    if (major * 10 + minor <= 43) { printf("version %d.%d is not above 4.3\n", major, minor); return 14; }
    if (!has_extension("GL_ARB_buffer_storage")) { printf("no GL_ARB_buffer_storage\n"); return 15; }
    if (!has_extension("GL_AMD_pinned_memory")) { printf("no GL_AMD_pinned_memory\n"); return 16; }

    vsh = p_glCreateShader(GL_VERTEX_SHADER);
    p_glShaderSource(vsh, 1, &vs, NULL);
    p_glCompileShader(vsh);
    fsh = p_glCreateShader(GL_FRAGMENT_SHADER);
    p_glShaderSource(fsh, 1, &fs, NULL);
    p_glCompileShader(fsh);
    program = p_glCreateProgram();
    p_glAttachShader(program, vsh);
    p_glAttachShader(program, fsh);
    p_glLinkProgram(program);
    p_glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) return 17;
    p_glUseProgram(program);
    color = p_glGetUniformLocation(program, "color");

    p_glGenFramebuffers(1, &fbo);
    p_glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    p_glGenRenderbuffers(1, &rb);
    p_glBindRenderbuffer(GL_RENDERBUFFER, rb);
    p_glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, 64, 64);
    p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
    glViewport(0, 0, 64, 64);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    p_glGenVertexArrays(1, &vao);
    p_glBindVertexArray(vao);
    p_glGenBuffers(2, buffers);

    /* Persistent, coherent mapping: the second draw sees the rewritten quad. */
    p_glBindBuffer(GL_ARRAY_BUFFER, buffers[0]);
    p_glBufferStorage(GL_ARRAY_BUFFER, 4096, NULL, flags);
    mapped = p_glMapBufferRange(GL_ARRAY_BUFFER, 0, 4096, flags);
    if (!mapped || glGetError()) { printf("persistent map failed\n"); return 18; }
    p_glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, NULL);
    p_glEnableVertexAttribArray(0);
    quad(mapped, 0, 16);
    draw(color, 1, 0, 0);
    glFinish();
    quad(mapped, 16, 32);
    draw(color, 0, 1, 0);
    if (!pixel_is(8, 0xff0000ff) || !pixel_is(24, 0xff00ff00)) return 19;
    printf("WINEMETALGL_BUFFER_STORAGE_OK\n");

    /* Pinned memory: the program's pages are the buffer. */
    pinned = VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!pinned) return 20;
    quad(pinned, 32, 48);
    p_glBindBuffer(GL_EXTERNAL_VIRTUAL_MEMORY_BUFFER_AMD, buffers[1]);
    p_glBufferData(GL_EXTERNAL_VIRTUAL_MEMORY_BUFFER_AMD, 4096, pinned, GL_STREAM_DRAW);
    if (glGetError()) { printf("pinned buffer failed\n"); return 21; }
    p_glBindBuffer(GL_ARRAY_BUFFER, buffers[1]);
    p_glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, NULL);
    draw(color, 0, 0, 1);
    glFinish();
    quad(pinned, 48, 64);
    draw(color, 1, 1, 1);
    if (!pixel_is(40, 0xffff0000) || !pixel_is(56, 0xffffffff)) return 22;
    printf("WINEMETALGL_PINNED_MEMORY_OK\n");

    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(rc);
    ReleaseDC(hwnd, dc);
    DestroyWindow(hwnd);
    return 0;
}
