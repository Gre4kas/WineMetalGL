/* scene4x: a deterministic GL 4.x frame for WineMetalGL's game corpus
 * (tests/games). A tessellated terrain (control and evaluation shaders, an
 * indirect draw, a double-precision wave height) goes through a geometry
 * shader with two invocations that each send it to one viewport of a
 * viewport array; a subroutine picks the shading; an RGBA16F + depth render
 * target is tonemapped to the default framebuffer.
 *
 *   scene4x.exe [--frames N] [--size WxH] [--profile core41|core42]
 *               [--dump out.ppm] [--hidden] [--benchmark]
 *
 * core41 (a forward-compatible 4.1 core context) is what Apple's OpenGL can
 * run, so it records the reference. core42 renders the same frame with GL 4.2
 * features: the tonemap pass stores the frame to an rgba8 image and counts
 * its fragments with an atomic counter, and a second pass displays the image.
 * Both profiles must produce the same pixels.
 *
 * Prints "SCENE4X_OK <crc32>" on success; the CRC covers the last frame's
 * default-framebuffer pixels (RGBA8, bottom-up). Build with mingw:
 *   x86_64-w64-mingw32-gcc -O2 scene4x.c -o scene4x.exe -lopengl32 -lgdi32 -luser32
 */
#include <windows.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/wglext.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GLFUNCS(X) \
    X(PFNGLGENBUFFERSPROC, glGenBuffers) X(PFNGLBINDBUFFERPROC, glBindBuffer) X(PFNGLBUFFERDATAPROC, glBufferData) \
    X(PFNGLBUFFERSUBDATAPROC, glBufferSubData) X(PFNGLBINDBUFFERBASEPROC, glBindBufferBase) \
    X(PFNGLGETBUFFERSUBDATAPROC, glGetBufferSubData) \
    X(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays) X(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray) \
    X(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray) X(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer) \
    X(PFNGLCREATESHADERPROC, glCreateShader) X(PFNGLSHADERSOURCEPROC, glShaderSource) \
    X(PFNGLCOMPILESHADERPROC, glCompileShader) X(PFNGLGETSHADERIVPROC, glGetShaderiv) \
    X(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog) X(PFNGLCREATEPROGRAMPROC, glCreateProgram) \
    X(PFNGLATTACHSHADERPROC, glAttachShader) X(PFNGLLINKPROGRAMPROC, glLinkProgram) \
    X(PFNGLGETPROGRAMIVPROC, glGetProgramiv) X(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog) \
    X(PFNGLUSEPROGRAMPROC, glUseProgram) X(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation) \
    X(PFNGLUNIFORM1IPROC, glUniform1i) X(PFNGLUNIFORMMATRIX4FVPROC, glUniformMatrix4fv) \
    X(PFNGLBINDFRAGDATALOCATIONPROC, glBindFragDataLocation) X(PFNGLACTIVETEXTUREPROC, glActiveTexture) \
    X(PFNGLGENFRAMEBUFFERSPROC, glGenFramebuffers) X(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer) \
    X(PFNGLFRAMEBUFFERTEXTURE2DPROC, glFramebufferTexture2D) X(PFNGLCHECKFRAMEBUFFERSTATUSPROC, glCheckFramebufferStatus) \
    X(PFNGLPATCHPARAMETERIPROC, glPatchParameteri) X(PFNGLDRAWARRAYSINDIRECTPROC, glDrawArraysIndirect) \
    X(PFNGLUNIFORM2DPROC, glUniform2d) X(PFNGLGETSUBROUTINEINDEXPROC, glGetSubroutineIndex) \
    X(PFNGLUNIFORMSUBROUTINESUIVPROC, glUniformSubroutinesuiv) X(PFNGLVIEWPORTINDEXEDFPROC, glViewportIndexedf)
#define GLFUNCS42(X) \
    X(PFNGLBINDIMAGETEXTUREPROC, glBindImageTexture) X(PFNGLMEMORYBARRIERPROC, glMemoryBarrier) \
    X(PFNGLTEXSTORAGE2DPROC, glTexStorage2D)
#define DECLARE(type, name) static type p_##name;
GLFUNCS(DECLARE)
GLFUNCS42(DECLARE)
#define LOAD(type, name) p_##name = (type)wglGetProcAddress(#name); if (!p_##name) { fprintf(stderr, "missing %s\n", #name); return 0; }

