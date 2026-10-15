// ARB assembly programs (GL_ARB_vertex_program, GL_ARB_fragment_program;
// issue #25): drawn through their GLSL translation, alone and with
// fixed-function code for the other stage.

#include <cstring>
#include <string>

#include "gpu.h"

#define WMGL_ARB_FUNCS(X)                                                                                        \
    X(glBegin) X(glEnd) X(glVertex2f) X(glColor4f) X(glTexCoord2f) X(glMultiTexCoord4f) X(glMatrixMode)           \
    X(glLoadIdentity) X(glTranslatef) X(glScalef) X(glVertexPointer) X(glColorPointer) X(glTexCoordPointer)        \
    X(glEnableClientState) X(glDisableClientState) X(glClientActiveTexture) X(glIsEnabled) X(glShadeModel)          \
    X(glTexEnvi) X(glGenProgramsARB) X(glDeleteProgramsARB) X(glBindProgramARB) X(glIsProgramARB)                 \
    X(glProgramStringARB) X(glGetProgramivARB) X(glGetProgramStringARB) X(glProgramEnvParameter4fARB)              \
    X(glProgramLocalParameter4fARB) X(glProgramEnvParameters4fvEXT) X(glGetProgramEnvParameterfvARB)               \
    X(glGetProgramLocalParameterfvARB) X(glVertexAttribPointerARB) X(glEnableVertexAttribArrayARB)                 \
    X(glDisableVertexAttribArrayARB) X(glPushAttrib) X(glPopAttrib)

#define WMGL_ARB_DECLARE(name) static PFN_##name name;
WMGL_ARB_FUNCS(WMGL_ARB_DECLARE)
#undef WMGL_ARB_DECLARE

namespace {

void loadArb() {
#define WMGL_ARB_LOAD(name) name = reinterpret_cast<PFN_##name>(wmgl_get_proc_address(#name));
    WMGL_ARB_FUNCS(WMGL_ARB_LOAD)
#undef WMGL_ARB_LOAD
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

// Loads `source` into a new program object bound to `target`; checks it loaded.
GLuint load(GLenum target, const char* source) {
    GLuint id = 0;
    glGenProgramsARB(1, &id);
    glBindProgramARB(target, id);
    glProgramStringARB(target, GL_PROGRAM_FORMAT_ASCII_ARB, static_cast<GLsizei>(std::strlen(source)), source);
    const GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        GLint position = 0;
        glGetIntegerv(GL_PROGRAM_ERROR_POSITION_ARB, &position);
        std::printf("  program failed to load at %d: %s\n", position,
                    reinterpret_cast<const char*>(glGetString(GL_PROGRAM_ERROR_STRING_ARB)));
    }
    CHECK_EQ(error, static_cast<GLenum>(GL_NO_ERROR));
    return id;
}

// A quad over x in [x0, x1] (clip space), full height, in immediate mode.
void quad(float x0, float x1) {
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0);
    glVertex2f(x0, -1);
    glTexCoord2f(1, 0);
    glVertex2f(x1, -1);
    glTexCoord2f(1, 1);
    glVertex2f(x1, 1);
    glTexCoord2f(0, 1);
    glVertex2f(x0, 1);
    glEnd();
}

void identity() {
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
}

// A 2x2 RGBA texture on the active unit: red, green / blue, white.
GLuint checkerTexture() {
    const uint8_t texels[16] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255};
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    return tex;
}

const char kVp[] =
    "!!ARBvp1.0\n"
    "PARAM mvp[4] = { state.matrix.mvp };\n"
    "DP4 result.position.x, mvp[0], vertex.position;\n"
    "DP4 result.position.y, mvp[1], vertex.position;\n"
    "DP4 result.position.z, mvp[2], vertex.position;\n"
    "DP4 result.position.w, mvp[3], vertex.position;\n"
    "MOV result.color, vertex.color;\n"
    "MOV result.texcoord[0], vertex.texcoord[0];\n"
    "END\n";

}  // namespace

#define ARB_TEST(name)                                \
    static void name##_body();                        \
    TEST(name) {                                      \
        gpu::Fixture fixture_(64, 64, false);         \
        CHECK(fixture_.ok);                           \
        loadArb();                                    \
        if (fixture_.ok) name##_body();               \
    }                                                 \
    static void name##_body()

