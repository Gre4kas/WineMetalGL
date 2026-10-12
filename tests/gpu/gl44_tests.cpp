// GL 4.4 features (contexts above 4.3 go through the development override
// until the version is advertised).
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

#include "gpu.h"

// glClearTexImage and glClearTexSubImage (GL_ARB_clear_texture): a whole level
// from float data, a sub-region from bytes, a depth image, and the errors.
GPU_TEST_VERSION(gpu_gl44_clear_texture, 44) {
    GLuint tex[2];
    glGenTextures(2, tex);
    glBindTexture(GL_TEXTURE_2D, tex[0]);
    glTexStorage2D(GL_TEXTURE_2D, 2, GL_RGBA8, 4, 4);
    const float orange[4] = {1.0f, 0.5f, 0.0f, 1.0f};
    glClearTexImage(tex[0], 0, GL_RGBA, GL_FLOAT, orange);
    const uint8_t blue[4] = {0, 0, 255, 255};
    glClearTexSubImage(tex[0], 0, 1, 2, 0, 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, blue);
    glClearTexImage(tex[0], 1, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);  // zeros
    CHECK_GL();
    uint8_t level0[64], level1[16];
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, level0);
    glGetTexImage(GL_TEXTURE_2D, 1, GL_RGBA, GL_UNSIGNED_BYTE, level1);
    for (int i = 0; i < 16; ++i) {
        const bool region = i / 4 == 2 && (i % 4 == 1 || i % 4 == 2);
        const uint8_t* p = &level0[i * 4];
        if (region) {
            CHECK(p[0] == 0 && p[1] == 0 && p[2] == 255 && p[3] == 255);
        } else {
            CHECK(p[0] == 255 && std::abs(p[1] - 128) <= 1 && p[2] == 0 && p[3] == 255);
        }
    }
    for (int i = 0; i < 16; ++i) CHECK_EQ(static_cast<int>(level1[i]), 0);
    glBindTexture(GL_TEXTURE_2D, tex[1]);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_DEPTH_COMPONENT32F, 4, 4);
    const float depth = 0.25f;
    glClearTexImage(tex[1], 0, GL_DEPTH_COMPONENT, GL_FLOAT, &depth);
    CHECK_GL();
    float back[16] = {};
    glGetTexImage(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, GL_FLOAT, back);
    for (float v : back) CHECK(std::fabs(v - 0.25f) < 1e-6f);
    // A colour format for a depth image, and a region past the image.
    glClearTexImage(tex[1], 0, GL_RGBA, GL_FLOAT, orange);
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_OPERATION));
    glClearTexSubImage(tex[0], 0, 3, 3, 0, 2, 2, 1, GL_RGBA, GL_FLOAT, orange);
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_OPERATION));
    glClearTexSubImage(tex[0], 0, 0, 0, 0, -1, 1, 1, GL_RGBA, GL_FLOAT, orange);
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_VALUE));
    glDeleteTextures(2, tex);
}

// GL_ARB_multi_bind: ranges of texture, sampler, buffer and vertex buffer
// bindings, the indexed binding queries, and a range past the last unit.
GPU_TEST_VERSION(gpu_gl44_multi_bind, 44) {
    GLuint tex[2], samplers[2], buffers[3], vao = 0;
    glGenTextures(2, tex);
    glBindTexture(GL_TEXTURE_2D, tex[0]);
    glBindTexture(GL_TEXTURE_3D, tex[1]);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindTexture(GL_TEXTURE_3D, 0);
    glGenSamplers(2, samplers);
    glBindTextures(4, 2, tex);
    glBindSamplers(4, 2, samplers);
    CHECK_GL();
    GLint binding = -1;
    glGetIntegeri_v(GL_TEXTURE_BINDING_2D, 4, &binding);
    CHECK_EQ(binding, static_cast<GLint>(tex[0]));
    glGetIntegeri_v(GL_TEXTURE_BINDING_3D, 5, &binding);
    CHECK_EQ(binding, static_cast<GLint>(tex[1]));
    glGetIntegeri_v(GL_SAMPLER_BINDING, 5, &binding);
    CHECK_EQ(binding, static_cast<GLint>(samplers[1]));
    glBindTextures(4, 2, nullptr);  // unbind both units
    glGetIntegeri_v(GL_TEXTURE_BINDING_2D, 4, &binding);
    CHECK_EQ(binding, 0);
    glGenBuffers(3, buffers);
    for (GLuint b : buffers) {
        glBindBuffer(GL_UNIFORM_BUFFER, b);
        glBufferData(GL_UNIFORM_BUFFER, 512, nullptr, GL_STATIC_DRAW);
    }
    glBindBuffer(GL_UNIFORM_BUFFER, buffers[0]);
    const GLintptr offsets[3] = {0, 256, 0};
    const GLsizeiptr sizes[3] = {64, 128, 256};
    glBindBuffersRange(GL_UNIFORM_BUFFER, 1, 3, buffers, offsets, sizes);
    CHECK_GL();
    GLint64 start = -1;
    glGetIntegeri_v(GL_UNIFORM_BUFFER_BINDING, 2, &binding);
    CHECK_EQ(binding, static_cast<GLint>(buffers[1]));
    glGetInteger64i_v(GL_UNIFORM_BUFFER_START, 2, &start);
    CHECK_EQ(start, static_cast<GLint64>(256));
    glGetIntegerv(GL_UNIFORM_BUFFER_BINDING, &binding);  // the generic binding is unchanged
    CHECK_EQ(binding, static_cast<GLint>(buffers[0]));
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    const GLintptr vOffsets[2] = {16, 32};
    const GLsizei strides[2] = {12, 24};
    glBindVertexBuffers(0, 2, buffers, vOffsets, strides);
    glGetIntegeri_v(GL_VERTEX_BINDING_STRIDE, 1, &binding);
    CHECK_EQ(binding, 24);
    CHECK_GL();
    // A range past the last unit binds nothing.
    glBindTextures(79, 2, tex);
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_OPERATION));
    glDeleteVertexArrays(1, &vao);
    glDeleteBuffers(3, buffers);
    glDeleteSamplers(2, samplers);
    glDeleteTextures(2, tex);
}

