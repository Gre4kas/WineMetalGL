/* GL 4.0-4.2 through Wine's opengl32: a 4.2 core context from
 * wglCreateContextAttribsARB, then features those versions add, each
 * checked by a readback. */
#include <windows.h>
#include <GL/gl.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define VERTEX_SHADER 0x8b31
#define FRAGMENT_SHADER 0x8b30
#define COMPILE_STATUS 0x8b81
#define LINK_STATUS 0x8b82
#define ARRAY_BUFFER 0x8892
#define DRAW_INDIRECT_BUFFER 0x8f3f
#define ATOMIC_COUNTER_BUFFER 0x92c0
#define STATIC_DRAW 0x88e4
#define DYNAMIC_READ 0x88e9
#define R32UI 0x8236
#define RED_INTEGER 0x8d94
#define READ_WRITE 0x88ba
#define ALL_BARRIER_BITS 0xffffffffu
#define SHADING_LANGUAGE_VERSION 0x8b8c
#define VIEWPORT 0x0ba2

typedef HGLRC(WINAPI *CreateContextAttribs)(HDC, HGLRC, const int *);
typedef GLuint(APIENTRY *CreateShader)(GLenum);
typedef void(APIENTRY *ShaderSource)(GLuint, GLsizei, const char *const *, const GLint *);
typedef void(APIENTRY *CompileShader)(GLuint);
typedef void(APIENTRY *GetShaderiv)(GLuint, GLenum, GLint *);
typedef void(APIENTRY *GetShaderInfoLog)(GLuint, GLsizei, GLsizei *, char *);
typedef GLuint(APIENTRY *CreateProgram)(void);
typedef void(APIENTRY *AttachShader)(GLuint, GLuint);
typedef void(APIENTRY *LinkProgram)(GLuint);
typedef void(APIENTRY *GetProgramiv)(GLuint, GLenum, GLint *);
typedef void(APIENTRY *UseProgram)(GLuint);
typedef void(APIENTRY *GenBuffers)(GLsizei, GLuint *);
typedef void(APIENTRY *BindBuffer)(GLenum, GLuint);
typedef void(APIENTRY *BindBufferBase)(GLenum, GLuint, GLuint);
typedef void(APIENTRY *BufferData)(GLenum, intptr_t, const void *, GLenum);
typedef void(APIENTRY *GetBufferSubData)(GLenum, intptr_t, intptr_t, void *);
typedef void(APIENTRY *GenVertexArrays)(GLsizei, GLuint *);
typedef void(APIENTRY *BindVertexArray)(GLuint);
typedef void(APIENTRY *EnableVertexAttribArray)(GLuint);
typedef void(APIENTRY *VertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void *);
typedef void(APIENTRY *DrawArraysIndirect)(GLenum, const void *);
typedef void(APIENTRY *TexStorage2D)(GLenum, GLsizei, GLenum, GLsizei, GLsizei);
typedef void(APIENTRY *BindImageTexture)(GLuint, GLuint, GLint, GLboolean, GLint, GLenum, GLenum);
typedef void(APIENTRY *MemoryBarrierProc)(GLbitfield);
typedef GLuint(APIENTRY *GetSubroutineIndex)(GLuint, GLenum, const char *);
typedef void(APIENTRY *UniformSubroutinesuiv)(GLenum, GLsizei, const GLuint *);
typedef GLint(APIENTRY *GetUniformLocation)(GLuint, const char *);
typedef void(APIENTRY *Uniform1d)(GLint, double);
typedef void(APIENTRY *ViewportIndexedf)(GLuint, GLfloat, GLfloat, GLfloat, GLfloat);
typedef void(APIENTRY *GetFloati_v)(GLenum, GLuint, GLfloat *);

static CreateShader createShader;
static ShaderSource shaderSource;
static CompileShader compileShader;
static GetShaderiv getShaderiv;
static GetShaderInfoLog getShaderInfoLog;
static CreateProgram createProgram;
static AttachShader attachShader;
static LinkProgram linkProgram;
static GetProgramiv getProgramiv;
static UseProgram useProgram;

static void *proc(const char *name) {
    PROC p = wglGetProcAddress(name);
    if (!p) printf("FAIL missing %s\n", name);
    return (void *)p;
}

