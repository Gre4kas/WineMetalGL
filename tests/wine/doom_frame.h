/* A stand-in for a DOOM (2016) frame through OpenGL, shaped by
 * WINEMETALGL_STATS on its gameplay (issue #36): about 680 indexed draws in
 * about 50 render passes, with
 * - a program change every few draws (about 145), among material,
 *   shadow and post-process programs;
 * - two attribute pointers a draw: positions from a static buffer, and four
 *   unnormalized shorts read as floats (a CPU conversion, as DOOM's 460 a
 *   frame) from a persistently mapped buffer the frame rewrites;
 * - a vec4-array uniform update a draw, a texture and sampler bind (and the
 *   active texture), a uniform buffer range every few draws;
 * - occlusion queries around about 40 draws, timestamps, compute dispatches,
 *   texture uploads.
 *
 * GL is called through DoomGL, which each harness fills: the Windows
 * program tests/wine/opengl_doom_bench.c (through opengl32 and the Wine
 * module) and the core's GPU test bench_doom_frame (native, for profiling).
 */
#ifndef WMGL_DOOM_FRAME_H
#define WMGL_DOOM_FRAME_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef APIENTRY
#define APIENTRY
#endif

#define DOOM_GL_FUNCS(X) \
    X(void, ActiveTexture, (GLenum)) \
    X(void, AttachShader, (GLuint, GLuint)) \
    X(void, BeginQuery, (GLenum, GLuint)) \
    X(void, BindBuffer, (GLenum, GLuint)) \
    X(void, BindBufferBase, (GLenum, GLuint, GLuint)) \
    X(void, BindBufferRange, (GLenum, GLuint, GLuint, intptr_t, intptr_t)) \
    X(void, BindFramebuffer, (GLenum, GLuint)) \
    X(void, BindSampler, (GLuint, GLuint)) \
    X(void, BindTexture, (GLenum, GLuint)) \
    X(void, BindVertexArray, (GLuint)) \
    X(void, BlendFunc, (GLenum, GLenum)) \
    X(void, BufferData, (GLenum, intptr_t, const void *, GLenum)) \
    X(void, BufferStorage, (GLenum, intptr_t, const void *, GLbitfield)) \
    X(GLenum, CheckFramebufferStatus, (GLenum)) \
    X(void, Clear, (GLbitfield)) \
    X(void, ClearColor, (float, float, float, float)) \
    X(void, CompileShader, (GLuint)) \
    X(GLuint, CreateProgram, (void)) \
    X(GLuint, CreateShader, (GLenum)) \
    X(void, DepthFunc, (GLenum)) \
    X(void, DepthMask, (GLboolean)) \
    X(void, Disable, (GLenum)) \
    X(void, DispatchCompute, (GLuint, GLuint, GLuint)) \
    X(void, DrawArrays, (GLenum, GLint, GLsizei)) \
    X(void, DrawBuffers, (GLsizei, const GLenum *)) \
    X(void, DrawElementsBaseVertex, (GLenum, GLsizei, GLenum, const void *, GLint)) \
    X(void, Enable, (GLenum)) \
    X(void, EnableVertexAttribArray, (GLuint)) \
    X(void, EndQuery, (GLenum)) \
    X(void, Finish, (void)) \
    X(void, Flush, (void)) \
    X(void, FramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint)) \
    X(void, GenBuffers, (GLsizei, GLuint *)) \
    X(void, GenFramebuffers, (GLsizei, GLuint *)) \
    X(void, GenQueries, (GLsizei, GLuint *)) \
    X(void, GenSamplers, (GLsizei, GLuint *)) \
    X(void, GenTextures, (GLsizei, GLuint *)) \
    X(void, GenVertexArrays, (GLsizei, GLuint *)) \
    X(GLenum, GetError, (void)) \
    X(void, GetProgramInfoLog, (GLuint, GLsizei, GLsizei *, char *)) \
    X(void, GetProgramiv, (GLuint, GLenum, GLint *)) \
    X(void, GetQueryObjectuiv, (GLuint, GLenum, GLuint *)) \
    X(void, GetShaderInfoLog, (GLuint, GLsizei, GLsizei *, char *)) \
    X(void, GetShaderiv, (GLuint, GLenum, GLint *)) \
    X(GLint, GetUniformLocation, (GLuint, const char *)) \
    X(void, LinkProgram, (GLuint)) \
    X(void *, MapBufferRange, (GLenum, intptr_t, intptr_t, GLbitfield)) \
    X(void, MemoryBarrier, (GLbitfield)) \
    X(void, QueryCounter, (GLuint, GLenum)) \
    X(void, SamplerParameteri, (GLuint, GLenum, GLint)) \
    X(void, ShaderSource, (GLuint, GLsizei, const char *const *, const GLint *)) \
    X(void, TexParameteri, (GLenum, GLenum, GLint)) \
    X(void, TexStorage2D, (GLenum, GLsizei, GLenum, GLsizei, GLsizei)) \
    X(void, TexSubImage2D, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *)) \
    X(void, Uniform1i, (GLint, GLint)) \
    X(void, Uniform4fv, (GLint, GLsizei, const float *)) \
    X(void, UseProgram, (GLuint)) \
    X(void, VertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *)) \
    X(void, Viewport, (GLint, GLint, GLsizei, GLsizei))