// GL_ARB_query_buffer_object: query results written into a buffer at an
// offset, as 32-bit and 64-bit values, and the availability flag.
GPU_TEST_VERSION(gpu_gl44_query_buffer, 44) {
    GLuint p = gpu::program("#version 440 core\nin vec2 position;\nvoid main() { gl_Position = vec4(position, 0.0, 1.0); }\n",
                            "#version 440 core\nout vec4 o;\nvoid main() { o = vec4(1.0); }\n");
    glUseProgram(p);
    GLuint vao = gpu::quad(), query = 0, buffer = 0;
    glGenQueries(1, &query);
    glBeginQuery(GL_SAMPLES_PASSED, query);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glEndQuery(GL_SAMPLES_PASSED);
    glGenBuffers(1, &buffer);
    glBindBuffer(GL_QUERY_BUFFER, buffer);
    const uint32_t fill[6] = {0xdeadbeef, 0xdeadbeef, 0xdeadbeef, 0xdeadbeef, 0xdeadbeef, 0xdeadbeef};
    glBufferData(GL_QUERY_BUFFER, sizeof(fill), fill, GL_DYNAMIC_READ);
    glGetQueryObjectuiv(query, GL_QUERY_RESULT, reinterpret_cast<GLuint*>(4));
    glGetQueryObjectui64v(query, GL_QUERY_RESULT, reinterpret_cast<GLuint64*>(8));
    glGetQueryObjectuiv(query, GL_QUERY_RESULT_AVAILABLE, reinterpret_cast<GLuint*>(16));
    CHECK_GL();
    GLint bound = 0;
    glGetIntegerv(GL_QUERY_BUFFER_BINDING, &bound);
    CHECK_EQ(bound, static_cast<GLint>(buffer));
    uint32_t back[6] = {};
    glGetBufferSubData(GL_QUERY_BUFFER, 0, sizeof(back), back);
    CHECK_EQ(back[0], 0xdeadbeefu);
    CHECK_EQ(back[1], 64u * 64u);
    CHECK_EQ(back[2], 64u * 64u);
    CHECK_EQ(back[3], 0u);
    CHECK_EQ(back[4], 1u);
    // An offset past the buffer is INVALID_OPERATION.
    glGetQueryObjectuiv(query, GL_QUERY_RESULT, reinterpret_cast<GLuint*>(24));
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_OPERATION));
    glBindBuffer(GL_QUERY_BUFFER, 0);
    glDeleteBuffers(1, &buffer);
    glDeleteQueries(1, &query);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
}

// GL_ARB_texture_stencil8: a STENCIL_INDEX8 texture uploaded, read back and
// sampled as unsigned integers; GL_MIRROR_CLAMP_TO_EDGE is a wrap mode.
GPU_TEST_VERSION(gpu_gl44_stencil8_texture, 44) {
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    const uint8_t stencil[4] = {7, 42, 200, 255};
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_STENCIL_INDEX8, 2, 2, 0, GL_STENCIL_INDEX, GL_UNSIGNED_BYTE, stencil);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_MIRROR_CLAMP_TO_EDGE);
    CHECK_GL();
    uint8_t back[4] = {};
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_STENCIL_INDEX, GL_UNSIGNED_BYTE, back);
    CHECK_GL();
    CHECK(std::memcmp(back, stencil, 4) == 0);
    GLuint p = gpu::program("#version 440 core\nin vec2 position;\nvoid main() { gl_Position = vec4(position, 0.0, 1.0); }\n",
                            "#version 440 core\nuniform usampler2D s;\nout vec4 o;\n"
                            "void main() { o = vec4(float(texelFetch(s, ivec2(1, 0), 0).r) / 255.0, 0.0, 0.0, 1.0); }\n");
    glUseProgram(p);
    GLuint vao = gpu::quad();
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    CHECK_GL();
    const uint8_t want[4] = {42, 0, 0, 255};
    CHECK(gpu::near(gpu::readRGBA8(32, 32, 1, 1).data(), want));
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
    glDeleteTextures(1, &tex);
}

