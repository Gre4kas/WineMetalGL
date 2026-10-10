// Compatibility profile (W22): fixed-function processing, immediate mode,
// display lists, client arrays, legacy GLSL built-ins and pixel rectangles.

#include "gpu.h"

#define WMGL_COMPAT_FUNCS(X)                                                                                   \
    X(glBegin) X(glEnd) X(glVertex2f) X(glVertex3f) X(glColor3f) X(glColor4f) X(glColor4ub) X(glNormal3f)       \
    X(glTexCoord2f) X(glMatrixMode) X(glLoadIdentity) X(glOrtho) X(glTranslatef) X(glRotatef) X(glScalef)       \
    X(glPushMatrix) X(glPopMatrix) X(glRectf) X(glLightfv) X(glMaterialfv) X(glLightModelfv) X(glShadeModel)    \
    X(glTexEnvi) X(glTexEnvfv) X(glTexGeni) X(glTexGenfv) X(glFogi) X(glFogf) X(glFogfv) X(glAlphaFunc)          \
    X(glClipPlane) X(glNewList) X(glEndList) X(glCallList) X(glCallLists) X(glGenLists) X(glDeleteLists)         \
    X(glIsList) X(glListBase) X(glVertexPointer) X(glColorPointer) X(glTexCoordPointer)                          \
    X(glEnableClientState) X(glDisableClientState) X(glClientActiveTexture) X(glMultiTexCoord2f)                \
    X(glPushAttrib) X(glPopAttrib) X(glRasterPos2f) X(glWindowPos2i) X(glBitmap) X(glDrawPixels)                \
    X(glPixelZoom) X(glIsEnabled) X(glColorMaterial) X(glInterleavedArrays) X(glArrayElement)                  \
    X(glFrustum) X(glMultMatrixf) X(glLoadMatrixf)

#define WMGL_COMPAT_DECLARE(name) static PFN_##name name;
WMGL_COMPAT_FUNCS(WMGL_COMPAT_DECLARE)
#undef WMGL_COMPAT_DECLARE

namespace {

void loadCompat() {
#define WMGL_COMPAT_LOAD(name) name = reinterpret_cast<PFN_##name>(wmgl_get_proc_address(#name));
    WMGL_COMPAT_FUNCS(WMGL_COMPAT_LOAD)
#undef WMGL_COMPAT_LOAD
}

const uint8_t kRed[4] = {255, 0, 0, 255}, kGreen[4] = {0, 255, 0, 255}, kBlue[4] = {0, 0, 255, 255},
              kBlack[4] = {0, 0, 0, 255}, kWhite[4] = {255, 255, 255, 255};

bool pixel(int x, int y, const uint8_t want[4], int tolerance = 2) {
    const std::vector<uint8_t> got = gpu::readRGBA8(x, y, 1, 1);
    if (gpu::near(got.data(), want, tolerance)) return true;
    std::printf("  pixel (%d,%d) = %d %d %d %d, want %d %d %d %d\n", x, y, got[0], got[1], got[2], got[3], want[0], want[1],
                want[2], want[3]);
    return false;
}

void clear() {
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

// Window coordinates 0..64 in both directions.
void pixelProjection() {
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, 64, 0, 64, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
}

}  // namespace

#define COMPAT_TEST(name)                         \
    static void name##_body();                    \
    TEST(name) {                                  \
        gpu::Fixture fixture_(64, 64, false);     \
        CHECK(fixture_.ok);                       \
        loadCompat();                             \
        if (fixture_.ok) name##_body();           \
    }                                             \
    static void name##_body()

COMPAT_TEST(compat_immediate_mode) {
    clear();
    glBegin(GL_TRIANGLES);
    glColor3f(1, 0, 0);
    glVertex2f(-1, -1);
    glVertex2f(3, -1);
    glVertex2f(-1, 3);
    glEnd();
    CHECK(pixel(32, 32, kRed));
    // Quads, and a colour that changes between vertices (smooth by default: the left half is green).
    glBegin(GL_QUADS);
    glColor3f(0, 1, 0);
    glVertex2f(-1, -1);
    glVertex2f(0, -1);
    glVertex2f(0, 1);
    glVertex2f(-1, 1);
    glEnd();
    CHECK(pixel(16, 32, kGreen));
    CHECK(pixel(48, 32, kRed));
    // Flat shading takes the last vertex of each quad.
    glShadeModel(GL_FLAT);
    glBegin(GL_QUADS);
    glColor3f(1, 0, 0);
    glVertex2f(0, -1);
    glVertex2f(1, -1);
    glVertex2f(1, 1);
    glColor3f(0, 0, 1);
    glVertex2f(0, 1);
    glEnd();
    glShadeModel(GL_SMOOTH);
    CHECK(pixel(40, 10, kBlue));
    CHECK(pixel(60, 60, kBlue));
    // glColor outside Begin/End is the current colour; glGet reports it.
    float current[4] = {};
    glGetFloatv(GL_CURRENT_COLOR, current);
    CHECK(current[0] == 0 && current[1] == 0 && current[2] == 1 && current[3] == 1);
    // Most commands (glGetError too) are invalid between Begin and End.
    glBegin(GL_POINTS);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnd();
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_OPERATION));
    CHECK_GL();
}

