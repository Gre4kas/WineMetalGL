/* Performance probe: one small scene per optimisation of the performance
 * plan, for before/after comparisons of cores. tests/wine/perf_probe.py runs
 * every scene in its own process with WINEMETALGL_STATS=1 and reads the
 * per-frame counters from the log; this program measures CPU frame times.
 * Not part of the gate.
 *
 *   opengl_perf_probe.exe SCENE [frames] [warmup] [salt]
 *   opengl_perf_probe.exe list
 *
 * Each frame ends with SwapBuffers (swap interval 0). Prints, for the frames
 * after the warm-up, the CPU wall time between presents (mean, median, 95th
 * percentile, max), the first frame's time, any scene-specific values
 * ("extra name=value"), then WMGL_PERF_PROBE_OK SCENE. GL errors are reported
 * as "glerror" lines.
 */
#include <windows.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WGL_CONTEXT_MAJOR_VERSION_ARB 0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB 0x2092
#define WGL_CONTEXT_PROFILE_MASK_ARB 0x9126
#define WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB 0x00000002
#define WGL_DRAW_TO_WINDOW_ARB 0x2001
#define WGL_SUPPORT_OPENGL_ARB 0x2010
#define WGL_DOUBLE_BUFFER_ARB 0x2011
#define WGL_PIXEL_TYPE_ARB 0x2013
#define WGL_TYPE_RGBA_ARB 0x202B
#define WGL_COLOR_BITS_ARB 0x2014
#define WGL_DEPTH_BITS_ARB 0x2022
#define WGL_STENCIL_BITS_ARB 0x2023
#define WGL_SAMPLE_BUFFERS_ARB 0x2041
#define WGL_SAMPLES_ARB 0x2042
typedef HGLRC (WINAPI *CreateAttribs)(HDC, HGLRC, const int *);
typedef BOOL (WINAPI *ChoosePixelFormatArb)(HDC, const int *, const FLOAT *, UINT, int *, UINT *);
typedef BOOL (WINAPI *SwapInterval)(int);

#define FUNCS(X) \
    X(PFNGLACTIVETEXTUREPROC, glActiveTexture) \
    X(PFNGLGENBUFFERSPROC, glGenBuffers) X(PFNGLBINDBUFFERPROC, glBindBuffer) \
    X(PFNGLBUFFERDATAPROC, glBufferData) X(PFNGLBUFFERSUBDATAPROC, glBufferSubData) \
    X(PFNGLMAPBUFFERRANGEPROC, glMapBufferRange) X(PFNGLUNMAPBUFFERPROC, glUnmapBuffer) \
    X(PFNGLBINDBUFFERBASEPROC, glBindBufferBase) X(PFNGLCOPYBUFFERSUBDATAPROC, glCopyBufferSubData) \
    X(PFNGLDELETEBUFFERSPROC, glDeleteBuffers) \
    X(PFNGLCREATESHADERPROC, glCreateShader) X(PFNGLSHADERSOURCEPROC, glShaderSource) \
    X(PFNGLCOMPILESHADERPROC, glCompileShader) X(PFNGLGETSHADERIVPROC, glGetShaderiv) \
    X(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog) X(PFNGLDELETESHADERPROC, glDeleteShader) \
    X(PFNGLCREATEPROGRAMPROC, glCreateProgram) X(PFNGLATTACHSHADERPROC, glAttachShader) \
    X(PFNGLLINKPROGRAMPROC, glLinkProgram) X(PFNGLGETPROGRAMIVPROC, glGetProgramiv) \
    X(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog) X(PFNGLUSEPROGRAMPROC, glUseProgram) \
    X(PFNGLDELETEPROGRAMPROC, glDeleteProgram) X(PFNGLBINDATTRIBLOCATIONPROC, glBindAttribLocation) \
    X(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation) X(PFNGLUNIFORM1IPROC, glUniform1i) \
    X(PFNGLUNIFORM4FPROC, glUniform4f) \
    X(PFNGLGETUNIFORMBLOCKINDEXPROC, glGetUniformBlockIndex) X(PFNGLUNIFORMBLOCKBINDINGPROC, glUniformBlockBinding) \
    X(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays) X(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray) \
    X(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer) X(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray) \
    X(PFNGLGENFRAMEBUFFERSPROC, glGenFramebuffers) X(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer) \
    X(PFNGLFRAMEBUFFERTEXTURE2DPROC, glFramebufferTexture2D) X(PFNGLCHECKFRAMEBUFFERSTATUSPROC, glCheckFramebufferStatus) \
    X(PFNGLBLITFRAMEBUFFERPROC, glBlitFramebuffer) X(PFNGLINVALIDATEFRAMEBUFFERPROC, glInvalidateFramebuffer) \
    X(PFNGLFENCESYNCPROC, glFenceSync) X(PFNGLCLIENTWAITSYNCPROC, glClientWaitSync) \
    X(PFNGLDELETESYNCPROC, glDeleteSync) \
    X(PFNGLGENQUERIESPROC, glGenQueries) X(PFNGLBEGINQUERYPROC, glBeginQuery) X(PFNGLENDQUERYPROC, glEndQuery) \
    X(PFNGLGETQUERYOBJECTIVPROC, glGetQueryObjectiv) X(PFNGLGETQUERYOBJECTUI64VPROC, glGetQueryObjectui64v) \
    X(PFNGLDEBUGMESSAGECALLBACKPROC, glDebugMessageCallback) \
    X(PFNGLGENPROGRAMPIPELINESPROC, glGenProgramPipelines) X(PFNGLBINDPROGRAMPIPELINEPROC, glBindProgramPipeline) \
    X(PFNGLUSEPROGRAMSTAGESPROC, glUseProgramStages) X(PFNGLCREATESHADERPROGRAMVPROC, glCreateShaderProgramv) \
    X(PFNGLGETPROGRAMBINARYPROC, glGetProgramBinary) X(PFNGLPROGRAMBINARYPROC, glProgramBinary) \
    X(PFNGLPROGRAMPARAMETERIPROC, glProgramParameteri) \
    X(PFNGLGENERATEMIPMAPPROC, glGenerateMipmap) X(PFNGLTEXSTORAGE2DPROC, glTexStorage2D)
#define DECLARE(type, name) static type p_##name;
FUNCS(DECLARE)
#define LOAD(type, name) \
    if (!(p_##name = (type)wglGetProcAddress(#name))) { printf("missing %s\n", #name); return 0; }
static int load(void) { FUNCS(LOAD) return 1; }

enum { kWidth = 1280, kHeight = 720 };

static HDC g_dc;
static int g_frame;  /* the frame being recorded, from 0 */

static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcA(h, m, w, l); }

static double seconds(void)
{
    LARGE_INTEGER now, frequency;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    return (double)now.QuadPart / (double)frequency.QuadPart;
}

static void check(const char *where)
{
    GLenum e;
    while ((e = glGetError()) != GL_NO_ERROR) printf("glerror 0x%04x in %s\n", e, where);
}

/* ------------------------------------------------------------ helpers */

static GLuint compile(GLenum type, const char *src)
{
    GLuint s = p_glCreateShader(type);
    GLint ok = 0;
    p_glShaderSource(s, 1, &src, NULL);
    p_glCompileShader(s);
    p_glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        p_glGetShaderInfoLog(s, sizeof(log), NULL, log);
        printf("compile failed: %s\n", log);
    }
    return s;
}