/* The first GL error after each step, with the step's name. */
static void check(const char* step) {
    static int reported = 0;
    const GLenum e = glGetError();
    if (e && !reported++) fprintf(stderr, "GL error 0x%04x after %s\n", e, step);
}

static int load_funcs(int gl42) {
    GLFUNCS(LOAD)
    if (gl42) { GLFUNCS42(LOAD) }
    return 1;
}

/* The terrain: 4-vertex patches on the XZ plane, tessellated 8 x 8. */
static const char* vs_terrain =
    "#version 410 core\n"
    "layout(location = 0) in vec2 grid;\n"
    "out vec2 v_grid;\n"
    "void main() { v_grid = grid; }\n";
static const char* tcs_terrain =
    "#version 410 core\n"
    "layout(vertices = 4) out;\n"
    "in vec2 v_grid[];\n"
    "out vec2 c_grid[];\n"
    "void main() {\n"
    "    c_grid[gl_InvocationID] = v_grid[gl_InvocationID];\n"
    "    if (gl_InvocationID == 0) {\n"
    "        gl_TessLevelInner[0] = 8.0; gl_TessLevelInner[1] = 8.0;\n"
    "        gl_TessLevelOuter[0] = 8.0; gl_TessLevelOuter[1] = 8.0;\n"
    "        gl_TessLevelOuter[2] = 8.0; gl_TessLevelOuter[3] = 8.0;\n"
    "    }\n"
    "}\n";
static const char* tes_terrain =
    "#version 410 core\n"
    "layout(quads, equal_spacing, ccw) in;\n"
    "uniform dvec2 wave;  /* amplitude, phase */\n"
    "in vec2 c_grid[];\n"
    "out vec3 t_normal; out float t_height;\n"
    "void main() {\n"
    "    vec2 p = mix(mix(c_grid[0], c_grid[1], gl_TessCoord.x), mix(c_grid[3], c_grid[2], gl_TessCoord.x), gl_TessCoord.y);\n"
    "    float phase = float(wave.y);\n"
    "    double h = wave.x * double(sin(p.x * 0.6 + phase) + cos(p.y * 0.5 - phase * 0.5));\n"
    "    float dx = float(wave.x) * 0.6 * cos(p.x * 0.6 + phase);\n"
    "    float dz = -float(wave.x) * 0.5 * sin(p.y * 0.5 - phase * 0.5);\n"
    "    t_normal = normalize(vec3(-dx, 1.0, -dz));\n"
    "    t_height = float(h);\n"
    "    gl_Position = vec4(p.x, float(h), p.y, 1.0);\n"
    "}\n";
static const char* gs_terrain =
    "#version 410 core\n"
    "layout(triangles, invocations = 2) in;\n"
    "layout(triangle_strip, max_vertices = 3) out;\n"
    "uniform mat4 view_proj[2];\n"
    "in vec3 t_normal[]; in float t_height[];\n"
    "out vec3 g_normal; out float g_height;\n"
    "void main() {\n"
    "    for (int i = 0; i < 3; ++i) {\n"
    "        gl_Position = view_proj[gl_InvocationID] * gl_in[i].gl_Position;\n"
    "        gl_ViewportIndex = gl_InvocationID;\n"
    "        g_normal = t_normal[i]; g_height = t_height[i];\n"
    "        EmitVertex();\n"
    "    }\n"
    "    EndPrimitive();\n"
    "}\n";
static const char* fs_terrain =
    "#version 410 core\n"
    "subroutine vec3 Shade(vec3 n, float h);\n"
    "subroutine(Shade) vec3 sunny(vec3 n, float h) {\n"
    "    float d = max(dot(n, normalize(vec3(0.4, 1.0, 0.3))), 0.0);\n"
    "    return mix(vec3(0.15, 0.45, 0.12), vec3(0.85, 0.8, 0.6), clamp(h * 0.4 + 0.5, 0.0, 1.0)) * (0.3 + 1.6 * d);\n"
    "}\n"
    "subroutine(Shade) vec3 dusk(vec3 n, float h) {\n"
    "    float d = max(dot(n, normalize(vec3(-0.6, 0.5, 0.2))), 0.0);\n"
    "    return mix(vec3(0.1, 0.12, 0.3), vec3(1.2, 0.5, 0.25), clamp(h * 0.4 + 0.5, 0.0, 1.0)) * (0.2 + 1.8 * d);\n"
    "}\n"
    "subroutine uniform Shade shade;\n"
    "in vec3 g_normal; in float g_height;\n"
    "out vec4 color;\n"
    "void main() { color = vec4(shade(normalize(g_normal), g_height), 1.0); }\n";

