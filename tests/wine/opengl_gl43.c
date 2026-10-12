/* GL 4.3 through Wine's opengl32: a 4.3 core context from
 * wglCreateContextAttribsARB, then features 4.3 adds, each checked by a
 * readback. */
#include <windows.h>
#include <GL/gl.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define VERTEX_SHADER 0x8b31
#define FRAGMENT_SHADER 0x8b30
#define COMPILE_STATUS 0x8b81
#define LINK_STATUS 0x8b82
#define ARRAY_BUFFER 0x8892
#define DRAW_INDIRECT_BUFFER 0x8f3f
#define SHADER_STORAGE_BUFFER 0x90d2
#define STATIC_DRAW 0x88e4
#define DYNAMIC_READ 0x88e9
#define RGBA8 0x8058
#define TEXTURE0 0x84c0
#define ALL_BARRIER_BITS 0xffffffffu
#define SHADING_LANGUAGE_VERSION 0x8b8c
#define NUM_EXTENSIONS 0x821d

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
typedef void(APIENTRY *MultiDrawArraysIndirect)(GLenum, const void *, GLsizei, GLsizei);
typedef void(APIENTRY *TexStorage2D)(GLenum, GLsizei, GLenum, GLsizei, GLsizei);
typedef void(APIENTRY *TextureView)(GLuint, GLenum, GLuint, GLenum, GLuint, GLuint, GLuint, GLuint);
typedef void(APIENTRY *MemoryBarrierProc)(GLbitfield);
typedef const GLubyte *(APIENTRY *GetStringi)(GLenum, GLuint);

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
    "#version 430 core\n"
    "layout(location = 0) in vec2 position;\n"
    "void main() { gl_Position = vec4(position, 0.0, 1.0); }\n";

