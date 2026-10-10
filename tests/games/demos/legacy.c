/* legacy: a deterministic fixed-function OpenGL frame for WineMetalGL's game
 * corpus (tests/games), in the style of games from the GL 1.x/2.x era:
 * per-vertex lighting with two lights and specular materials, display
 * lists, immediate mode, client vertex arrays with glDrawElements(GL_QUADS),
 * multitexturing (MODULATE and ADD), sphere-map texture generation, linear
 * fog, alpha-tested cut-outs, a user clip plane, blended point sprites and a
 * HUD drawn with glBitmap text, glDrawPixels and glPushAttrib. Animation is
 * driven by the frame index only, so every run renders the same image.
 *
 *   legacy.exe [--frames N] [--size WxH] [--profile legacy|compat33]
 *              [--dump out.ppm] [--hidden] [--benchmark]
 *
 * "legacy" uses wglCreateContext (Apple's OpenGL can run it, so it records
 * the reference); "compat33" asks for a 3.3 compatibility-profile context
 * and must render the same frame.
 *
 * Prints "LEGACY_OK <crc32>" on success. Build with mingw:
 *   x86_64-w64-mingw32-gcc -O2 legacy.c -o legacy.exe -lopengl32 -lgdi32 -luser32
 */
#include <windows.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/wglext.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GLFUNCS(X)                                                                                                 \
    X(PFNGLACTIVETEXTUREPROC, glActiveTexture) X(PFNGLCLIENTACTIVETEXTUREPROC, glClientActiveTexture)             \
    X(PFNGLMULTITEXCOORD2FPROC, glMultiTexCoord2f) X(PFNGLPOINTPARAMETERFVPROC, glPointParameterfv)               \
    X(PFNGLPOINTPARAMETERFPROC, glPointParameterf)

#define DECLARE(type, name) static type p_##name;
GLFUNCS(DECLARE)
#undef DECLARE

