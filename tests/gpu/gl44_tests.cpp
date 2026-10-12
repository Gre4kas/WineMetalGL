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