ARB_TEST(arb_extensions_and_limits) {
    // What Doom 3: BFG Edition checks before it starts (issue #25).
    const std::string extensions = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
    CHECK(extensions.find("GL_ARB_fragment_program ") != std::string::npos);
    CHECK(extensions.find("GL_ARB_vertex_program ") != std::string::npos);
    GLint coords = 0, units = 0, matrices = 0;
    glGetIntegerv(GL_MAX_TEXTURE_COORDS_ARB, &coords);
    glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS_ARB, &units);
    glGetIntegerv(GL_MAX_PROGRAM_MATRICES_ARB, &matrices);
    CHECK(coords >= 8);
    CHECK(units >= 16);
    CHECK_EQ(matrices, 8);
    GLint value = 0;
    glGetProgramivARB(GL_VERTEX_PROGRAM_ARB, GL_MAX_PROGRAM_ENV_PARAMETERS_ARB, &value);
    CHECK(value >= 96);
    glGetProgramivARB(GL_FRAGMENT_PROGRAM_ARB, GL_MAX_PROGRAM_LOCAL_PARAMETERS_ARB, &value);
    CHECK(value >= 24);
    glGetProgramivARB(GL_FRAGMENT_PROGRAM_ARB, GL_MAX_PROGRAM_TEX_INSTRUCTIONS_ARB, &value);
    CHECK(value >= 24);
    glGetProgramivARB(GL_VERTEX_PROGRAM_ARB, GL_MAX_PROGRAM_ALU_INSTRUCTIONS_ARB, &value);
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_ENUM));  // fragment programs only
    CHECK_GL();
}

ARB_TEST(arb_vertex_and_fragment_programs_draw) {
    identity();
    clear();
    const GLuint tex = checkerTexture();
    const GLuint vp = load(GL_VERTEX_PROGRAM_ARB, kVp);
    const GLuint fp = load(GL_FRAGMENT_PROGRAM_ARB,
                           "!!ARBfp1.0\n"
                           "TEMP t;\n"
                           "TEX t, fragment.texcoord[0], texture[0], 2D;\n"
                           "MUL t, t, fragment.color;\n"
                           "MUL result.color, t, program.local[0];\n"
                           "END\n");
    glProgramLocalParameter4fARB(GL_FRAGMENT_PROGRAM_ARB, 0, 1, 1, 1, 1);
    glEnable(GL_VERTEX_PROGRAM_ARB);
    glEnable(GL_FRAGMENT_PROGRAM_ARB);
    glColor4f(1, 1, 1, 1);
    quad(-1, 1);
    CHECK_GL();
    CHECK(pixel(16, 16, kRed));
    CHECK(pixel(48, 16, kGreen));
    CHECK(pixel(16, 48, kBlue));
    CHECK(pixel(48, 48, kWhite));
    // The vertex colour and a local parameter scale it.
    clear();
    glColor4f(1, 0, 1, 1);
    glProgramLocalParameter4fARB(GL_FRAGMENT_PROGRAM_ARB, 0, 0, 1, 1, 1);
    quad(-1, 1);
    CHECK(pixel(48, 48, kBlue));
    CHECK(pixel(16, 16, kBlack));
    glDisable(GL_VERTEX_PROGRAM_ARB);
    glDisable(GL_FRAGMENT_PROGRAM_ARB);
    glDeleteProgramsARB(1, &vp);
    glDeleteProgramsARB(1, &fp);
    glDeleteTextures(1, &tex);
    CHECK_GL();
}

ARB_TEST(arb_env_parameters_change_between_draws) {
    // Doom 3 sets its light parameters with ProgramEnvParameter between draws.
    identity();
    clear();
    load(GL_FRAGMENT_PROGRAM_ARB, "!!ARBfp1.0\nMOV result.color, program.env[7];\nEND\n");
    glEnable(GL_FRAGMENT_PROGRAM_ARB);
    glProgramEnvParameter4fARB(GL_FRAGMENT_PROGRAM_ARB, 7, 1, 0, 0, 1);
    quad(-1, 0);
    const float green[4] = {0, 1, 0, 1};
    glProgramEnvParameters4fvEXT(GL_FRAGMENT_PROGRAM_ARB, 7, 1, green);
    quad(0, 1);
    CHECK_GL();
    CHECK(pixel(16, 32, kRed));
    CHECK(pixel(48, 32, kGreen));
    float got[4] = {};
    glGetProgramEnvParameterfvARB(GL_FRAGMENT_PROGRAM_ARB, 7, got);
    CHECK(got[0] == 0 && got[1] == 1 && got[3] == 1);
    // The vertex target's environment is separate.
    glGetProgramEnvParameterfvARB(GL_VERTEX_PROGRAM_ARB, 7, got);
    CHECK(got[0] == 0 && got[1] == 0 && got[3] == 0);
    glDisable(GL_FRAGMENT_PROGRAM_ARB);
    CHECK_GL();
}