COMPAT_TEST(compat_matrices) {
    clear();
    pixelProjection();
    glColor3f(0, 1, 0);
    glPushMatrix();
    glTranslatef(16, 16, 0);
    glRectf(0, 0, 32, 32);
    glPopMatrix();
    CHECK(pixel(32, 32, kGreen));
    CHECK(pixel(8, 8, kBlack));
    CHECK(pixel(56, 56, kBlack));
    float mv[16] = {};
    glTranslatef(1, 2, 3);
    glGetFloatv(GL_MODELVIEW_MATRIX, mv);
    CHECK(mv[12] == 1 && mv[13] == 2 && mv[14] == 3 && mv[0] == 1 && mv[15] == 1);
    GLint depth = 0;
    glGetIntegerv(GL_MODELVIEW_STACK_DEPTH, &depth);
    CHECK_EQ(depth, 1);
    glPopMatrix();
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_STACK_UNDERFLOW));
    // A rotated quad: rotate 90 degrees about z around the centre.
    clear();
    glLoadIdentity();
    glTranslatef(32, 32, 0);
    glRotatef(90, 0, 0, 1);
    glColor3f(1, 0, 0);
    glRectf(0, -4, 30, 4);  // after rotation: a vertical bar above the centre
    CHECK(pixel(32, 50, kRed));
    CHECK(pixel(50, 32, kBlack));
    CHECK_GL();
}

COMPAT_TEST(compat_lighting) {
    clear();
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    const float position[4] = {0, 0, 1, 0}, white[4] = {1, 1, 1, 1}, red[4] = {1, 0, 0, 1}, none[4] = {0, 0, 0, 1};
    glLightfv(GL_LIGHT0, GL_POSITION, position);
    glLightfv(GL_LIGHT0, GL_DIFFUSE, white);
    glLightModelfv(GL_LIGHT_MODEL_AMBIENT, none);
    glMaterialfv(GL_FRONT_AND_BACK, GL_DIFFUSE, red);
    glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT, none);
    glBegin(GL_QUADS);
    glNormal3f(0, 0, 1);  // facing the light: full diffuse
    glVertex2f(-1, -1);
    glVertex2f(0, -1);
    glVertex2f(0, 1);
    glVertex2f(-1, 1);
    glNormal3f(0, 0, -1);  // facing away: black
    glVertex2f(0, -1);
    glVertex2f(1, -1);
    glVertex2f(1, 1);
    glVertex2f(0, 1);
    glEnd();
    CHECK(pixel(16, 32, kRed));
    CHECK(pixel(48, 32, kBlack));
    // Colour material tracks glColor for the diffuse term.
    glEnable(GL_COLOR_MATERIAL);
    glColorMaterial(GL_FRONT_AND_BACK, GL_DIFFUSE);
    glColor3f(0, 0, 1);
    glBegin(GL_TRIANGLES);
    glNormal3f(0, 0, 1);
    glVertex2f(-1, -1);
    glVertex2f(3, -1);
    glVertex2f(-1, 3);
    glEnd();
    CHECK(pixel(32, 32, kBlue));
    glDisable(GL_COLOR_MATERIAL);
    // A light at 45 degrees gives cos(45) of the diffuse colour.
    const float slanted[4] = {1, 0, 1, 0};
    glLightfv(GL_LIGHT0, GL_POSITION, slanted);
    glBegin(GL_TRIANGLES);
    glNormal3f(0, 0, 1);
    glVertex2f(-1, -1);
    glVertex2f(3, -1);
    glVertex2f(-1, 3);
    glEnd();
    const uint8_t dim[4] = {181, 0, 0, 255};
    CHECK(pixel(32, 32, dim, 3));
    CHECK(glIsEnabled(GL_LIGHTING) == GL_TRUE);
    glDisable(GL_LIGHTING);
    CHECK_GL();
}

