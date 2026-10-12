// GL 4.4 features (contexts above 4.3 go through the development override
// until the version is advertised).
#include <cmath>
#include <cstring>

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
