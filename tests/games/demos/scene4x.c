/* scene4x: a deterministic GL 4.x frame for WineMetalGL's game corpus
 * (tests/games). A tessellated terrain (control and evaluation shaders, an
 * indirect draw, a double-precision wave height) goes through a geometry
 * shader with two invocations that each send it to one viewport of a
 * viewport array; a subroutine picks the shading; an RGBA16F + depth render
 * target is tonemapped to the default framebuffer.
 *
 *   scene4x.exe [--frames N] [--size WxH] [--profile core41|...|core46]
 *               [--dump out.ppm] [--hidden] [--benchmark]
 *
 * core41 (a forward-compatible 4.1 core context) is what Apple's OpenGL can
 * run, so it records the reference. core42 renders the same frame with GL 4.2
 * features: the tonemap pass stores the frame to an rgba8 image and counts
 * its fragments with an atomic counter, and a second pass displays the image.
 * core43 draws the terrain with a multi-draw indirect command and tonemaps in
 * a compute shader that stores the image and counts its pixels in a shader
 * storage block. core44 keeps that compute tonemap and adds GL 4.4: buffer
 * storage (a persistent, coherent mapping of the pixel count, reset and read
 * through the mapping behind a fence), multi-bind, texture clears in place of
 * glClear, and enhanced layouts (the terrain's normal and height packed into
 * one location by components, an explicit block member offset). core45
 * creates every object with direct state access, culls triangles beyond the
 * left clip plane with a cull distance, and reads a pixel of the stored image
 * back with glGetTextureSubImage. core46 draws the terrain as four indirect
 * draws whose count comes from a parameter buffer, with gl_DrawID and
 * gl_BaseInstance cancelling out in the vertex shader (wrong values would move
 * the terrain); counts the submitted vertices and primitives and the compute
 * invocations with statistics queries; votes in the compute shader; and
 * displays the image with SPIR-V shaders. Every profile must produce the same
 * pixels.
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
#define GLFUNCS43(X) \
    X(PFNGLDISPATCHCOMPUTEPROC, glDispatchCompute) X(PFNGLMULTIDRAWARRAYSINDIRECTPROC, glMultiDrawArraysIndirect)
#define GLFUNCS44(X) \
    X(PFNGLBUFFERSTORAGEPROC, glBufferStorage) X(PFNGLMAPBUFFERRANGEPROC, glMapBufferRange) \
    X(PFNGLBINDTEXTURESPROC, glBindTextures) X(PFNGLBINDIMAGETEXTURESPROC, glBindImageTextures) \
    X(PFNGLBINDBUFFERSBASEPROC, glBindBuffersBase) X(PFNGLCLEARTEXIMAGEPROC, glClearTexImage) \
    X(PFNGLFENCESYNCPROC, glFenceSync) X(PFNGLCLIENTWAITSYNCPROC, glClientWaitSync) X(PFNGLDELETESYNCPROC, glDeleteSync)
#define GLFUNCS45(X) \
    X(PFNGLCREATEBUFFERSPROC, glCreateBuffers) X(PFNGLNAMEDBUFFERSTORAGEPROC, glNamedBufferStorage) \
    X(PFNGLMAPNAMEDBUFFERRANGEPROC, glMapNamedBufferRange) X(PFNGLCREATETEXTURESPROC, glCreateTextures) \
    X(PFNGLTEXTURESTORAGE2DPROC, glTextureStorage2D) X(PFNGLTEXTUREPARAMETERIPROC, glTextureParameteri) \
    X(PFNGLCREATEFRAMEBUFFERSPROC, glCreateFramebuffers) X(PFNGLNAMEDFRAMEBUFFERTEXTUREPROC, glNamedFramebufferTexture) \
    X(PFNGLCHECKNAMEDFRAMEBUFFERSTATUSPROC, glCheckNamedFramebufferStatus) \
    X(PFNGLCREATEVERTEXARRAYSPROC, glCreateVertexArrays) X(PFNGLVERTEXARRAYVERTEXBUFFERPROC, glVertexArrayVertexBuffer) \
    X(PFNGLVERTEXARRAYATTRIBFORMATPROC, glVertexArrayAttribFormat) \
    X(PFNGLVERTEXARRAYATTRIBBINDINGPROC, glVertexArrayAttribBinding) \
    X(PFNGLENABLEVERTEXARRAYATTRIBPROC, glEnableVertexArrayAttrib) X(PFNGLBINDTEXTUREUNITPROC, glBindTextureUnit) \
    X(PFNGLGETTEXTURESUBIMAGEPROC, glGetTextureSubImage)
#define GLFUNCS46(X) \
    X(PFNGLMULTIDRAWARRAYSINDIRECTCOUNTPROC, glMultiDrawArraysIndirectCount) \
    X(PFNGLSHADERBINARYPROC, glShaderBinary) X(PFNGLSPECIALIZESHADERPROC, glSpecializeShader) \
    X(PFNGLGENQUERIESPROC, glGenQueries) X(PFNGLBEGINQUERYPROC, glBeginQuery) X(PFNGLENDQUERYPROC, glEndQuery) \
    X(PFNGLGETQUERYOBJECTUI64VPROC, glGetQueryObjectui64v)
#define DECLARE(type, name) static type p_##name;
GLFUNCS(DECLARE)
GLFUNCS42(DECLARE)
GLFUNCS43(DECLARE)
GLFUNCS44(DECLARE)
GLFUNCS45(DECLARE)
GLFUNCS46(DECLARE)
#define LOAD(type, name) p_##name = (type)wglGetProcAddress(#name); if (!p_##name) { fprintf(stderr, "missing %s\n", #name); return 0; }

/* The first GL error after each step, with the step's name. */
static void check(const char* step) {
    static int reported = 0;
    const GLenum e = glGetError();
    if (e && !reported++) fprintf(stderr, "GL error 0x%04x after %s\n", e, step);
}