COMPAT_TEST(compat_texture_environment) {
    // A 2x2 texture: red, green / blue, white with alpha 128 in the last texel.
    const uint8_t texels[16] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 128};
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glEnable(GL_TEXTURE_2D);
    auto quad = [] {
        glBegin(GL_QUADS);
        glTexCoord2f(0, 0); glVertex2f(-1, -1);
        glTexCoord2f(1, 0); glVertex2f(1, -1);
        glTexCoord2f(1, 1); glVertex2f(1, 1);
        glTexCoord2f(0, 1); glVertex2f(-1, 1);
        glEnd();
    };
    clear();
    glColor4f(1, 1, 1, 1);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    quad();
    CHECK(pixel(16, 16, kRed));
    CHECK(pixel(48, 16, kGreen));
    CHECK(pixel(16, 48, kBlue));
    // MODULATE with a half-intensity colour.
    glColor4f(0.5f, 0.5f, 0.5f, 1);
    quad();
    const uint8_t halfRed[4] = {128, 0, 0, 255};
    CHECK(pixel(16, 16, halfRed, 3));
    // REPLACE ignores the colour.
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    quad();
    CHECK(pixel(48, 16, kGreen));
    // DECAL blends by texture alpha: white at alpha 0.5 over red primary.
    glColor4f(1, 0, 0, 1);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_DECAL);
    quad();
    const uint8_t pink[4] = {255, 128, 128, 255};
    CHECK(pixel(48, 48, pink, 3));
    // ADD clamps.
    glColor4f(0, 0, 1, 1);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_ADD);
    quad();
    const uint8_t magenta[4] = {255, 0, 255, 255};
    CHECK(pixel(16, 16, magenta));
    // BLEND: primary * (1 - Cs) + env colour * Cs.
    const float env[4] = {0, 1, 0, 1};
    glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, env);
    glColor4f(0, 0, 1, 1);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_BLEND);
    quad();
    // Red texel: (0,0,1)*(0,1,1) + (0,1,0)*(1,0,0) = blue; green texel: (0,0,1)*(1,0,1) + (0,1,0)*(0,1,0) = cyan.
    const uint8_t cyan[4] = {0, 255, 255, 255};
    CHECK(pixel(16, 16, kBlue));
    CHECK(pixel(48, 16, cyan));
    // COMBINE: SUBTRACT texture - primary.
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
    glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_SUBTRACT);
    glTexEnvi(GL_TEXTURE_ENV, GL_SRC0_RGB, GL_TEXTURE);
    glTexEnvi(GL_TEXTURE_ENV, GL_SRC1_RGB, GL_PRIMARY_COLOR);
    glColor4f(0.5f, 0, 0, 1);
    quad();
    CHECK(pixel(16, 16, halfRed, 3));
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glDisable(GL_TEXTURE_2D);
    glDeleteTextures(1, &tex);
    CHECK_GL();
}