/* Full-screen passes. */
static const char* vs_post =
    "#version 410 core\n"
    "void main() {\n"
    "    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);\n"
    "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";
#define TONEMAP \
    "vec3 tonemap(vec3 c) { c = c / (c + vec3(1.0)); return pow(c, vec3(1.0 / 2.2)); }\n"
static const char* fs_post41 =
    "#version 410 core\n"
    "uniform sampler2D hdr;\n"
    "out vec4 color;\n" TONEMAP
    "void main() { color = vec4(tonemap(texelFetch(hdr, ivec2(gl_FragCoord.xy), 0).rgb), 1.0); }\n";
/* core42: the tonemapped frame goes to an image, every fragment counted. */
static const char* fs_store42 =
    "#version 420 core\n"
    "layout(binding = 0) uniform sampler2D hdr;\n"
    "layout(rgba8, binding = 0) writeonly uniform image2D ldr;\n"
    "layout(binding = 0, offset = 0) uniform atomic_uint written;\n"
    "out vec4 color;\n" TONEMAP
    "void main() {\n"
    "    ivec2 p = ivec2(gl_FragCoord.xy);\n"
    "    imageStore(ldr, p, vec4(tonemap(texelFetch(hdr, p, 0).rgb), 1.0));\n"
    "    atomicCounterIncrement(written);\n"
    "    color = vec4(0.0);\n"
    "}\n";
static const char* fs_show42 =
    "#version 420 core\n"
    "layout(binding = 1) uniform sampler2D ldr;\n"
    "out vec4 color;\n"
    "void main() { color = texelFetch(ldr, ivec2(gl_FragCoord.xy), 0); }\n";

static GLuint compile(GLenum type, const char* source) {
    GLuint shader = p_glCreateShader(type);
    GLint ok = 0;
    p_glShaderSource(shader, 1, &source, NULL);
    p_glCompileShader(shader);
    p_glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[2048]; p_glGetShaderInfoLog(shader, sizeof(log), NULL, log); fprintf(stderr, "compile failed: %s\n", log); exit(2); }
    return shader;
}

static GLuint link_stages(const char* vs, const char* tcs, const char* tes, const char* gs, const char* fs) {
    GLuint program = p_glCreateProgram();
    GLint ok = 0;
    p_glAttachShader(program, compile(GL_VERTEX_SHADER, vs));
    if (tcs) p_glAttachShader(program, compile(GL_TESS_CONTROL_SHADER, tcs));
    if (tes) p_glAttachShader(program, compile(GL_TESS_EVALUATION_SHADER, tes));
    if (gs) p_glAttachShader(program, compile(GL_GEOMETRY_SHADER, gs));
    p_glAttachShader(program, compile(GL_FRAGMENT_SHADER, fs));
    p_glBindFragDataLocation(program, 0, "color");
    p_glLinkProgram(program);
    p_glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) { char log[2048]; p_glGetProgramInfoLog(program, sizeof(log), NULL, log); fprintf(stderr, "link failed: %s\n", log); exit(2); }
    return program;
}