ARB_TEST(arb_fragment_program_with_fixed_function_vertex) {
    // Texture coordinates of a unit with no texture enabled, through the
    // fixed-function vertex stage (its texture matrix applied).
    identity();
    clear();
    load(GL_FRAGMENT_PROGRAM_ARB, "!!ARBfp1.0\nMOV result.color, fragment.texcoord[3];\nEND\n");
    glEnable(GL_FRAGMENT_PROGRAM_ARB);
    glActiveTexture(GL_TEXTURE3);
    glMatrixMode(GL_TEXTURE);
    glLoadIdentity();
    glScalef(0.5f, 1, 1);
    glMatrixMode(GL_MODELVIEW);
    glActiveTexture(GL_TEXTURE0);
    glMultiTexCoord4f(GL_TEXTURE3, 1, 1, 0, 1);
    glBegin(GL_TRIANGLES);
    glVertex2f(-1, -1);
    glVertex2f(3, -1);
    glVertex2f(-1, 3);
    glEnd();
    CHECK_GL();
    const uint8_t want[4] = {128, 255, 0, 255};
    CHECK(pixel(32, 32, want));
    glActiveTexture(GL_TEXTURE3);
    glMatrixMode(GL_TEXTURE);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glActiveTexture(GL_TEXTURE0);
    glDisable(GL_FRAGMENT_PROGRAM_ARB);
}

ARB_TEST(arb_vertex_program_with_fixed_function_fragment) {
    // A vertex program's colour and coordinates into fixed-function texturing.
    identity();
    clear();
    const GLuint tex = checkerTexture();
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    load(GL_VERTEX_PROGRAM_ARB,
         "!!ARBvp1.0\nOPTION ARB_position_invariant;\n"
         "MOV result.color, program.local[2];\nMOV result.texcoord[0], vertex.texcoord[0];\nEND\n");
    glProgramLocalParameter4fARB(GL_VERTEX_PROGRAM_ARB, 2, 0, 1, 1, 1);
    glEnable(GL_VERTEX_PROGRAM_ARB);
    quad(-1, 1);
    CHECK_GL();
    CHECK(pixel(16, 16, kBlack));   // red * (0, 1, 1)
    CHECK(pixel(48, 16, kGreen));
    CHECK(pixel(16, 48, kBlue));
    const uint8_t cyan[4] = {0, 255, 255, 255};
    CHECK(pixel(48, 48, cyan));
    glDisable(GL_VERTEX_PROGRAM_ARB);
    glDisable(GL_TEXTURE_2D);
    glDeleteTextures(1, &tex);
}