/* The run's salt (argv[4]): written into fragment shaders as a negligible
 * constant, so a run's shaders are new to Metal's compiler cache, which
 * otherwise keeps them across runs (compile stutter is then measured cold). */
static int g_salt;
static char *salted(const char *src)
{
    const char *at = strstr(src, "o = ");
    size_t n = strlen(src) + 64;
    char *out = malloc(n);
    if (!at || !g_salt) { snprintf(out, n, "%s", src); return out; }
    snprintf(out, n, "%.*so = vec4(%d.0e-30) + %s", (int)(at - src), src, g_salt, at + 4);
    return out;
}

static GLuint program(const char *vs, const char *fsSource)
{
    char *fs = salted(fsSource);
    GLuint p = p_glCreateProgram(), v = compile(GL_VERTEX_SHADER, vs), f = compile(GL_FRAGMENT_SHADER, fs);
    free(fs);
    GLint ok = 0;
    p_glAttachShader(p, v);
    p_glAttachShader(p, f);
    p_glBindAttribLocation(p, 0, "pos");
    p_glBindAttribLocation(p, 1, "uv");
    p_glLinkProgram(p);
    p_glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        p_glGetProgramInfoLog(p, sizeof(log), NULL, log);
        printf("link failed: %s\n", log);
    }
    p_glDeleteShader(v);
    p_glDeleteShader(f);
    return p;
}

static const char *kBasicVs =
    "#version 330\n"
    "in vec2 pos; in vec2 uv; uniform vec4 u_off; out vec2 v_uv;\n"
    "void main() { v_uv = uv; gl_Position = vec4(pos * u_off.z + u_off.xy, 0.0, 1.0); }\n";
static const char *kBasicFs =
    "#version 330\n"
    "in vec2 v_uv; uniform sampler2D tex; uniform vec4 u_color; out vec4 o;\n"
    "void main() { o = texture(tex, v_uv) * u_color; }\n";

static GLuint g_quadVao, g_quadVbo, g_basic;
static GLint g_basicOff, g_basicColor;

/* A unit quad as two triangles: pos.xy, uv.xy. */
static void createQuad(void)
{
    static const float quad[] = {-1, -1, 0, 0, 1, -1, 1, 0, 1, 1, 1, 1, -1, -1, 0, 0, 1, 1, 1, 1, -1, 1, 0, 1};
    p_glGenVertexArrays(1, &g_quadVao);
    p_glBindVertexArray(g_quadVao);
    p_glGenBuffers(1, &g_quadVbo);
    p_glBindBuffer(GL_ARRAY_BUFFER, g_quadVbo);
    p_glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    p_glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, (void *)0);
    p_glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, (void *)8);
    p_glEnableVertexAttribArray(0);
    p_glEnableVertexAttribArray(1);
    g_basic = program(kBasicVs, kBasicFs);
    g_basicOff = p_glGetUniformLocation(g_basic, "u_off");
    g_basicColor = p_glGetUniformLocation(g_basic, "u_color");
    p_glUseProgram(g_basic);
    p_glUniform1i(p_glGetUniformLocation(g_basic, "tex"), 0);
    p_glUniform4f(g_basicColor, 1, 1, 1, 1);
}

/* A small quad at a position derived from i, with the basic program bound. */
static void basicQuad(int i)
{
    float x = (float)(i % 32) / 16.0f - 0.97f, y = (float)((i / 32) % 32) / 16.0f - 0.97f;
    p_glUniform4f(g_basicOff, x, y, 0.025f, 0);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

static GLuint texture(int w, int h, unsigned seed)
{
    GLuint t;
    unsigned *data = malloc((size_t)w * h * 4);
    int i;
    for (i = 0; i < w * h; i++) data[i] = 0xff000000u | ((i * 2654435761u + seed) & 0x00ffffffu);
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    free(data);
    return t;
}

static GLuint g_fbo, g_fboColor, g_fboDepth;

/* An FBO with an RGBA8 colour texture and a DEPTH24_STENCIL8 texture. */
static void createFbo(int w, int h)
{
    p_glGenFramebuffers(1, &g_fbo);
    p_glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    glGenTextures(1, &g_fboColor);
    glBindTexture(GL_TEXTURE_2D, g_fboColor);
    p_glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, w, h);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_fboColor, 0);
    glGenTextures(1, &g_fboDepth);
    glBindTexture(GL_TEXTURE_2D, g_fboDepth);
    p_glTexStorage2D(GL_TEXTURE_2D, 1, GL_DEPTH24_STENCIL8, w, h);
    p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, g_fboDepth, 0);
    if (p_glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) printf("fbo incomplete\n");
    p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

/* Draws g_fboColor over the default framebuffer. */
static void presentFbo(void)
{
    p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, kWidth, kHeight);
    p_glUseProgram(g_basic);
    p_glBindVertexArray(g_quadVao);
    glBindTexture(GL_TEXTURE_2D, g_fboColor);
    p_glUniform4f(g_basicOff, 0, 0, 1, 0);
    p_glUniform4f(g_basicColor, 1, 1, 1, 1);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

/* Extra scene values, printed after the frame times. */
static char g_extra[1024];
static void extra(const char *name, double value)
{
    size_t n = strlen(g_extra);
    snprintf(g_extra + n, sizeof(g_extra) - n, "extra %s=%.3f\n", name, value);
}

/* ------------------------------------------------------------ scenes
 * Each scene has a setup (once, before the first frame) and a frame. The
 * comment names the plan items it measures and the counters to watch. */

/* Q1: glClientWaitSync(GL_SYNC_FLUSH_COMMANDS_BIT, 0) polling an older fence
 * in the middle of a pass. Watch: commits, pass ends "commit", passes. */
static GLsync g_fences[64];
static void fencePollSetup(void) { createQuad(); glBindTexture(GL_TEXTURE_2D, texture(64, 64, 1)); }
static void fencePollFrame(void)
{
    int group, i;
    glClear(GL_COLOR_BUFFER_BIT);
    for (group = 0; group < 32; group++) {
        for (i = 0; i < 10; i++) basicQuad(group * 10 + i);
        g_fences[group] = p_glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        if (group >= 3) p_glClientWaitSync(g_fences[group - 3], GL_SYNC_FLUSH_COMMANDS_BIT, 0);
    }
    for (group = 0; group < 32; group++) p_glDeleteSync(g_fences[group]);
}

/* Q2: glClear, then a texture upload to a texture that is not an attachment,
 * then draws. Watch: pass ends "clear" (a clear run as its own pass), passes. */
static GLuint g_uploadTex;
static unsigned g_uploadData[128 * 128];
static void clearUploadSetup(void)
{
    createQuad();
    createFbo(1024, 1024);
    g_uploadTex = texture(256, 256, 2);
}
static void clearUploadFrame(void)
{
    int target, i;
    for (target = 0; target < 8; target++) {
        p_glBindFramebuffer(GL_FRAMEBUFFER, target & 1 ? 0 : g_fbo);
        glViewport(0, 0, target & 1 ? kWidth : 1024, target & 1 ? kHeight : 1024);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        g_uploadData[0] = (unsigned)(g_frame * 8 + target);
        glBindTexture(GL_TEXTURE_2D, g_uploadTex);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 128, 128, GL_RGBA, GL_UNSIGNED_BYTE, g_uploadData);
        p_glUseProgram(g_basic);
        p_glBindVertexArray(g_quadVao);
        for (i = 0; i < 20; i++) basicQuad(i);
    }
    presentFbo();
}

/* M2: texture uploads between draws of one pass (font atlas, video).
 * Watch: pass ends "blit", passes. */