// GL_ARB_enhanced_layouts: transform feedback declared in the shader (two
// buffers, an explicit stride with a gap) without glTransformFeedbackVaryings,
// and layout(offset) in a uniform block.
GPU_TEST_VERSION(gpu_gl44_enhanced_layouts, 44) {
    const char* vs = R"(#version 440 core
layout(xfb_buffer = 0, xfb_offset = 0) out vec4 a;
layout(xfb_buffer = 0, xfb_offset = 16) out float b;
layout(xfb_buffer = 1, xfb_stride = 16, xfb_offset = 4) out vec2 c;
layout(std140) uniform Block { layout(offset = 32) vec4 shift; };
void main() {
    a = vec4(gl_VertexID, 1.0, 2.0, 3.0) + shift;
    b = 10.0 + float(gl_VertexID);
    c = vec2(20.0, 21.0) + float(gl_VertexID);
    gl_Position = vec4(0.0);
})";
    const char* fs = "#version 440 core\nout vec4 o;\nvoid main() { o = vec4(1.0); }\n";
    GLuint p = gpu::program(vs, fs);
    glUseProgram(p);
    GLuint block = glGetUniformBlockIndex(p, "Block");
    GLint dataSize = 0;
    glGetActiveUniformBlockiv(p, block, GL_UNIFORM_BLOCK_DATA_SIZE, &dataSize);
    CHECK_EQ(dataSize, 48);
    float ubo[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0.5f, 0.5f, 0.5f, 0.5f};
    GLuint buffers[3], vao = 0;
    glGenBuffers(3, buffers);
    glBindBufferBase(GL_UNIFORM_BUFFER, 0, buffers[2]);
    glBufferData(GL_UNIFORM_BUFFER, sizeof(ubo), ubo, GL_STATIC_DRAW);
    glUniformBlockBinding(p, block, 0);
    std::vector<float> zeros(64, -1.0f);
    for (int i = 0; i < 2; ++i) {
        glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, static_cast<GLuint>(i), buffers[i]);
        glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, 64 * sizeof(float), zeros.data(), GL_DYNAMIC_READ);
    }
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glEnable(GL_RASTERIZER_DISCARD);
    glBeginTransformFeedback(GL_POINTS);
    glDrawArrays(GL_POINTS, 0, 2);
    glEndTransformFeedback();
    glDisable(GL_RASTERIZER_DISCARD);
    CHECK_GL();
    float out0[10] = {}, out1[8] = {};
    glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, buffers[0]);
    glGetBufferSubData(GL_TRANSFORM_FEEDBACK_BUFFER, 0, sizeof(out0), out0);
    glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, buffers[1]);
    glGetBufferSubData(GL_TRANSFORM_FEEDBACK_BUFFER, 0, sizeof(out1), out1);
    // Buffer 0: stride 20 bytes (a, b); buffer 1: stride 16 bytes, c at byte 4.
    const float want0[10] = {0.5f, 1.5f, 2.5f, 3.5f, 10.0f, 1.5f, 1.5f, 2.5f, 3.5f, 11.0f};
    for (int i = 0; i < 10; ++i) CHECK(std::fabs(out0[i] - want0[i]) < 1e-6f);
    const float want1[8] = {-1.0f, 20.0f, 21.0f, -1.0f, -1.0f, 21.0f, 22.0f, -1.0f};
    for (int i = 0; i < 8; ++i) {
        if (std::fabs(out1[i] - want1[i]) >= 1e-6f) std::fprintf(stderr, "  buffer 1 word %d: %g, expected %g\n", i, out1[i], want1[i]);
        CHECK(std::fabs(out1[i] - want1[i]) < 1e-6f);
    }
    glDeleteVertexArrays(1, &vao);
    glDeleteBuffers(3, buffers);
    glDeleteProgram(p);
}

// GL 4.5 direct state access: a frame set up without binding anything to
// edit it (buffers, a texture, a framebuffer and a vertex array), drawn into
// the framebuffer and read back through the texture.
GPU_TEST_VERSION(gpu_gl45_direct_state_access, 45) {
    const float quad[8] = {-1, -1, 1, -1, -1, 1, 1, 1};
    GLuint vbo = 0, tex = 0, src = 0, fbo = 0, vao = 0;
    glCreateBuffers(1, &vbo);
    glNamedBufferData(vbo, sizeof(quad), nullptr, GL_STATIC_DRAW);
    glNamedBufferSubData(vbo, 0, sizeof(quad), quad);
    float back[8] = {};
    glGetNamedBufferSubData(vbo, 0, sizeof(back), back);
    CHECK(std::memcmp(back, quad, sizeof(quad)) == 0);
    glCreateTextures(GL_TEXTURE_2D, 1, &tex);
    glTextureStorage2D(tex, 1, GL_RGBA8, 4, 4);
    glCreateTextures(GL_TEXTURE_2D, 1, &src);
    glTextureStorage2D(src, 1, GL_RGBA8, 1, 1);
    const uint8_t green[4] = {0, 255, 0, 255};
    glTextureSubImage2D(src, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, green);
    glTextureParameteri(src, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glCreateFramebuffers(1, &fbo);
    glNamedFramebufferTexture(fbo, GL_COLOR_ATTACHMENT0, tex, 0);
    CHECK_EQ(glCheckNamedFramebufferStatus(fbo, GL_FRAMEBUFFER), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE));
    const float red[4] = {1, 0, 0, 1};
    glClearNamedFramebufferfv(fbo, GL_COLOR, 0, red);
    glCreateVertexArrays(1, &vao);
    glVertexArrayVertexBuffer(vao, 0, vbo, 0, 8);
    glVertexArrayAttribFormat(vao, 0, 2, GL_FLOAT, GL_FALSE, 0);
    glVertexArrayAttribBinding(vao, 0, 0);
    glEnableVertexArrayAttrib(vao, 0);
    GLuint p = gpu::program("#version 450 core\nlayout(location = 0) in vec2 position;\n"
                            "void main() { gl_Position = vec4(position * 0.5 - 0.5, 0.0, 1.0); }\n",
                            "#version 450 core\nlayout(binding = 3) uniform sampler2D s;\nout vec4 o;\n"
                            "void main() { o = texture(s, vec2(0.5)); }\n");
    glBindTextureUnit(3, src);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, 4, 4);
    glUseProgram(p);
    glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);  // the lower-left quarter
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, 64, 64);
    CHECK_GL();
    uint8_t pixels[64] = {};
    glGetTextureImage(tex, 0, GL_RGBA, GL_UNSIGNED_BYTE, sizeof(pixels), pixels);
    CHECK_GL();
    CHECK(pixels[0] == 0 && pixels[1] == 255);          // (0, 0): drawn
    CHECK(pixels[15 * 4] == 255 && pixels[15 * 4 + 1] == 0);  // (3, 3): cleared
    // A name that is not an object is INVALID_OPERATION.
    glNamedBufferData(4321, 4, nullptr, GL_STATIC_DRAW);
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_OPERATION));
    glDeleteProgram(p);
    glDeleteVertexArrays(1, &vao);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &src);
    glDeleteTextures(1, &tex);
    glDeleteBuffers(1, &vbo);
}

