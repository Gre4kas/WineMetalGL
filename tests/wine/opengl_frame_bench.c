/* Frame times of a stand-in for GPU-driven, tessellated rendering (the
 * pattern WINEMETALGL_STATS showed for Foundation, issue #12), and of one
 * long render pass, for A/B comparisons of cores. Not part of the gate.
 *
 *   opengl_frame_bench.exe [foundation | foundation-tess | foundation-passes | longpass] [frames]
 *
 * foundation, per frame: three rounds of (a compute shader writes 10000
 * DrawElementsIndirectCommands, about 0.8% of them non-empty, then
 * glMultiDrawElementsIndirect draws them as quad patches through a
 * tessellation program), then 100 render passes of 6 small draws across 3
 * framebuffers, then a present. foundation-tess and foundation-passes run
 * only one of the two parts. longpass: 3000 draws with a uniform change each
 * into one framebuffer, then a present. Another first argument (the probe
 * script's marker file) runs foundation for 60 frames.
 *
 * Prints the CPU wall time between presents (swap interval 0): mean, median
 * and 95th percentile, and frames per second, then
 * WINEMETALGL_FRAME_BENCH_OK.
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

#define FUNCS(X) \
    X(PFNGLCREATESHADERPROC, glCreateShader) X(PFNGLSHADERSOURCEPROC, glShaderSource) \
    X(PFNGLCOMPILESHADERPROC, glCompileShader) X(PFNGLGETSHADERIVPROC, glGetShaderiv) \
    X(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog) X(PFNGLCREATEPROGRAMPROC, glCreateProgram) \
    X(PFNGLATTACHSHADERPROC, glAttachShader) X(PFNGLLINKPROGRAMPROC, glLinkProgram) \
    X(PFNGLGETPROGRAMIVPROC, glGetProgramiv) X(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog) \
    X(PFNGLUSEPROGRAMPROC, glUseProgram) X(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation) \
    X(PFNGLUNIFORM1UIPROC, glUniform1ui) X(PFNGLUNIFORM4FPROC, glUniform4f) \
    X(PFNGLGENBUFFERSPROC, glGenBuffers) X(PFNGLBINDBUFFERPROC, glBindBuffer) \
    X(PFNGLBUFFERDATAPROC, glBufferData) X(PFNGLBINDBUFFERBASEPROC, glBindBufferBase) \
    X(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays) X(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray) \
    X(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer) X(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray) \
    X(PFNGLDISPATCHCOMPUTEPROC, glDispatchCompute) X(PFNGLMEMORYBARRIERPROC, glMemoryBarrier) \
    X(PFNGLMULTIDRAWELEMENTSINDIRECTPROC, glMultiDrawElementsIndirect) X(PFNGLPATCHPARAMETERIPROC, glPatchParameteri) \
    X(PFNGLGENFRAMEBUFFERSPROC, glGenFramebuffers) X(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer) \
    X(PFNGLFRAMEBUFFERTEXTURE2DPROC, glFramebufferTexture2D) X(PFNGLCHECKFRAMEBUFFERSTATUSPROC, glCheckFramebufferStatus) \
    X(PFNGLACTIVETEXTUREPROC, glActiveTexture) X(PFNGLUNIFORM1IPROC, glUniform1i)
#define DECLARE(type, name) static type p_##name;
FUNCS(DECLARE)
#define LOAD(type, name) if (!(p_##name = (type)wglGetProcAddress(#name))) { printf("missing %s\n", #name); return 0; }

static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcA(h, m, w, l); }

static int load(void) { FUNCS(LOAD) return 1; }

static GLuint shader(GLenum type, const char *src)
{
    GLuint s = p_glCreateShader(type);
    GLint ok = 0;
    p_glShaderSource(s, 1, &src, NULL);
    p_glCompileShader(s);
    p_glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[2048]; p_glGetShaderInfoLog(s, sizeof(log), NULL, log); printf("compile: %s\n", log); exit(3); }
    return s;
}

static GLuint program(const char *vs, const char *tcs, const char *tes, const char *fs, const char *cs)
{
    GLuint p = p_glCreateProgram();
    GLint ok = 0;
    if (vs) p_glAttachShader(p, shader(GL_VERTEX_SHADER, vs));
    if (tcs) p_glAttachShader(p, shader(GL_TESS_CONTROL_SHADER, tcs));
    if (tes) p_glAttachShader(p, shader(GL_TESS_EVALUATION_SHADER, tes));
    if (fs) p_glAttachShader(p, shader(GL_FRAGMENT_SHADER, fs));
    if (cs) p_glAttachShader(p, shader(GL_COMPUTE_SHADER, cs));
    p_glLinkProgram(p);
    p_glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { char log[2048]; p_glGetProgramInfoLog(p, sizeof(log), NULL, log); printf("link: %s\n", log); exit(4); }
    return p;
}

static const char *kCull =
    "#version 430 core\nlayout(local_size_x = 64) in;\n"
    "struct Command { uint count, instances, first, baseVertex, baseInstance; };\n"
    "layout(std430, binding = 0) writeonly buffer Commands { Command commands[]; };\n"
    "uniform uint frame; uniform uint round;\n"
    "void main() {\n"
    "    uint i = gl_GlobalInvocationID.x;\n"
    "    if (i >= 10000u) return;\n"
    "    uint h = (i * 2654435761u) ^ (frame * 40503u + round * 977u);\n"
    "    h ^= h >> 15; h *= 2246822519u; h ^= h >> 13;\n"
    "    bool visible = (h % 1000u) < 8u;\n"   /* ~0.8%: ~75 chunks per round */
    "    uint chunk = i % 1024u;\n"
    "    commands[i] = Command(visible ? 64u : 0u, 1u, chunk * 64u, 0u, 0u);\n"
    "}\n";