static void perspective_look(float* m, float aspect, float t, float yaw) {
    /* view_proj = P * V for a camera circling the terrain; column-major. */
    float eye[3] = {11.0f * cosf(t * 0.2f + yaw), 6.0f, 11.0f * sinf(t * 0.2f + yaw)}, at[3] = {0, 0, 0}, up[3] = {0, 1, 0};
    float f[3] = {at[0] - eye[0], at[1] - eye[1], at[2] - eye[2]}, s[3], u[3], v[16], p[16];
    float len = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    int i, j, k;
    for (i = 0; i < 3; ++i) f[i] /= len;
    s[0] = f[1] * up[2] - f[2] * up[1]; s[1] = f[2] * up[0] - f[0] * up[2]; s[2] = f[0] * up[1] - f[1] * up[0];
    len = sqrtf(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
    for (i = 0; i < 3; ++i) s[i] /= len;
    u[0] = s[1] * f[2] - s[2] * f[1]; u[1] = s[2] * f[0] - s[0] * f[2]; u[2] = s[0] * f[1] - s[1] * f[0];
    memset(v, 0, sizeof(v));
    v[0] = s[0]; v[4] = s[1]; v[8] = s[2];
    v[1] = u[0]; v[5] = u[1]; v[9] = u[2];
    v[2] = -f[0]; v[6] = -f[1]; v[10] = -f[2];
    v[12] = -(s[0] * eye[0] + s[1] * eye[1] + s[2] * eye[2]);
    v[13] = -(u[0] * eye[0] + u[1] * eye[1] + u[2] * eye[2]);
    v[14] = f[0] * eye[0] + f[1] * eye[1] + f[2] * eye[2];
    v[15] = 1.0f;
    {
        float fov = 1.0f / tanf(0.5f * 1.0f), zn = 0.1f, zf = 100.0f;
        memset(p, 0, sizeof(p));
        p[0] = fov / aspect; p[5] = fov; p[10] = (zf + zn) / (zn - zf); p[11] = -1.0f; p[14] = 2.0f * zf * zn / (zn - zf);
    }
    for (i = 0; i < 4; ++i)
        for (j = 0; j < 4; ++j) {
            float sum = 0;
            for (k = 0; k < 4; ++k) sum += p[k * 4 + j] * v[i * 4 + k];
            m[i * 4 + j] = sum;
        }
}

static unsigned crc32_bytes(const unsigned char* data, size_t size) {
    unsigned crc = 0xFFFFFFFFu;
    size_t i;
    int b;
    for (i = 0; i < size; ++i) {
        crc ^= data[i];
        for (b = 0; b < 8; ++b) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) { return DefWindowProcA(hwnd, msg, wp, lp); }

/* --benchmark: vsync off, per-frame wall times (start of a frame to the end
 * of SwapBuffers) after a warm-up, without the final readback frame. */
static int compare_double(const void* a, const void* b) {
    const double x = *(const double*)a, y = *(const double*)b;
    return x < y ? -1 : x > y;
}

static void report_frame_times(double* times, int count) {
    const int warmup = count / 4 < 10 ? count / 4 : 10;
    int n = count - warmup - 1, k;
    double sum = 0.0, max = 0.0;
    if (n < 1) return;
    for (k = 0; k < n; ++k) {
        sum += times[warmup + k];
        if (times[warmup + k] > max) max = times[warmup + k];
    }
    qsort(times + warmup, (size_t)n, sizeof(double), compare_double);
    printf("FRAMETIME avg=%.3f p95=%.3f max=%.3f frames=%d\n", sum / n, times[warmup + (n - 1) * 95 / 100], max, n);
}

int main(int argc, char** argv) {
    int frames = 120, width = 640, height = 360, gl42 = 0, hidden = 0, benchmark = 0, i, frame;
    const char* dump = NULL;
    double* frame_ms = NULL;
    unsigned counted = 0;
    LARGE_INTEGER frequency, frame_start, frame_end;
    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--size") && i + 1 < argc) sscanf(argv[++i], "%dx%d", &width, &height);
        else if (!strcmp(argv[i], "--profile") && i + 1 < argc) gl42 = !strcmp(argv[++i], "core42");
        else if (!strcmp(argv[i], "--dump") && i + 1 < argc) dump = argv[++i];
        else if (!strcmp(argv[i], "--hidden")) hidden = 1;
        else if (!strcmp(argv[i], "--benchmark")) benchmark = 1;
    }
    if (frames < 1) frames = 1;
    width &= ~1;  /* two viewports of equal width */

    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "scene4x";
    RegisterClassA(&wc);
    RECT rect = {0, 0, width, height};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    HWND window = CreateWindowA("scene4x", "scene4x", WS_OVERLAPPEDWINDOW, 0, 0, rect.right - rect.left,
                                rect.bottom - rect.top, NULL, NULL, wc.hInstance, NULL);
    if (!window) return 1;
    if (!hidden) ShowWindow(window, SW_SHOW);
    HDC dc = GetDC(window);
    PIXELFORMATDESCRIPTOR pfd;
    memset(&pfd, 0, sizeof(pfd));
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    int format = ChoosePixelFormat(dc, &pfd);
    if (!format || !SetPixelFormat(dc, format, &pfd)) return 1;
    HGLRC legacy = wglCreateContext(dc);
    if (!legacy || !wglMakeCurrent(dc, legacy)) return 1;
    PFNWGLCREATECONTEXTATTRIBSARBPROC create = (PFNWGLCREATECONTEXTATTRIBSARBPROC)wglGetProcAddress("wglCreateContextAttribsARB");
    if (!create) { fprintf(stderr, "no wglCreateContextAttribsARB\n"); return 1; }
    int attribs[] = {WGL_CONTEXT_MAJOR_VERSION_ARB, 4, WGL_CONTEXT_MINOR_VERSION_ARB, gl42 ? 2 : 1,
                     WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
                     WGL_CONTEXT_FLAGS_ARB, WGL_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB, 0};
    HGLRC context = create(dc, NULL, attribs);
    if (!context) { fprintf(stderr, "no GL 4.%d core context\n", gl42 ? 2 : 1); return 1; }
    wglMakeCurrent(dc, context);
    wglDeleteContext(legacy);
    if (!load_funcs(gl42)) return 1;
    printf("GL_VERSION %s\n", (const char*)glGetString(GL_VERSION));

    /* 16 x 16 patches over [-8, 8]^2, four corners each. */
    enum { kPatches = 16 };
    static float grid[kPatches * kPatches * 4][2];
    int px, pz, n = 0;
    for (pz = 0; pz < kPatches; ++pz)
        for (px = 0; px < kPatches; ++px) {
            const float x0 = -8.0f + (float)px, z0 = -8.0f + (float)pz;
            grid[n][0] = x0;        grid[n++][1] = z0;
            grid[n][0] = x0 + 1.0f; grid[n++][1] = z0;
            grid[n][0] = x0 + 1.0f; grid[n++][1] = z0 + 1.0f;
            grid[n][0] = x0;        grid[n++][1] = z0 + 1.0f;
        }
    const GLuint command[4] = {(GLuint)n, 1, 0, 0};  /* count, instances, first, base instance */
    GLuint buffers[3], vaos[2];
    p_glGenBuffers(3, buffers);
    p_glGenVertexArrays(2, vaos);
    p_glBindVertexArray(vaos[0]);
    p_glBindBuffer(GL_ARRAY_BUFFER, buffers[0]);
    p_glBufferData(GL_ARRAY_BUFFER, sizeof(grid), grid, GL_STATIC_DRAW);
    p_glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (void*)0);
    p_glEnableVertexAttribArray(0);
    p_glBindBuffer(GL_DRAW_INDIRECT_BUFFER, buffers[1]);
    p_glBufferData(GL_DRAW_INDIRECT_BUFFER, sizeof(command), command, GL_STATIC_DRAW);
    p_glPatchParameteri(GL_PATCH_VERTICES, 4);
    check("patch setup");

    /* HDR target and, for core42, the stored frame. */
    GLuint textures[3], fbo;
    glGenTextures(3, textures);
    glBindTexture(GL_TEXTURE_2D, textures[0]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA, GL_HALF_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, textures[1]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, width, height, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    p_glGenFramebuffers(1, &fbo);
    p_glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, textures[0], 0);
    p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, textures[1], 0);
    if (p_glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { fprintf(stderr, "FBO incomplete\n"); return 1; }
    p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
    check("fbo setup");
    if (gl42) {
        glBindTexture(GL_TEXTURE_2D, textures[2]);
        p_glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, width, height);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        p_glBindBuffer(GL_ATOMIC_COUNTER_BUFFER, buffers[2]);
        p_glBufferData(GL_ATOMIC_COUNTER_BUFFER, sizeof(GLuint), NULL, GL_DYNAMIC_READ);
        p_glBindBufferBase(GL_ATOMIC_COUNTER_BUFFER, 0, buffers[2]);
    }

    check("resources");
    GLuint terrain = link_stages(vs_terrain, tcs_terrain, tes_terrain, gs_terrain, fs_terrain);
    GLuint post = 0, store = 0, show = 0;
    const GLint view_proj = p_glGetUniformLocation(terrain, "view_proj"), wave = p_glGetUniformLocation(terrain, "wave");
    const GLuint shades[2] = {p_glGetSubroutineIndex(terrain, GL_FRAGMENT_SHADER, "sunny"),
                              p_glGetSubroutineIndex(terrain, GL_FRAGMENT_SHADER, "dusk")};
    if (gl42) {
        store = link_stages(vs_post, NULL, NULL, NULL, fs_store42);
        show = link_stages(vs_post, NULL, NULL, NULL, fs_show42);
    } else {
        post = link_stages(vs_post, NULL, NULL, NULL, fs_post41);
        p_glUseProgram(post);
        p_glUniform1i(p_glGetUniformLocation(post, "hdr"), 0);
    }

    unsigned char* readback = (unsigned char*)malloc((size_t)width * height * 4);
    frame_ms = calloc((size_t)frames, sizeof(double));
    QueryPerformanceFrequency(&frequency);
    if (benchmark) {
        PFNWGLSWAPINTERVALEXTPROC swap_interval = (PFNWGLSWAPINTERVALEXTPROC)wglGetProcAddress("wglSwapIntervalEXT");
        if (swap_interval) swap_interval(0);
    }
    for (frame = 0; frame < frames; ++frame) {
        QueryPerformanceCounter(&frame_start);
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
        float t = (float)frame / 30.0f, cameras[32];
        const float aspect = (float)(width / 2) / (float)height;
        perspective_look(cameras, aspect, t, 0.0f);
        perspective_look(cameras + 16, aspect, t, 2.2f);

        /* Terrain into the HDR target, left and right viewports. */
        p_glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glViewport(0, 0, width, height);
        glClearColor(0.05f, 0.07f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        check("hdr clear");
        p_glViewportIndexedf(0, 0.0f, 0.0f, (float)(width / 2), (float)height);
        p_glViewportIndexedf(1, (float)(width / 2), 0.0f, (float)(width / 2), (float)height);
        check("viewports");
        glEnable(GL_DEPTH_TEST);
        p_glUseProgram(terrain);
        check("use terrain");
        p_glUniformMatrix4fv(view_proj, 2, GL_FALSE, cameras);
        check("view_proj");
        p_glUniform2d(wave, 0.9, (double)t);
        check("wave");
        p_glUniformSubroutinesuiv(GL_FRAGMENT_SHADER, 1, &shades[frame & 1]);
        check("terrain uniforms");
        p_glBindVertexArray(vaos[0]);
        p_glDrawArraysIndirect(GL_PATCHES, (void*)0);
        check("terrain draw");
        glDisable(GL_DEPTH_TEST);

        /* Tonemap to the default framebuffer. */
        p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, width, height);
        p_glBindVertexArray(vaos[1]);
        p_glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, textures[0]);
        if (gl42) {
            const GLuint zero = 0;
            p_glBufferSubData(GL_ATOMIC_COUNTER_BUFFER, 0, sizeof(zero), &zero);
            p_glBindImageTexture(0, textures[2], 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
            glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
            p_glUseProgram(store);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            p_glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_ATOMIC_COUNTER_BARRIER_BIT);
            p_glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, textures[2]);
            p_glUseProgram(show);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            p_glActiveTexture(GL_TEXTURE0);
        } else {
            p_glUseProgram(post);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }
        if (frame == frames - 1) {
            glPixelStorei(GL_PACK_ALIGNMENT, 4);
            glReadBuffer(GL_BACK);
            glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, readback);
            if (gl42) p_glGetBufferSubData(GL_ATOMIC_COUNTER_BUFFER, 0, sizeof(counted), &counted);
        }
        SwapBuffers(dc);
        QueryPerformanceCounter(&frame_end);
        frame_ms[frame] = 1000.0 * (double)(frame_end.QuadPart - frame_start.QuadPart) / (double)frequency.QuadPart;
    }
    if (benchmark) report_frame_times(frame_ms, frames);
    free(frame_ms);
    check("frame");
    GLenum error = glGetError();
    if (gl42 && counted != (unsigned)(width * height)) {
        fprintf(stderr, "atomic counter: %u fragments, expected %d\n", counted, width * height);
        error = error ? error : GL_INVALID_OPERATION;
    }
    unsigned crc = crc32_bytes(readback, (size_t)width * height * 4);
    if (dump) {
        FILE* out = fopen(dump, "wb");
        if (out) {
            int y, x;
            fprintf(out, "P6\n%d %d\n255\n", width, height);
            for (y = height - 1; y >= 0; --y)
                for (x = 0; x < width; ++x) fwrite(readback + ((size_t)y * width + x) * 4, 1, 3, out);
            fclose(out);
        }
    }
    printf("glGetError 0x%04x\n", error);
    printf("SCENE4X_%s %08x\n", error == GL_NO_ERROR ? "OK" : "GLERROR", crc);
    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(context);
    return error == GL_NO_ERROR ? 0 : 3;
}