COMPAT_TEST(compat_texgen_and_multitexture) {
    const uint8_t stripes[8] = {255, 0, 0, 255, 0, 0, 255, 255};  // 2x1: red | blue
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, stripes);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glEnable(GL_TEXTURE_2D);
    // OBJECT_LINEAR s = (x + 1) / 2 over the screen.
    const float plane[4] = {0.5f, 0, 0, 0.5f};
    glTexGeni(GL_S, GL_TEXTURE_GEN_MODE, GL_OBJECT_LINEAR);
    glTexGenfv(GL_S, GL_OBJECT_PLANE, plane);
    glEnable(GL_TEXTURE_GEN_S);
    clear();
    glColor4f(1, 1, 1, 1);
    glBegin(GL_TRIANGLES);
    glVertex2f(-1, -1);
    glVertex2f(3, -1);
    glVertex2f(-1, 3);
    glEnd();
    CHECK(pixel(16, 32, kRed));
    CHECK(pixel(48, 32, kBlue));
    glDisable(GL_TEXTURE_GEN_S);
    // Unit 1 modulates unit 0: both bound to the stripes, unit 1 coordinates flipped.
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, tex);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_ADD);
    glActiveTexture(GL_TEXTURE0);
    clear();
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0); glMultiTexCoord2f(GL_TEXTURE1, 1, 0); glVertex2f(-1, -1);
    glTexCoord2f(1, 0); glMultiTexCoord2f(GL_TEXTURE1, 0, 0); glVertex2f(1, -1);
    glTexCoord2f(1, 1); glMultiTexCoord2f(GL_TEXTURE1, 0, 1); glVertex2f(1, 1);
    glTexCoord2f(0, 1); glMultiTexCoord2f(GL_TEXTURE1, 1, 1); glVertex2f(-1, 1);
    glEnd();
    const uint8_t magenta[4] = {255, 0, 255, 255};
    CHECK(pixel(16, 32, magenta));
    CHECK(pixel(48, 32, magenta));
    glActiveTexture(GL_TEXTURE1);
    glDisable(GL_TEXTURE_2D);
    glActiveTexture(GL_TEXTURE0);
    glDisable(GL_TEXTURE_2D);
    glDeleteTextures(1, &tex);
    CHECK_GL();
}

COMPAT_TEST(compat_fog_and_alpha_test) {
    clear();
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(-1, 1, -1, 1, 0, 10);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glEnable(GL_FOG);
    glFogi(GL_FOG_MODE, GL_LINEAR);
    glFogf(GL_FOG_START, 0);
    glFogf(GL_FOG_END, 2);
    const float fogColor[4] = {0, 0, 0, 1};
    glFogfv(GL_FOG_COLOR, fogColor);
    glColor3f(1, 1, 1);
    glBegin(GL_TRIANGLES);  // eye z = -1: fog factor (2 - 1) / 2
    glVertex3f(-1, -1, -1);
    glVertex3f(3, -1, -1);
    glVertex3f(-1, 3, -1);
    glEnd();
    const uint8_t gray[4] = {128, 128, 128, 255};
    CHECK(pixel(32, 32, gray, 3));
    glDisable(GL_FOG);
    // Alpha test: fragments with alpha 0.4 fail GREATER 0.5.
    clear();
    glEnable(GL_ALPHA_TEST);
    glAlphaFunc(GL_GREATER, 0.5f);
    glColor4f(1, 0, 0, 0.4f);
    glRectf(-1, -1, 0, 1);
    glColor4f(0, 1, 0, 0.6f);
    glRectf(0, -1, 1, 1);
    CHECK(pixel(16, 32, kBlack));
    const uint8_t greenish[4] = {0, 255, 0, 153};
    CHECK(pixel(48, 32, greenish, 3));
    GLint func = 0;
    glGetIntegerv(GL_ALPHA_TEST_FUNC, &func);
    CHECK_EQ(func, GL_GREATER);
    glDisable(GL_ALPHA_TEST);
    CHECK_GL();
}

COMPAT_TEST(compat_clip_plane) {
    clear();
    const double rightHalf[4] = {1, 0, 0, 0};  // keep x >= 0
    glClipPlane(GL_CLIP_PLANE0, rightHalf);
    glEnable(GL_CLIP_PLANE0);
    glColor3f(0, 0, 1);
    glRectf(-1, -1, 1, 1);
    CHECK(pixel(16, 32, kBlack));
    CHECK(pixel(48, 32, kBlue));
    glDisable(GL_CLIP_PLANE0);
    CHECK_GL();
}