static void midpassUploadSetup(void) { clearUploadSetup(); }
static void midpassUploadFrame(void)
{
    int i;
    p_glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    glViewport(0, 0, 1024, 1024);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    p_glUseProgram(g_basic);
    p_glBindVertexArray(g_quadVao);
    for (i = 0; i < 400; i++) {
        if (i % 20 == 0) {
            g_uploadData[0] = (unsigned)(g_frame * 400 + i);
            glBindTexture(GL_TEXTURE_2D, g_uploadTex);
            glTexSubImage2D(GL_TEXTURE_2D, 0, (i / 20 % 2) * 128, 0, 128, 128, GL_RGBA, GL_UNSIGNED_BYTE, g_uploadData);
        }
        basicQuad(i);
    }
    presentFbo();
}

/* Q3, Q4, M7, M8: many draws with a uniform change, a blend toggle and a
 * texture switch each. The debug variant first installs a debug callback
 * (asynchronous output), which turns off the Wine module's batching today.
 * Watch: frame CPU time, gl ms, draw cpu breakdown. */
static GLuint g_textures[4];
static void APIENTRY debugCallback(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length,
                                   const GLchar *message, const void *user)
{
    (void)source; (void)type; (void)id; (void)severity; (void)length; (void)message; (void)user;
}
static void stateDrawsSetup(void)
{
    int i;
    createQuad();
    for (i = 0; i < 4; i++) g_textures[i] = texture(64, 64, (unsigned)i);
}
static void stateDrawsDebugSetup(void)
{
    stateDrawsSetup();
    glDisable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
    p_glDebugMessageCallback(debugCallback, NULL);
}
static void stateDrawsFrame(void)
{
    int i;
    glClear(GL_COLOR_BUFFER_BIT);
    for (i = 0; i < 3000; i++) {
        if (i & 1) glEnable(GL_BLEND); else glDisable(GL_BLEND);
        glBindTexture(GL_TEXTURE_2D, g_textures[i & 3]);
        p_glUniform4f(g_basicColor, (float)(i % 7) / 7.0f, 0.5f, 0.25f, 1.0f);
        basicQuad(i);
    }
    glDisable(GL_BLEND);
}

/* Q5, M8: fixed-function draws whose state combinations keep changing; new
 * combinations appear over the first frames (lights 1 to 8). Watch:
 * pipelines, links (ms), the max frame time, and steady-state frame CPU time. */
static void fixedVariantsSetup(void)
{
    static const float ambient[] = {0.2f, 0.2f, 0.2f, 1.0f};
    int i;
    for (i = 0; i < 4; i++) g_textures[i] = texture(64, 64, (unsigned)i);
    glLightModelfv(GL_LIGHT_MODEL_AMBIENT, ambient);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
}
static void fixedVariantsFrame(void)
{
    static const GLint modes[4] = {GL_MODULATE, GL_REPLACE, GL_DECAL, GL_ADD};
    const int lights = 1 + (g_frame / 10) % 8;
    int combo, l;
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    for (combo = 0; combo < 64; combo++) {
        float x = (float)(combo % 8) / 4.0f - 0.95f, y = (float)(combo / 8) / 4.0f - 0.95f;
        if (combo & 1) {
            glEnable(GL_LIGHTING);
            for (l = 0; l < 8; l++) if (l < lights) glEnable(GL_LIGHT0 + l); else glDisable(GL_LIGHT0 + l);
        } else {
            glDisable(GL_LIGHTING);
        }
        if (combo & 2) glEnable(GL_FOG); else glDisable(GL_FOG);
        if (combo & 4) glEnable(GL_ALPHA_TEST); else glDisable(GL_ALPHA_TEST);
        if (combo & 8) { glEnable(GL_TEXTURE_GEN_S); glEnable(GL_TEXTURE_GEN_T); }
        else { glDisable(GL_TEXTURE_GEN_S); glDisable(GL_TEXTURE_GEN_T); }
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, g_textures[combo & 3]);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, modes[(combo >> 4) & 3]);
        glBegin(GL_QUADS);
        glNormal3f(0, 0, 1);
        glColor3f(1, 0.5f, 0.25f);
        glTexCoord2f(0, 0); glVertex2f(x, y);
        glTexCoord2f(1, 0); glVertex2f(x + 0.2f, y);
        glTexCoord2f(1, 1); glVertex2f(x + 0.2f, y + 0.2f);
        glTexCoord2f(0, 1); glVertex2f(x, y + 0.2f);
        glEnd();
    }
    glDisable(GL_LIGHTING);
    glDisable(GL_FOG);
    glDisable(GL_ALPHA_TEST);
    glDisable(GL_TEXTURE_GEN_S);
    glDisable(GL_TEXTURE_GEN_T);
}

/* Q6: eight uniform blocks bound once, many draws that change only a
 * default uniform. Watch: draw cpu "bind", frame CPU time. */
static GLuint g_uboProgram;
static GLint g_uboOff;
static void uboBindSetup(void)
{
    static const char *fs =
        "#version 330\n"
        "layout(std140) uniform B0 { vec4 c0; }; layout(std140) uniform B1 { vec4 c1; };\n"
        "layout(std140) uniform B2 { vec4 c2; }; layout(std140) uniform B3 { vec4 c3; };\n"
        "layout(std140) uniform B4 { vec4 c4; }; layout(std140) uniform B5 { vec4 c5; };\n"
        "layout(std140) uniform B6 { vec4 c6; }; layout(std140) uniform B7 { vec4 c7; };\n"
        "in vec2 v_uv; out vec4 o;\n"
        "void main() { o = (c0 + c1 + c2 + c3 + c4 + c5 + c6 + c7) * 0.125 + vec4(v_uv, 0.0, 0.0); }\n";
    GLuint buffers[8];
    float color[4] = {0.5f, 0.25f, 1.0f, 1.0f};
    char name[4];
    int i;
    createQuad();
    g_uboProgram = program(kBasicVs, fs);
    g_uboOff = p_glGetUniformLocation(g_uboProgram, "u_off");
    p_glGenBuffers(8, buffers);
    for (i = 0; i < 8; i++) {
        snprintf(name, sizeof(name), "B%d", i);
        p_glUniformBlockBinding(g_uboProgram, p_glGetUniformBlockIndex(g_uboProgram, name), (GLuint)i);
        p_glBindBuffer(GL_UNIFORM_BUFFER, buffers[i]);
        p_glBufferData(GL_UNIFORM_BUFFER, 256, NULL, GL_STATIC_DRAW);
        p_glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(color), color);
        p_glBindBufferBase(GL_UNIFORM_BUFFER, (GLuint)i, buffers[i]);
    }
    p_glUseProgram(g_uboProgram);
}
static void uboBindFrame(void)
{
    int i;
    glClear(GL_COLOR_BUFFER_BIT);
    for (i = 0; i < 3000; i++) {
        float x = (float)(i % 32) / 16.0f - 0.97f, y = (float)((i / 32) % 32) / 16.0f - 0.97f;
        p_glUniform4f(g_uboOff, x, y, 0.025f, 0);
        glDrawArrays(GL_TRIANGLES, 0, 6);
    }
}

/* Q7, M10: GL_TIME_ELAPSED queries around groups of draws, read three frames
 * later. Watch: commits (query), passes. */
