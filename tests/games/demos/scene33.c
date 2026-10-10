/* scene33: a deterministic GL 3.3 "mini engine" frame for WineMetalGL's game
 * corpus (tests/games). It exercises the paths games hit every frame: a UBO
 * camera, interleaved VBO + u16 IBO, instanced draws with attribute
 * divisors, a mipmapped texture, an RGBA16F + depth render target,
 * a post-process pass to the default framebuffer, blended point sprites and
 * readback. Animation is driven by the frame index only, so every run of the
 * same build produces the same image.
 *
 *   scene33.exe [--frames N] [--size WxH] [--profile core|compat]
 *               [--dump out.ppm] [--hidden] [--benchmark] [--no-forward]
 *
 * Core contexts are forward-compatible unless --no-forward is given (macOS
 * GL only offers forward-compatible 3.2+ contexts, and many cross-platform
 * engines request them).
 *
 * Prints "SCENE33_OK <crc32>" on success; the CRC covers the last frame's
 * default-framebuffer pixels (RGBA8, bottom-up). Build with mingw:
 *   x86_64-w64-mingw32-gcc -O2 scene33.c -o scene33.exe -lopengl32 -lgdi32 -luser32
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
    X(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays) X(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray) \
    X(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray) X(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer) \
    X(PFNGLVERTEXATTRIBDIVISORPROC, glVertexAttribDivisor) X(PFNGLCREATESHADERPROC, glCreateShader) \
    X(PFNGLSHADERSOURCEPROC, glShaderSource) X(PFNGLCOMPILESHADERPROC, glCompileShader) \
    X(PFNGLGETSHADERIVPROC, glGetShaderiv) X(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog) \
    X(PFNGLCREATEPROGRAMPROC, glCreateProgram) X(PFNGLATTACHSHADERPROC, glAttachShader) \
    X(PFNGLLINKPROGRAMPROC, glLinkProgram) X(PFNGLGETPROGRAMIVPROC, glGetProgramiv) \
    X(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog) X(PFNGLUSEPROGRAMPROC, glUseProgram) \
    X(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation) X(PFNGLUNIFORM1IPROC, glUniform1i) \
    X(PFNGLUNIFORM1FPROC, glUniform1f) X(PFNGLUNIFORM2FPROC, glUniform2f) \
    X(PFNGLGETUNIFORMBLOCKINDEXPROC, glGetUniformBlockIndex) X(PFNGLUNIFORMBLOCKBINDINGPROC, glUniformBlockBinding) \
    X(PFNGLBINDATTRIBLOCATIONPROC, glBindAttribLocation) X(PFNGLBINDFRAGDATALOCATIONPROC, glBindFragDataLocation) \
    X(PFNGLACTIVETEXTUREPROC, glActiveTexture) X(PFNGLGENERATEMIPMAPPROC, glGenerateMipmap) \
    X(PFNGLGENFRAMEBUFFERSPROC, glGenFramebuffers) X(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer) \
    X(PFNGLFRAMEBUFFERTEXTURE2DPROC, glFramebufferTexture2D) X(PFNGLCHECKFRAMEBUFFERSTATUSPROC, glCheckFramebufferStatus) \
    X(PFNGLDRAWELEMENTSINSTANCEDPROC, glDrawElementsInstanced) X(PFNGLGENSAMPLERSPROC, glGenSamplers) \
    X(PFNGLBINDSAMPLERPROC, glBindSampler) X(PFNGLSAMPLERPARAMETERIPROC, glSamplerParameteri) \
    X(PFNGLBLENDFUNCSEPARATEPROC, glBlendFuncSeparate)
#define DECLARE(type, name) static type p_##name;
GLFUNCS(DECLARE)
#define LOAD(type, name) p_##name = (type)wglGetProcAddress(#name); if (!p_##name) { fprintf(stderr, "missing %s\n", #name); return 0; }

static int load_funcs(void) { GLFUNCS(LOAD) return 1; }

static const char* vs_scene =
    "#version 330 core\n"
    "layout(std140) uniform Camera { mat4 view_proj; vec4 light_dir; vec4 time; };\n"
    "layout(location = 0) in vec3 position;\n"
    "layout(location = 1) in vec3 normal;\n"
    "layout(location = 2) in vec2 uv;\n"
    "layout(location = 3) in vec4 instance;  /* xyz offset, w scale */\n"
    "out vec3 v_normal; out vec2 v_uv; out float v_height;\n"
    "void main() {\n"
    "    float spin = time.x * 0.7 + instance.x;\n"
    "    mat3 rot = mat3(cos(spin), 0.0, -sin(spin), 0.0, 1.0, 0.0, sin(spin), 0.0, cos(spin));\n"
    "    vec3 world = rot * (position * instance.w) + instance.xyz;\n"
    "    v_normal = rot * normal; v_uv = uv; v_height = world.y;\n"
    "    gl_Position = view_proj * vec4(world, 1.0);\n"
    "}\n";