// GL 4.5 cull distances (GL_ARB_cull_distance): a primitive is dropped only
// when one distance is negative at every vertex; a negative vertex alone
// leaves it whole (a clip distance would clip it). Triangles, lines and
// points, then the triangles again through a passthrough geometry shader.
GPU_TEST_VERSION(gpu_gl45_cull_distance, 45) {
    const char* vs = R"(#version 450 core
layout(location = 0) in vec2 pos;
layout(location = 1) in vec2 cull;
out float gl_CullDistance[2];
void main() {
    gl_CullDistance[0] = cull.x;
    gl_CullDistance[1] = cull.y;
    gl_Position = vec4(pos, 0.0, 1.0);
})";
    const char* gs = R"(#version 450 core
layout(triangles) in;
layout(triangle_strip, max_vertices = 3) out;
out float gl_CullDistance[2];
void main() {
    for (int i = 0; i < 3; ++i) {
        gl_Position = gl_in[i].gl_Position;
        gl_CullDistance[0] = gl_in[i].gl_CullDistance[0];
        gl_CullDistance[1] = gl_in[i].gl_CullDistance[1];
        EmitVertex();
    }
})";
    const char* fs = "#version 450 core\nout vec4 o;\nvoid main() { o = vec4(1.0); }\n";
    GLuint p = gpu::program(vs, fs);
    GLuint withGs = glCreateProgram();
    {
        const GLenum types[3] = {GL_VERTEX_SHADER, GL_GEOMETRY_SHADER, GL_FRAGMENT_SHADER};
        const char* sources[3] = {vs, gs, fs};
        for (int i = 0; i < 3; ++i) {
            GLuint s = glCreateShader(types[i]);
            glShaderSource(s, 1, &sources[i], nullptr);
            glCompileShader(s);
            glAttachShader(withGs, s);
            glDeleteShader(s);
        }
        glLinkProgram(withGs);
        GLint linked = 0;
        glGetProgramiv(withGs, GL_LINK_STATUS, &linked);
        CHECK(linked);
    }
    GLint limit = 0;
    glGetIntegerv(GL_MAX_CULL_DISTANCES, &limit);
    CHECK_EQ(limit, 8);
    glGetIntegerv(GL_MAX_COMBINED_CLIP_AND_CULL_DISTANCES, &limit);
    CHECK_EQ(limit, 8);
    // x, y, cull0, cull1 per vertex. Pixel (px, py) has its centre at (px + 0.5) / 32 - 1.
    const float tris[] = {
        // A, bottom left: distance 0 negative at vertex 0 only -> drawn whole.
        -1, -1, -1, 1, 0, -1, 1, 1, -1, 0, 1, 1,
        // B, bottom right: distance 1 negative everywhere -> culled.
        0, -1, 5, -1, 1, -1, 5, -2, 0, 0, 5, -0.5f,
        // C, top left: each distance negative somewhere, neither everywhere -> drawn.
        -1, 0, -1, 1, 0, 0, -1, -1, -1, 1, 1, -1,
    };
    const float lines[] = {
        // Row 40: one end negative -> drawn. Row 44: distance 0 negative at both ends -> culled.
        0.0f, 40.5f / 32 - 1, -1, 1, 1.0f, 40.5f / 32 - 1, 1, 1,
        0.0f, 44.5f / 32 - 1, -1, 1, 1.0f, 44.5f / 32 - 1, -2, 1,
    };
    const float points[] = {
        48.5f / 32 - 1, 56.5f / 32 - 1, 1, -1,  // culled
        56.5f / 32 - 1, 56.5f / 32 - 1, 1, 1,   // drawn
    };
    GLuint vao = 0, vbo = 0;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    const auto draw = [&](GLuint program, GLenum mode, const float* data, size_t bytes) {
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(bytes), data, GL_STATIC_DRAW);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, nullptr);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, reinterpret_cast<const void*>(8));
        glUseProgram(program);
        glDrawArrays(mode, 0, static_cast<GLsizei>(bytes / 16));
    };
    const uint8_t white[4] = {255, 255, 255, 255}, black[4] = {0, 0, 0, 0};
    for (GLuint program : {p, withGs}) {
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        draw(program, GL_TRIANGLES, tris, sizeof(tris));
        if (program == p) {
            draw(p, GL_LINES, lines, sizeof(lines));
            draw(p, GL_POINTS, points, sizeof(points));
        }
        CHECK_GL();
        const std::vector<uint8_t> px = gpu::readRGBA8(0, 0, 64, 64);
        const auto at = [&](int x, int y) { return &px[static_cast<size_t>(y * 64 + x) * 4]; };
        CHECK(gpu::near(at(1, 1), white));   // A, next to its negative vertex
        CHECK(gpu::near(at(20, 8), white));  // A
        CHECK(gpu::near(at(34, 2), black));  // B
        CHECK(gpu::near(at(2, 34), white));  // C
        CHECK(gpu::near(at(20, 40), white));  // C
        if (program == p) {
            CHECK(gpu::near(at(33, 40), white));  // line, next to its negative end
            CHECK(gpu::near(at(48, 40), white));
            CHECK(gpu::near(at(48, 44), black));
            CHECK(gpu::near(at(48, 56), black));
            CHECK(gpu::near(at(56, 56), white));
        }
    }
    glDeleteBuffers(1, &vbo);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
    glDeleteProgram(withGs);
}