static GLuint program(const char *vs, const char *fs) {
    const char *sources[2] = {vs, fs};
    const GLenum types[2] = {VERTEX_SHADER, FRAGMENT_SHADER};
    GLuint p = createProgram();
    for (int i = 0; i < 2; ++i) {
        GLuint s = createShader(types[i]);
        GLint ok = 0;
        shaderSource(s, 1, &sources[i], NULL);
        compileShader(s);
        getShaderiv(s, COMPILE_STATUS, &ok);
        if (!ok) {
            char log[1024] = {0};
            getShaderInfoLog(s, sizeof(log), NULL, log);
            printf("FAIL compile: %s\n", log);
            return 0;
        }
        attachShader(p, s);
    }
    GLint linked = 0;
    linkProgram(p);
    getProgramiv(p, LINK_STATUS, &linked);
    if (!linked) {
        printf("FAIL link\n");
        return 0;
    }
    return p;
}

static int pixelIs(int x, int y, unsigned r, unsigned g, unsigned b) {
    unsigned char px[4] = {0};
    glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    if (abs(px[0] - (int)r) > 2 || abs(px[1] - (int)g) > 2 || abs(px[2] - (int)b) > 2) {
        printf("pixel %d,%d = %u,%u,%u, expected %u,%u,%u\n", x, y, px[0], px[1], px[2], r, g, b);
        return 0;
    }
    return 1;
}

static const char *kQuadVs =
    "#version 420 core\n"
    "layout(location = 0) in vec2 position;\n"
    "void main() { gl_Position = vec4(position, 0.0, 1.0); }\n";