COMPAT_TEST(compat_display_lists) {
    const GLuint base = glGenLists(3);
    CHECK(base != 0);
    CHECK(glIsList(base) == GL_TRUE);
    // List 0: a red full-screen quad, compiled only.
    glNewList(base, GL_COMPILE);
    glColor3f(1, 0, 0);
    glRectf(-1, -1, 1, 1);
    glEndList();
    clear();
    CHECK(pixel(32, 32, kBlack));  // GL_COMPILE does not draw
    glCallList(base);
    CHECK(pixel(32, 32, kRed));
    // List 1: matrix state plus a green right half using list 0 nested... then blue left.
    glNewList(base + 1, GL_COMPILE_AND_EXECUTE);
    glPushMatrix();
    glTranslatef(1, 0, 0);
    glColor3f(0, 1, 0);
    glBegin(GL_QUADS);
    glVertex2f(-1, -1);
    glVertex2f(0, -1);
    glVertex2f(0, 1);
    glVertex2f(-1, 1);
    glEnd();
    glPopMatrix();
    glEndList();
    CHECK(pixel(48, 32, kGreen));  // executed while compiling
    CHECK(pixel(16, 32, kRed));
    clear();
    glCallList(base + 1);
    CHECK(pixel(48, 32, kGreen));
    CHECK(pixel(16, 32, kBlack));
    float color[4] = {};
    glGetFloatv(GL_CURRENT_COLOR, color);
    CHECK(color[1] == 1 && color[0] == 0);  // the list's glColor became current
    // Vertex arrays are read when the list is compiled.
    float verts[8] = {-1, -1, 0, -1, 0, 1, -1, 1};
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(2, GL_FLOAT, 0, verts);
    glNewList(base + 2, GL_COMPILE);
    glColor3f(0, 0, 1);
    glDrawArrays(GL_QUADS, 0, 4);
    glEndList();
    for (float& v : verts) v = 5;  // the list keeps the old positions
    clear();
    glCallList(base + 2);
    CHECK(pixel(16, 32, kBlue));
    glDisableClientState(GL_VERTEX_ARRAY);
    // glCallLists with a base.
    clear();
    const GLubyte names[2] = {0, 2};
    glListBase(base);
    glCallLists(2, GL_UNSIGNED_BYTE, names);
    CHECK(pixel(16, 32, kBlue));
    CHECK(pixel(48, 32, kRed));
    glListBase(0);
    glDeleteLists(base, 3);
    CHECK(glIsList(base) == GL_FALSE);
    CHECK_GL();
}

COMPAT_TEST(compat_client_arrays) {
    clear();
    pixelProjection();
    struct Vertex {
        float x, y;
        uint8_t r, g, b, a;
    };
    const Vertex quad[4] = {{0, 0, 255, 0, 0, 255}, {32, 0, 255, 0, 0, 255}, {32, 64, 255, 0, 0, 255}, {0, 64, 255, 0, 0, 255}};
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glVertexPointer(2, GL_FLOAT, sizeof(Vertex), &quad[0].x);
    glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(Vertex), &quad[0].r);
    glDrawArrays(GL_QUADS, 0, 4);
    CHECK(pixel(16, 32, kRed));
    CHECK(pixel(48, 32, kBlack));
    // Indexed, from a buffer object, with a fixed colour (colour array off).
    glDisableClientState(GL_COLOR_ARRAY);
    const float right[8] = {32, 0, 64, 0, 64, 64, 32, 64};
    GLuint vbo = 0;
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(right), right, GL_STATIC_DRAW);
    glVertexPointer(2, GL_FLOAT, 0, nullptr);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glColor3f(0, 1, 0);
    const GLushort indices[6] = {0, 1, 2, 0, 2, 3};
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, indices);
    CHECK(pixel(48, 32, kGreen));
    CHECK(glIsEnabled(GL_VERTEX_ARRAY) == GL_TRUE);
    GLint size = 0;
    glGetIntegerv(GL_VERTEX_ARRAY_SIZE, &size);
    CHECK_EQ(size, 2);
    // Immediate mode with glArrayElement.
    clear();
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glVertexPointer(2, GL_FLOAT, sizeof(Vertex), &quad[0].x);
    glColor3f(0, 0, 1);
    glBegin(GL_QUADS);
    for (int i = 0; i < 4; ++i) glArrayElement(i);
    glEnd();
    CHECK(pixel(16, 32, kBlue));
    glDisableClientState(GL_VERTEX_ARRAY);
    glDeleteBuffers(1, &vbo);
    CHECK_GL();
}