static int load_funcs(int profile) {
    GLFUNCS(LOAD)
    if (profile >= 42) { GLFUNCS42(LOAD) }
    if (profile >= 43) { GLFUNCS43(LOAD) }
    if (profile >= 44) { GLFUNCS44(LOAD) }
    if (profile >= 45) { GLFUNCS45(LOAD) }
    if (profile >= 46) { GLFUNCS46(LOAD) }
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

/* core43: a compute shader tonemaps into the image, counting its pixels. */
static const char* cs_tonemap43 =
    "#version 430 core\n"
    "layout(local_size_x = 8, local_size_y = 8) in;\n"
    "layout(binding = 0) uniform sampler2D hdr;\n"
    "layout(rgba8, binding = 0) writeonly uniform image2D ldr;\n"
    "layout(std430, binding = 0) buffer Count { uint written; };\n" TONEMAP
    "void main() {\n"
    "    ivec2 p = ivec2(gl_GlobalInvocationID.xy);\n"
    "    if (any(greaterThanEqual(p, textureSize(hdr, 0)))) return;\n"
    "    imageStore(ldr, p, vec4(tonemap(texelFetch(hdr, p, 0).rgb), 1.0));\n"
    "    atomicAdd(written, 1u);\n"
    "}\n";

/* GL 4.4 and later: the version line is a separate source string
 * (version_header), so one body serves 4.4 to 4.6; CULL (4.5+), DRAW_PARAMETERS
 * and VOTE (4.6) select what the profile adds. */
static const char* vs_terrain44 =
    "layout(location = 0) in vec2 grid;\n"
    "out vec2 v_grid;\n"
    "void main() {\n"
    "#if DRAW_PARAMETERS\n"
    "    /* Draw i of the four has base instance 7 * i: zero unless both are right. */\n"
    "    v_grid = grid + vec2(float(gl_BaseInstance - 7 * gl_DrawID));\n"
    "#else\n"
    "    v_grid = grid;\n"
    "#endif\n"
    "}\n";
/* Enhanced layouts: the normal and the height share location 0. */
static const char* tes_terrain44 =
    "layout(quads, equal_spacing, ccw) in;\n"
    "uniform dvec2 wave;  /* amplitude, phase */\n"
    "in vec2 c_grid[];\n"
    "layout(location = 0) out vec3 t_normal;\n"
    "layout(location = 0, component = 3) out float t_height;\n"
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
static const char* gs_terrain44 =
    "layout(triangles, invocations = 2) in;\n"
    "layout(triangle_strip, max_vertices = 3) out;\n"
    "uniform mat4 view_proj[2];\n"
    "layout(location = 0) in vec3 t_normal[];\n"
    "layout(location = 0, component = 3) in float t_height[];\n"
    "layout(location = 0) out vec3 g_normal;\n"
    "layout(location = 0, component = 3) out float g_height;\n"
    "#if CULL\n"
    "out float gl_CullDistance[1];\n"
    "#endif\n"
    "void main() {\n"
    "    for (int i = 0; i < 3; ++i) {\n"
    "        gl_Position = view_proj[gl_InvocationID] * gl_in[i].gl_Position;\n"
    "#if CULL\n"
    "        /* Beyond the left clip plane: culled, as clipping would drop it. */\n"
    "        gl_CullDistance[0] = gl_Position.x + gl_Position.w;\n"
    "#endif\n"
    "        gl_ViewportIndex = gl_InvocationID;\n"
    "        g_normal = t_normal[i]; g_height = t_height[i];\n"
    "        EmitVertex();\n"
    "    }\n"
    "    EndPrimitive();\n"
    "}\n";
static const char* fs_terrain44 =
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
    "layout(location = 0) in vec3 g_normal;\n"
    "layout(location = 0, component = 3) in float g_height;\n"
    "out vec4 color;\n"
    "void main() { color = vec4(shade(normalize(g_normal), g_height), 1.0); }\n";
/* The compute tonemap with an explicit member offset; core46 votes on
 * whether the whole subgroup is inside the image (the same stores either way). */
static const char* cs_tonemap44 =
    "layout(local_size_x = 8, local_size_y = 8) in;\n"
    "layout(binding = 0) uniform sampler2D hdr;\n"
    "layout(rgba8, binding = 0) writeonly uniform image2D ldr;\n"
    "layout(std430, binding = 0) buffer Count { layout(offset = 0) uint written; };\n" TONEMAP
    "void store(ivec2 p) {\n"
    "    imageStore(ldr, p, vec4(tonemap(texelFetch(hdr, p, 0).rgb), 1.0));\n"
    "    atomicAdd(written, 1u);\n"
    "}\n"
    "void main() {\n"
    "    ivec2 p = ivec2(gl_GlobalInvocationID.xy);\n"
    "    bool inside = all(lessThan(p, textureSize(hdr, 0)));\n"
    "#if VOTE\n"
    "    if (allInvocations(inside)) store(p);\n"
    "    else if (inside) store(p);\n"
    "#else\n"
    "    if (inside) store(p);\n"
    "#endif\n"
    "}\n";
/* core46's display pass as SPIR-V for OpenGL (glslangValidator 16.4 -G):
 *   vertex:   #version 460 core
 *             void main() {
 *                 vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
 *                 gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
 *             }
 *   fragment: #version 460 core
 *             layout(binding = 1) uniform sampler2D ldr;
 *             layout(location = 0) out vec4 color;
 *             void main() { color = texelFetch(ldr, ivec2(gl_FragCoord.xy), 0); }
 */
static const unsigned spv_show_vs[] = {
    0x07230203, 0x00010000, 0x0008000b, 0x0000002c, 0x00000000, 0x00020011, 0x00000001, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001,
    0x0008000f, 0x00000000, 0x00000004, 0x6e69616d, 0x00000000, 0x0000000c, 0x0000001d, 0x0000002b,
    0x00030003, 0x00000002, 0x000001cc, 0x00040005, 0x00000004, 0x6e69616d, 0x00000000, 0x00030005,
    0x00000009, 0x00000070, 0x00050005, 0x0000000c, 0x565f6c67, 0x65747265, 0x00444978, 0x00060005,
    0x0000001b, 0x505f6c67, 0x65567265, 0x78657472, 0x00000000, 0x00060006, 0x0000001b, 0x00000000,
    0x505f6c67, 0x7469736f, 0x006e6f69, 0x00070006, 0x0000001b, 0x00000001, 0x505f6c67, 0x746e696f,
    0x657a6953, 0x00000000, 0x00070006, 0x0000001b, 0x00000002, 0x435f6c67, 0x4470696c, 0x61747369,
    0x0065636e, 0x00070006, 0x0000001b, 0x00000003, 0x435f6c67, 0x446c6c75, 0x61747369, 0x0065636e,
    0x00030005, 0x0000001d, 0x00000000, 0x00060005, 0x0000002b, 0x495f6c67, 0x6174736e, 0x4965636e,
    0x00000044, 0x00040047, 0x0000000c, 0x0000000b, 0x00000005, 0x00030047, 0x0000001b, 0x00000002,
    0x00050048, 0x0000001b, 0x00000000, 0x0000000b, 0x00000000, 0x00050048, 0x0000001b, 0x00000001,
    0x0000000b, 0x00000001, 0x00050048, 0x0000001b, 0x00000002, 0x0000000b, 0x00000003, 0x00050048,
    0x0000001b, 0x00000003, 0x0000000b, 0x00000004, 0x00040047, 0x0000002b, 0x0000000b, 0x00000006,
    0x00020013, 0x00000002, 0x00030021, 0x00000003, 0x00000002, 0x00030016, 0x00000006, 0x00000020,
    0x00040017, 0x00000007, 0x00000006, 0x00000002, 0x00040020, 0x00000008, 0x00000007, 0x00000007,
    0x00040015, 0x0000000a, 0x00000020, 0x00000001, 0x00040020, 0x0000000b, 0x00000001, 0x0000000a,
    0x0004003b, 0x0000000b, 0x0000000c, 0x00000001, 0x0004002b, 0x0000000a, 0x0000000e, 0x00000001,
    0x0004002b, 0x0000000a, 0x00000010, 0x00000002, 0x00040017, 0x00000017, 0x00000006, 0x00000004,
    0x00040015, 0x00000018, 0x00000020, 0x00000000, 0x0004002b, 0x00000018, 0x00000019, 0x00000001,
    0x0004001c, 0x0000001a, 0x00000006, 0x00000019, 0x0006001e, 0x0000001b, 0x00000017, 0x00000006,
    0x0000001a, 0x0000001a, 0x00040020, 0x0000001c, 0x00000003, 0x0000001b, 0x0004003b, 0x0000001c,
    0x0000001d, 0x00000003, 0x0004002b, 0x0000000a, 0x0000001e, 0x00000000, 0x0004002b, 0x00000006,
    0x00000020, 0x40000000, 0x0004002b, 0x00000006, 0x00000022, 0x3f800000, 0x0004002b, 0x00000006,
    0x00000025, 0x00000000, 0x00040020, 0x00000029, 0x00000003, 0x00000017, 0x0004003b, 0x0000000b,
    0x0000002b, 0x00000001, 0x00050036, 0x00000002, 0x00000004, 0x00000000, 0x00000003, 0x000200f8,
    0x00000005, 0x0004003b, 0x00000008, 0x00000009, 0x00000007, 0x0004003d, 0x0000000a, 0x0000000d,
    0x0000000c, 0x000500c4, 0x0000000a, 0x0000000f, 0x0000000d, 0x0000000e, 0x000500c7, 0x0000000a,
    0x00000011, 0x0000000f, 0x00000010, 0x0004006f, 0x00000006, 0x00000012, 0x00000011, 0x0004003d,
    0x0000000a, 0x00000013, 0x0000000c, 0x000500c7, 0x0000000a, 0x00000014, 0x00000013, 0x00000010,
    0x0004006f, 0x00000006, 0x00000015, 0x00000014, 0x00050050, 0x00000007, 0x00000016, 0x00000012,
    0x00000015, 0x0003003e, 0x00000009, 0x00000016, 0x0004003d, 0x00000007, 0x0000001f, 0x00000009,
    0x0005008e, 0x00000007, 0x00000021, 0x0000001f, 0x00000020, 0x00050050, 0x00000007, 0x00000023,
    0x00000022, 0x00000022, 0x00050083, 0x00000007, 0x00000024, 0x00000021, 0x00000023, 0x00050051,
    0x00000006, 0x00000026, 0x00000024, 0x00000000, 0x00050051, 0x00000006, 0x00000027, 0x00000024,
    0x00000001, 0x00070050, 0x00000017, 0x00000028, 0x00000026, 0x00000027, 0x00000025, 0x00000022,
    0x00050041, 0x00000029, 0x0000002a, 0x0000001d, 0x0000001e, 0x0003003e, 0x0000002a, 0x00000028,
    0x000100fd, 0x00010038,
};
static const unsigned spv_show_fs[] = {
    0x07230203, 0x00010000, 0x0008000b, 0x0000001a, 0x00000000, 0x00020011, 0x00000001, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001,
    0x0007000f, 0x00000004, 0x00000004, 0x6e69616d, 0x00000000, 0x00000009, 0x00000010, 0x00030010,
    0x00000004, 0x00000008, 0x00030003, 0x00000002, 0x000001cc, 0x00040005, 0x00000004, 0x6e69616d,
    0x00000000, 0x00040005, 0x00000009, 0x6f6c6f63, 0x00000072, 0x00030005, 0x0000000d, 0x0072646c,
    0x00060005, 0x00000010, 0x465f6c67, 0x43676172, 0x64726f6f, 0x00000000, 0x00040047, 0x00000009,
    0x0000001e, 0x00000000, 0x00040047, 0x0000000d, 0x00000021, 0x00000001, 0x00040047, 0x0000000d,
    0x00000022, 0x00000000, 0x00040047, 0x00000010, 0x0000000b, 0x0000000f, 0x00020013, 0x00000002,
    0x00030021, 0x00000003, 0x00000002, 0x00030016, 0x00000006, 0x00000020, 0x00040017, 0x00000007,
    0x00000006, 0x00000004, 0x00040020, 0x00000008, 0x00000003, 0x00000007, 0x0004003b, 0x00000008,
    0x00000009, 0x00000003, 0x00090019, 0x0000000a, 0x00000006, 0x00000001, 0x00000000, 0x00000000,
    0x00000000, 0x00000001, 0x00000000, 0x0003001b, 0x0000000b, 0x0000000a, 0x00040020, 0x0000000c,
    0x00000000, 0x0000000b, 0x0004003b, 0x0000000c, 0x0000000d, 0x00000000, 0x00040020, 0x0000000f,
    0x00000001, 0x00000007, 0x0004003b, 0x0000000f, 0x00000010, 0x00000001, 0x00040017, 0x00000011,
    0x00000006, 0x00000002, 0x00040015, 0x00000014, 0x00000020, 0x00000001, 0x00040017, 0x00000015,
    0x00000014, 0x00000002, 0x0004002b, 0x00000014, 0x00000017, 0x00000000, 0x00050036, 0x00000002,
    0x00000004, 0x00000000, 0x00000003, 0x000200f8, 0x00000005, 0x0004003d, 0x0000000b, 0x0000000e,
    0x0000000d, 0x0004003d, 0x00000007, 0x00000012, 0x00000010, 0x0007004f, 0x00000011, 0x00000013,
    0x00000012, 0x00000012, 0x00000000, 0x00000001, 0x0004006e, 0x00000015, 0x00000016, 0x00000013,
    0x00040064, 0x0000000a, 0x00000018, 0x0000000e, 0x0007005f, 0x00000007, 0x00000019, 0x00000018,
    0x00000016, 0x00000002, 0x00000017, 0x0003003e, 0x00000009, 0x00000019, 0x000100fd, 0x00010038,
};

static char version_header[128];

static GLuint compile(GLenum type, const char* source) {
    GLuint shader = p_glCreateShader(type);
    GLint ok = 0;
    /* A source without a version line is a 4.4+ body (version_header). */
    const char* sources[2] = {version_header, source};
    if (!strncmp(source, "#version", 8)) p_glShaderSource(shader, 1, &source, NULL);
    else p_glShaderSource(shader, 2, sources, NULL);
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

static GLuint link_compute(const char* cs) {
    GLuint program = p_glCreateProgram();
    GLint ok = 0;
    p_glAttachShader(program, compile(GL_COMPUTE_SHADER, cs));
    p_glLinkProgram(program);
    p_glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) { char log[2048]; p_glGetProgramInfoLog(program, sizeof(log), NULL, log); fprintf(stderr, "link failed: %s\n", log); exit(2); }
    return program;
}

/* A program from SPIR-V vertex and fragment modules (GL 4.6). */
static GLuint link_spirv(const unsigned* vs, size_t vs_size, const unsigned* fs, size_t fs_size) {
    GLuint program = p_glCreateProgram(), shaders[2];
    GLint ok = 0;
    int i;
    shaders[0] = p_glCreateShader(GL_VERTEX_SHADER);
    shaders[1] = p_glCreateShader(GL_FRAGMENT_SHADER);
    p_glShaderBinary(1, &shaders[0], GL_SHADER_BINARY_FORMAT_SPIR_V, vs, (GLsizei)vs_size);
    p_glShaderBinary(1, &shaders[1], GL_SHADER_BINARY_FORMAT_SPIR_V, fs, (GLsizei)fs_size);
    for (i = 0; i < 2; ++i) {
        p_glSpecializeShader(shaders[i], "main", 0, NULL, NULL);
        p_glGetShaderiv(shaders[i], GL_COMPILE_STATUS, &ok);
        if (!ok) { char log[2048]; p_glGetShaderInfoLog(shaders[i], sizeof(log), NULL, log); fprintf(stderr, "specialize failed: %s\n", log); exit(2); }
        p_glAttachShader(program, shaders[i]);
    }
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
    int frames = 120, width = 640, height = 360, profile = 41, hidden = 0, benchmark = 0, i, frame;
    const char* dump = NULL;
    double* frame_ms = NULL;
    unsigned counted = 0;
    LARGE_INTEGER frequency, frame_start, frame_end;
    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--size") && i + 1 < argc) sscanf(argv[++i], "%dx%d", &width, &height);
        else if (!strcmp(argv[i], "--profile") && i + 1 < argc) {
            ++i;
            profile = !strncmp(argv[i], "core4", 5) && argv[i][5] >= '1' && argv[i][5] <= '6' && !argv[i][6]
                          ? 40 + (argv[i][5] - '0')
                          : 41;
        }
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
    int attribs[] = {WGL_CONTEXT_MAJOR_VERSION_ARB, 4, WGL_CONTEXT_MINOR_VERSION_ARB, profile - 40,
                     WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
                     WGL_CONTEXT_FLAGS_ARB, WGL_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB, 0};
    HGLRC context = create(dc, NULL, attribs);
    if (!context) { fprintf(stderr, "no GL 4.%d core context\n", profile - 40); return 1; }
    wglMakeCurrent(dc, context);
    wglDeleteContext(legacy);
    if (!load_funcs(profile)) return 1;
    printf("GL_VERSION %s\n", (const char*)glGetString(GL_VERSION));
    snprintf(version_header, sizeof(version_header), "#version %d0 core\n#define CULL %d\n#define DRAW_PARAMETERS %d\n#define VOTE %d\n",
             profile, profile >= 45, profile >= 46, profile >= 46);

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
    /* core46: four draws of a quarter of the patches each, base instance 7 * i. */
    GLuint commands46[4][4];
    const GLuint draw_count = 4;
    for (i = 0; i < 4; ++i) {
        commands46[i][0] = (GLuint)n / 4; commands46[i][1] = 1;
        commands46[i][2] = (GLuint)(i * n / 4); commands46[i][3] = (GLuint)(7 * i);
    }
    const void* command_data = profile >= 46 ? (const void*)commands46 : (const void*)command;
    const GLsizeiptr command_size = profile >= 46 ? (GLsizeiptr)sizeof(commands46) : (GLsizeiptr)sizeof(command);
    const GLbitfield count_flags = GL_MAP_READ_BIT | GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
    GLuint buffers[4], vaos[2], textures[3], fbo;
    volatile GLuint* count_map = NULL;  /* core44+: the pixel count, persistently mapped */
    if (profile <= 43) {
        p_glGenBuffers(3, buffers);
        p_glGenVertexArrays(2, vaos);
        p_glBindVertexArray(vaos[0]);
        p_glBindBuffer(GL_ARRAY_BUFFER, buffers[0]);
        p_glBufferData(GL_ARRAY_BUFFER, sizeof(grid), grid, GL_STATIC_DRAW);
        p_glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (void*)0);
        p_glEnableVertexAttribArray(0);
        p_glBindBuffer(GL_DRAW_INDIRECT_BUFFER, buffers[1]);
        p_glBufferData(GL_DRAW_INDIRECT_BUFFER, sizeof(command), command, GL_STATIC_DRAW);
    } else if (profile == 44) {
        /* Immutable buffer storage. */
        p_glGenBuffers(3, buffers);
        p_glGenVertexArrays(2, vaos);
        p_glBindVertexArray(vaos[0]);
        p_glBindBuffer(GL_ARRAY_BUFFER, buffers[0]);
        p_glBufferStorage(GL_ARRAY_BUFFER, sizeof(grid), grid, 0);
        p_glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (void*)0);
        p_glEnableVertexAttribArray(0);
        p_glBindBuffer(GL_DRAW_INDIRECT_BUFFER, buffers[1]);
        p_glBufferStorage(GL_DRAW_INDIRECT_BUFFER, command_size, command_data, 0);
        p_glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffers[2]);
        p_glBufferStorage(GL_SHADER_STORAGE_BUFFER, sizeof(GLuint), NULL, count_flags);
        count_map = (volatile GLuint*)p_glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, sizeof(GLuint), count_flags);
    } else {
        /* Direct state access: nothing is bound to be created or set up. */
        p_glCreateBuffers(4, buffers);
        p_glCreateVertexArrays(2, vaos);
        p_glNamedBufferStorage(buffers[0], sizeof(grid), grid, 0);
        p_glVertexArrayVertexBuffer(vaos[0], 0, buffers[0], 0, 2 * sizeof(float));
        p_glVertexArrayAttribFormat(vaos[0], 0, 2, GL_FLOAT, GL_FALSE, 0);
        p_glVertexArrayAttribBinding(vaos[0], 0, 0);
        p_glEnableVertexArrayAttrib(vaos[0], 0);
        p_glNamedBufferStorage(buffers[1], command_size, command_data, 0);
        p_glBindBuffer(GL_DRAW_INDIRECT_BUFFER, buffers[1]);
        p_glNamedBufferStorage(buffers[2], sizeof(GLuint), NULL, count_flags);
        count_map = (volatile GLuint*)p_glMapNamedBufferRange(buffers[2], 0, sizeof(GLuint), count_flags);
        if (profile >= 46) {
            p_glNamedBufferStorage(buffers[3], sizeof(draw_count), &draw_count, 0);
            p_glBindBuffer(GL_PARAMETER_BUFFER, buffers[3]);
        }
    }
    if (profile >= 44 && !count_map) { fprintf(stderr, "persistent map failed\n"); return 1; }
    p_glPatchParameteri(GL_PATCH_VERTICES, 4);
    check("patch setup");

    /* HDR target and, for core42 and later, the stored frame. */
    if (profile <= 44) {
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
    } else {
        p_glCreateTextures(GL_TEXTURE_2D, 3, textures);
        p_glTextureStorage2D(textures[0], 1, GL_RGBA16F, width, height);
        p_glTextureStorage2D(textures[1], 1, GL_DEPTH_COMPONENT24, width, height);
        p_glTextureStorage2D(textures[2], 1, GL_RGBA8, width, height);
        for (i = 0; i < 3; ++i) {
            p_glTextureParameteri(textures[i], GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            p_glTextureParameteri(textures[i], GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        }
        p_glCreateFramebuffers(1, &fbo);
        p_glNamedFramebufferTexture(fbo, GL_COLOR_ATTACHMENT0, textures[0], 0);
        p_glNamedFramebufferTexture(fbo, GL_DEPTH_ATTACHMENT, textures[1], 0);
        if (p_glCheckNamedFramebufferStatus(fbo, GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { fprintf(stderr, "FBO incomplete\n"); return 1; }
    }
    check("fbo setup");
    /* The pixel count: an atomic counter (core42) or a storage block (core43 and later). */
    const GLenum count_target = profile >= 43 ? GL_SHADER_STORAGE_BUFFER : GL_ATOMIC_COUNTER_BUFFER;
    if (profile == 42 || profile == 43) {
        glBindTexture(GL_TEXTURE_2D, textures[2]);
        p_glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, width, height);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        p_glBindBuffer(count_target, buffers[2]);
        p_glBufferData(count_target, sizeof(GLuint), NULL, GL_DYNAMIC_READ);
        p_glBindBufferBase(count_target, 0, buffers[2]);
    } else if (profile == 44) {
        glBindTexture(GL_TEXTURE_2D, textures[2]);
        p_glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, width, height);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }
    if (profile >= 44) p_glBindBuffersBase(GL_SHADER_STORAGE_BUFFER, 0, 1, &buffers[2]);

    check("resources");
    GLuint terrain = profile >= 44 ? link_stages(vs_terrain44, tcs_terrain, tes_terrain44, gs_terrain44, fs_terrain44)
                                   : link_stages(vs_terrain, tcs_terrain, tes_terrain, gs_terrain, fs_terrain);
    GLuint post = 0, store = 0, show = 0;
    const GLint view_proj = p_glGetUniformLocation(terrain, "view_proj"), wave = p_glGetUniformLocation(terrain, "wave");
    const GLuint shades[2] = {p_glGetSubroutineIndex(terrain, GL_FRAGMENT_SHADER, "sunny"),
                              p_glGetSubroutineIndex(terrain, GL_FRAGMENT_SHADER, "dusk")};
    if (profile >= 42) {
        store = profile >= 44 ? link_compute(cs_tonemap44)
                : profile == 43 ? link_compute(cs_tonemap43) : link_stages(vs_post, NULL, NULL, NULL, fs_store42);
        show = profile >= 46 ? link_spirv(spv_show_vs, sizeof(spv_show_vs), spv_show_fs, sizeof(spv_show_fs))
                             : link_stages(vs_post, NULL, NULL, NULL, fs_show42);
    } else {
        post = link_stages(vs_post, NULL, NULL, NULL, fs_post41);
        p_glUseProgram(post);
        p_glUniform1i(p_glGetUniformLocation(post, "hdr"), 0);
    }
    /* core46: statistics of the terrain draw and the tonemap dispatch. */
    GLuint queries[3] = {0, 0, 0};
    GLuint64 submitted_vertices = 0, submitted_primitives = 0, compute_invocations = 0;
    if (profile >= 46) p_glGenQueries(3, queries);
    GLsync fence = NULL;
    unsigned char stored_pixel[4] = {0, 0, 0, 0};

    unsigned char* readback = (unsigned char*)malloc((size_t)width * height * 4);
    frame_ms = calloc((size_t)frames, sizeof(double));
    QueryPerformanceFrequency(&frequency);
    if (benchmark) {
        PFNWGLSWAPINTERVALEXTPROC swap_interval = (PFNWGLSWAPINTERVALEXTPROC)wglGetProcAddress("wglSwapIntervalEXT");
        if (swap_interval) swap_interval(0);
    }
    for (frame = 0; frame < frames; ++frame) {
        const int last = frame == frames - 1;
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
        if (profile >= 44) {
            /* Texture clears in place of glClear. */
            static const float clear_color[4] = {0.05f, 0.07f, 0.12f, 1.0f}, far_depth = 1.0f;
            p_glClearTexImage(textures[0], 0, GL_RGBA, GL_FLOAT, clear_color);
            p_glClearTexImage(textures[1], 0, GL_DEPTH_COMPONENT, GL_FLOAT, &far_depth);
        } else {
            glClearColor(0.05f, 0.07f, 0.12f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        }
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
        if (profile >= 46) {
            if (last) {
                p_glBeginQuery(GL_VERTICES_SUBMITTED, queries[0]);
                p_glBeginQuery(GL_PRIMITIVES_SUBMITTED, queries[1]);
            }
            p_glMultiDrawArraysIndirectCount(GL_PATCHES, (void*)0, 0, 4, 0);
            if (last) {
                p_glEndQuery(GL_VERTICES_SUBMITTED);
                p_glEndQuery(GL_PRIMITIVES_SUBMITTED);
            }
        }
        else if (profile >= 43) p_glMultiDrawArraysIndirect(GL_PATCHES, (void*)0, 1, 0);
        else p_glDrawArraysIndirect(GL_PATCHES, (void*)0);
        check("terrain draw");
        glDisable(GL_DEPTH_TEST);

        /* Tonemap to the default framebuffer. */
        p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, width, height);
        p_glBindVertexArray(vaos[1]);
        if (profile >= 44) {
            /* The previous frame's count is read; reset it through the mapping. */
            if (fence) {
                p_glClientWaitSync(fence, GL_SYNC_FLUSH_COMMANDS_BIT, 5000000000ull);
                p_glDeleteSync(fence);
                fence = NULL;
            }
            *count_map = 0;
            if (profile >= 45) p_glBindTextureUnit(0, textures[0]);
            else p_glBindTextures(0, 1, &textures[0]);
            p_glBindImageTextures(0, 1, &textures[2]);
            p_glUseProgram(store);
            if (profile >= 46 && last) p_glBeginQuery(GL_COMPUTE_SHADER_INVOCATIONS, queries[2]);
            p_glDispatchCompute((GLuint)(width + 7) / 8, (GLuint)(height + 7) / 8, 1);
            if (profile >= 46 && last) p_glEndQuery(GL_COMPUTE_SHADER_INVOCATIONS);
            p_glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT | GL_TEXTURE_UPDATE_BARRIER_BIT);
            if (profile >= 45) p_glBindTextureUnit(1, textures[2]);
            else p_glBindTextures(1, 1, &textures[2]);
            p_glUseProgram(show);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            fence = p_glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        } else if (profile >= 42) {
            p_glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, textures[0]);
            const GLuint zero = 0;
            p_glBufferSubData(count_target, 0, sizeof(zero), &zero);
            p_glBindImageTexture(0, textures[2], 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
            p_glUseProgram(store);
            if (profile == 43) {
                p_glDispatchCompute((GLuint)(width + 7) / 8, (GLuint)(height + 7) / 8, 1);
                p_glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
            } else {
                glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
                glDrawArrays(GL_TRIANGLES, 0, 3);
                glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                p_glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_ATOMIC_COUNTER_BARRIER_BIT);
            }
            p_glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, textures[2]);
            p_glUseProgram(show);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            p_glActiveTexture(GL_TEXTURE0);
        } else {
            p_glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, textures[0]);
            p_glUseProgram(post);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }
        if (last) {
            glPixelStorei(GL_PACK_ALIGNMENT, 4);
            glReadBuffer(GL_BACK);
            glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, readback);
            if (profile >= 44) {
                p_glClientWaitSync(fence, GL_SYNC_FLUSH_COMMANDS_BIT, 5000000000ull);
                counted = *count_map;
            } else if (profile >= 42) {
                p_glGetBufferSubData(count_target, 0, sizeof(counted), &counted);
            }
            if (profile >= 45)
                p_glGetTextureSubImage(textures[2], 0, width / 2, height / 2, 0, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE,
                                       sizeof(stored_pixel), stored_pixel);
            if (profile >= 46) {
                p_glGetQueryObjectui64v(queries[0], GL_QUERY_RESULT, &submitted_vertices);
                p_glGetQueryObjectui64v(queries[1], GL_QUERY_RESULT, &submitted_primitives);
                p_glGetQueryObjectui64v(queries[2], GL_QUERY_RESULT, &compute_invocations);
            }
        }
        SwapBuffers(dc);
        QueryPerformanceCounter(&frame_end);
        frame_ms[frame] = 1000.0 * (double)(frame_end.QuadPart - frame_start.QuadPart) / (double)frequency.QuadPart;
    }
    if (benchmark) report_frame_times(frame_ms, frames);
    free(frame_ms);
    check("frame");
    GLenum error = glGetError();
    if (profile >= 42 && counted != (unsigned)(width * height)) {
        fprintf(stderr, "pixel count: %u, expected %d\n", counted, width * height);
        error = error ? error : GL_INVALID_OPERATION;
    }
    /* Readbacks: the stored image's centre is the displayed pixel; the
     * statistics match the draw and the dispatch. */
    if (profile >= 45 && memcmp(stored_pixel, readback + ((size_t)(height / 2) * width + width / 2) * 4, 4)) {
        fprintf(stderr, "glGetTextureSubImage: %02x%02x%02x%02x differs from the displayed pixel\n", stored_pixel[0],
                stored_pixel[1], stored_pixel[2], stored_pixel[3]);
        error = error ? error : GL_INVALID_OPERATION;
    }
    if (profile >= 46) {
        const GLuint64 invocations = (GLuint64)((width + 7) / 8 * 8) * (GLuint64)((height + 7) / 8 * 8);
        printf("statistics: vertices %llu, patches %llu, compute invocations %llu\n",
               (unsigned long long)submitted_vertices, (unsigned long long)submitted_primitives,
               (unsigned long long)compute_invocations);
        if (submitted_vertices != (GLuint64)n || submitted_primitives != (GLuint64)(n / 4) ||
            compute_invocations != invocations) {
            fprintf(stderr, "statistics differ: expected %d vertices, %d patches, %llu invocations\n", n, n / 4,
                    (unsigned long long)invocations);
            error = error ? error : GL_INVALID_OPERATION;
        }
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