// GL 4.5 glGetTextureSubImage: a region of a 2D array (two layers) and of a
// cube map (faces as z), its errors, and glGetCompressedTextureSubImage's
// block alignment.
GPU_TEST_VERSION(gpu_gl45_get_texture_sub_image, 45) {
    GLuint tex[3];
    glCreateTextures(GL_TEXTURE_2D_ARRAY, 1, &tex[0]);
    glTextureStorage3D(tex[0], 1, GL_RGBA8, 4, 4, 3);
    std::vector<uint8_t> texels(4 * 4 * 3 * 4);
    for (size_t i = 0; i < texels.size(); ++i) texels[i] = static_cast<uint8_t>(i);
    glTextureSubImage3D(tex[0], 0, 0, 0, 0, 4, 4, 3, GL_RGBA, GL_UNSIGNED_BYTE, texels.data());
    uint8_t region[2 * 2 * 2 * 4] = {};
    glGetTextureSubImage(tex[0], 0, 1, 2, 1, 2, 2, 2, GL_RGBA, GL_UNSIGNED_BYTE, sizeof(region), region);
    CHECK_GL();
    for (int z = 0; z < 2; ++z)
        for (int y = 0; y < 2; ++y)
            for (int x = 0; x < 2; ++x)
                for (int c = 0; c < 4; ++c) {
                    const size_t src = (((static_cast<size_t>(1 + z) * 4 + 2 + y) * 4) + 1 + x) * 4 + c;
                    CHECK_EQ(static_cast<int>(region[((z * 2 + y) * 2 + x) * 4 + c]), static_cast<int>(texels[src]));
                }
    // Past the image, a negative offset, and a buffer too small.
    glGetTextureSubImage(tex[0], 0, 3, 0, 0, 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, sizeof(region), region);
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_VALUE));
    glGetTextureSubImage(tex[0], 0, -1, 0, 0, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, sizeof(region), region);
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_VALUE));
    glGetTextureSubImage(tex[0], 0, 0, 0, 0, 2, 2, 2, GL_RGBA, GL_UNSIGNED_BYTE, 4, region);
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_OPERATION));
    // Cube map faces 2 and 3 (one texel each).
    glCreateTextures(GL_TEXTURE_CUBE_MAP, 1, &tex[1]);
    glTextureStorage2D(tex[1], 1, GL_RGBA8, 2, 2);
    for (int f = 0; f < 6; ++f) {
        const uint8_t face[16] = {static_cast<uint8_t>(f), 0, 0, 255, static_cast<uint8_t>(f), 0, 0, 255,
                                  static_cast<uint8_t>(f), 0, 0, 255, static_cast<uint8_t>(f), 0, 0, 255};
        glTextureSubImage3D(tex[1], 0, 0, 0, f, 2, 2, 1, GL_RGBA, GL_UNSIGNED_BYTE, face);
    }
    uint8_t faces[8] = {};
    glGetTextureSubImage(tex[1], 0, 1, 1, 2, 1, 1, 2, GL_RGBA, GL_UNSIGNED_BYTE, sizeof(faces), faces);
    CHECK_GL();
    CHECK_EQ(static_cast<int>(faces[0]), 2);
    CHECK_EQ(static_cast<int>(faces[4]), 3);
    // Compressed: whole 4x4 blocks only.
    glCreateTextures(GL_TEXTURE_2D, 1, &tex[2]);
    glTextureStorage2D(tex[2], 1, GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, 8, 8);
    uint8_t blocks[64] = {};
    glGetCompressedTextureSubImage(tex[2], 0, 4, 0, 0, 4, 8, 1, sizeof(blocks), blocks);
    CHECK_GL();
    glGetCompressedTextureSubImage(tex[2], 0, 2, 0, 0, 4, 4, 1, sizeof(blocks), blocks);
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_OPERATION));
    glDeleteTextures(3, tex);
}