enum { kTimers = 48, kTimerFrames = 4 };
static GLuint g_timers[kTimerFrames][kTimers];
static GLuint64 g_timerSum;
static void timerQueriesSetup(void)
{
    stateDrawsSetup();
    p_glGenQueries(kTimers * kTimerFrames, &g_timers[0][0]);
}
static void timerQueriesFrame(void)
{
    GLuint *timers = g_timers[g_frame % kTimerFrames];
    int q, i;
    if (g_frame >= kTimerFrames)
        for (q = 0; q < kTimers; q++) {
            GLint ready = 0;
            p_glGetQueryObjectiv(timers[q], GL_QUERY_RESULT_AVAILABLE, &ready);
            if (ready) {
                GLuint64 ns = 0;
                p_glGetQueryObjectui64v(timers[q], GL_QUERY_RESULT, &ns);
                g_timerSum += ns;
            }
        }
    glClear(GL_COLOR_BUFFER_BIT);
    for (q = 0; q < kTimers; q++) {
        p_glBeginQuery(GL_TIME_ELAPSED, timers[q]);
        for (i = 0; i < 20; i++) basicQuad(q * 20 + i);
        p_glEndQuery(GL_TIME_ELAPSED);
    }
}

/* Q7, M7: glFlush every 100 draws (wined3d and older engines).
 * Watch: commits, passes. */
static void flushFrame(void)
{
    int i;
    glClear(GL_COLOR_BUFFER_BIT);
    for (i = 0; i < 3000; i++) {
        basicQuad(i);
        if (i % 100 == 99) glFlush();
    }
}

/* M1: a 64 KiB uniform buffer updated with glBufferSubData before every draw
 * (busy, so copy-on-write), and a 1 MiB vertex buffer orphaned each frame.
 * Watch: frame CPU time, gl ms, metal MiB. */
static GLuint g_subProgram, g_ubo, g_streamVbo;
static GLint g_subOff;
static void bufferSubdataSetup(void)
{
    static const char *fs =
        "#version 330\n"
        "layout(std140) uniform Params { vec4 color; vec4 data[1023]; };\n"
        "in vec2 v_uv; out vec4 o;\n"
        "void main() { o = color + data[int(v_uv.x * 7.0)] * 0.0; }\n";
    createQuad();
    g_subProgram = program(kBasicVs, fs);
    g_subOff = p_glGetUniformLocation(g_subProgram, "u_off");
    p_glUniformBlockBinding(g_subProgram, p_glGetUniformBlockIndex(g_subProgram, "Params"), 0);
    p_glGenBuffers(1, &g_ubo);
    p_glBindBuffer(GL_UNIFORM_BUFFER, g_ubo);
    p_glBufferData(GL_UNIFORM_BUFFER, 64 * 1024, NULL, GL_DYNAMIC_DRAW);
    p_glBindBufferBase(GL_UNIFORM_BUFFER, 0, g_ubo);
    p_glGenBuffers(1, &g_streamVbo);
    p_glUseProgram(g_subProgram);
}
static void bufferSubdataFrame(void)
{
    static float stream[1 << 18];
    float params[64];
    int i;
    glClear(GL_COLOR_BUFFER_BIT);
    for (i = 0; i < 1000; i++) {
        float x = (float)(i % 32) / 16.0f - 0.97f, y = (float)((i / 32) % 32) / 16.0f - 0.97f;
        params[0] = (float)(i % 5) / 5.0f;
        params[1] = 0.5f;
        params[2] = 0.25f;
        params[3] = 1.0f;
        p_glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(params), params);
        p_glUniform4f(g_subOff, x, y, 0.025f, 0);
        glDrawArrays(GL_TRIANGLES, 0, 6);
    }
    /* Orphan and refill a stream buffer, then draw from it. */
    stream[0] = (float)g_frame;
    p_glBindBuffer(GL_ARRAY_BUFFER, g_streamVbo);
    p_glBufferData(GL_ARRAY_BUFFER, sizeof(stream), NULL, GL_STREAM_DRAW);
    p_glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(stream), stream);
    p_glBindBuffer(GL_ARRAY_BUFFER, g_quadVbo);
}

/* M1: glMapBufferRange with GL_MAP_INVALIDATE_RANGE_BIT on a busy 4 MiB
 * vertex buffer before each draw. Watch: frame CPU time, waits, metal MiB. */
static GLuint g_mapVao, g_mapVbo;
static void mapInvalidateSetup(void)
{
    createQuad();
    p_glGenVertexArrays(1, &g_mapVao);
    p_glBindVertexArray(g_mapVao);
    p_glGenBuffers(1, &g_mapVbo);
    p_glBindBuffer(GL_ARRAY_BUFFER, g_mapVbo);
    p_glBufferData(GL_ARRAY_BUFFER, 4 << 20, NULL, GL_DYNAMIC_DRAW);
    p_glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, (void *)0);
    p_glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, (void *)8);
    p_glEnableVertexAttribArray(0);
    p_glEnableVertexAttribArray(1);
}
static void mapInvalidateFrame(void)
{
    static const float quad[] = {-1, -1, 0, 0, 1, -1, 1, 0, 1, 1, 1, 1, -1, -1, 0, 0, 1, 1, 1, 1, -1, 1, 0, 1};
    int i;
    glClear(GL_COLOR_BUFFER_BIT);
    for (i = 0; i < 200; i++) {
        const GLintptr offset = (GLintptr)(i % 64) * 65536;
        float *p = p_glMapBufferRange(GL_ARRAY_BUFFER, offset, 65536, GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_RANGE_BIT);
        if (p) memcpy(p, quad, sizeof(quad));
        p_glUnmapBuffer(GL_ARRAY_BUFFER);
        p_glUniform4f(g_basicOff, (float)(i % 16) / 8.0f - 0.95f, (float)(i / 16) / 8.0f - 0.95f, 0.05f, 0);
        glDrawArrays(GL_TRIANGLES, (GLint)(offset / 16), 6);
    }
}

/* M3: a 2048x2048 colour + depth/stencil pass whose depth/stencil is
 * invalidated after use, then sampled from another pass, three times a
 * frame. Watch: gpu busy ms. */
static void invalidateSetup(void)
{
    createQuad();
    createFbo(2048, 2048);
    g_textures[0] = texture(64, 64, 3);
}
static void invalidateFrame(void)
{
    static const GLenum attachments[] = {GL_DEPTH_ATTACHMENT, GL_STENCIL_ATTACHMENT};
    int round, i;
    for (round = 0; round < 3; round++) {
        p_glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
        glViewport(0, 0, 2048, 2048);
        glEnable(GL_DEPTH_TEST);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        p_glUseProgram(g_basic);
        p_glBindVertexArray(g_quadVao);
        glBindTexture(GL_TEXTURE_2D, g_textures[0]);
        for (i = 0; i < 200; i++) basicQuad(i);
        p_glInvalidateFramebuffer(GL_FRAMEBUFFER, 2, attachments);
        glDisable(GL_DEPTH_TEST);
        presentFbo();
    }
}

/* M4: a 1 MiB index buffer partly rewritten with glBufferSubData while busy
 * (an ordered GPU copy today), then drawn from. Watch: waits (ms), commits. */