COMPAT_TEST(compat_legacy_glsl_builtins) {
    // GLSL 1.20 reading the fixed-function matrices, gl_Color and gl_TexCoord.
    GLuint p = gpu::program(R"(#version 120
void main() {
    gl_Position = gl_ModelViewProjectionMatrix * gl_Vertex;
    gl_FrontColor = gl_Color;
    gl_TexCoord[0] = vec4(gl_Normal, 1.0);
})",
                            R"(#version 120
void main() { gl_FragColor = gl_Color * gl_TexCoord[0]; })");
    clear();
    pixelProjection();
    glUseProgram(p);
    glColor3f(1, 1, 0);
    glNormal3f(0, 1, 1);
    glRectf(0, 0, 32, 64);
    CHECK(pixel(16, 32, kGreen));
    CHECK(pixel(48, 32, kBlack));
    // The alpha test applies to shader output too.
    glEnable(GL_ALPHA_TEST);
    glAlphaFunc(GL_LESS, 0.5f);
    glRectf(32, 0, 64, 64);  // alpha 1: discarded
    CHECK(pixel(48, 32, kBlack));
    glDisable(GL_ALPHA_TEST);
    // A user clip plane through gl_ClipVertex.
    GLuint clipped = gpu::program(R"(#version 120
void main() { gl_Position = ftransform(); gl_ClipVertex = gl_ModelViewMatrix * gl_Vertex; gl_FrontColor = gl_Color; })",
                                  R"(#version 120
void main() { gl_FragColor = gl_Color; })");
    glUseProgram(clipped);
    const double plane[4] = {1, 0, 0, -32};  // keep x >= 32
    glClipPlane(GL_CLIP_PLANE0, plane);
    glEnable(GL_CLIP_PLANE0);
    clear();
    glColor3f(1, 0, 0);
    glRectf(0, 0, 64, 64);
    CHECK(pixel(16, 32, kBlack));
    CHECK(pixel(48, 32, kRed));
    glDisable(GL_CLIP_PLANE0);
    // Built-in attributes are reported as gl_* (location -1).
    GLint count = 0;
    glGetProgramiv(p, GL_ACTIVE_ATTRIBUTES, &count);
    CHECK_EQ(count, 3);
    CHECK_EQ(glGetAttribLocation(p, "gl_Vertex"), -1);
    GLint uniforms = -1;
    glGetProgramiv(p, GL_ACTIVE_UNIFORM_BLOCKS, &uniforms);
    CHECK_EQ(uniforms, 0);  // the fixed-function block is internal
    glUseProgram(0);
    glDeleteProgram(p);
    glDeleteProgram(clipped);
    CHECK_GL();
}

COMPAT_TEST(compat_push_pop_attrib) {
    glColor3f(1, 0, 0);
    glEnable(GL_LIGHTING);
    glPushAttrib(GL_CURRENT_BIT | GL_ENABLE_BIT);
    glColor3f(0, 1, 0);
    glDisable(GL_LIGHTING);
    glEnable(GL_FOG);
    glPopAttrib();
    float color[4] = {};
    glGetFloatv(GL_CURRENT_COLOR, color);
    CHECK(color[0] == 1 && color[1] == 0);
    CHECK(glIsEnabled(GL_LIGHTING) == GL_TRUE);
    CHECK(glIsEnabled(GL_FOG) == GL_FALSE);
    glDisable(GL_LIGHTING);
    glPopAttrib();
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_STACK_UNDERFLOW));
    CHECK_GL();
}

COMPAT_TEST(compat_bitmap_and_draw_pixels) {
    clear();
    pixelProjection();
    // An 8x8 bitmap: the left 4 columns set (bits MSB first).
    GLubyte bits[8];
    for (GLubyte& b : bits) b = 0xF0;
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glColor3f(0, 1, 0);
    glRasterPos2f(10, 10);
    glBitmap(8, 8, 0, 0, 8, 0, bits);
    CHECK(pixel(11, 12, kGreen));
    CHECK(pixel(16, 12, kBlack));
    glBitmap(8, 8, 0, 0, 8, 0, bits);  // advanced by xmove
    CHECK(pixel(19, 12, kGreen));
    float raster[4] = {};
    glGetFloatv(GL_CURRENT_RASTER_POSITION, raster);
    CHECK(raster[0] == 26 && raster[1] == 10);
    // DrawPixels at a window position, zoomed 2x.
    uint8_t image[4 * 4 * 4];
    for (int i = 0; i < 16; ++i) {
        image[i * 4] = 0;
        image[i * 4 + 1] = 0;
        image[i * 4 + 2] = 255;
        image[i * 4 + 3] = 255;
    }
    glWindowPos2i(40, 40);
    glPixelZoom(2, 2);
    glDrawPixels(4, 4, GL_RGBA, GL_UNSIGNED_BYTE, image);
    glPixelZoom(1, 1);
    CHECK(pixel(41, 41, kBlue));
    CHECK(pixel(47, 47, kBlue));
    CHECK(pixel(49, 49, kBlack));
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    CHECK_GL();
}