// GL 4.5 conditional rendering inverted: a draw runs only if the query
// passed no samples; and GL 4.5 range rules for a mapped buffer's sub-data.
GPU_TEST_VERSION(gpu_gl45_conditional_render_inverted, 45) {
    GLuint p = gpu::program("#version 450 core\nlayout(location = 0) in vec2 pos;\nvoid main() { gl_Position = vec4(pos, 0.0, 1.0); }\n",
                            "#version 450 core\nout vec4 o;\nvoid main() { o = vec4(1.0); }\n");
    GLuint vao = gpu::quad();
    glUseProgram(p);
    GLuint queries[2];
    glGenQueries(2, queries);
    glBeginQuery(GL_ANY_SAMPLES_PASSED, queries[0]);  // passes samples
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glEndQuery(GL_ANY_SAMPLES_PASSED);
    glBeginQuery(GL_ANY_SAMPLES_PASSED, queries[1]);  // passes none
    glEndQuery(GL_ANY_SAMPLES_PASSED);
    const uint8_t white[4] = {255, 255, 255, 255}, black[4] = {0, 0, 0, 0};
    for (int i = 0; i < 2; ++i) {
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        glBeginConditionalRender(queries[i], GL_QUERY_WAIT_INVERTED);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glEndConditionalRender();
        CHECK_GL();
        const std::vector<uint8_t> px = gpu::readRGBA8(8, 8, 1, 1);
        CHECK(gpu::near(px.data(), i == 0 ? black : white));
    }
    GLuint buffer = 0;
    glGenBuffers(1, &buffer);
    glBindBuffer(GL_ARRAY_BUFFER, buffer);
    glBufferData(GL_ARRAY_BUFFER, 64, nullptr, GL_DYNAMIC_DRAW);
    glMapBufferRange(GL_ARRAY_BUFFER, 16, 16, GL_MAP_WRITE_BIT);
    const uint8_t bytes[16] = {};
    glBufferSubData(GL_ARRAY_BUFFER, 32, 16, bytes);  // outside the mapping
    CHECK_GL();
    glBufferSubData(GL_ARRAY_BUFFER, 24, 16, bytes);  // across it
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_OPERATION));
    glUnmapBuffer(GL_ARRAY_BUFFER);
    glDeleteBuffers(1, &buffer);
    glDeleteQueries(2, queries);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
}

// Sampler border colours Metal does not have, and integer, shadow and
// rectangle textures' borders (clamp-to-border emulated in the shader):
// nearest and linear samples, a gather and depth comparisons, from a
// compute shader.
GPU_TEST_VERSION(gpu_gl44_border_colors, 44) {
    const char* cs = R"(#version 450 core
layout(local_size_x = 1) in;
layout(binding = 0) uniform sampler2D colorTex;
layout(binding = 1) uniform isampler2D intTex;
layout(binding = 2) uniform sampler2D linearTex;
layout(binding = 3) uniform sampler2DShadow depthTex;
layout(binding = 4) uniform sampler2DRect rectTex;
layout(std430, binding = 0) buffer Out { vec4 f[6]; ivec4 i[2]; };
void main() {
    f[0] = textureLod(colorTex, vec2(-0.5, 0.5), 0.0);
    f[1] = textureLod(colorTex, vec2(0.25, 0.25), 0.0);
    f[2] = textureLod(linearTex, vec2(0.0, 0.5), 0.0);
    f[3] = vec4(texture(depthTex, vec3(-0.5, 0.5, 0.25)));
    f[4] = vec4(texture(depthTex, vec3(-0.5, 0.5, 0.75)));
    f[5] = texture(rectTex, vec2(-1.0, 0.5));
    i[0] = texture(intTex, vec2(1.5, 0.5));
    i[1] = textureGather(intTex, vec2(0.0, 0.5), 0);
})";
    GLuint shader = glCreateShader(GL_COMPUTE_SHADER);
    glShaderSource(shader, 1, &cs, nullptr);
    glCompileShader(shader);
    GLuint p = glCreateProgram();
    glAttachShader(p, shader);
    glLinkProgram(p);
    glDeleteShader(shader);
    GLint linked = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &linked);
    CHECK(linked);
    GLuint tex[5];
    glGenTextures(5, tex);
    const auto setup = [&](int unit, GLenum target, GLenum filter) {
        glActiveTexture(GL_TEXTURE0 + unit);
        glBindTexture(target, tex[unit]);
        glTexParameteri(target, GL_TEXTURE_MIN_FILTER, filter);
        glTexParameteri(target, GL_TEXTURE_MAG_FILTER, filter);
        glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
        glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    };
    const uint8_t rgba[16] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255};
    const uint8_t white[16] = {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255};
    setup(0, GL_TEXTURE_2D, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    const float color[4] = {0.25f, 0.5f, 0.75f, 1.0f};
    glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, color);
    setup(1, GL_TEXTURE_2D, GL_NEAREST);
    int32_t ints[16] = {};
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 2; ++x) ints[(y * 2 + x) * 4] = 10 * y + x + 1;
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32I, 2, 2, 0, GL_RGBA_INTEGER, GL_INT, ints);
    const GLint intBorder[4] = {-5, 7, -9, 11};
    glTexParameterIiv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, intBorder);
    setup(2, GL_TEXTURE_2D, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
    const float grey[4] = {0.5f, 0.5f, 0.5f, 0.5f};
    glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, grey);
    setup(3, GL_TEXTURE_2D, GL_NEAREST);
    const float depths[4] = {0.9f, 0.9f, 0.9f, 0.9f};
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, 2, 2, 0, GL_DEPTH_COMPONENT, GL_FLOAT, depths);
    const float depthBorder[4] = {0.5f, 0.0f, 0.0f, 0.0f};
    glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, depthBorder);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
    setup(4, GL_TEXTURE_RECTANGLE, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_RECTANGLE, 0, GL_RGBA8, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
    CHECK_GL();
    GLuint buffer = 0;
    glGenBuffers(1, &buffer);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, buffer);
    std::vector<float> init(32, -1.0f);
    glBufferData(GL_SHADER_STORAGE_BUFFER, init.size() * 4, init.data(), GL_DYNAMIC_READ);
    glUseProgram(p);
    glDispatchCompute(1, 1, 1);
    glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
    CHECK_GL();
    float out[32];
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(out), out);
    const auto close = [](const float* got, std::initializer_list<float> want) {
        int k = 0;
        bool ok = true;
        for (float w : want) ok = ok && std::fabs(got[k++] - w) < 0.02f;
        if (!ok) std::fprintf(stderr, "  got (%g, %g, %g, %g)\n", got[0], got[1], got[2], got[3]);
        return ok;
    };
    CHECK(close(&out[0], {0.25f, 0.5f, 0.75f, 1.0f}));  // nearest, outside
    CHECK(close(&out[4], {1.0f, 0.0f, 0.0f, 1.0f}));    // nearest, inside
    CHECK(close(&out[8], {0.75f, 0.75f, 0.75f, 0.75f}));  // linear, half outside
    CHECK(close(&out[12], {1.0f, 1.0f, 1.0f, 1.0f}));   // 0.25 <= border depth 0.5
    CHECK(close(&out[16], {0.0f, 0.0f, 0.0f, 0.0f}));   // 0.75 > 0.5
    CHECK(close(&out[20], {0.0f, 0.0f, 0.0f, 0.0f}));   // rectangle, outside
    int32_t is[8];
    std::memcpy(is, &out[24], sizeof(is));
    CHECK(is[0] == -5 && is[1] == 7 && is[2] == -9 && is[3] == 11);
    CHECK(is[4] == -5 && is[5] == 11 && is[6] == 1 && is[7] == -5);
    if (!(is[4] == -5 && is[5] == 11 && is[6] == 1 && is[7] == -5))
        std::fprintf(stderr, "  gather (%d, %d, %d, %d)\n", is[4], is[5], is[6], is[7]);
    glDeleteBuffers(1, &buffer);
    glDeleteTextures(5, tex);
    glDeleteProgram(p);
}

