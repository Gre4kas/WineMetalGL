// GL_ARB_buffer_storage.
#include "gpu.h"

GPU_TEST(gpu_buffer_storage_persistent) {
    GLuint p = gpu::program("#version 330 core\nin vec2 position; in vec4 color; flat out vec4 c;\n"
                            "void main() { c = color; gl_Position = vec4(position, 0.0, 1.0); }",
                            "#version 330 core\nflat in vec4 c; out vec4 o; void main() { o = c; }");
    glBindAttribLocation(p, 1, "color");
    glLinkProgram(p);
    glUseProgram(p);
    GLuint vao = gpu::quad();
    GLuint vbo = 0;
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    const GLbitfield flags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
    glBufferStorage(GL_ARRAY_BUFFER, 4 * 4 * sizeof(float), nullptr, flags);
    CHECK_GL();
    auto* colors = static_cast<float*>(glMapBufferRange(GL_ARRAY_BUFFER, 0, 16 * sizeof(float), flags));
    CHECK(colors != nullptr);
    if (!colors) return;
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 0, nullptr);
    glEnableVertexAttribArray(1);
    for (int i = 0; i < 4; ++i) { colors[i * 4] = 1; colors[i * 4 + 1] = 0; colors[i * 4 + 2] = 0; colors[i * 4 + 3] = 1; }
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);  // allowed while persistently mapped
    CHECK_GL();
    const uint8_t red[4] = {255, 0, 0, 255}, blue[4] = {0, 0, 255, 255};
    CHECK(gpu::near(gpu::readRGBA8(32, 32, 1, 1).data(), red));
    for (int i = 0; i < 4; ++i) { colors[i * 4] = 0; colors[i * 4 + 2] = 1; }  // ReadPixels waited: safe to write
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    CHECK(gpu::near(gpu::readRGBA8(32, 32, 1, 1).data(), blue));
    // Immutable storage: no BufferData, no BufferSubData without DYNAMIC_STORAGE_BIT.
    glBufferData(GL_ARRAY_BUFFER, 16, nullptr, GL_STATIC_DRAW);
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_OPERATION));
    const float one = 1.0f;
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(one), &one);
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_OPERATION));
    CHECK(glUnmapBuffer(GL_ARRAY_BUFFER) == GL_TRUE);
    // Persistent maps need storage created with MAP_PERSISTENT_BIT.
    GLuint plain = 0;
    glGenBuffers(1, &plain);
    glBindBuffer(GL_ARRAY_BUFFER, plain);
    glBufferStorage(GL_ARRAY_BUFFER, 64, nullptr, GL_MAP_WRITE_BIT);
    CHECK(glMapBufferRange(GL_ARRAY_BUFFER, 0, 64, GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT) == nullptr);
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_OPERATION));
    glDeleteBuffers(1, &plain);
    glDeleteBuffers(1, &vbo);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
    CHECK_GL();
}