int main(void) {
    PIXELFORMATDESCRIPTOR pfd = {0};
    HWND window = CreateWindowA("STATIC", "WineMetalGL GL 4.2", WS_POPUP, 0, 0, 64, 64, 0, 0, 0, 0);
    if (!window) return 11;
    HDC dc = GetDC(window);
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    int format = ChoosePixelFormat(dc, &pfd);
    if (!format || !SetPixelFormat(dc, format, &pfd)) return 12;
    HGLRC legacy = wglCreateContext(dc);
    if (!legacy || !wglMakeCurrent(dc, legacy)) return 13;
    CreateContextAttribs create = (CreateContextAttribs)wglGetProcAddress("wglCreateContextAttribsARB");
    if (!create) return 14;

    /* A 4.2 core context. */
    const int attribs[] = {0x2091 /* MAJOR */, 4, 0x2092 /* MINOR */, 2, 0x9126 /* PROFILE_MASK */, 1 /* CORE */, 0};
    HGLRC context = create(dc, NULL, attribs);
    if (!context || !wglMakeCurrent(dc, context)) {
        printf("FAIL 4.2 context error=%lu\n", GetLastError());
        return 15;
    }
    const char *version = (const char *)glGetString(GL_VERSION);
    const char *glsl = (const char *)glGetString(SHADING_LANGUAGE_VERSION);
    printf("GL_VERSION=%s GLSL=%s\n", version ? version : "(null)", glsl ? glsl : "(null)");
    /* The highest compatible version: 4.3, GLSL 4.30 (WGL_ARB_create_context). */
    if (!version || strncmp(version, "4.3", 3) != 0 || !glsl || strncmp(glsl, "4.30", 4) != 0) return 16;
    printf("WINEMETALGL_GL42_CONTEXT_OK\n");

    createShader = (CreateShader)proc("glCreateShader");
    shaderSource = (ShaderSource)proc("glShaderSource");
    compileShader = (CompileShader)proc("glCompileShader");
    getShaderiv = (GetShaderiv)proc("glGetShaderiv");
    getShaderInfoLog = (GetShaderInfoLog)proc("glGetShaderInfoLog");
    createProgram = (CreateProgram)proc("glCreateProgram");
    attachShader = (AttachShader)proc("glAttachShader");
    linkProgram = (LinkProgram)proc("glLinkProgram");
    getProgramiv = (GetProgramiv)proc("glGetProgramiv");
    useProgram = (UseProgram)proc("glUseProgram");
    GenBuffers genBuffers = (GenBuffers)proc("glGenBuffers");
    BindBuffer bindBuffer = (BindBuffer)proc("glBindBuffer");
    BindBufferBase bindBufferBase = (BindBufferBase)proc("glBindBufferBase");
    BufferData bufferData = (BufferData)proc("glBufferData");
    GetBufferSubData getBufferSubData = (GetBufferSubData)proc("glGetBufferSubData");
    GenVertexArrays genVertexArrays = (GenVertexArrays)proc("glGenVertexArrays");
    BindVertexArray bindVertexArray = (BindVertexArray)proc("glBindVertexArray");
    EnableVertexAttribArray enableVertexAttribArray = (EnableVertexAttribArray)proc("glEnableVertexAttribArray");
    VertexAttribPointer vertexAttribPointer = (VertexAttribPointer)proc("glVertexAttribPointer");
    DrawArraysIndirect drawArraysIndirect = (DrawArraysIndirect)proc("glDrawArraysIndirect");
    TexStorage2D texStorage2D = (TexStorage2D)proc("glTexStorage2D");
    BindImageTexture bindImageTexture = (BindImageTexture)proc("glBindImageTexture");
    MemoryBarrierProc memoryBarrier = (MemoryBarrierProc)proc("glMemoryBarrier");
    GetSubroutineIndex getSubroutineIndex = (GetSubroutineIndex)proc("glGetSubroutineIndex");
    UniformSubroutinesuiv uniformSubroutinesuiv = (UniformSubroutinesuiv)proc("glUniformSubroutinesuiv");
    GetUniformLocation getUniformLocation = (GetUniformLocation)proc("glGetUniformLocation");
    Uniform1d uniform1d = (Uniform1d)proc("glUniform1d");
    ViewportIndexedf viewportIndexedf = (ViewportIndexedf)proc("glViewportIndexedf");
    GetFloati_v getFloati_v = (GetFloati_v)proc("glGetFloati_v");
    if (!createShader || !shaderSource || !compileShader || !getShaderiv || !getShaderInfoLog || !createProgram ||
        !attachShader || !linkProgram || !getProgramiv || !useProgram || !genBuffers || !bindBuffer || !bindBufferBase ||
        !bufferData || !getBufferSubData || !genVertexArrays || !bindVertexArray || !enableVertexAttribArray ||
        !vertexAttribPointer || !drawArraysIndirect || !texStorage2D || !bindImageTexture || !memoryBarrier ||
        !getSubroutineIndex || !uniformSubroutinesuiv || !getUniformLocation || !uniform1d || !viewportIndexedf ||
        !getFloati_v)
        return 17;

    /* A full-window quad as two triangles. */
    static const float quad[] = {-1, -1, 1, -1, -1, 1, -1, 1, 1, -1, 1, 1};
    GLuint vao = 0, vbo = 0;
    genVertexArrays(1, &vao);
    bindVertexArray(vao);
    genBuffers(1, &vbo);
    bindBuffer(ARRAY_BUFFER, vbo);
    bufferData(ARRAY_BUFFER, sizeof(quad), quad, STATIC_DRAW);
    vertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, NULL);
    enableVertexAttribArray(0);
    glViewport(0, 0, 64, 64);

    /* GL 4.0: glDrawArraysIndirect reads the draw from a buffer. */
    static const char *kGreenFs =
        "#version 420 core\n"
        "out vec4 color;\n"
        "void main() { color = vec4(0.0, 1.0, 0.0, 1.0); }\n";
    GLuint green = program(kQuadVs, kGreenFs);
    if (!green) return 18;
    useProgram(green);
    const GLuint command[4] = {6 /* count */, 1 /* instances */, 0 /* first */, 0 /* base instance */};
    GLuint indirect = 0;
    genBuffers(1, &indirect);
    bindBuffer(DRAW_INDIRECT_BUFFER, indirect);
    bufferData(DRAW_INDIRECT_BUFFER, sizeof(command), command, STATIC_DRAW);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    drawArraysIndirect(GL_TRIANGLES, NULL);
    if (glGetError() || !pixelIs(32, 32, 0, 255, 0)) return 19;
    printf("WINEMETALGL_GL40_DRAW_INDIRECT_OK\n");

    /* GL 4.0: a subroutine uniform selects the fragment colour. */
    static const char *kSubroutineFs =
        "#version 420 core\n"
        "subroutine vec4 Shade();\n"
        "subroutine(Shade) vec4 red() { return vec4(1.0, 0.0, 0.0, 1.0); }\n"
        "subroutine(Shade) vec4 blue() { return vec4(0.0, 0.0, 1.0, 1.0); }\n"
        "subroutine uniform Shade shade;\n"
        "out vec4 color;\n"
        "void main() { color = shade(); }\n";
    GLuint routed = program(kQuadVs, kSubroutineFs);
    if (!routed) return 20;
    useProgram(routed);
    GLuint blue = getSubroutineIndex(routed, FRAGMENT_SHADER, "blue");
    uniformSubroutinesuiv(FRAGMENT_SHADER, 1, &blue);
    drawArraysIndirect(GL_TRIANGLES, NULL);
    if (glGetError() || !pixelIs(32, 32, 0, 0, 255)) return 21;
    printf("WINEMETALGL_GL40_SUBROUTINE_OK\n");

    /* GL 4.0: a double-precision uniform. */
    static const char *kDoubleFs =
        "#version 420 core\n"
        "uniform double level;\n"
        "out vec4 color;\n"
        "void main() { color = vec4(float(level * 2.0), 0.0, 0.0, 1.0); }\n";
    GLuint doubles = program(kQuadVs, kDoubleFs);
    if (!doubles) return 22;
    useProgram(doubles);
    uniform1d(getUniformLocation(doubles, "level"), 0.25);
    drawArraysIndirect(GL_TRIANGLES, NULL);
    if (glGetError() || !pixelIs(32, 32, 128, 0, 0)) return 23;
    printf("WINEMETALGL_GL40_FP64_OK\n");

    /* GL 4.1: the viewport array. */
    GLfloat viewport[4] = {0};
    viewportIndexedf(3, 1.0f, 2.0f, 30.0f, 40.0f);
    getFloati_v(VIEWPORT, 3, viewport);
    if (glGetError() || viewport[0] != 1.0f || viewport[1] != 2.0f || viewport[2] != 30.0f || viewport[3] != 40.0f) {
        printf("viewport 3 = %g %g %g %g\n", viewport[0], viewport[1], viewport[2], viewport[3]);
        return 24;
    }
    glViewport(0, 0, 64, 64);
    printf("WINEMETALGL_GL41_VIEWPORT_ARRAY_OK\n");

    /* GL 4.2: every fragment stores to an image and counts itself. */
    static const char *kImageFs =
        "#version 420 core\n"
        "layout(binding = 0, offset = 0) uniform atomic_uint fragments;\n"
        "layout(r32ui, binding = 1) uniform writeonly uimage2D marks;\n"
        "out vec4 color;\n"
        "void main() {\n"
        "    atomicCounterIncrement(fragments);\n"
        "    imageStore(marks, ivec2(gl_FragCoord.xy), uvec4(7u));\n"
        "    color = vec4(1.0);\n"
        "}\n";
    GLuint images = program(kQuadVs, kImageFs);
    if (!images) return 25;
    useProgram(images);
    GLuint texture = 0, counter = 0, zero = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    texStorage2D(GL_TEXTURE_2D, 1, R32UI, 4, 4);
    bindImageTexture(1, texture, 0, GL_FALSE, 0, READ_WRITE, R32UI);
    genBuffers(1, &counter);
    bindBuffer(ATOMIC_COUNTER_BUFFER, counter);
    bufferData(ATOMIC_COUNTER_BUFFER, sizeof(zero), &zero, DYNAMIC_READ);
    bindBufferBase(ATOMIC_COUNTER_BUFFER, 0, counter);
    glViewport(0, 0, 4, 4);
    drawArraysIndirect(GL_TRIANGLES, NULL);
    glViewport(0, 0, 64, 64);
    memoryBarrier(ALL_BARRIER_BITS);
    GLuint counted = 0, marks[16] = {0};
    getBufferSubData(ATOMIC_COUNTER_BUFFER, 0, sizeof(counted), &counted);
    glGetTexImage(GL_TEXTURE_2D, 0, RED_INTEGER, GL_UNSIGNED_INT, marks);
    int marked = 0;
    for (int i = 0; i < 16; ++i) marked += marks[i] == 7u;
    printf("image: %u fragments counted, %d of 16 texels stored, error=0x%x\n", counted, marked, glGetError());
    if (counted != 16 || marked != 16) return 26;
    printf("WINEMETALGL_GL42_IMAGE_ATOMIC_OK\n");

    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(context);
    wglDeleteContext(legacy);
    ReleaseDC(window, dc);
    DestroyWindow(window);
    return 0;
}