// glClearTexSubImage of a multisample texture: the whole image, then a
// region, read back through a resolving blit.
GPU_TEST_VERSION(gpu_gl44_clear_texture_multisample, 44) {
    GLuint tex[2], fbo[2];
    glGenTextures(2, tex);
    glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, tex[0]);
    glTexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, 4, GL_RGBA8, 4, 4, GL_TRUE);
    glBindTexture(GL_TEXTURE_2D, tex[1]);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 4, 4);
    const uint8_t red[4] = {255, 0, 0, 255}, blue[4] = {0, 0, 255, 255};
    glClearTexImage(tex[0], 0, GL_RGBA, GL_UNSIGNED_BYTE, red);
    glClearTexSubImage(tex[0], 0, 1, 1, 0, 2, 2, 1, GL_RGBA, GL_UNSIGNED_BYTE, blue);
    CHECK_GL();
    glGenFramebuffers(2, fbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo[0]);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D_MULTISAMPLE, tex[0], 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo[1]);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex[1], 0);
    glBlitFramebuffer(0, 0, 4, 4, 0, 0, 4, 4, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    CHECK_GL();
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo[1]);
    const std::vector<uint8_t> px = gpu::readRGBA8(0, 0, 4, 4);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) {
            const bool inside = x >= 1 && x <= 2 && y >= 1 && y <= 2;
            CHECK(gpu::near(&px[static_cast<size_t>(y * 4 + x) * 4], inside ? blue : red));
        }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(2, fbo);
    glDeleteTextures(2, tex);
}

// GL 4.4 vertex inputs that alias one location (whole or by component):
// each reads its components of the same attribute.
GPU_TEST_VERSION(gpu_gl44_vertex_input_aliasing, 44) {
    const char* vs = R"(#version 440 core
layout(location = 0) in vec2 pos;
layout(location = 1, component = 0) in float a;
layout(location = 1, component = 0) in float b;
layout(location = 1, component = 2) in vec2 zw;
flat out vec4 v;
void main() {
    v = vec4(a, b, zw);
    gl_Position = vec4(pos, 0.0, 1.0);
})";
    const char* fs = "#version 440 core\nflat in vec4 v;\nout vec4 o;\nvoid main() { o = v; }\n";
    GLuint p = gpu::program(vs, fs);
    GLuint vao = gpu::quad();
    const float values[4] = {0.25f, 0.5f, 0.75f, 1.0f};
    glVertexAttrib4fv(1, values);
    glUseProgram(p);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    CHECK_GL();
    const std::vector<uint8_t> px = gpu::readRGBA8(8, 8, 1, 1);
    const uint8_t want[4] = {64, 64, 191, 255};
    if (!gpu::near(px.data(), want, 2)) std::fprintf(stderr, "  got (%d, %d, %d, %d)\n", px[0], px[1], px[2], px[3]);
    CHECK(gpu::near(px.data(), want, 2));
    // The same from a vertex buffer.
    GLuint vbo = 0;
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    const float array[16] = {0.25f, 0.5f, 0.75f, 1.0f, 0.25f, 0.5f, 0.75f, 1.0f,
                             0.25f, 0.5f, 0.75f, 1.0f, 0.25f, 0.5f, 0.75f, 1.0f};
    glBufferData(GL_ARRAY_BUFFER, sizeof(array), array, GL_STATIC_DRAW);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 0, nullptr);
    glEnableVertexAttribArray(1);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    CHECK_GL();
    const std::vector<uint8_t> fromArray = gpu::readRGBA8(8, 8, 1, 1);
    if (!gpu::near(fromArray.data(), want, 2))
        std::fprintf(stderr, "  array: got (%d, %d, %d, %d)\n", fromArray[0], fromArray[1], fromArray[2], fromArray[3]);
    CHECK(gpu::near(fromArray.data(), want, 2));
    glDeleteBuffers(1, &vbo);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
}

