// Uniform blocks: bindings, slots per stage, unused declared blocks.
#include <cstring>

#include "gpu.h"

GPU_TEST(gpu_uniform_blocks_fragment_slots) {
    // The fragment stage declares a std140 block it never reads before the
    // blocks it does read; each block comes from its own buffer.
    const char* vs = R"(#version 330
in vec4 position;
void main() { gl_Position = position; })";
    const char* fs = R"(#version 330
layout(location = 0) out vec4 color;
layout(shared) uniform BlockB { bool a; } blockB;
layout(std140) uniform BlockC { uint b; bool c; uint d; int e; uint f; };
layout(std140) uniform BlockD { float g; };
layout(shared) uniform BlockF { float m; float n; uint o; };
void main() {
    float r = 1.0;
    r *= blockB.a ? 0.0 : 1.0;
    r *= c ? 1.0 : 0.0;
    r *= d == 3u ? 1.0 : 0.0;
    r *= e == 5 ? 1.0 : 0.0;
    r *= f == 7u ? 1.0 : 0.0;
    r *= m == -1.0 ? 1.0 : 0.0;
    r *= n == -4.0 ? 1.0 : 0.0;
    r *= o == 1u ? 1.0 : 0.0;
    color = vec4(r, 0.0, 0.0, 1.0);
})";
    GLuint p = gpu::program(vs, fs);
    glUseProgram(p);
    const char* names[] = {"BlockB", "BlockC", "BlockD", "BlockF"};
    GLuint buffers[4];
    glGenBuffers(4, buffers);
    const uint32_t dataB[1] = {0};
    const uint32_t dataC[5] = {2, 1, 3, 5, 7};
    const float dataD[1] = {8.0f};
    uint32_t dataF[3];
    const float m = -1.0f, n = -4.0f;
    std::memcpy(&dataF[0], &m, 4);
    std::memcpy(&dataF[1], &n, 4);
    dataF[2] = 1;
    const void* data[] = {dataB, dataC, dataD, dataF};
    const size_t sizes[] = {sizeof(dataB), sizeof(dataC), sizeof(dataD), sizeof(dataF)};
    for (int i = 0; i < 4; ++i) {
        const GLuint index = glGetUniformBlockIndex(p, names[i]);
        CHECK(index != GL_INVALID_INDEX);
        glUniformBlockBinding(p, index, static_cast<GLuint>(i + 1));
        glBindBuffer(GL_UNIFORM_BUFFER, buffers[i]);
        glBufferData(GL_UNIFORM_BUFFER, static_cast<GLsizeiptr>(sizes[i]), data[i], GL_STATIC_DRAW);
        glBindBufferBase(GL_UNIFORM_BUFFER, static_cast<GLuint>(i + 1), buffers[i]);
    }
    GLuint vao = gpu::quad();
    glClearColor(0, 0, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    const uint8_t red[4] = {255, 0, 0, 255};
    CHECK(gpu::near(gpu::readRGBA8(32, 32, 1, 1).data(), red));
    glDeleteVertexArrays(1, &vao);
    glDeleteBuffers(4, buffers);
    glDeleteProgram(p);
    CHECK_GL();
}