static const char* fs_scene =
    "#version 330 core\n"
    "layout(std140) uniform Camera { mat4 view_proj; vec4 light_dir; vec4 time; };\n"
    "uniform sampler2D albedo;\n"
    "in vec3 v_normal; in vec2 v_uv; in float v_height;\n"
    "out vec4 color;\n"
    "void main() {\n"
    "    float diffuse = max(dot(normalize(v_normal), normalize(light_dir.xyz)), 0.0);\n"
    "    vec3 base = texture(albedo, v_uv).rgb;\n"
    "    color = vec4(base * (0.25 + 1.4 * diffuse) + vec3(0.02, 0.03, 0.05) * v_height, 1.0);\n"
    "}\n";
static const char* vs_post =
    "#version 330 core\n"
    "out vec2 v_uv;\n"
    "void main() {\n"
    "    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);\n"
    "    v_uv = p; gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";
static const char* fs_post =
    "#version 330 core\n"
    "uniform sampler2D hdr; uniform vec2 inv_size;\n"
    "in vec2 v_uv; out vec4 color;\n"
    "void main() {\n"
    "    vec3 c = texture(hdr, v_uv).rgb * 0.5;\n"
    "    c += texture(hdr, v_uv + vec2(inv_size.x, 0.0)).rgb * 0.125 + texture(hdr, v_uv - vec2(inv_size.x, 0.0)).rgb * 0.125;\n"
    "    c += texture(hdr, v_uv + vec2(0.0, inv_size.y)).rgb * 0.125 + texture(hdr, v_uv - vec2(0.0, inv_size.y)).rgb * 0.125;\n"
    "    c = c / (c + vec3(1.0));\n"
    "    float vignette = 1.0 - 0.6 * dot(v_uv - 0.5, v_uv - 0.5);\n"
    "    color = vec4(pow(c * vignette, vec3(1.0 / 2.2)), 1.0);\n"
    "}\n";
static const char* vs_points =
    "#version 330 core\n"
    "layout(std140) uniform Camera { mat4 view_proj; vec4 light_dir; vec4 time; };\n"
    "layout(location = 0) in vec4 particle;\n"
    "out float v_alpha;\n"
    "void main() {\n"
    "    vec3 p = particle.xyz + vec3(0.0, fract(time.x * 0.1 + particle.w) * 3.0, 0.0);\n"
    "    v_alpha = 1.0 - fract(time.x * 0.1 + particle.w);\n"
    "    gl_Position = view_proj * vec4(p, 1.0);\n"
    "    gl_PointSize = 6.0;\n"
    "}\n";
static const char* fs_points =
    "#version 330 core\n"
    "in float v_alpha; out vec4 color;\n"
    "void main() {\n"
    "    vec2 d = gl_PointCoord - 0.5;\n"
    "    if (dot(d, d) > 0.25) discard;\n"
    "    color = vec4(1.0, 0.7, 0.3, v_alpha * 0.6);\n"
    "}\n";

static GLuint compile(GLenum type, const char* source) {
    GLuint shader = p_glCreateShader(type);
    GLint ok = 0;
    p_glShaderSource(shader, 1, &source, NULL);
    p_glCompileShader(shader);
    p_glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[2048]; p_glGetShaderInfoLog(shader, sizeof(log), NULL, log); fprintf(stderr, "compile failed: %s\n", log); exit(2); }
    return shader;
}

static GLuint link(const char* vs, const char* fs) {
    GLuint program = p_glCreateProgram();
    GLint ok = 0;
    p_glAttachShader(program, compile(GL_VERTEX_SHADER, vs));
    p_glAttachShader(program, compile(GL_FRAGMENT_SHADER, fs));
    p_glBindFragDataLocation(program, 0, "color");
    p_glLinkProgram(program);
    p_glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) { char log[2048]; p_glGetProgramInfoLog(program, sizeof(log), NULL, log); fprintf(stderr, "link failed: %s\n", log); exit(2); }
    return program;
}