// GL 4.4 enhanced layouts: an array of double matrices captured by
// xfb_offset (each element's columns in order, low word first).
GPU_TEST_VERSION(gpu_gl44_xfb_double_matrices, 44) {
    const char* vs = R"(#version 440 core
layout(std140, binding = 0) uniform Values { dmat2 values[3]; };
layout(location = 0, xfb_offset = 0) flat out dmat2 goku[3];
void main() {
    goku = values;
    gl_Position = vec4(0.0);
})";
    const char* fs = "#version 440 core\nout vec4 o;\nvoid main() { o = vec4(1.0); }\n";
    GLuint p = gpu::program(vs, fs);
    GLuint buffers[2], vao = 0;
    glGenBuffers(2, buffers);
    // std140: each dmat2 column is a dvec2 at a 16-byte stride (no padding to 32).
    double ubo[12];
    for (int i = 0; i < 12; ++i) ubo[i] = 1.5 + i;
    glBindBufferBase(GL_UNIFORM_BUFFER, 0, buffers[0]);
    glBufferData(GL_UNIFORM_BUFFER, sizeof(ubo), ubo, GL_STATIC_DRAW);
    glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, buffers[1]);
    std::vector<double> init(12, -1.0);
    glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, init.size() * 8, init.data(), GL_DYNAMIC_READ);
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glUseProgram(p);
    glEnable(GL_RASTERIZER_DISCARD);
    glBeginTransformFeedback(GL_POINTS);
    glDrawArrays(GL_POINTS, 0, 1);
    glEndTransformFeedback();
    glDisable(GL_RASTERIZER_DISCARD);
    CHECK_GL();
    double out[12] = {};
    glGetBufferSubData(GL_TRANSFORM_FEEDBACK_BUFFER, 0, sizeof(out), out);
    for (int i = 0; i < 12; ++i) {
        if (out[i] != ubo[i]) std::fprintf(stderr, "  element %d: %g, expected %g\n", i, out[i], ubo[i]);
        CHECK(out[i] == ubo[i]);
    }
    glDeleteVertexArrays(1, &vao);
    glDeleteBuffers(2, buffers);
    glDeleteProgram(p);
}

// GL_AMD_pinned_memory: page-aligned application memory as a buffer's
// storage (Wine maps WoW64 guests' buffers this way): the GPU reads what
// the application writes there, and the buffer reads back through it.
GPU_TEST_VERSION(gpu_gl44_amd_pinned_memory, 44) {
    const size_t page = static_cast<size_t>(getpagesize());
    void* memory = nullptr;
    CHECK(posix_memalign(&memory, page, page) == 0);
    auto* values = static_cast<float*>(memory);
    for (int i = 0; i < 4; ++i) values[i] = 0.25f * static_cast<float>(i + 1);
    GLuint buffer = 0;
    glGenBuffers(1, &buffer);
    glBindBuffer(GL_EXTERNAL_VIRTUAL_MEMORY_BUFFER_AMD, buffer);
    glBufferData(GL_EXTERNAL_VIRTUAL_MEMORY_BUFFER_AMD, 16, memory, GL_DYNAMIC_COPY);
    CHECK_GL();
    // Unaligned memory is refused.
    GLuint other = 0;
    glGenBuffers(1, &other);
    glBindBuffer(GL_EXTERNAL_VIRTUAL_MEMORY_BUFFER_AMD, other);
    glBufferData(GL_EXTERNAL_VIRTUAL_MEMORY_BUFFER_AMD, 16, static_cast<char*>(memory) + 4, GL_DYNAMIC_COPY);
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_OPERATION));
    // The pinned buffer as a uniform buffer.
    GLuint p = gpu::program("#version 440 core\nlayout(location = 0) in vec2 pos;\nvoid main() { gl_Position = vec4(pos, 0.0, 1.0); }\n",
                            "#version 440 core\nlayout(std140, binding = 3) uniform U { vec4 color; };\nout vec4 o;\n"
                            "void main() { o = color; }\n");
    GLuint vao = gpu::quad();
    glBindBufferBase(GL_UNIFORM_BUFFER, 3, buffer);
    glUseProgram(p);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    CHECK_GL();
    const uint8_t want[4] = {64, 128, 191, 255};
    CHECK(gpu::near(gpu::readRGBA8(4, 4, 1, 1).data(), want, 2));
    // A write through the GL lands in the application's memory.
    const float zero[4] = {0, 0, 0, 0};
    glBindBuffer(GL_UNIFORM_BUFFER, buffer);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, 16, zero);
    glFinish();
    CHECK(values[0] == 0.0f && values[3] == 0.0f);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
    glDeleteBuffers(1, &buffer);
    glDeleteBuffers(1, &other);
    free(memory);
}

// GL 4.5 derivative control (GL_ARB_derivative_control): fine and coarse
// derivatives of the window position are one pixel per pixel.
GPU_TEST_VERSION(gpu_gl45_derivative_control, 45) {
    GLuint p = gpu::program("#version 450 core\nlayout(location = 0) in vec2 pos;\nvoid main() { gl_Position = vec4(pos, 0.0, 1.0); }\n",
                            "#version 450 core\nout vec4 o;\nvoid main() {\n"
                            "    o = vec4(dFdxFine(gl_FragCoord.x), dFdyCoarse(gl_FragCoord.y), fwidthFine(gl_FragCoord.x) * 0.5,\n"
                            "             dFdxCoarse(gl_FragCoord.y) + 1.0);\n}\n");
    GLuint vao = gpu::quad();
    glUseProgram(p);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    CHECK_GL();
    const uint8_t want[4] = {255, 255, 128, 255};
    CHECK(gpu::near(gpu::readRGBA8(9, 9, 1, 1).data(), want, 2));
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
}