static int load_funcs(void) {
#define LOAD(type, name)                                              \
    p_##name = (type)wglGetProcAddress(#name);                        \
    if (!p_##name) { fprintf(stderr, "missing %s\n", #name); return 0; }
    GLFUNCS(LOAD)
#undef LOAD
    return 1;
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    return DefWindowProcA(hwnd, msg, wparam, lparam);
}

static unsigned crc32_bytes(const unsigned char* data, size_t size) {
    unsigned crc = 0xffffffffu;
    size_t i;
    int k;
    for (i = 0; i < size; ++i) {
        crc ^= data[i];
        for (k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

/* A checkerboard with its mip chain computed here (box filter), so every
 * implementation samples identical texels. */
static GLuint checker_texture(void) {
    GLuint tex;
    int size = 64, level = 0;
    unsigned char* texels = malloc((size_t)size * size * 4);
    int x, y;
    for (y = 0; y < size; ++y)
        for (x = 0; x < size; ++x) {
            unsigned char* t = texels + (y * size + x) * 4;
            const int on = ((x / 8) + (y / 8)) & 1;
            t[0] = on ? 230 : 90;
            t[1] = on ? 220 : 120;
            t[2] = on ? 200 : 70;
            t[3] = 255;
        }
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    for (;;) {
        glTexImage2D(GL_TEXTURE_2D, level, GL_RGBA, size, size, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels);
        if (size == 1) break;
        {
            int half = size / 2, c;
            for (y = 0; y < half; ++y)
                for (x = 0; x < half; ++x)
                    for (c = 0; c < 4; ++c) {
                        const unsigned char* s = texels + ((2 * y) * size + 2 * x) * 4 + c;
                        const int sum = s[0] + s[4] + s[size * 4] + s[size * 4 + 4];
                        texels[(y * half + x) * 4 + c] = (unsigned char)((sum + 2) / 4);
                    }
            size = half;
            ++level;
        }
    }
    free(texels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    return tex;
}

/* A soft radial "light map" in luminance. */
static GLuint glow_texture(void) {
    GLuint tex;
    unsigned char texels[32 * 32];
    int x, y;
    for (y = 0; y < 32; ++y)
        for (x = 0; x < 32; ++x) {
            const float dx = (x - 15.5f) / 16.0f, dy = (y - 15.5f) / 16.0f;
            const float d = sqrtf(dx * dx + dy * dy);
            texels[y * 32 + x] = (unsigned char)(d >= 1.0f ? 40 : 40 + 215 * (1.0f - d));
        }
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, 32, 32, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, texels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return tex;
}

/* Leaves cut out by alpha: an RGBA texture whose alpha is a leaf mask. */
static GLuint leaf_texture(void) {
    GLuint tex;
    unsigned char texels[32 * 32 * 4];
    int x, y;
    for (y = 0; y < 32; ++y)
        for (x = 0; x < 32; ++x) {
            unsigned char* t = texels + (y * 32 + x) * 4;
            const float u = (x - 15.5f) / 16.0f, v = (y - 15.5f) / 16.0f;
            const int inside = u * u * 2.2f + v * v < 0.8f;
            t[0] = (unsigned char)(40 + x * 3);
            t[1] = (unsigned char)(140 + y * 3);
            t[2] = 50;
            t[3] = inside ? 255 : 0;
        }
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 32, 32, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    return tex;
}

/* A unit cube as immediate-mode quads with normals and texture coordinates. */
static void cube(void) {
    static const float n[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    int f, c;
    glBegin(GL_QUADS);
    for (f = 0; f < 6; ++f) {
        const float nx = n[f][0], ny = n[f][1], nz = n[f][2];
        const float ax[3] = {ny, nz, nx};
        const float bx[3] = {ny * ax[2] - nz * ax[1], nz * ax[0] - nx * ax[2], nx * ax[1] - ny * ax[0]};
        glNormal3f(nx, ny, nz);
        for (c = 0; c < 4; ++c) {
            const float su = (c == 1 || c == 2) ? 1.0f : -1.0f, sv = c >= 2 ? 1.0f : -1.0f;
            glTexCoord2f(su * 0.5f + 0.5f, sv * 0.5f + 0.5f);
            glVertex3f(0.5f * (nx + su * ax[0] + sv * bx[0]), 0.5f * (ny + su * ax[1] + sv * bx[1]),
                       0.5f * (nz + su * ax[2] + sv * bx[2]));
        }
    }
    glEnd();
}

/* A latitude/longitude sphere as quad strips. */
static void sphere(float radius, int slices, int stacks) {
    int i, j;
    for (i = 0; i < stacks; ++i) {
        const float a0 = 3.14159265f * ((float)i / stacks - 0.5f), a1 = 3.14159265f * ((float)(i + 1) / stacks - 0.5f);
        glBegin(GL_QUAD_STRIP);
        for (j = 0; j <= slices; ++j) {
            const float b = 2.0f * 3.14159265f * j / slices;
            const float x0 = cosf(a0) * cosf(b), y0 = sinf(a0), z0 = cosf(a0) * sinf(b);
            const float x1 = cosf(a1) * cosf(b), y1 = sinf(a1), z1 = cosf(a1) * sinf(b);
            glNormal3f(x1, y1, z1);
            glVertex3f(radius * x1, radius * y1, radius * z1);
            glNormal3f(x0, y0, z0);
            glVertex3f(radius * x0, radius * y0, radius * z0);
        }
        glEnd();
    }
}

/* 8x8 glyphs (bit 7 = left column, bottom row first) for the HUD text. */
static const GLubyte glyph_L[8] = {0x7e, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x00};
static const GLubyte glyph_E[8] = {0x7e, 0x60, 0x60, 0x7c, 0x60, 0x60, 0x7e, 0x00};
static const GLubyte glyph_G[8] = {0x3c, 0x66, 0x66, 0x6e, 0x60, 0x66, 0x3c, 0x00};
static const GLubyte glyph_A[8] = {0x66, 0x66, 0x7e, 0x66, 0x66, 0x3c, 0x18, 0x00};
static const GLubyte glyph_C[8] = {0x3c, 0x66, 0x60, 0x60, 0x60, 0x66, 0x3c, 0x00};
static const GLubyte glyph_Y[8] = {0x18, 0x18, 0x18, 0x3c, 0x66, 0x66, 0x66, 0x00};

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
    int frames = 60, width = 640, height = 360, compat33 = 0, hidden = 0, i, frame;
    const char* dump = NULL;
    int benchmark = 0;
    double* frame_ms = NULL;
    LARGE_INTEGER frequency, frame_start, frame_end;
    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--size") && i + 1 < argc) sscanf(argv[++i], "%dx%d", &width, &height);
        else if (!strcmp(argv[i], "--profile") && i + 1 < argc) compat33 = !strcmp(argv[++i], "compat33");
        else if (!strcmp(argv[i], "--dump") && i + 1 < argc) dump = argv[++i];
        else if (!strcmp(argv[i], "--hidden")) hidden = 1;
        else if (!strcmp(argv[i], "--benchmark")) benchmark = 1;
    }

    WNDCLASSA wc = {0};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "legacy";
    wc.style = CS_OWNDC;
    RegisterClassA(&wc);
    RECT rect = {0, 0, width, height};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowA("legacy", "legacy", WS_OVERLAPPEDWINDOW, 0, 0, rect.right - rect.left, rect.bottom - rect.top,
                              NULL, NULL, wc.hInstance, NULL);
    if (!hidden) ShowWindow(hwnd, SW_SHOW);
    HDC dc = GetDC(hwnd);
    PIXELFORMATDESCRIPTOR pfd = {sizeof(pfd), 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 32};
    pfd.cDepthBits = 24;
    pfd.cStencilBits = 8;
    SetPixelFormat(dc, ChoosePixelFormat(dc, &pfd), &pfd);
    HGLRC context = wglCreateContext(dc);
    wglMakeCurrent(dc, context);
    if (compat33) {
        PFNWGLCREATECONTEXTATTRIBSARBPROC create =
            (PFNWGLCREATECONTEXTATTRIBSARBPROC)wglGetProcAddress("wglCreateContextAttribsARB");
        int attribs[] = {WGL_CONTEXT_MAJOR_VERSION_ARB, 3, WGL_CONTEXT_MINOR_VERSION_ARB, 3, WGL_CONTEXT_PROFILE_MASK_ARB,
                         WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB, 0};
        HGLRC modern = create ? create(dc, NULL, attribs) : NULL;
        if (!modern) {
            fprintf(stderr, "3.3 compatibility context creation failed\n");
            return 1;
        }
        wglMakeCurrent(dc, modern);
        wglDeleteContext(context);
        context = modern;
    }
    if (!load_funcs()) return 1;
    printf("GL_VERSION %s\nGL_RENDERER %s\n", glGetString(GL_VERSION), glGetString(GL_RENDERER));

    const GLuint checker = checker_texture(), glow = glow_texture(), leaf = leaf_texture();
    unsigned char* readback = malloc((size_t)width * height * 4);

    /* Display list: a lit, textured cube. */
    const GLuint lists = glGenLists(2);
    glNewList(lists, GL_COMPILE);
    cube();
    glEndList();
    /* Display list: a sphere (quad strips). */
    glNewList(lists + 1, GL_COMPILE);
    sphere(0.6f, 24, 12);
    glEndList();

    /* The ground: a 9x9 grid of quads as client arrays (positions, normals,
       two sets of texture coordinates), drawn with glDrawElements(GL_QUADS). */
    float ground[10 * 10 * 3], ground_n[10 * 10 * 3], ground_t0[10 * 10 * 2], ground_t1[10 * 10 * 2];
    GLushort ground_i[9 * 9 * 4];
    {
        int x, z, n = 0;
        for (z = 0; z < 10; ++z)
            for (x = 0; x < 10; ++x) {
                const int v = z * 10 + x;
                ground[v * 3] = (float)x - 4.5f;
                ground[v * 3 + 1] = 0.0f;
                ground[v * 3 + 2] = (float)z - 4.5f;
                ground_n[v * 3] = 0.0f;
                ground_n[v * 3 + 1] = 1.0f;
                ground_n[v * 3 + 2] = 0.0f;
                ground_t0[v * 2] = x * 0.5f;
                ground_t0[v * 2 + 1] = z * 0.5f;
                ground_t1[v * 2] = x / 9.0f;
                ground_t1[v * 2 + 1] = z / 9.0f;
            }
        for (z = 0; z < 9; ++z)
            for (x = 0; x < 9; ++x) {
                ground_i[n++] = (GLushort)(z * 10 + x);
                ground_i[n++] = (GLushort)((z + 1) * 10 + x);
                ground_i[n++] = (GLushort)((z + 1) * 10 + x + 1);
                ground_i[n++] = (GLushort)(z * 10 + x + 1);
            }
    }

    /* A small RGBA badge for glDrawPixels. */
    unsigned char badge[16 * 16 * 4];
    {
        int x, y;
        for (y = 0; y < 16; ++y)
            for (x = 0; x < 16; ++x) {
                unsigned char* b = badge + (y * 16 + x) * 4;
                b[0] = (unsigned char)(x * 16);
                b[1] = (unsigned char)(y * 16);
                b[2] = 200;
                b[3] = 255;
            }
    }

    frame_ms = calloc((size_t)frames, sizeof(double));
    QueryPerformanceFrequency(&frequency);
    if (benchmark) {
        PFNWGLSWAPINTERVALEXTPROC swap_interval = (PFNWGLSWAPINTERVALEXTPROC)wglGetProcAddress("wglSwapIntervalEXT");
        if (swap_interval) swap_interval(0);
    }
    for (frame = 0; frame < frames; ++frame) {
        QueryPerformanceCounter(&frame_start);
        const float t = (float)frame / 60.0f;
        glViewport(0, 0, width, height);
        glClearColor(0.55f, 0.65f, 0.8f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        {
            const float aspect = (float)width / (float)height, nearz = 0.5f, top = nearz * 0.55f;
            glFrustum(-top * aspect, top * aspect, -top, top, nearz, 40.0f);
        }
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        glTranslatef(0.0f, -1.2f, -7.0f);
        glRotatef(22.0f, 1.0f, 0.0f, 0.0f);
        glRotatef(30.0f * t, 0.0f, 1.0f, 0.0f);

        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        glEnable(GL_CULL_FACE);
        glShadeModel(GL_SMOOTH);

        /* Lights in world space (specified after the camera transform). */
        {
            const float sun[4] = {0.4f, 1.0f, 0.3f, 0.0f}, lamp[4] = {1.5f, 1.2f, 1.5f, 1.0f};
            const float white[4] = {1, 1, 1, 1}, warm[4] = {1.0f, 0.7f, 0.4f, 1.0f}, dark[4] = {0.15f, 0.15f, 0.2f, 1.0f};
            const float ambient[4] = {0.25f, 0.25f, 0.3f, 1.0f};
            glLightfv(GL_LIGHT0, GL_POSITION, sun);
            glLightfv(GL_LIGHT0, GL_DIFFUSE, white);
            glLightfv(GL_LIGHT0, GL_SPECULAR, white);
            glLightfv(GL_LIGHT0, GL_AMBIENT, dark);
            glLightfv(GL_LIGHT1, GL_POSITION, lamp);
            glLightfv(GL_LIGHT1, GL_DIFFUSE, warm);
            glLightfv(GL_LIGHT1, GL_SPECULAR, warm);
            glLightf(GL_LIGHT1, GL_CONSTANT_ATTENUATION, 0.6f);
            glLightf(GL_LIGHT1, GL_QUADRATIC_ATTENUATION, 0.15f);
            glLightModelfv(GL_LIGHT_MODEL_AMBIENT, ambient);
            glEnable(GL_LIGHT0);
            glEnable(GL_LIGHT1);
            glEnable(GL_LIGHTING);
        }

        /* Linear fog. */
        {
            const float fog[4] = {0.55f, 0.65f, 0.8f, 1.0f};
            glFogi(GL_FOG_MODE, GL_LINEAR);
            glFogfv(GL_FOG_COLOR, fog);
            glFogf(GL_FOG_START, 5.0f);
            glFogf(GL_FOG_END, 16.0f);
            glEnable(GL_FOG);
        }

        /* Ground: checker (unit 0, MODULATE with lighting) times a glow map (unit 1, MODULATE). */
        {
            const float ground_color[4] = {0.9f, 0.9f, 0.85f, 1.0f}, black[4] = {0, 0, 0, 1};
            glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE, ground_color);
            glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, black);
            p_glActiveTexture(GL_TEXTURE0);
            glEnable(GL_TEXTURE_2D);
            glBindTexture(GL_TEXTURE_2D, checker);
            glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
            p_glActiveTexture(GL_TEXTURE1);
            glEnable(GL_TEXTURE_2D);
            glBindTexture(GL_TEXTURE_2D, glow);
            glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
            glEnableClientState(GL_VERTEX_ARRAY);
            glEnableClientState(GL_NORMAL_ARRAY);
            glVertexPointer(3, GL_FLOAT, 0, ground);
            glNormalPointer(GL_FLOAT, 0, ground_n);
            p_glClientActiveTexture(GL_TEXTURE0);
            glEnableClientState(GL_TEXTURE_COORD_ARRAY);
            glTexCoordPointer(2, GL_FLOAT, 0, ground_t0);
            p_glClientActiveTexture(GL_TEXTURE1);
            glEnableClientState(GL_TEXTURE_COORD_ARRAY);
            glTexCoordPointer(2, GL_FLOAT, 0, ground_t1);
            glDrawElements(GL_QUADS, 9 * 9 * 4, GL_UNSIGNED_SHORT, ground_i);
            glDisableClientState(GL_TEXTURE_COORD_ARRAY);
            p_glClientActiveTexture(GL_TEXTURE0);
            glDisableClientState(GL_TEXTURE_COORD_ARRAY);
            glDisableClientState(GL_NORMAL_ARRAY);
            glDisableClientState(GL_VERTEX_ARRAY);
            glDisable(GL_TEXTURE_2D);
            p_glActiveTexture(GL_TEXTURE0);
        }

        /* Cubes from the display list: textured, specular, colour material. */
        {
            const float specular[4] = {0.8f, 0.8f, 0.8f, 1.0f};
            int k;
            glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, specular);
            glMaterialf(GL_FRONT_AND_BACK, GL_SHININESS, 32.0f);
            glEnable(GL_COLOR_MATERIAL);
            glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
            glEnable(GL_TEXTURE_2D);
            glBindTexture(GL_TEXTURE_2D, checker);
            for (k = 0; k < 4; ++k) {
                glPushMatrix();
                glTranslatef(-2.4f + 1.6f * k, 0.5f, -1.0f + (k & 1) * 1.5f);
                glRotatef(25.0f * k + 40.0f * t, 0.3f, 1.0f, 0.1f);
                glColor3f(k == 0 ? 1.0f : 0.5f, k == 1 ? 1.0f : 0.6f, k == 2 ? 1.0f : 0.5f);
                glCallList(lists);
                glPopMatrix();
            }
            glDisable(GL_TEXTURE_2D);
            glDisable(GL_COLOR_MATERIAL);
        }

        /* A chrome sphere: sphere-map texture generation, ADD over its lit colour, cut by a clip plane. */
        {
            const double cut[4] = {0.0, -1.0, 0.0, 1.45};
            const float steel[4] = {0.35f, 0.4f, 0.45f, 1.0f};
            glPushMatrix();
            glTranslatef(0.8f, 1.0f, 1.6f);
            glClipPlane(GL_CLIP_PLANE0, cut);
            glEnable(GL_CLIP_PLANE0);
            glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE, steel);
            glEnable(GL_TEXTURE_2D);
            glBindTexture(GL_TEXTURE_2D, glow);
            glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_ADD);
            glTexGeni(GL_S, GL_TEXTURE_GEN_MODE, GL_SPHERE_MAP);
            glTexGeni(GL_T, GL_TEXTURE_GEN_MODE, GL_SPHERE_MAP);
            glEnable(GL_TEXTURE_GEN_S);
            glEnable(GL_TEXTURE_GEN_T);
            glCallList(lists + 1);
            glDisable(GL_TEXTURE_GEN_S);
            glDisable(GL_TEXTURE_GEN_T);
            glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
            glDisable(GL_TEXTURE_2D);
            glDisable(GL_CLIP_PLANE0);
            glPopMatrix();
        }

        /* Alpha-tested foliage, unlit, two crossed quads per plant. */
        {
            int k;
            glDisable(GL_LIGHTING);
            glDisable(GL_CULL_FACE);
            glEnable(GL_ALPHA_TEST);
            glAlphaFunc(GL_GREATER, 0.5f);
            glEnable(GL_TEXTURE_2D);
            glBindTexture(GL_TEXTURE_2D, leaf);
            glColor3f(1.0f, 1.0f, 1.0f);
            for (k = 0; k < 3; ++k) {
                glPushMatrix();
                glTranslatef(-3.0f + 2.8f * k, 0.0f, 2.6f - 1.2f * k);
                glBegin(GL_QUADS);
                glTexCoord2f(0, 0); glVertex3f(-0.6f, 0.0f, 0.0f);
                glTexCoord2f(1, 0); glVertex3f(0.6f, 0.0f, 0.0f);
                glTexCoord2f(1, 1); glVertex3f(0.6f, 1.2f, 0.0f);
                glTexCoord2f(0, 1); glVertex3f(-0.6f, 1.2f, 0.0f);
                glTexCoord2f(0, 0); glVertex3f(0.0f, 0.0f, -0.6f);
                glTexCoord2f(1, 0); glVertex3f(0.0f, 0.0f, 0.6f);
                glTexCoord2f(1, 1); glVertex3f(0.0f, 1.2f, 0.6f);
                glTexCoord2f(0, 1); glVertex3f(0.0f, 1.2f, -0.6f);
                glEnd();
                glPopMatrix();
            }
            glDisable(GL_TEXTURE_2D);
            glDisable(GL_ALPHA_TEST);
        }

        /* Fireflies: blended, depth-tested points with size attenuation. */
        {
            const float attenuation[3] = {0.0f, 0.0f, 0.04f};
            int k;
            glDisable(GL_FOG);
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE);
            glDepthMask(GL_FALSE);
            p_glPointParameterfv(GL_POINT_DISTANCE_ATTENUATION, attenuation);
            p_glPointParameterf(GL_POINT_SIZE_MIN, 1.0f);
            p_glPointParameterf(GL_POINT_SIZE_MAX, 16.0f);
            glPointSize(12.0f);
            glBegin(GL_POINTS);
            for (k = 0; k < 12; ++k) {
                const float a = 0.52f * k + t;
                glColor4f(1.0f, 0.9f, 0.4f, 0.6f);
                glVertex3f(2.5f * cosf(a), 1.6f + 0.3f * sinf(3.0f * a), 2.5f * sinf(a));
            }
            glEnd();
            glDepthMask(GL_TRUE);
            glDisable(GL_BLEND);
        }

        /* HUD in window coordinates. */
        glPushAttrib(GL_ENABLE_BIT | GL_CURRENT_BIT | GL_TRANSFORM_BIT);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_LIGHTING);
        glDisable(GL_FOG);
        glMatrixMode(GL_PROJECTION);
        glPushMatrix();
        glLoadIdentity();
        glOrtho(0, width, 0, height, -1, 1);
        glMatrixMode(GL_MODELVIEW);
        glPushMatrix();
        glLoadIdentity();
        /* A translucent panel. */
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glColor4f(0.05f, 0.05f, 0.1f, 0.6f);
        glRectf(8.0f, 8.0f, 150.0f, 40.0f);
        glDisable(GL_BLEND);
        /* Bitmap text "LEGACY". */
        glColor3f(1.0f, 0.85f, 0.3f);
        glRasterPos2i(16, 18);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glBitmap(8, 8, 0, 0, 10, 0, glyph_L);
        glBitmap(8, 8, 0, 0, 10, 0, glyph_E);
        glBitmap(8, 8, 0, 0, 10, 0, glyph_G);
        glBitmap(8, 8, 0, 0, 10, 0, glyph_A);
        glBitmap(8, 8, 0, 0, 10, 0, glyph_C);
        glBitmap(8, 8, 0, 0, 10, 0, glyph_Y);
        /* A badge from glDrawPixels, zoomed. */
        glRasterPos2i(110, 12);
        glPixelZoom(1.5f, 1.5f);
        glDrawPixels(16, 16, GL_RGBA, GL_UNSIGNED_BYTE, badge);
        glPixelZoom(1.0f, 1.0f);
        /* A crosshair. */
        glLineWidth(1.0f);
        glColor3f(1.0f, 1.0f, 1.0f);
        glBegin(GL_LINES);
        glVertex2f(width * 0.5f - 6.0f, height * 0.5f);
        glVertex2f(width * 0.5f + 6.0f, height * 0.5f);
        glVertex2f(width * 0.5f, height * 0.5f - 6.0f);
        glVertex2f(width * 0.5f, height * 0.5f + 6.0f);
        glEnd();
        glPopMatrix();
        glMatrixMode(GL_PROJECTION);
        glPopMatrix();
        glPopAttrib();

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
    printf("LEGACY_%s %08x\n", error == GL_NO_ERROR ? "OK" : "GLERROR", crc);
    glDeleteLists(lists, 2);
    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(context);
    free(readback);
    return error == GL_NO_ERROR ? 0 : 3;
}