COMPAT_TEST(compat_framebuffer_bits) {
    GLint red = 0, depth = 0;
    glGetIntegerv(GL_RED_BITS, &red);
    glGetIntegerv(GL_DEPTH_BITS, &depth);
    CHECK_EQ(red, 8);
    CHECK(depth >= 16);
    GLint lights = 0, units = 0;
    glGetIntegerv(GL_MAX_LIGHTS, &lights);
    glGetIntegerv(GL_MAX_TEXTURE_UNITS, &units);
    CHECK_EQ(lights, 8);
    CHECK_EQ(units, 8);
    CHECK_GL();
}

COMPAT_TEST(compat_transform_feedback_legacy_varyings) {
    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    const char* source = "#version 120\nvoid main() { gl_Position = gl_Vertex; gl_FrontColor = gl_Color; }\n";
    glShaderSource(vs, 1, &source, nullptr);
    glCompileShader(vs);
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    const char* names[2] = {"gl_Position", "gl_FrontColor"};
    glTransformFeedbackVaryings(p, 2, names, GL_INTERLEAVED_ATTRIBS);
    glLinkProgram(p);
    GLint linked = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &linked);
    CHECK(linked == GL_TRUE);
    glUseProgram(p);
    GLuint buffer = 0;
    glGenBuffers(1, &buffer);
    glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, buffer);
    glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, 24 * sizeof(float), nullptr, GL_STATIC_DRAW);
    glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, buffer);
    glEnable(GL_RASTERIZER_DISCARD);
    glBeginTransformFeedback(GL_TRIANGLES);
    glBegin(GL_TRIANGLES);
    glColor4f(1, 0, 0, 1);
    glVertex3f(-1, -1, 0);
    glColor4f(0, 1, 0, 1);
    glVertex3f(3, -1, 0);
    glVertex3f(-1, 3, 0);
    glEnd();
    glEndTransformFeedback();
    glDisable(GL_RASTERIZER_DISCARD);
    float out[24] = {};
    glGetBufferSubData(GL_TRANSFORM_FEEDBACK_BUFFER, 0, sizeof(out), out);
    CHECK(out[0] == -1 && out[1] == -1 && out[3] == 1);
    CHECK(out[4] == 1 && out[5] == 0 && out[7] == 1);  // vertex 0: red
    CHECK(out[8] == 3 && out[12] == 0 && out[13] == 1);  // vertex 1: green
    glUseProgram(0);
    glDeleteBuffers(1, &buffer);
    glDeleteProgram(p);
    glDeleteShader(vs);
    CHECK_GL();
}

COMPAT_TEST(compat_fragment_only_program) {
    // No vertex shader: fixed-function vertex processing feeds the fragment shader.
    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    const char* source = "#version 120\nvoid main() { gl_FragColor = gl_Color.bgra; }\n";
    glShaderSource(fs, 1, &source, nullptr);
    glCompileShader(fs);
    GLuint p = glCreateProgram();
    glAttachShader(p, fs);
    glLinkProgram(p);
    GLint linked = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &linked);
    CHECK(linked == GL_TRUE);
    clear();
    pixelProjection();
    glUseProgram(p);
    glColor3f(1, 0, 0);
    glRectf(0, 0, 32, 64);
    CHECK(pixel(16, 32, kBlue));
    CHECK(pixel(48, 32, kBlack));
    glUseProgram(0);
    glDeleteProgram(p);
    glDeleteShader(fs);
    CHECK_GL();
}