static const char *kTerrainVs =
    "#version 430 core\nlayout(location = 0) in vec2 pos;\nout vec2 p;\nvoid main() { p = pos; }\n";
static const char *kTerrainTcs =
    "#version 430 core\nlayout(vertices = 4) out;\nin vec2 p[];\nout vec2 cp[];\n"
    "void main() {\n"
    "    cp[gl_InvocationID] = p[gl_InvocationID];\n"
    "    for (int i = 0; i < 4; ++i) gl_TessLevelOuter[i] = 6.0;\n"
    "    gl_TessLevelInner[0] = 6.0; gl_TessLevelInner[1] = 6.0;\n"
    "}\n";
static const char *kTerrainTes =
    "#version 430 core\nlayout(quads, equal_spacing, ccw) in;\nin vec2 cp[];\nout float h;\n"
    "void main() {\n"
    "    vec2 q = mix(mix(cp[0], cp[1], gl_TessCoord.x), mix(cp[3], cp[2], gl_TessCoord.x), gl_TessCoord.y);\n"
    "    h = 0.5 + 0.5 * sin(q.x * 9.0) * cos(q.y * 7.0);\n"
    "    gl_Position = vec4(q, h * 0.5, 1.0);\n"
    "}\n";
static const char *kColorFs =
    "#version 430 core\nin float h;\nout vec4 o;\nvoid main() { o = vec4(h, 0.6, 1.0 - h, 1.0); }\n";

static const char *kQuadVs =
    "#version 430 core\nlayout(location = 0) in vec2 pos;\nuniform vec4 at;\n"
    "void main() { gl_Position = vec4(pos * at.zw + at.xy, 0.0, 1.0); }\n";
static const char *kQuadFs =
    "#version 430 core\nuniform vec4 color;\nout vec4 o;\nvoid main() { o = color; }\n";
static const char *kBlitFs =
    "#version 430 core\nuniform sampler2D source;\nout vec4 o;\n"
    "void main() { o = texelFetch(source, ivec2(gl_FragCoord.xy) / 2, 0); }\n";