int main(void) {
    PIXELFORMATDESCRIPTOR pfd = {0};
    HWND window = CreateWindowA("STATIC", "WineMetalGL GL 4.3", WS_POPUP, 0, 0, 64, 64, 0, 0, 0, 0);
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

    /* A 4.3 core context. */
    const int attribs[] = {0x2091 /* MAJOR */, 4, 0x2092 /* MINOR */, 3, 0x9126 /* PROFILE_MASK */, 1 /* CORE */, 0};
    HGLRC context = create(dc, NULL, attribs);
    if (!context || !wglMakeCurrent(dc, context)) {
        printf("FAIL 4.3 context error=%lu\n", GetLastError());
        return 15;
    }
    const char *version = (const char *)glGetString(GL_VERSION);
    const char *glsl = (const char *)glGetString(SHADING_LANGUAGE_VERSION);
    printf("GL_VERSION=%s GLSL=%s\n", version ? version : "(null)", glsl ? glsl : "(null)");
    if (!version || strncmp(version, "4.3", 3) != 0 || !glsl || strncmp(glsl, "4.30", 4) != 0) return 16;
    GetStringi getStringi = (GetStringi)proc("glGetStringi");
    if (!getStringi) return 17;
    GLint extensions = 0, compute = 0;
    glGetIntegerv(NUM_EXTENSIONS, &extensions);
    for (GLint i = 0; i < extensions; ++i)
        compute |= !strcmp((const char *)getStringi(GL_EXTENSIONS, (GLuint)i), "GL_ARB_compute_shader");
    if (!compute) return 16;
    printf("WINEMETALGL_GL43_CONTEXT_OK\n");

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
    MultiDrawArraysIndirect multiDrawArraysIndirect = (MultiDrawArraysIndirect)proc("glMultiDrawArraysIndirect");
    TexStorage2D texStorage2D = (TexStorage2D)proc("glTexStorage2D");
    TextureView textureView = (TextureView)proc("glTextureView");
    MemoryBarrierProc memoryBarrier = (MemoryBarrierProc)proc("glMemoryBarrier");
    if (!createShader || !shaderSource || !compileShader || !getShaderiv || !getShaderInfoLog || !createProgram ||
        !attachShader || !linkProgram || !getProgramiv || !useProgram || !genBuffers || !bindBuffer || !bindBufferBase ||
        !bufferData || !getBufferSubData || !genVertexArrays || !bindVertexArray || !enableVertexAttribArray ||
        !vertexAttribPointer || !multiDrawArraysIndirect || !texStorage2D || !textureView || !memoryBarrier)
        return 17;

    /* Two quads, left and right halves of the window, one after the other. */
    const float halves[16] = {-1, -1, 0, -1, -1, 1, 0, 1, 0, -1, 1, -1, 0, 1, 1, 1};
    GLuint vao = 0, vbo = 0;
    genVertexArrays(1, &vao);
    bindVertexArray(vao);
    genBuffers(1, &vbo);
    bindBuffer(ARRAY_BUFFER, vbo);
    bufferData(ARRAY_BUFFER, sizeof(halves), halves, STATIC_DRAW);
    vertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
    enableVertexAttribArray(0);
    glViewport(0, 0, 64, 64);

    /* Multi-draw indirect: one command per half. */
    GLuint flat = program(kQuadVs, "#version 430 core\nout vec4 color;\nvoid main() { color = vec4(0.0, 1.0, 0.0, 1.0); }\n");
    if (!flat) return 18;
    useProgram(flat);
    const GLuint commands[8] = {4, 1, 0, 0, 4, 1, 4, 0}; /* count, instances, first, base instance */
    GLuint indirect = 0;
    genBuffers(1, &indirect);
    bindBuffer(DRAW_INDIRECT_BUFFER, indirect);
    bufferData(DRAW_INDIRECT_BUFFER, sizeof(commands), commands, STATIC_DRAW);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    multiDrawArraysIndirect(GL_TRIANGLE_STRIP, NULL, 2, 0);
    if (glGetError() != GL_NO_ERROR || !pixelIs(16, 32, 0, 255, 0) || !pixelIs(48, 32, 0, 255, 0)) return 19;
    printf("WINEMETALGL_GL43_MULTI_DRAW_INDIRECT_OK\n");

    /* A shader storage block written by every fragment of the left half. */
    GLuint counter = program(kQuadVs, "#version 430 core\n"
                                      "layout(std430, binding = 2) buffer Count { uint fragments; };\n"
                                      "out vec4 color;\n"
                                      "void main() { atomicAdd(fragments, 1u); color = vec4(1.0); }\n");
    if (!counter) return 18;
    const GLuint zero = 0;
    GLuint ssbo = 0;
    genBuffers(1, &ssbo);
    bindBufferBase(SHADER_STORAGE_BUFFER, 2, ssbo);
    bufferData(SHADER_STORAGE_BUFFER, sizeof(zero), &zero, DYNAMIC_READ);
    useProgram(counter);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    memoryBarrier(ALL_BARRIER_BITS);
    GLuint fragments = 0;
    bindBuffer(SHADER_STORAGE_BUFFER, ssbo);
    getBufferSubData(SHADER_STORAGE_BUFFER, 0, sizeof(fragments), &fragments);
    printf("fragments=%u\n", fragments);
    if (fragments != 32 * 64) return 20;
    printf("WINEMETALGL_GL43_SSBO_FRAGMENT_OK\n");

    /* A texture view of level 1 of a two-level texture. */
    GLuint textures[2];
    glGenTextures(2, textures);
    glBindTexture(GL_TEXTURE_2D, textures[0]);
    texStorage2D(GL_TEXTURE_2D, 2, RGBA8, 2, 2);
    const unsigned char level1[4] = {255, 0, 255, 255};
    glTexSubImage2D(GL_TEXTURE_2D, 1, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, level1);
    textureView(textures[1], GL_TEXTURE_2D, textures[0], RGBA8, 1, 1, 0, 1);
    if (glGetError() != GL_NO_ERROR) return 21;
    glBindTexture(GL_TEXTURE_2D, textures[1]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    GLuint sample = program(kQuadVs, "#version 430 core\n"
                                     "layout(binding = 0) uniform sampler2D view;\n"
                                     "out vec4 color;\n"
                                     "void main() { color = texture(view, vec2(0.5)); }\n");
    if (!sample) return 18;
    useProgram(sample);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    if (!pixelIs(16, 32, 255, 0, 255)) return 21;
    printf("WINEMETALGL_GL43_TEXTURE_VIEW_OK\n");

    /* GLSL ES 3.00 (GL_ARB_ES3_compatibility). */
    GLuint es = program("#version 300 es\n"
                        "layout(location = 0) in vec2 position;\n"
                        "void main() { gl_Position = vec4(position, 0.0, 1.0); }\n",
                        "#version 300 es\n"
                        "precision mediump float;\n"
                        "out vec4 color;\n"
                        "void main() { color = vec4(0.0, 0.0, 1.0, 1.0); }\n");
    if (!es) return 18;
    useProgram(es);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_STRIP, 4, 4);
    if (!pixelIs(48, 32, 0, 0, 255) || !pixelIs(16, 32, 0, 0, 0)) return 22;
    printf("WINEMETALGL_GL43_ES3_SHADER_OK\n");

    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(context);
    wglDeleteContext(legacy);
    ReleaseDC(window, dc);
    DestroyWindow(window);
    return 0;
}