static GLuint g_ibo;
static void indexUpdateSetup(void)
{
    static GLuint indices[1 << 18];
    int i;
    createQuad();
    for (i = 0; i < (1 << 18); i++) indices[i] = (GLuint)(i % 6);
    p_glGenBuffers(1, &g_ibo);
    p_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_ibo);
    p_glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_DYNAMIC_DRAW);
    glBindTexture(GL_TEXTURE_2D, texture(64, 64, 4));
}
static void indexUpdateFrame(void)
{
    static GLuint update[16384];
    int i;
    glClear(GL_COLOR_BUFFER_BIT);
    for (i = 0; i < 100; i++) {
        float x = (float)(i % 10) / 5.0f - 0.95f, y = (float)(i / 10) / 5.0f - 0.95f;
        if (i % 25 == 0) {
            int j;
            for (j = 0; j < 16384; j++) update[j] = (GLuint)(j % 6);
            p_glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, (GLintptr)(i / 25) * sizeof(update), sizeof(update), update);
        }
        p_glUniform4f(g_basicOff, x, y, 0.08f, 0);
        glDrawElements(GL_TRIANGLES, 6 * 100, GL_UNSIGNED_INT, (void *)(uintptr_t)((i % 32) * 6 * 100 * 4));
    }
}

/* M5: client-memory vertex arrays (float3 position, ubyte4 colour, float2
 * texcoord) in the compatibility profile. Watch: converted vertices
 * (client), frame CPU time. */
enum { kClientVerts = 120 };
static float g_clientPos[kClientVerts * 3], g_clientUv[kClientVerts * 2];
static unsigned char g_clientColor[kClientVerts * 4];
static void clientArraysSetup(void)
{
    int i;
    for (i = 0; i < kClientVerts; i++) {
        g_clientPos[i * 3] = (float)((i * 7) % 13) / 13.0f * 0.1f;
        g_clientPos[i * 3 + 1] = (float)((i * 5) % 11) / 11.0f * 0.1f;
        g_clientPos[i * 3 + 2] = 0;
        g_clientUv[i * 2] = (float)(i & 1);
        g_clientUv[i * 2 + 1] = (float)((i >> 1) & 1);
        g_clientColor[i * 4] = (unsigned char)(i * 3);
        g_clientColor[i * 4 + 1] = 128;
        g_clientColor[i * 4 + 2] = 64;
        g_clientColor[i * 4 + 3] = 255;
    }
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, texture(64, 64, 5));
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0, g_clientPos);
    glColorPointer(4, GL_UNSIGNED_BYTE, 0, g_clientColor);
    glTexCoordPointer(2, GL_FLOAT, 0, g_clientUv);
}
static void clientArraysFrame(void)
{
    int i;
    glClear(GL_COLOR_BUFFER_BIT);
    for (i = 0; i < 500; i++) {
        glLoadIdentity();
        glTranslatef((float)(i % 25) / 12.5f - 0.98f, (float)(i / 25) / 10.0f - 0.98f, 0);
        glDrawArrays(GL_TRIANGLES, 0, kClientVerts);
    }
}

/* M6: a display list of 2000 immediate-mode triangles called 50 times, and
 * 1000 separate glBegin/glEnd quads. Watch: converted vertices, transient
 * KiB, frame CPU time. */
static GLuint g_list;
static void displayListsSetup(void)
{
    int i;
    g_list = glGenLists(1);
    glNewList(g_list, GL_COMPILE);
    glBegin(GL_TRIANGLES);
    for (i = 0; i < 2000; i++) {
        float x = (float)(i % 50) / 25.0f - 1.0f, y = (float)(i / 50) / 20.0f - 1.0f;
        glColor3f((float)(i % 3) / 3.0f, 0.5f, 0.25f);
        glVertex2f(x, y);
        glVertex2f(x + 0.03f, y);
        glVertex2f(x, y + 0.03f);
    }
    glEnd();
    glEndList();
}
static void displayListsFrame(void)
{
    int i;
    glClear(GL_COLOR_BUFFER_BIT);
    for (i = 0; i < 50; i++) glCallList(g_list);
    for (i = 0; i < 1000; i++) {
        float x = (float)(i % 40) / 20.0f - 1.0f, y = (float)(i / 40) / 12.5f - 1.0f;
        glBegin(GL_QUADS);
        glColor3f(0.25f, (float)(i % 5) / 5.0f, 0.5f);
        glVertex2f(x, y);
        glVertex2f(x + 0.04f, y);
        glVertex2f(x + 0.04f, y + 0.04f);
        glVertex2f(x, y + 0.04f);
        glEnd();
    }
}

/* M9: glReadPixels(GL_BGRA) of the frame into a pack buffer, mapped two
 * frames later (the async readback pattern). Watch: reads async/waiting,
 * waits (ms). */
static GLuint g_pbos[3];
static int g_readWidth, g_readHeight;  /* the window's client area: reads stay inside the framebuffer */
static void readbackSetup(void)
{
    RECT client;
    int i;
    stateDrawsSetup();
    GetClientRect(WindowFromDC(g_dc), &client);
    g_readWidth = client.right - client.left;
    g_readHeight = client.bottom - client.top;
    if (g_readWidth <= 0 || g_readWidth > kWidth) g_readWidth = kWidth / 2;
    if (g_readHeight <= 0 || g_readHeight > kHeight) g_readHeight = kHeight / 2;
    p_glGenBuffers(3, g_pbos);
    for (i = 0; i < 3; i++) {
        p_glBindBuffer(GL_PIXEL_PACK_BUFFER, g_pbos[i]);
        p_glBufferData(GL_PIXEL_PACK_BUFFER, kWidth * kHeight * 4, NULL, GL_STREAM_READ);
    }
    p_glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
}
static void readbackFrame(void)
{
    int i;
    glClear(GL_COLOR_BUFFER_BIT);
    for (i = 0; i < 200; i++) basicQuad(i);
    p_glBindBuffer(GL_PIXEL_PACK_BUFFER, g_pbos[g_frame % 3]);
    glReadPixels(0, 0, g_readWidth, g_readHeight, GL_BGRA, GL_UNSIGNED_BYTE, 0);
    if (g_frame >= 2) {
        p_glBindBuffer(GL_PIXEL_PACK_BUFFER, g_pbos[(g_frame + 1) % 3]);
        if (p_glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, g_readWidth * g_readHeight * 4, GL_MAP_READ_BIT))
            p_glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
    }
    p_glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
}

/* M9: a 1024x1024 RGBA upload from a pixel unpack buffer the GPU just wrote
 * (glCopyBufferSubData), each frame. Watch: waits (ms), frame CPU time. */
static GLuint g_unpackSrc, g_unpack, g_pboTex;
static void pboUploadSetup(void)
{
    static unsigned data[1024 * 1024];
    int i;
    createQuad();
    for (i = 0; i < 1024 * 1024; i++) data[i] = 0xff000000u | (unsigned)i;
    p_glGenBuffers(1, &g_unpackSrc);
    p_glBindBuffer(GL_COPY_READ_BUFFER, g_unpackSrc);
    p_glBufferData(GL_COPY_READ_BUFFER, sizeof(data), data, GL_STATIC_DRAW);
    p_glGenBuffers(1, &g_unpack);
    p_glBindBuffer(GL_PIXEL_UNPACK_BUFFER, g_unpack);
    p_glBufferData(GL_PIXEL_UNPACK_BUFFER, sizeof(data), NULL, GL_STREAM_DRAW);
    p_glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    glGenTextures(1, &g_pboTex);
    glBindTexture(GL_TEXTURE_2D, g_pboTex);
    p_glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 1024, 1024);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
}
static void pboUploadFrame(void)
{
    int i;
    glClear(GL_COLOR_BUFFER_BIT);
    p_glBindBuffer(GL_COPY_WRITE_BUFFER, g_unpack);
    p_glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 0, 0, 1024 * 1024 * 4);
    p_glBindBuffer(GL_PIXEL_UNPACK_BUFFER, g_unpack);
    glBindTexture(GL_TEXTURE_2D, g_pboTex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1024, 1024, GL_RGBA, GL_UNSIGNED_BYTE, 0);
    p_glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    for (i = 0; i < 100; i++) basicQuad(i);
}