ARB_TEST(arb_generic_attributes_alias_conventional_ones) {
    // Doom 3 feeds vertex.texcoord through generic attribute 8 and reads
    // generic attribute 9 that glTexCoordPointer of unit 1 supplies.
    identity();
    clear();
    load(GL_VERTEX_PROGRAM_ARB,
         "!!ARBvp1.0\nOPTION ARB_position_invariant;\n"
         "ADD result.color, vertex.texcoord[0], vertex.attrib[9];\nEND\n");
    glEnable(GL_VERTEX_PROGRAM_ARB);
    const float positions[8] = {-1, -1, 3, -1, -1, 3, 0, 0};
    const float red[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    const float blue[12] = {0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1};
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(2, GL_FLOAT, 0, positions);
    glVertexAttribPointerARB(8, 4, GL_FLOAT, GL_FALSE, 0, red);
    glEnableVertexAttribArrayARB(8);
    glClientActiveTexture(GL_TEXTURE1);
    glTexCoordPointer(4, GL_FLOAT, 0, blue);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glClientActiveTexture(GL_TEXTURE0);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    CHECK_GL();
    const uint8_t magenta[4] = {255, 0, 255, 255};
    CHECK(pixel(32, 32, magenta));
    glDisableVertexAttribArrayARB(8);
    glClientActiveTexture(GL_TEXTURE1);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glClientActiveTexture(GL_TEXTURE0);
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisable(GL_VERTEX_PROGRAM_ARB);
}

ARB_TEST(arb_relative_addressing_kill_and_program_matrices) {
    identity();
    clear();
    // ARL picks a colour from a parameter array; a program matrix moves the quad.
    load(GL_VERTEX_PROGRAM_ARB,
         "!!ARBvp1.0\n"
         "ADDRESS a;\n"
         "PARAM colors[3] = { { 1, 0, 0, 1 }, { 0, 1, 0, 1 }, program.env[0] };\n"
         "PARAM m[4] = { state.matrix.program[1] };\n"
         "ARL a.x, vertex.color.x;\n"
         "MOV result.color, colors[a.x + 1];\n"
         "DP4 result.position.x, m[0], vertex.position;\n"
         "DP4 result.position.y, m[1], vertex.position;\n"
         "DP4 result.position.z, m[2], vertex.position;\n"
         "DP4 result.position.w, m[3], vertex.position;\n"
         "END\n");
    // Fragments left of the middle (x < 0.5 of the window) are discarded.
    load(GL_FRAGMENT_PROGRAM_ARB,
         "!!ARBfp1.0\n"
         "PARAM half = { 32, 0, 0, 0 };\n"
         "TEMP d;\n"
         "SUB d, fragment.position, half;\n"
         "KIL d.x;\n"
         "MOV result.color, fragment.color;\n"
         "END\n");
    glProgramEnvParameter4fARB(GL_VERTEX_PROGRAM_ARB, 0, 0, 0, 1, 1);
    glMatrixMode(GL_MATRIX1_ARB);
    glLoadIdentity();
    glScalef(2, 1, 1);  // the quad below covers the whole window once scaled
    glMatrixMode(GL_MODELVIEW);
    glEnable(GL_VERTEX_PROGRAM_ARB);
    glEnable(GL_FRAGMENT_PROGRAM_ARB);
    glColor4f(1, 0, 0, 1);  // ARL 1: colors[2] = env[0], blue
    quad(-0.5f, 0.5f);
    CHECK_GL();
    CHECK(pixel(48, 32, kBlue));
    CHECK(pixel(16, 32, kBlack));
    GLint depth = 0;
    glMatrixMode(GL_MATRIX1_ARB);
    glGetIntegerv(GL_CURRENT_MATRIX_STACK_DEPTH_ARB, &depth);
    CHECK_EQ(depth, 1);
    float m[16] = {};
    glGetFloatv(GL_CURRENT_MATRIX_ARB, m);
    CHECK(m[0] == 2 && m[5] == 1);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glDisable(GL_VERTEX_PROGRAM_ARB);
    glDisable(GL_FRAGMENT_PROGRAM_ARB);
    CHECK_GL();
}

ARB_TEST(arb_program_errors_and_queries) {
    const char bad[] = "!!ARBfp1.0\nMOV result.color, nowhere;\nEND\n";
    GLuint id = 0;
    glGenProgramsARB(1, &id);
    CHECK(!glIsProgramARB(id));  // a program object once bound
    glBindProgramARB(GL_FRAGMENT_PROGRAM_ARB, id);
    CHECK(glIsProgramARB(id));
    glProgramStringARB(GL_FRAGMENT_PROGRAM_ARB, GL_PROGRAM_FORMAT_ASCII_ARB, static_cast<GLsizei>(std::strlen(bad)), bad);
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_OPERATION));
    GLint position = 0;
    glGetIntegerv(GL_PROGRAM_ERROR_POSITION_ARB, &position);
    CHECK_EQ(position, static_cast<GLint>(std::strstr(bad, "nowhere") - bad));
    CHECK(std::strlen(reinterpret_cast<const char*>(glGetString(GL_PROGRAM_ERROR_STRING_ARB))) > 0);
    // Drawing with an enabled program that never loaded is an error.
    glEnable(GL_FRAGMENT_PROGRAM_ARB);
    quad(-1, 1);
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_OPERATION));
    // A good string loads; a bad one after it keeps it.
    const char good[] = "!!ARBfp1.0\nTEMP t;\nMOV t, fragment.color;\nMOV result.color, t;\nEND\n";
    glProgramStringARB(GL_FRAGMENT_PROGRAM_ARB, GL_PROGRAM_FORMAT_ASCII_ARB, static_cast<GLsizei>(std::strlen(good)), good);
    CHECK_GL();
    glGetIntegerv(GL_PROGRAM_ERROR_POSITION_ARB, &position);
    CHECK_EQ(position, -1);
    glProgramStringARB(GL_FRAGMENT_PROGRAM_ARB, GL_PROGRAM_FORMAT_ASCII_ARB, static_cast<GLsizei>(std::strlen(bad)), bad);
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_OPERATION));
    GLint value = 0;
    glGetProgramivARB(GL_FRAGMENT_PROGRAM_ARB, GL_PROGRAM_LENGTH_ARB, &value);
    CHECK_EQ(value, static_cast<GLint>(std::strlen(good)));
    char text[128] = {};
    glGetProgramStringARB(GL_FRAGMENT_PROGRAM_ARB, GL_PROGRAM_STRING_ARB, text);
    CHECK(std::strcmp(text, good) == 0);
    glGetProgramivARB(GL_FRAGMENT_PROGRAM_ARB, GL_PROGRAM_INSTRUCTIONS_ARB, &value);
    CHECK_EQ(value, 2);
    glGetProgramivARB(GL_FRAGMENT_PROGRAM_ARB, GL_PROGRAM_TEMPORARIES_ARB, &value);
    CHECK_EQ(value, 1);
    glGetProgramivARB(GL_FRAGMENT_PROGRAM_ARB, GL_PROGRAM_BINDING_ARB, &value);
    CHECK_EQ(value, static_cast<GLint>(id));
    clear();
    glColor4f(0, 1, 0, 1);
    quad(-1, 1);
    CHECK_GL();
    CHECK(pixel(32, 32, kGreen));
    // The program belongs to the fragment target; parameters past the limit.
    glBindProgramARB(GL_VERTEX_PROGRAM_ARB, id);
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_OPERATION));
    glProgramEnvParameter4fARB(GL_FRAGMENT_PROGRAM_ARB, 256, 0, 0, 0, 0);
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_VALUE));
    glProgramStringARB(GL_FRAGMENT_PROGRAM_ARB, GL_RGBA, 1, "x");
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_ENUM));
    // Deleting the bound program binds the default one, which has none loaded.
    glDeleteProgramsARB(1, &id);
    CHECK(!glIsProgramARB(id));
    glGetProgramivARB(GL_FRAGMENT_PROGRAM_ARB, GL_PROGRAM_BINDING_ARB, &value);
    CHECK_EQ(value, 0);
    quad(-1, 1);
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_OPERATION));
    glDisable(GL_FRAGMENT_PROGRAM_ARB);
    // The enables are GL_ENABLE_BIT state.
    glPushAttrib(GL_ENABLE_BIT);
    glEnable(GL_VERTEX_PROGRAM_ARB);
    glPopAttrib();
    CHECK(!glIsEnabled(GL_VERTEX_PROGRAM_ARB));
    CHECK_GL();
}

ARB_TEST(arb_glsl_program_takes_precedence) {
    // A current GLSL program draws even with assembly program modes on.
    identity();
    clear();
    load(GL_FRAGMENT_PROGRAM_ARB, "!!ARBfp1.0\nMOV result.color, { 1, 0, 0, 1 };\nEND\n");
    glEnable(GL_FRAGMENT_PROGRAM_ARB);
    const GLuint p = gpu::program("#version 120\nvoid main() { gl_Position = gl_Vertex; }\n",
                                  "#version 120\nvoid main() { gl_FragColor = vec4(0.0, 0.0, 1.0, 1.0); }\n");
    glUseProgram(p);
    quad(-1, 1);
    CHECK_GL();
    CHECK(pixel(32, 32, kBlue));
    glUseProgram(0);
    clear();
    quad(-1, 1);
    CHECK(pixel(32, 32, kRed));
    glDeleteProgram(p);
    glDisable(GL_FRAGMENT_PROGRAM_ARB);
    CHECK_GL();
}