typedef struct DoomGL {
#define DOOM_GL_MEMBER(ret, name, args) ret(APIENTRY *name) args;
    DOOM_GL_FUNCS(DOOM_GL_MEMBER)
#undef DOOM_GL_MEMBER
} DoomGL;

/* Fills `gl` with `lookup("glName")`; returns the first missing name, or NULL. */
static const char *doom_gl_load(DoomGL *gl, void *(*lookup)(const char *name))
{
#define DOOM_GL_LOAD(ret, name, args) \
    if (!(*(void **)&gl->name = lookup("gl" #name))) return "gl" #name;
    DOOM_GL_FUNCS(DOOM_GL_LOAD)
#undef DOOM_GL_LOAD
    return NULL;
}

enum {
    kDoomMaterials = 48, kDoomShadowPrograms = 8, kDoomPostPrograms = 6, kDoomTextures = 128,
    kDoomVertices = 65536, kDoomIndices = 3000 * 400, kDoomShadowMaps = 4, kDoomPostTargets = 3,
    kDoomQueries = 40, kDoomTimestamps = 6, kDoomUboBytes = 1 << 20
};

typedef struct DoomFrame {
    const DoomGL *gl;
    int width, height;
    GLuint vao, positions, shorts, indices, ubo, samplers[8], textures[kDoomTextures];
    GLuint materials[kDoomMaterials], gbufferMaterials[kDoomMaterials / 2], shadows[kDoomShadowPrograms],
        posts[kDoomPostPrograms], compute, ssbo;
    GLint materialVa[kDoomMaterials], materialFa[kDoomMaterials], gbufferVa[kDoomMaterials / 2],
        gbufferFa[kDoomMaterials / 2], shadowVa[kDoomShadowPrograms];
    GLuint mainFbo, gbufferFbo, shadowFbos[kDoomShadowMaps], postFbos[kDoomPostTargets];
    GLuint mainColor, mainDepth, gbuffer[2], gbufferDepth, shadowMaps[kDoomShadowMaps], postColor[kDoomPostTargets];
    GLuint queries[2][kDoomQueries], timestamps[kDoomTimestamps];
    int16_t *mappedShorts;
    uint8_t upload[64 * 64 * 4];
    float uniforms[48 * 4];
    unsigned frame;
    int draws;
} DoomFrame;

static GLuint doom_shader(const DoomGL *gl, GLenum type, const char *source)
{
    GLuint s = gl->CreateShader(type);
    GLint ok = 0;
    gl->ShaderSource(s, 1, &source, NULL);
    gl->CompileShader(s);
    gl->GetShaderiv(s, 0x8B81 /* GL_COMPILE_STATUS */, &ok);
    if (!ok) {
        char log[2048];
        gl->GetShaderInfoLog(s, sizeof(log), NULL, log);
        fprintf(stderr, "doom frame: compile failed: %s\n", log);
        exit(3);
    }
    return s;
}

static GLuint doom_program(const DoomGL *gl, const char *vs, const char *fs, const char *cs)
{
    GLuint p = gl->CreateProgram();
    GLint ok = 0;
    if (vs) gl->AttachShader(p, doom_shader(gl, 0x8B31 /* GL_VERTEX_SHADER */, vs));
    if (fs) gl->AttachShader(p, doom_shader(gl, 0x8B30 /* GL_FRAGMENT_SHADER */, fs));
    if (cs) gl->AttachShader(p, doom_shader(gl, 0x91B9 /* GL_COMPUTE_SHADER */, cs));
    gl->LinkProgram(p);
    gl->GetProgramiv(p, 0x8B82 /* GL_LINK_STATUS */, &ok);
    if (!ok) {
        char log[2048];
        gl->GetProgramInfoLog(p, sizeof(log), NULL, log);
        fprintf(stderr, "doom frame: link failed: %s\n", log);
        exit(4);
    }
    return p;
}

/* A material program: 8 samplers (4 read), a per-view uniform block, vec4
 * arrays; `targets` colour outputs (1, or 2 for the G-buffer pass). */
static GLuint doom_material(const DoomGL *gl, int variant, int targets)
{
    char vs[1024], fs[2048];
    snprintf(vs, sizeof(vs),
             "#version 450 core\n"
             "layout(location = 0) in vec3 position;\n"
             "layout(location = 1) in vec4 attrib;\n"
             "layout(std140, binding = 0) uniform PerView { mat4 viewProjection; vec4 eye; vec4 fog; };\n"
             "uniform vec4 _va_[24];\n"
             "out vec4 v;\nout vec2 uv;\n"
             "void main() {\n"
             "  vec4 p = vec4(dot(vec4(position, 1.0), _va_[0]), dot(vec4(position, 1.0), _va_[1]),\n"
             "                dot(vec4(position, 1.0), _va_[2]), 1.0);\n"
             "  gl_Position = viewProjection * p;\n"
             "  gl_Position.xy = gl_Position.xy * 0.003 + _va_[%d].xy * 0.5;\n"
             "  v = attrib * _va_[%d] * 0.0001 + eye;\n"
             "  uv = position.xy * _va_[3].xy;\n"
             "}\n",
             4 + variant % 20, 4 + variant % 20);
    snprintf(fs, sizeof(fs),
             "#version 450 core\n"
             "in vec4 v;\nin vec2 uv;\n"
             "uniform sampler2D t0, t1, t2, t3, t4, t5, t6, t7;\n"
             "uniform vec4 _fa_[8];\n"
             "layout(std140, binding = 0) uniform PerView { mat4 viewProjection; vec4 eye; vec4 fog; };\n"
             "layout(location = 0) out vec4 o0;\n%s"
             "void main() {\n"
             "  vec4 c = texture(t%d, uv) * texture(t%d, uv * 2.0) + texture(t%d, v.xy) * _fa_[%d] + texture(t%d, v.zw) * fog;\n"
             "  o0 = c;\n%s"
             "}\n",
             targets > 1 ? "layout(location = 1) out vec4 o1;\n" : "", variant % 8, (variant + 1) % 8, (variant + 3) % 8,
             variant % 8, (variant + 5) % 8, targets > 1 ? "  o1 = c * 0.5;\n" : "");
    GLuint p = doom_program(gl, vs, fs, NULL);
    gl->UseProgram(p);
    for (int i = 0; i < 8; ++i) {
        char name[4] = {'t', (char)('0' + i), 0};
        gl->Uniform1i(gl->GetUniformLocation(p, name), i);
    }
    return p;
}

static void doom_target(const DoomGL *gl, GLuint fbo, int colors, const GLuint *color, GLuint depth)
{
    static const GLenum buffers[2] = {0x8CE0, 0x8CE1};  /* GL_COLOR_ATTACHMENT0, 1 */
    gl->BindFramebuffer(0x8D40 /* GL_FRAMEBUFFER */, fbo);
    for (int i = 0; i < colors; ++i)
        gl->FramebufferTexture2D(0x8D40, buffers[i], 0x0DE1 /* GL_TEXTURE_2D */, color[i], 0);
    if (depth) gl->FramebufferTexture2D(0x8D40, 0x8D00 /* GL_DEPTH_ATTACHMENT */, 0x0DE1, depth, 0);
    if (colors) gl->DrawBuffers(colors, buffers);
    if (gl->CheckFramebufferStatus(0x8D40) != 0x8CD5 /* GL_FRAMEBUFFER_COMPLETE */) {
        fprintf(stderr, "doom frame: framebuffer incomplete\n");
        exit(5);
    }
}

static GLuint doom_texture(const DoomGL *gl, GLenum format, int width, int height)
{
    GLuint t;
    gl->GenTextures(1, &t);
    gl->BindTexture(0x0DE1, t);
    gl->TexStorage2D(0x0DE1, 1, format, width, height);
    gl->TexParameteri(0x0DE1, 0x2801 /* MIN_FILTER */, 0x2601 /* LINEAR */);
    return t;
}

static void doom_setup(DoomFrame *d, const DoomGL *gl, int width, int height)
{
    memset(d, 0, sizeof(*d));
    d->gl = gl;
    d->width = width;
    d->height = height;
    /* Geometry: static positions, a persistently mapped short4 stream the
     * frame rewrites, 16-bit indices. */
    {
        float *p = (float *)malloc(sizeof(float) * 3 * kDoomVertices);
        uint16_t *idx = (uint16_t *)malloc(sizeof(uint16_t) * kDoomIndices);
        for (int i = 0; i < 3 * kDoomVertices; ++i) p[i] = (float)((i * 7919) % 2000) / 1000.0f - 1.0f;
        for (int i = 0; i < kDoomIndices; ++i) idx[i] = (uint16_t)((i * 31) % 4096);
        gl->GenVertexArrays(1, &d->vao);
        gl->BindVertexArray(d->vao);
        gl->GenBuffers(1, &d->positions);
        gl->BindBuffer(0x8892 /* ARRAY_BUFFER */, d->positions);
        gl->BufferData(0x8892, (intptr_t)(sizeof(float) * 3 * kDoomVertices), p, 0x88E4 /* STATIC_DRAW */);
        gl->GenBuffers(1, &d->shorts);
        gl->BindBuffer(0x8892, d->shorts);
        /* MAP_WRITE | MAP_PERSISTENT | MAP_COHERENT */
        gl->BufferStorage(0x8892, (intptr_t)(8 * kDoomVertices), NULL, 0x0002 | 0x0040 | 0x0080);
        d->mappedShorts = (int16_t *)gl->MapBufferRange(0x8892, 0, (intptr_t)(8 * kDoomVertices), 0x0002 | 0x0040 | 0x0080);
        gl->GenBuffers(1, &d->indices);
        gl->BindBuffer(0x8893 /* ELEMENT_ARRAY_BUFFER */, d->indices);
        gl->BufferData(0x8893, (intptr_t)(sizeof(uint16_t) * kDoomIndices), idx, 0x88E4);
        gl->EnableVertexAttribArray(0);
        gl->EnableVertexAttribArray(1);
        free(p);
        free(idx);
    }
    gl->GenBuffers(1, &d->ubo);
    gl->BindBuffer(0x8A11 /* UNIFORM_BUFFER */, d->ubo);
    {
        float *u = (float *)calloc(1, kDoomUboBytes);
        for (int i = 0; i < kDoomUboBytes / 4; i += 64) u[i] = u[i + 5] = u[i + 10] = u[i + 15] = 1.0f;
        gl->BufferData(0x8A11, kDoomUboBytes, u, 0x88E8 /* DYNAMIC_DRAW */);
        free(u);
    }
    gl->GenBuffers(1, &d->ssbo);
    gl->BindBuffer(0x90D2 /* SHADER_STORAGE_BUFFER */, d->ssbo);
    gl->BufferData(0x90D2, 1 << 16, NULL, 0x88E8);
    /* Textures and samplers. */
    for (int i = 0; i < kDoomTextures; ++i) d->textures[i] = doom_texture(gl, 0x8058 /* RGBA8 */, 256, 256);
    gl->GenSamplers(8, d->samplers);
    for (int i = 0; i < 8; ++i) {
        gl->SamplerParameteri(d->samplers[i], 0x2801, i & 1 ? 0x2703 /* LINEAR_MIPMAP_LINEAR */ : 0x2601);
        gl->SamplerParameteri(d->samplers[i], 0x2800 /* MAG_FILTER */, 0x2601);
        gl->SamplerParameteri(d->samplers[i], 0x2802 /* WRAP_S */, i & 2 ? 0x812F /* CLAMP_TO_EDGE */ : 0x2901 /* REPEAT */);
    }
    for (int i = 0; i < kDoomTextures; ++i) {  /* a full mip chain for the mipmapped samplers: one level */
        gl->BindTexture(0x0DE1, d->textures[i]);
        gl->TexParameteri(0x0DE1, 0x813D /* MAX_LEVEL */, 0);
    }
    /* Render targets. */
    d->mainColor = doom_texture(gl, 0x881A /* RGBA16F */, width, height);
    d->mainDepth = doom_texture(gl, 0x8CAC /* DEPTH_COMPONENT32F */, width, height);
    d->gbuffer[0] = doom_texture(gl, 0x8058, width, height);
    d->gbuffer[1] = doom_texture(gl, 0x8058, width, height);
    d->gbufferDepth = doom_texture(gl, 0x8CAC, width, height);
    gl->GenFramebuffers(1, &d->mainFbo);
    gl->GenFramebuffers(1, &d->gbufferFbo);
    gl->GenFramebuffers(kDoomShadowMaps, d->shadowFbos);
    gl->GenFramebuffers(kDoomPostTargets, d->postFbos);
    doom_target(gl, d->mainFbo, 1, &d->mainColor, d->mainDepth);
    doom_target(gl, d->gbufferFbo, 2, d->gbuffer, d->gbufferDepth);
    for (int i = 0; i < kDoomShadowMaps; ++i) {
        d->shadowMaps[i] = doom_texture(gl, 0x8CAC, 1024, 1024);
        doom_target(gl, d->shadowFbos[i], 0, NULL, d->shadowMaps[i]);
    }
    for (int i = 0; i < kDoomPostTargets; ++i) {
        d->postColor[i] = doom_texture(gl, 0x8058, width / 2, height / 2);
        doom_target(gl, d->postFbos[i], 1, &d->postColor[i], 0);
    }
    gl->BindFramebuffer(0x8D40, 0);
    /* Programs. */
    for (int i = 0; i < kDoomMaterials; ++i) {
        d->materials[i] = doom_material(gl, i, 1);
        d->materialVa[i] = gl->GetUniformLocation(d->materials[i], "_va_");
        d->materialFa[i] = gl->GetUniformLocation(d->materials[i], "_fa_");
    }
    for (int i = 0; i < kDoomMaterials / 2; ++i) {
        d->gbufferMaterials[i] = doom_material(gl, i + 100, 2);
        d->gbufferVa[i] = gl->GetUniformLocation(d->gbufferMaterials[i], "_va_");
        d->gbufferFa[i] = gl->GetUniformLocation(d->gbufferMaterials[i], "_fa_");
    }
    for (int i = 0; i < kDoomShadowPrograms; ++i) {
        char vs[512];
        snprintf(vs, sizeof(vs),
                 "#version 450 core\nlayout(location = 0) in vec3 position;\nuniform vec4 _va_[8];\n"
                 "void main() { gl_Position = vec4(dot(vec4(position, 1.0), _va_[0]), dot(vec4(position, 1.0), _va_[1]),"
                 " dot(vec4(position, 1.0), _va_[%d]) * 0.5 + 0.5, 1.0); gl_Position.xy *= 0.003; }\n",
                 2 + i % 6);
        d->shadows[i] = doom_program(gl, vs, "#version 450 core\nvoid main() {}\n", NULL);
        d->shadowVa[i] = gl->GetUniformLocation(d->shadows[i], "_va_");
    }
    for (int i = 0; i < kDoomPostPrograms; ++i) {
        char fs[512];
        snprintf(fs, sizeof(fs),
                 "#version 450 core\nuniform sampler2D a, b;\nout vec4 o;\n"
                 "void main() { vec2 uv = gl_FragCoord.xy / vec2(%d.0, %d.0);"
                 " o = texture(a, uv) * %d.0 * 0.1 + texture(b, uv * 0.5); }\n",
                 width / 2, height / 2, i + 1);
        d->posts[i] = doom_program(gl,
                                   "#version 450 core\nvoid main() { vec2 p = vec2((gl_VertexID & 1) * 4 - 1,"
                                   " (gl_VertexID & 2) * 2 - 1); gl_Position = vec4(p, 0.0, 1.0); }\n",
                                   fs, NULL);
        gl->UseProgram(d->posts[i]);
        gl->Uniform1i(gl->GetUniformLocation(d->posts[i], "a"), 0);
        gl->Uniform1i(gl->GetUniformLocation(d->posts[i], "b"), 1);
    }
    d->compute = doom_program(gl, NULL, NULL,
                              "#version 450 core\nlayout(local_size_x = 64) in;\n"
                              "layout(std430, binding = 0) buffer Data { uint data[]; };\n"
                              "void main() { data[gl_GlobalInvocationID.x] += gl_GlobalInvocationID.x; }\n");
    gl->GenQueries(kDoomQueries, d->queries[0]);
    gl->GenQueries(kDoomQueries, d->queries[1]);
    gl->GenQueries(kDoomTimestamps, d->timestamps);
    for (int i = 0; i < 48 * 4; ++i) d->uniforms[i] = 0.01f * (float)(i % 97);
    for (size_t i = 0; i < sizeof(d->upload); ++i) d->upload[i] = (uint8_t)i;
}

/* Whether WMGL_DOOM_SKIP (a comma-separated list) names a part of the frame
 * to leave out: compute, upload, shadow, gbuffer, main, post; shorts reads
 * the second attribute as floats instead. */
static int doom_skip(const char *part)
{
    const char *skip = getenv("WMGL_DOOM_SKIP");
    return skip && strstr(skip, part) != NULL;
}

/* One indexed draw with DOOM's per-draw calls. */
static void doom_draw(DoomFrame *d, GLint va, GLint fa, int shortAttrib)
{
    const DoomGL *gl = d->gl;
    const int n = d->draws++;
    const unsigned count = 300 + (unsigned)(n * 37) % 2700 / 3 * 3;
    const uintptr_t first = (uintptr_t)((n * 2999) % (kDoomIndices - 3000));
    const intptr_t base = (intptr_t)((n * 977) % 60000);
    if (n % 5 != 0) gl->BindBuffer(0x8892, d->positions);
    gl->VertexAttribPointer(0, 3, 0x1406 /* FLOAT */, 0, 12, (const void *)(base * 12));
    if (shortAttrib && !doom_skip("shorts")) {
        gl->BindBuffer(0x8892, d->shorts);
        gl->VertexAttribPointer(1, 4, 0x1402 /* SHORT */, 0, 8, (const void *)(base * 8));
    } else {
        gl->VertexAttribPointer(1, 4, 0x1406, 0, 12, (const void *)(base * 12));
    }
    d->uniforms[0] = (float)n;
    if (va >= 0) gl->Uniform4fv(va, 12, d->uniforms);
    if (fa >= 0 && n % 2 == 0) gl->Uniform4fv(fa, 4, d->uniforms + 16);
    if (n % 3 == 0) gl->BindBufferRange(0x8A11, 0, d->ubo, (intptr_t)((n % 4000) * 256), 256);
    gl->ActiveTexture(0x84C0 /* TEXTURE0 */ + (GLenum)(n % 8));
    gl->BindTexture(0x0DE1, d->textures[(n * 13) % kDoomTextures]);
    gl->BindSampler((GLuint)(n % 8), d->samplers[n % 8]);
    gl->DrawElementsBaseVertex(0x0004 /* TRIANGLES */, (GLsizei)count, 0x1403 /* UNSIGNED_SHORT */,
                               (const void *)(first * 2), (GLint)base);
}

/* One frame into the default framebuffer (the caller presents). */
static void doom_run_frame(DoomFrame *d)
{
    const DoomGL *gl = d->gl;
    const unsigned set = d->frame & 1;
    GLuint ready = 0;
    d->draws = 0;
    /* Last frame's occlusion results, without waiting. */
    if (d->frame) {
        for (int i = 0; i < kDoomQueries; ++i) {
            gl->GetQueryObjectuiv(d->queries[set ^ 1][i], 0x8867 /* QUERY_RESULT_AVAILABLE */, &ready);
            if (ready) gl->GetQueryObjectuiv(d->queries[set ^ 1][i], 0x8866 /* QUERY_RESULT */, &ready);
        }
    }
    gl->QueryCounter(d->timestamps[0], 0x8E28 /* TIMESTAMP */);
    /* Rewrite the dynamic short stream (DOOM's skinned and particle data). */
    for (int i = 0; i < kDoomVertices * 4; i += 4) d->mappedShorts[i] = (int16_t)(i + d->frame);
    /* Compute (culling, lights) and texture streaming. */
    gl->UseProgram(d->compute);
    gl->BindBufferBase(0x90D2, 0, d->ssbo);
    for (int i = 0; i < (doom_skip("compute") ? 0 : 4); ++i) gl->DispatchCompute(16, 1, 1);
    gl->MemoryBarrier(0xFFFFFFFF);
    for (int i = 0; i < (doom_skip("upload") ? 0 : 8); ++i) {
        gl->BindTexture(0x0DE1, d->textures[(d->frame * 8 + (unsigned)i) % kDoomTextures]);
        gl->TexSubImage2D(0x0DE1, 0, 64 * (i % 4), 0, 64, 64, 0x1908 /* RGBA */, 0x1401 /* UNSIGNED_BYTE */, d->upload);
    }
    gl->BindVertexArray(d->vao);
    gl->BindBuffer(0x8893, d->indices);
    gl->BindBufferRange(0x8A11, 0, d->ubo, 0, 256);
    gl->Enable(0x0B71 /* DEPTH_TEST */);
    gl->DepthFunc(0x0203 /* LEQUAL */);
    /* Shadow maps: 4 cascades of 40 depth-only draws. */
    gl->QueryCounter(d->timestamps[1], 0x8E28);
    for (int s = 0; s < (doom_skip("shadow") ? 0 : kDoomShadowMaps); ++s) {
        gl->BindFramebuffer(0x8D40, d->shadowFbos[s]);
        gl->Viewport(0, 0, 1024, 1024);
        gl->Clear(0x0100 /* DEPTH */);
        for (int i = 0; i < 40; ++i) {
            if (i % 5 == 0) gl->UseProgram(d->shadows[(s * 8 + i / 5) % kDoomShadowPrograms]);
            doom_draw(d, d->shadowVa[(s * 8 + i / 5) % kDoomShadowPrograms], -1, 0);
        }
    }
    /* G-buffer: 220 draws, two colour targets. */
    gl->QueryCounter(d->timestamps[2], 0x8E28);
    gl->BindFramebuffer(0x8D40, d->gbufferFbo);
    gl->Viewport(0, 0, d->width, d->height);
    gl->Clear(0x4000 | 0x0100);
    for (int i = 0; i < (doom_skip("gbuffer") ? 0 : 220); ++i) {
        const int p = (i / 5) % (kDoomMaterials / 2);
        if (i % 5 == 0) gl->UseProgram(d->gbufferMaterials[p]);
        doom_draw(d, d->gbufferVa[p], d->gbufferFa[p], i % 3 != 0);
    }
    /* Main forward pass: 260 draws, blending for some, occlusion queries
     * around 40 of them. */
    gl->QueryCounter(d->timestamps[3], 0x8E28);
    gl->BindFramebuffer(0x8D40, d->mainFbo);
    gl->Clear(0x4000 | 0x0100);
    for (int i = 0; i < (doom_skip("main") ? 0 : 260); ++i) {
        const int p = (i / 4) % kDoomMaterials;
        if (i % 4 == 0) gl->UseProgram(d->materials[p]);
        if (i == 180) {
            gl->Enable(0x0BE2 /* BLEND */);
            gl->BlendFunc(1, 1);
            gl->DepthMask(0);
        }
        const int query = i % 6 == 0 && i / 6 < kDoomQueries;
        if (query) gl->BeginQuery(0x8C2F /* ANY_SAMPLES_PASSED */, d->queries[set][i / 6]);
        doom_draw(d, d->materialVa[p], d->materialFa[p], i % 3 != 0);
        if (query) gl->EndQuery(0x8C2F);
    }
    gl->Disable(0x0BE2);
    gl->DepthMask(1);
    gl->Disable(0x0B71);
    /* Post-processing: 40 passes of one full-screen draw (in a quarter of
     * the half-size targets: the CPU's side is what is measured). */
    gl->QueryCounter(d->timestamps[4], 0x8E28);
    gl->Viewport(0, 0, d->width / 4, d->height / 4);
    for (int i = 0; i < (doom_skip("post") ? 0 : 40); ++i) {
        gl->BindFramebuffer(0x8D40, d->postFbos[i % kDoomPostTargets]);
        gl->UseProgram(d->posts[i % kDoomPostPrograms]);
        gl->ActiveTexture(0x84C0);
        gl->BindTexture(0x0DE1, i ? d->postColor[(i + 2) % kDoomPostTargets] : d->mainColor);
        gl->BindSampler(0, d->samplers[0]);
        gl->ActiveTexture(0x84C1);
        gl->BindTexture(0x0DE1, d->gbuffer[i & 1]);
        gl->BindSampler(1, d->samplers[2]);
        gl->DrawArrays(0x0004, 0, 3);
        ++d->draws;
    }
    /* To the window. */
    gl->BindFramebuffer(0x8D40, 0);
    gl->Viewport(0, 0, d->width, d->height);
    gl->UseProgram(d->posts[0]);
    gl->ActiveTexture(0x84C0);
    gl->BindTexture(0x0DE1, d->postColor[(39) % kDoomPostTargets]);
    gl->DrawArrays(0x0004, 0, 3);
    ++d->draws;
    gl->QueryCounter(d->timestamps[5], 0x8E28);
    ++d->frame;
}

#endif