/* M11: textures sampled with a different LOD bias and border colour each
 * frame for the first 100 frames, then fixed. Watch: pipelines (ms), the max
 * frame time; steady state should have no pipeline misses. */
static void lodBiasSetup(void)
{
    int i;
    createQuad();
    for (i = 0; i < 4; i++) {
        g_textures[i] = texture(64, 64, (unsigned)i);
        p_glGenerateMipmap(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    }
}
static void lodBiasFrame(void)
{
    const int step = g_frame < 100 ? g_frame : 100;
    int i;
    glClear(GL_COLOR_BUFFER_BIT);
    for (i = 0; i < 4; i++) {
        const float border[4] = {(float)step / 100.0f, 0.5f, (float)i / 4.0f, 1.0f};
        glBindTexture(GL_TEXTURE_2D, g_textures[i]);
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS, (float)step * 0.01f + (float)i * 0.25f);
        glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, border);
    }
    for (i = 0; i < 400; i++) {
        glBindTexture(GL_TEXTURE_2D, g_textures[i & 3]);
        basicQuad(i);
    }
}

/* M12: one program pipeline whose stages cycle through 4 vertex x 4
 * fragment separable programs. Watch: pipelines per steady frame (should be
 * 0 once every combination was seen), frame CPU time. */
static GLuint g_vsPrograms[4], g_fsPrograms[4], g_pipeline;
static void ssoSetup(void)
{
    char vs[512], fs[512];
    const char *src;
    int i;
    createQuad();
    p_glUseProgram(0);
    for (i = 0; i < 4; i++) {
        snprintf(vs, sizeof(vs),
                 "#version 410\n"
                 "layout(location = 0) in vec2 pos; uniform vec4 u_off;\n"
                 "out gl_PerVertex { vec4 gl_Position; };\n"
                 "void main() { gl_Position = vec4(pos * u_off.z * %d.0 + u_off.xy, 0.0, 1.0); }\n", i + 1);
        snprintf(fs, sizeof(fs),
                 "#version 410\n"
                 "layout(location = 0) out vec4 o;\n"
                 "void main() { o = vec4(%d.0 / 4.0, 0.5, 0.25, 1.0); }\n", i);
        src = vs;
        g_vsPrograms[i] = p_glCreateShaderProgramv(GL_VERTEX_SHADER, 1, &src);
        {
            char *saltedFs = salted(fs);
            src = saltedFs;
            g_fsPrograms[i] = p_glCreateShaderProgramv(GL_FRAGMENT_SHADER, 1, &src);
            free(saltedFs);
        }
    }
    p_glGenProgramPipelines(1, &g_pipeline);
    p_glBindProgramPipeline(g_pipeline);
}
static void ssoFrame(void)
{
    int i;
    glClear(GL_COLOR_BUFFER_BIT);
    for (i = 0; i < 400; i++) {
        const GLuint vs = g_vsPrograms[i & 3];
        p_glUseProgramStages(g_pipeline, GL_VERTEX_SHADER_BIT, vs);
        p_glUseProgramStages(g_pipeline, GL_FRAGMENT_SHADER_BIT, g_fsPrograms[(i >> 2) & 3]);
        p_glUseProgram(vs);
        p_glUniform4f(p_glGetUniformLocation(vs, "u_off"), (float)(i % 20) / 10.0f - 0.95f,
                      (float)(i / 20) / 10.0f - 0.95f, 0.01f, 0);
        p_glUseProgram(0);
        glDrawArrays(GL_TRIANGLES, 0, 6);
    }
}

/* M13: a 1024x1024 GL_GENERATE_MIPMAP texture with 50 small sub-uploads a
 * frame, plus 20 new 256x256 textures created, uploaded and deleted a frame.
 * Watch: commits, pass ends blit, frame CPU time. */