static void perspective_look(float* m, float aspect, float t) {
    /* view_proj = P * V for a camera orbiting the origin; column-major. */
    float eye[3] = {9.0f * cosf(t * 0.2f), 5.0f, 9.0f * sinf(t * 0.2f)}, at[3] = {0, 0.5f, 0}, up[3] = {0, 1, 0};
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
    int frames = 120, width = 640, height = 360, compat = 0, hidden = 0, forward = 1, i, frame;
    const char* dump = NULL;
    int benchmark = 0;
    double* frame_ms = NULL;
    LARGE_INTEGER frequency, frame_start, frame_end;
    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--size") && i + 1 < argc) sscanf(argv[++i], "%dx%d", &width, &height);
        else if (!strcmp(argv[i], "--profile") && i + 1 < argc) compat = !strcmp(argv[++i], "compat");
        else if (!strcmp(argv[i], "--dump") && i + 1 < argc) dump = argv[++i];
        else if (!strcmp(argv[i], "--hidden")) hidden = 1;
        else if (!strcmp(argv[i], "--benchmark")) benchmark = 1;
        else if (!strcmp(argv[i], "--no-forward")) forward = 0;
    }

    WNDCLASSA wc = {0};
    wc.lpfnWndProc = wndproc; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "scene33"; wc.style = CS_OWNDC;
    RegisterClassA(&wc);
    RECT rect = {0, 0, width, height};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowA("scene33", "scene33", WS_OVERLAPPEDWINDOW, 0, 0, rect.right - rect.left, rect.bottom - rect.top, NULL, NULL, wc.hInstance, NULL);
    if (!hidden) ShowWindow(hwnd, SW_SHOW);
    HDC dc = GetDC(hwnd);
    PIXELFORMATDESCRIPTOR pfd = {sizeof(pfd), 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 32};
    pfd.cDepthBits = 24; pfd.cStencilBits = 8;
    SetPixelFormat(dc, ChoosePixelFormat(dc, &pfd), &pfd);
    HGLRC legacy = wglCreateContext(dc);
    wglMakeCurrent(dc, legacy);
    PFNWGLCREATECONTEXTATTRIBSARBPROC create = (PFNWGLCREATECONTEXTATTRIBSARBPROC)wglGetProcAddress("wglCreateContextAttribsARB");
    if (!create) { fprintf(stderr, "no wglCreateContextAttribsARB\n"); return 1; }
    int attribs[] = {WGL_CONTEXT_MAJOR_VERSION_ARB, 3, WGL_CONTEXT_MINOR_VERSION_ARB, 3, WGL_CONTEXT_PROFILE_MASK_ARB,
                     compat ? WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB : WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
                     WGL_CONTEXT_FLAGS_ARB, (forward && !compat) ? WGL_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB : 0, 0};
    HGLRC context = create(dc, NULL, attribs);
    if (!context) { fprintf(stderr, "3.3 %s context creation failed\n", compat ? "compat" : "core"); return 1; }
    wglMakeCurrent(dc, context);
    wglDeleteContext(legacy);
    if (!load_funcs()) return 1;
    printf("GL_VERSION %s\nGL_RENDERER %s\n", glGetString(GL_VERSION), glGetString(GL_RENDERER));

    /* Geometry: a unit cube (24 verts) followed by a ground quad (4 verts). */
    float vertices[28 * 8];
    unsigned short indices[36 + 6];
    {
        static const float face_n[6][3] = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
        int f, v = 0, n = 0;
        for (f = 0; f < 6; ++f) {
            float nx = face_n[f][0], ny = face_n[f][1], nz = face_n[f][2];
            float ax[3] = {ny, nz, nx}, bx[3];
            bx[0] = ny * ax[2] - nz * ax[1]; bx[1] = nz * ax[0] - nx * ax[2]; bx[2] = nx * ax[1] - ny * ax[0];
            int c;
            for (c = 0; c < 4; ++c) {
                float su = (c == 1 || c == 2) ? 1.0f : -1.0f, sv = (c >= 2) ? 1.0f : -1.0f;
                float* out = vertices + (v + c) * 8;
                out[0] = 0.5f * (nx + su * ax[0] + sv * bx[0]);
                out[1] = 0.5f * (ny + su * ax[1] + sv * bx[1]);
                out[2] = 0.5f * (nz + su * ax[2] + sv * bx[2]);
                out[3] = nx; out[4] = ny; out[5] = nz;
                out[6] = su * 0.5f + 0.5f; out[7] = sv * 0.5f + 0.5f;
            }
            indices[n++] = v; indices[n++] = v + 1; indices[n++] = v + 2;
            indices[n++] = v; indices[n++] = v + 2; indices[n++] = v + 3;
            v += 4;
        }
        static const float ground[4][8] = {{-12,0,-12,0,1,0,0,0},{12,0,-12,0,1,0,6,0},{12,0,12,0,1,0,6,6},{-12,0,12,0,1,0,0,6}};
        memcpy(vertices + 24 * 8, ground, sizeof(ground));
        indices[36] = 0; indices[37] = 2; indices[38] = 1; indices[39] = 0; indices[40] = 3; indices[41] = 2;
    }
    float instances[64 * 4];
    for (i = 0; i < 64; ++i) {
        instances[i * 4 + 0] = (float)(i % 8) * 2.2f - 7.7f;
        instances[i * 4 + 1] = 0.6f + 0.4f * (float)((i * 7) % 5) / 4.0f;
        instances[i * 4 + 2] = (float)(i / 8) * 2.2f - 7.7f;
        instances[i * 4 + 3] = 0.7f + 0.5f * (float)((i * 13) % 7) / 6.0f;
    }
    float ground_instance[4] = {0, 0, 0, 1};
    float particles[256 * 4];
    for (i = 0; i < 256; ++i) {
        particles[i * 4 + 0] = (float)((i * 37) % 97) / 97.0f * 10.0f - 5.0f;
        particles[i * 4 + 1] = 0.2f;
        particles[i * 4 + 2] = (float)((i * 61) % 89) / 89.0f * 10.0f - 5.0f;
        particles[i * 4 + 3] = (float)i / 256.0f;
    }

    GLuint buffers[6], vaos[3];
    p_glGenBuffers(6, buffers);
    p_glGenVertexArrays(3, vaos);
    /* cubes */
    p_glBindVertexArray(vaos[0]);
    p_glBindBuffer(GL_ARRAY_BUFFER, buffers[0]);
    p_glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    p_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffers[1]);
    p_glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);
    for (i = 0; i < 3; ++i) p_glEnableVertexAttribArray(i);
    p_glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 32, (void*)0);
    p_glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 32, (void*)12);
    p_glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 32, (void*)24);
    p_glBindBuffer(GL_ARRAY_BUFFER, buffers[2]);
    p_glBufferData(GL_ARRAY_BUFFER, sizeof(instances), instances, GL_STATIC_DRAW);
    p_glEnableVertexAttribArray(3);
    p_glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, 0, (void*)0);
    p_glVertexAttribDivisor(3, 1);
    /* ground: same vertex/index buffers, its own instance */
    p_glBindVertexArray(vaos[1]);
    p_glBindBuffer(GL_ARRAY_BUFFER, buffers[0]);
    p_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffers[1]);
    for (i = 0; i < 3; ++i) p_glEnableVertexAttribArray(i);
    p_glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 32, (void*)(24 * 32));
    p_glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 32, (void*)(24 * 32 + 12));
    p_glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 32, (void*)(24 * 32 + 24));
    p_glBindBuffer(GL_ARRAY_BUFFER, buffers[3]);
    p_glBufferData(GL_ARRAY_BUFFER, sizeof(ground_instance), ground_instance, GL_STATIC_DRAW);
    p_glEnableVertexAttribArray(3);
    p_glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, 0, (void*)0);
    p_glVertexAttribDivisor(3, 1);
    /* particles, rewritten every frame (orphaned) */
    p_glBindVertexArray(vaos[2]);
    p_glBindBuffer(GL_ARRAY_BUFFER, buffers[4]);
    p_glBufferData(GL_ARRAY_BUFFER, sizeof(particles), particles, GL_STREAM_DRAW);
    p_glEnableVertexAttribArray(0);
    p_glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, (void*)0);
    p_glBindVertexArray(0);
    /* camera UBO */
    p_glBindBuffer(GL_UNIFORM_BUFFER, buffers[5]);
    p_glBufferData(GL_UNIFORM_BUFFER, 96, NULL, GL_DYNAMIC_DRAW);
    p_glBindBufferBase(GL_UNIFORM_BUFFER, 0, buffers[5]);

    /* albedo: 64x64 RGBA8 checker/gradient with full mip chain */
    GLuint textures[3];
    glGenTextures(3, textures);
    {
        unsigned char* pixels = (unsigned char*)malloc(64 * 64 * 4);
        int x, y;
        for (y = 0; y < 64; ++y)
            for (x = 0; x < 64; ++x) {
                int checker = ((x >> 3) ^ (y >> 3)) & 1;
                unsigned char* p = pixels + (y * 64 + x) * 4;
                p[0] = (unsigned char)(checker ? 220 : 60 + x * 2);
                p[1] = (unsigned char)(checker ? 200 : 80 + y * 2);
                p[2] = (unsigned char)(checker ? 170 : 120);
                p[3] = 255;
            }
        p_glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, textures[0]);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        p_glGenerateMipmap(GL_TEXTURE_2D);
        free(pixels);
    }
    GLuint sampler;
    p_glGenSamplers(1, &sampler);
    p_glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    p_glSamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    p_glSamplerParameteri(sampler, GL_TEXTURE_WRAP_S, GL_REPEAT);
    p_glSamplerParameteri(sampler, GL_TEXTURE_WRAP_T, GL_REPEAT);
    /* HDR target */
    glBindTexture(GL_TEXTURE_2D, textures[1]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA, GL_HALF_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, textures[2]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, width, height, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    GLuint fbo;
    p_glGenFramebuffers(1, &fbo);
    p_glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, textures[1], 0);
    p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, textures[2], 0);
    if (p_glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { fprintf(stderr, "FBO incomplete\n"); return 1; }
    p_glBindFramebuffer(GL_FRAMEBUFFER, 0);

    GLuint scene = link(vs_scene, fs_scene), post = link(vs_post, fs_post), points = link(vs_points, fs_points);
    p_glUniformBlockBinding(scene, p_glGetUniformBlockIndex(scene, "Camera"), 0);
    p_glUniformBlockBinding(points, p_glGetUniformBlockIndex(points, "Camera"), 0);
    p_glUseProgram(scene);
    p_glUniform1i(p_glGetUniformLocation(scene, "albedo"), 0);
    p_glUseProgram(post);
    p_glUniform1i(p_glGetUniformLocation(post, "hdr"), 0);
    p_glUniform2f(p_glGetUniformLocation(post, "inv_size"), 1.0f / (float)width, 1.0f / (float)height);

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
        float t = (float)frame / 30.0f, camera[24];
        perspective_look(camera, (float)width / (float)height, t);
        camera[16] = 0.4f; camera[17] = 1.0f; camera[18] = 0.3f; camera[19] = 0.0f;
        camera[20] = t; camera[21] = camera[22] = camera[23] = 0.0f;
        p_glBindBuffer(GL_UNIFORM_BUFFER, buffers[5]);
        p_glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(camera), camera);

        p_glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glViewport(0, 0, width, height);
        glClearColor(0.05f, 0.07f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST);
        glEnable(GL_CULL_FACE);
        glDisable(GL_BLEND);
        p_glUseProgram(scene);
        p_glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, textures[0]);
        p_glBindSampler(0, sampler);
        p_glBindVertexArray(vaos[0]);
        p_glDrawElementsInstanced(GL_TRIANGLES, 36, GL_UNSIGNED_SHORT, (void*)0, 64);
        p_glBindVertexArray(vaos[1]);
        p_glDrawElementsInstanced(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, (void*)(36 * 2), 1);
        /* particles: orphan + rewrite, additive-ish blend, no depth writes */
        p_glBindBuffer(GL_ARRAY_BUFFER, buffers[4]);
        p_glBufferData(GL_ARRAY_BUFFER, sizeof(particles), NULL, GL_STREAM_DRAW);
        p_glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(particles), particles);
        glEnable(GL_BLEND);
        p_glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ZERO, GL_ONE);
        glDepthMask(GL_FALSE);
        glEnable(GL_PROGRAM_POINT_SIZE);
        p_glUseProgram(points);
        p_glBindVertexArray(vaos[2]);
        glDrawArrays(GL_POINTS, 0, 256);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);

        p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, width, height);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        p_glUseProgram(post);
        p_glBindSampler(0, 0);
        glBindTexture(GL_TEXTURE_2D, textures[1]);
        p_glBindVertexArray(vaos[2]);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        if (frame == frames - 1) {
            glPixelStorei(GL_PACK_ALIGNMENT, 4);
            glReadBuffer(GL_BACK);
            glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, readback);
        }
        SwapBuffers(dc);
        QueryPerformanceCounter(&frame_end);
        frame_ms[frame] = 1000.0 * (double)(frame_end.QuadPart - frame_start.QuadPart) / (double)frequency.QuadPart;
    }
    if (benchmark) report_frame_times(frame_ms, frames);
    free(frame_ms);
    GLenum error = glGetError();
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
    printf("SCENE33_%s %08x\n", error == GL_NO_ERROR ? "OK" : "GLERROR", crc);
    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(context);
    return error == GL_NO_ERROR ? 0 : 3;
}