static int compare(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

int main(int argc, char **argv)
{
    PIXELFORMATDESCRIPTOR pfd = {sizeof(pfd), 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 32};
    const int attribs[] = {WGL_CONTEXT_MAJOR_VERSION_ARB, 4, WGL_CONTEXT_MINOR_VERSION_ARB, 3,
                           WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB, 0};
    const char *mode = argc > 1 && (!strcmp(argv[1], "foundation") || !strcmp(argv[1], "foundation-tess") ||
                                    !strcmp(argv[1], "foundation-passes") || !strcmp(argv[1], "longpass"))
                           ? argv[1] : "foundation";
    const int foundation = strcmp(mode, "longpass") != 0;
    const int doTess = strcmp(mode, "foundation-passes") != 0, doPasses = strcmp(mode, "foundation-tess") != 0;
    const int frames = argc > 2 && atoi(argv[2]) > 0 ? atoi(argv[2]) : 60, warmup = 20;
    WNDCLASSA wc = {0};
    HWND hwnd;
    HDC dc;
    HGLRC legacy, context;
    CreateAttribs create;
    SwapInterval interval;
    GLuint cull, terrain, quad, blit, vao, vbo, ebo, quadVao, quadVbo, quadEbo, commands[3], fbos[4], textures[4];
    GLint atLoc, colorLoc, frameLoc, roundLoc;
    LARGE_INTEGER freq, last, now, start;
    double *times = malloc(sizeof(double) * (size_t)frames), sum = 0;
    int f, i, k;

    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "wmglbench";
    wc.style = CS_OWNDC;
    RegisterClassA(&wc);
    hwnd = CreateWindowA("wmglbench", "wmgl bench", WS_POPUP, 0, 0, 1280, 720, NULL, NULL, wc.hInstance, NULL);
    dc = GetDC(hwnd);
    SetPixelFormat(dc, ChoosePixelFormat(dc, &pfd), &pfd);
    legacy = wglCreateContext(dc);
    wglMakeCurrent(dc, legacy);
    create = (CreateAttribs)wglGetProcAddress("wglCreateContextAttribsARB");
    if (!create || !(context = create(dc, NULL, attribs)) || !wglMakeCurrent(dc, context)) { printf("no 4.3 context\n"); return 1; }
    wglDeleteContext(legacy);
    if (!load()) return 2;
    if ((interval = (SwapInterval)wglGetProcAddress("wglSwapIntervalEXT"))) interval(0);

    /* Terrain: 1024 chunks of 16 quad patches over [-1, 1]^2, 64 indices each. */
    {
        float *v = malloc(sizeof(float) * 2 * 1024 * 16 * 4);
        GLuint *idx = malloc(sizeof(GLuint) * 1024 * 64);
        for (i = 0; i < 1024 * 16; ++i) {
            const float x = -1.0f + 2.0f * (float)(i % 128) / 128.0f, y = -1.0f + 2.0f * (float)(i / 128) / 128.0f, s = 2.0f / 128.0f;
            const float c[8] = {x, y, x + s, y, x + s, y + s, x, y + s};
            memcpy(v + i * 8, c, sizeof(c));
            for (k = 0; k < 4; ++k) idx[i * 4 + k] = (GLuint)(i * 4 + k);
        }
        p_glGenVertexArrays(1, &vao);
        p_glBindVertexArray(vao);
        p_glGenBuffers(1, &vbo);
        p_glBindBuffer(GL_ARRAY_BUFFER, vbo);
        p_glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 2 * 1024 * 16 * 4, v, GL_STATIC_DRAW);
        p_glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, NULL);
        p_glEnableVertexAttribArray(0);
        p_glGenBuffers(1, &ebo);
        p_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
        p_glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(GLuint) * 1024 * 64, idx, GL_STATIC_DRAW);
        free(v);
        free(idx);
    }
    p_glGenBuffers(3, commands);
    for (k = 0; k < 3; ++k) {
        p_glBindBuffer(GL_SHADER_STORAGE_BUFFER, commands[k]);
        p_glBufferData(GL_SHADER_STORAGE_BUFFER, 10000 * 20, NULL, GL_DYNAMIC_DRAW);
    }
    {
        static const float q[] = {0, 0, 1, 0, 1, 1, 0, 1};
        static const GLushort qi[] = {0, 1, 2, 0, 2, 3};
        p_glGenVertexArrays(1, &quadVao);
        p_glBindVertexArray(quadVao);
        p_glGenBuffers(1, &quadVbo);
        p_glBindBuffer(GL_ARRAY_BUFFER, quadVbo);
        p_glBufferData(GL_ARRAY_BUFFER, sizeof(q), q, GL_STATIC_DRAW);
        p_glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, NULL);
        p_glEnableVertexAttribArray(0);
        p_glGenBuffers(1, &quadEbo);
        p_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, quadEbo);
        p_glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(qi), qi, GL_STATIC_DRAW);
    }
    glGenTextures(4, textures);
    p_glGenFramebuffers(4, fbos);
    for (k = 0; k < 4; ++k) {
        glBindTexture(GL_TEXTURE_2D, textures[k]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 640, 360, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        p_glBindFramebuffer(GL_FRAMEBUFFER, fbos[k]);
        p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, textures[k], 0);
        if (p_glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { printf("fbo incomplete\n"); return 5; }
    }
    cull = program(NULL, NULL, NULL, NULL, kCull);
    terrain = program(kTerrainVs, kTerrainTcs, kTerrainTes, kColorFs, NULL);
    quad = program(kQuadVs, NULL, NULL, kQuadFs, NULL);
    blit = program(kQuadVs, NULL, NULL, kBlitFs, NULL);
    frameLoc = p_glGetUniformLocation(cull, "frame");
    roundLoc = p_glGetUniformLocation(cull, "round");
    atLoc = p_glGetUniformLocation(quad, "at");
    colorLoc = p_glGetUniformLocation(quad, "color");
    p_glPatchParameteri(GL_PATCH_VERTICES, 4);

    QueryPerformanceFrequency(&freq);
    for (f = -warmup; f < frames; ++f) {
        if (f == 0) { QueryPerformanceCounter(&start); last = start; }
        if (foundation) {
            p_glBindFramebuffer(GL_FRAMEBUFFER, fbos[0]);
            glViewport(0, 0, 640, 360);
            glClear(GL_COLOR_BUFFER_BIT);
            for (k = 0; k < (doTess ? 3 : 0); ++k) {
                p_glUseProgram(cull);
                p_glUniform1ui(frameLoc, (GLuint)(f + warmup));
                p_glUniform1ui(roundLoc, (GLuint)k);
                p_glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, commands[k]);
                p_glDispatchCompute((10000 + 63) / 64, 1, 1);
                p_glMemoryBarrier(GL_COMMAND_BARRIER_BIT);
                p_glUseProgram(terrain);
                p_glBindVertexArray(vao);
                p_glBindBuffer(GL_DRAW_INDIRECT_BUFFER, commands[k]);
                p_glMultiDrawElementsIndirect(GL_PATCHES, GL_UNSIGNED_INT, NULL, 10000, 0);
            }
            p_glUseProgram(quad);
            p_glBindVertexArray(quadVao);
            for (i = 0; i < (doPasses ? 100 : 0); ++i) {
                p_glBindFramebuffer(GL_FRAMEBUFFER, fbos[1 + i % 3]);
                for (k = 0; k < 6; ++k) {
                    p_glUniform4f(atLoc, -1.0f + 0.3f * (float)k, -1.0f + 0.02f * (float)i, 0.25f, 0.25f);
                    p_glUniform4f(colorLoc, (float)k / 6.0f, (float)i / 100.0f, 0.5f, 1.0f);
                    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, NULL);
                }
            }
        } else {
            p_glBindFramebuffer(GL_FRAMEBUFFER, fbos[0]);
            glViewport(0, 0, 640, 360);
            glClear(GL_COLOR_BUFFER_BIT);
            p_glUseProgram(quad);
            p_glBindVertexArray(quadVao);
            for (i = 0; i < 3000; ++i) {
                p_glUniform4f(atLoc, -1.0f + 0.03f * (float)(i % 64), -1.0f + 0.04f * (float)(i / 64), 0.05f, 0.05f);
                p_glUniform4f(colorLoc, (float)(i % 7) / 7.0f, (float)(i % 11) / 11.0f, 0.5f, 1.0f);
                glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, NULL);
            }
        }
        /* Present: the first framebuffer scaled onto the window. */
        p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, 1280, 720);
        p_glUseProgram(blit);
        p_glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, textures[0]);
        p_glUniform1i(p_glGetUniformLocation(blit, "source"), 0);
        p_glUniform4f(p_glGetUniformLocation(blit, "at"), -1.0f, -1.0f, 2.0f, 2.0f);
        p_glBindVertexArray(quadVao);
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, NULL);
        SwapBuffers(dc);
        if (f >= 0) {
            QueryPerformanceCounter(&now);
            times[f] = (double)(now.QuadPart - last.QuadPart) * 1000.0 / (double)freq.QuadPart;
            last = now;
        }
    }
    glFinish();
    QueryPerformanceCounter(&now);
    for (f = 0; f < frames; ++f) sum += times[f];
    qsort(times, (size_t)frames, sizeof(double), compare);
    printf("BENCH %s frames=%d mean=%.2f p50=%.2f p95=%.2f fps=%.1f error=0x%x\n", mode, frames,
           sum / frames, times[frames / 2], times[frames * 95 / 100],
           frames * (double)freq.QuadPart / (double)(now.QuadPart - start.QuadPart), glGetError());
    printf("WINEMETALGL_FRAME_BENCH_OK\n");
    return 0;
}