static GLuint g_autoTex;
static void autoMipmapSetup(void)
{
    createQuad();
    glGenTextures(1, &g_autoTex);
    glBindTexture(GL_TEXTURE_2D, g_autoTex);
    glTexParameteri(GL_TEXTURE_2D, GL_GENERATE_MIPMAP, GL_TRUE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1024, 1024, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
}
static void autoMipmapFrame(void)
{
    static unsigned glyph[32 * 32], fresh[256 * 256];
    GLuint created[20];
    int i;
    glClear(GL_COLOR_BUFFER_BIT);
    glBindTexture(GL_TEXTURE_2D, g_autoTex);
    for (i = 0; i < 50; i++) {
        glyph[0] = (unsigned)(g_frame * 50 + i);
        glTexSubImage2D(GL_TEXTURE_2D, 0, (i % 32) * 32, (i / 32) * 32, 32, 32, GL_RGBA, GL_UNSIGNED_BYTE, glyph);
    }
    for (i = 0; i < 100; i++) basicQuad(i);
    glGenTextures(20, created);
    for (i = 0; i < 20; i++) {
        glBindTexture(GL_TEXTURE_2D, created[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, 256, 256, 0, GL_RGBA, GL_UNSIGNED_BYTE, fresh);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        basicQuad(i);
    }
    glDeleteTextures(20, created);
}

/* M14, Q8: 512x512 uploads in formats that miss the fast paths (and the
 * RGB->RGBA8 fast path), every frame. Watch: frame CPU time, gl ms. */
static GLuint g_formatTex[9];
static void textureFormatsSetup(void)
{
    static const GLenum internal[9] = {GL_RGBA8, GL_RGB8, GL_RGB8, GL_RGB8, GL_RGBA4, GL_RGB5_A1, GL_RGBA8, GL_RGBA16F, GL_RGB8};
    int i;
    createQuad();
    glGenTextures(9, g_formatTex);
    for (i = 0; i < 9; i++) {
        glBindTexture(GL_TEXTURE_2D, g_formatTex[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, (GLint)internal[i], 512, 512, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    }
}
static void textureFormatsFrame(void)
{
    static unsigned char pixels[512 * 512 * 16];
    static const GLenum formats[9] = {GL_RGB, GL_BGR, GL_RGB, GL_RGBA, GL_RGBA, GL_RGBA, GL_LUMINANCE, GL_RGBA, GL_RGBA};
    static const GLenum types[9] = {GL_UNSIGNED_BYTE, GL_UNSIGNED_BYTE, GL_UNSIGNED_SHORT_5_6_5, GL_UNSIGNED_BYTE,
                                    GL_UNSIGNED_SHORT_4_4_4_4, GL_UNSIGNED_SHORT_5_5_5_1, GL_UNSIGNED_BYTE, GL_FLOAT,
                                    GL_UNSIGNED_INT_8_8_8_8};
    int i;
    pixels[0] = (unsigned char)g_frame;
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    for (i = 0; i < 9; i++) {
        glBindTexture(GL_TEXTURE_2D, g_formatTex[i]);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 512, 512, formats[i], types[i], pixels);
        basicQuad(i);
    }
}

/* High effort: program links, and links from glProgramBinary. The runner runs
 * this scene twice in the same prefix (cold, then warm caches).
 * Extras: link_ms (60 distinct programs), binary_ms (60 programs from their
 * binaries), first_draw_ms (the first draw of each). */
static void shaderLinkSetup(void)
{
    char fs[512];
    GLuint programs[60];
    void *binary = malloc(1 << 20);
    double start;
    int i;
    createQuad();
    start = seconds();
    for (i = 0; i < 60; i++) {
        snprintf(fs, sizeof(fs),
                 "#version 330\n"
                 "in vec2 v_uv; uniform sampler2D tex; uniform vec4 u_color; out vec4 o;\n"
                 "void main() { vec4 c = texture(tex, v_uv * %d.0); o = c * u_color + vec4(sin(c.x * %d.0)); }\n",
                 i + 1, i + 3);
        programs[i] = program(kBasicVs, fs);
    }
    extra("link_ms", (seconds() - start) * 1000.0);
    start = seconds();
    for (i = 0; i < 60; i++) {
        GLint length = 0;
        GLenum format = 0;
        GLuint p;
        p_glGetProgramiv(programs[i], GL_PROGRAM_BINARY_LENGTH, &length);
        if (length <= 0 || length > (1 << 20)) continue;
        p_glGetProgramBinary(programs[i], length, NULL, &format, binary);
        p = p_glCreateProgram();
        p_glProgramBinary(p, format, binary, length);
        p_glDeleteProgram(p);
    }
    extra("binary_ms", (seconds() - start) * 1000.0);
    glBindTexture(GL_TEXTURE_2D, texture(64, 64, 6));
    start = seconds();
    for (i = 0; i < 60; i++) {
        p_glUseProgram(programs[i]);
        p_glUniform4f(p_glGetUniformLocation(programs[i], "u_off"), 0, 0, 0.1f, 0);
        p_glUniform4f(p_glGetUniformLocation(programs[i], "u_color"), 1, 1, 1, 1);
        glDrawArrays(GL_TRIANGLES, 0, 6);
    }
    glFinish();
    extra("first_draw_ms", (seconds() - start) * 1000.0);
    p_glUseProgram(g_basic);
    free(binary);
}
static void shaderLinkFrame(void)
{
    int i;
    glClear(GL_COLOR_BUFFER_BIT);
    for (i = 0; i < 100; i++) basicQuad(i);
}

/* High effort: a 4x multisampled default framebuffer (resolved at every
 * swap today with an extra pass). Watch: passes, gpu busy ms. */
static void msaaFrame(void)
{
    int i;
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    for (i = 0; i < 500; i++) basicQuad(i);
}

/* Q8: glDrawElements(GL_TRIANGLE_FAN) from an element buffer (rewritten on
 * the CPU; the rewrite cache uses a heap-allocated key). Watch: cpu indices,
 * draw cpu "index", frame CPU time. */
static GLuint g_fanVao;
static void fansSetup(void)
{
    static const GLushort fan[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    float vertices[8 * 4];
    GLuint vbo, ibo;
    int i;
    createQuad();
    for (i = 0; i < 8; i++) {
        vertices[i * 4] = cosf((float)i * 0.785398f);
        vertices[i * 4 + 1] = sinf((float)i * 0.785398f);
        vertices[i * 4 + 2] = vertices[i * 4 + 3] = 0.5f;
    }
    p_glGenVertexArrays(1, &g_fanVao);
    p_glBindVertexArray(g_fanVao);
    p_glGenBuffers(1, &vbo);
    p_glBindBuffer(GL_ARRAY_BUFFER, vbo);
    p_glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    p_glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, (void *)0);
    p_glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, (void *)8);
    p_glEnableVertexAttribArray(0);
    p_glEnableVertexAttribArray(1);
    p_glGenBuffers(1, &ibo);
    p_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
    p_glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(fan), fan, GL_STATIC_DRAW);
    glBindTexture(GL_TEXTURE_2D, texture(64, 64, 7));
}
static void fansFrame(void)
{
    int i;
    glClear(GL_COLOR_BUFFER_BIT);
    for (i = 0; i < 1000; i++) {
        float x = (float)(i % 32) / 16.0f - 0.97f, y = (float)((i / 32) % 32) / 16.0f - 0.97f;
        p_glUniform4f(g_basicOff, x, y, 0.025f, 0);
        glDrawElements(GL_TRIANGLE_FAN, 8, GL_UNSIGNED_SHORT, 0);
    }
}

/* M8, M7, high effort (unretained references, direct-to-drawable present):
 * a mixed "engine" frame: an FBO pass with 1500 draws over 8 textures and 4
 * programs, then a post pass to the back buffer. Watch: frame CPU time, gl
 * ms, draw cpu breakdown, gpu busy ms. */
static GLuint g_programs[4];
static void engineSetup(void)
{
    char fs[512];
    int i;
    createQuad();
    createFbo(kWidth, kHeight);
    for (i = 0; i < 4; i++) {
        snprintf(fs, sizeof(fs),
                 "#version 330\n"
                 "in vec2 v_uv; uniform sampler2D tex; uniform vec4 u_color; out vec4 o;\n"
                 "void main() { o = texture(tex, v_uv) * u_color * %d.0 / 4.0; }\n", i + 1);
        g_programs[i] = program(kBasicVs, fs);
        p_glUseProgram(g_programs[i]);
        p_glUniform1i(p_glGetUniformLocation(g_programs[i], "tex"), 0);
    }
    for (i = 0; i < 4; i++) g_textures[i] = texture(128, 128, (unsigned)i);
}
static void engineFrame(void)
{
    int i;
    p_glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    glViewport(0, 0, kWidth, kHeight);
    glEnable(GL_DEPTH_TEST);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    p_glBindVertexArray(g_quadVao);
    for (i = 0; i < 1500; i++) {
        const GLuint p = g_programs[(i / 50) & 3];
        float x = (float)(i % 40) / 20.0f - 0.97f, y = (float)((i / 40) % 40) / 20.0f - 0.97f;
        p_glUseProgram(p);
        glBindTexture(GL_TEXTURE_2D, g_textures[i & 3]);
        p_glUniform4f(p_glGetUniformLocation(p, "u_off"), x, y, 0.03f, 0);
        p_glUniform4f(p_glGetUniformLocation(p, "u_color"), 1, (float)(i % 3) / 3.0f, 0.5f, 1);
        if (i % 100 == 0) { if (i & 256) glEnable(GL_BLEND); else glDisable(GL_BLEND); }
        glDrawArrays(GL_TRIANGLES, 0, 6);
    }
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    presentFbo();
}

struct scene {
    const char *name;
    void (*setup)(void);
    void (*frame)(void);
    int msaa;
    const char *covers;
};
static void basicSetup(void) { stateDrawsSetup(); }

static const struct scene kScenes[] = {
    {"fence_poll", fencePollSetup, fencePollFrame, 0, "Q1"},
    {"clear_upload", clearUploadSetup, clearUploadFrame, 0, "Q2"},
    {"midpass_upload", midpassUploadSetup, midpassUploadFrame, 0, "M2"},
    {"state_draws", stateDrawsSetup, stateDrawsFrame, 0, "Q4 M7 M8"},
    {"state_draws_debug", stateDrawsDebugSetup, stateDrawsFrame, 0, "Q3"},
    {"fixed_variants", fixedVariantsSetup, fixedVariantsFrame, 0, "Q5 M8"},
    {"ubo_bind", uboBindSetup, uboBindFrame, 0, "Q6"},
    {"timer_queries", timerQueriesSetup, timerQueriesFrame, 0, "Q7 M10"},
    {"flush", basicSetup, flushFrame, 0, "Q7"},
    {"buffer_subdata", bufferSubdataSetup, bufferSubdataFrame, 0, "M1"},
    {"map_invalidate", mapInvalidateSetup, mapInvalidateFrame, 0, "M1"},
    {"invalidate_fb", invalidateSetup, invalidateFrame, 0, "M3"},
    {"index_update", indexUpdateSetup, indexUpdateFrame, 0, "M4"},
    {"client_arrays", clientArraysSetup, clientArraysFrame, 0, "M5"},
    {"display_lists", displayListsSetup, displayListsFrame, 0, "M6"},
    {"readback_bgra", readbackSetup, readbackFrame, 0, "M9"},
    {"pbo_upload", pboUploadSetup, pboUploadFrame, 0, "M9"},
    {"lod_bias", lodBiasSetup, lodBiasFrame, 0, "M11"},
    {"sso", ssoSetup, ssoFrame, 0, "M12"},
    {"auto_mipmap", autoMipmapSetup, autoMipmapFrame, 0, "M13"},
    {"texture_formats", textureFormatsSetup, textureFormatsFrame, 0, "M14 Q8"},
    {"fans", fansSetup, fansFrame, 0, "Q8"},
    {"shader_link", shaderLinkSetup, shaderLinkFrame, 0, "H shader cache, async link"},
    {"msaa", basicSetup, msaaFrame, 4, "H MSAA resolve"},
    {"engine", engineSetup, engineFrame, 0, "M7 M8 H unretained, direct present"},
};

static int compareDoubles(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

int main(int argc, char **argv)
{
    WNDCLASSA wc = {0};
    PIXELFORMATDESCRIPTOR pfd = {sizeof(pfd), 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER,
                                 PFD_TYPE_RGBA, 32, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 24, 8};
    static const int attribs[] = {WGL_CONTEXT_MAJOR_VERSION_ARB, 4, WGL_CONTEXT_MINOR_VERSION_ARB, 6,
                                  WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB, 0};
    const struct scene *scene = NULL;
    int frames = argc > 2 ? atoi(argv[2]) : 200, warmup = argc > 3 ? atoi(argv[3]) : 30;
    g_salt = argc > 4 ? atoi(argv[4]) : 0;
    double *times, start, last, first = 0, sum = 0;
    HWND hwnd;
    HGLRC legacy, context;
    CreateAttribs createAttribs;
    SwapInterval swapInterval;
    size_t i;
    int measured;

    if (argc < 2 || !strcmp(argv[1], "list")) {
        for (i = 0; i < sizeof(kScenes) / sizeof(kScenes[0]); i++) printf("%s\t%s\n", kScenes[i].name, kScenes[i].covers);
        return argc < 2;
    }
    for (i = 0; i < sizeof(kScenes) / sizeof(kScenes[0]); i++)
        if (!strcmp(argv[1], kScenes[i].name)) scene = &kScenes[i];
    if (!scene) { printf("unknown scene %s\n", argv[1]); return 1; }
    if (frames < 1) frames = 1;
    if (warmup < 0 || warmup >= frames + warmup) warmup = 0;

    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "wmgl_perf";
    wc.style = CS_OWNDC;
    RegisterClassA(&wc);

    /* A throwaway window and legacy context to reach the WGL extensions. */
    hwnd = CreateWindowA("wmgl_perf", "wmgl", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, NULL, NULL, wc.hInstance, NULL);
    g_dc = GetDC(hwnd);
    SetPixelFormat(g_dc, ChoosePixelFormat(g_dc, &pfd), &pfd);
    legacy = wglCreateContext(g_dc);
    wglMakeCurrent(g_dc, legacy);
    createAttribs = (CreateAttribs)wglGetProcAddress("wglCreateContextAttribsARB");
    {
        ChoosePixelFormatArb choose = (ChoosePixelFormatArb)wglGetProcAddress("wglChoosePixelFormatARB");
        HWND real = CreateWindowA("wmgl_perf", "wmgl perf probe", WS_OVERLAPPEDWINDOW, 0, 0, kWidth, kHeight, NULL, NULL,
                                  wc.hInstance, NULL);
        HDC dc = GetDC(real);
        int format = 0;
        UINT count = 0;
        if (scene->msaa && choose) {
            const int want[] = {WGL_DRAW_TO_WINDOW_ARB, 1, WGL_SUPPORT_OPENGL_ARB, 1, WGL_DOUBLE_BUFFER_ARB, 1,
                                WGL_PIXEL_TYPE_ARB, WGL_TYPE_RGBA_ARB, WGL_COLOR_BITS_ARB, 32, WGL_DEPTH_BITS_ARB, 24,
                                WGL_STENCIL_BITS_ARB, 8, WGL_SAMPLE_BUFFERS_ARB, 1, WGL_SAMPLES_ARB, scene->msaa, 0};
            if (!choose(dc, want, NULL, 1, &format, &count) || !count) { printf("no multisampled pixel format\n"); format = 0; }
        }
        if (!format) format = ChoosePixelFormat(dc, &pfd);
        DescribePixelFormat(dc, format, sizeof(pfd), &pfd);
        SetPixelFormat(dc, format, &pfd);
        wglMakeCurrent(NULL, NULL);
        wglDeleteContext(legacy);
        ReleaseDC(hwnd, g_dc);
        DestroyWindow(hwnd);
        hwnd = real;
        g_dc = dc;
    }
    ShowWindow(hwnd, SW_SHOW);
    context = createAttribs ? createAttribs(g_dc, NULL, attribs) : NULL;
    if (!context) { printf("no 4.6 compatibility context; using a legacy one\n"); context = wglCreateContext(g_dc); }
    if (!context || !wglMakeCurrent(g_dc, context)) { printf("no context\n"); return 1; }
    if (!load()) return 1;
    if ((swapInterval = (SwapInterval)wglGetProcAddress("wglSwapIntervalEXT"))) swapInterval(0);
    printf("scene %s (%s), %d frames after %d warm-up, GL %s\n", scene->name, scene->covers, frames, warmup,
           (const char *)glGetString(GL_VERSION));

    glViewport(0, 0, kWidth, kHeight);
    glClearColor(0.1f, 0.2f, 0.3f, 1.0f);
    scene->setup();
    check("setup");

    times = calloc((size_t)frames, sizeof(double));
    start = last = seconds();
    measured = 0;
    for (g_frame = 0; g_frame < warmup + frames; g_frame++) {
        MSG msg;
        double now;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) DispatchMessageA(&msg);
        scene->frame();
        SwapBuffers(g_dc);
        now = seconds();
        if (g_frame == 0) first = now - start;
        if (g_frame >= warmup) times[measured++] = now - last;
        last = now;
        if (g_frame == 0 || g_frame == warmup) check("frame");
    }
    check("frames");
    for (i = 0; i < (size_t)measured; i++) sum += times[i];
    qsort(times, (size_t)measured, sizeof(double), compareDoubles);
    printf("frames %d mean %.3f median %.3f p95 %.3f max %.3f first %.3f ms\n", measured, sum / measured * 1000.0,
           times[measured / 2] * 1000.0, times[(int)(measured * 0.95)] * 1000.0, times[measured - 1] * 1000.0,
           first * 1000.0);
    if (!strcmp(scene->name, "timer_queries")) extra("timer_gpu_ms", (double)g_timerSum / 1e6);
    fputs(g_extra, stdout);
    printf("WMGL_PERF_PROBE_OK %s\n", scene->name);
    fflush(stdout);
    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(context);
    ReleaseDC(hwnd, g_dc);
    DestroyWindow(hwnd);
    free(times);
    return 0;
}
