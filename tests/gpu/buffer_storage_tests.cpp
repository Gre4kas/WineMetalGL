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

// Vertices and indices copied from a mapped staging buffer into new buffers
// each frame, then drawn (Minecraft's uploads): the CPU reads of the copies
// (index range, an unsigned 16-bit attribute read as ivec2, converted on the
// CPU) see each copy's data, the second one into buffers already drawn too.
GPU_TEST(gpu_buffer_copy_from_staging_then_draw) {
    GLuint p = gpu::program("#version 330 core\nin vec2 position; in ivec2 shift; flat out ivec2 s;\n"
                            "void main() { s = shift; gl_Position = vec4(position, 0.0, 1.0); }",
                            "#version 330 core\nflat in ivec2 s; out vec4 o;\n"
                            "void main() { o = vec4(s.x == 40000 ? 1.0 : 0.0, s.y == 40000 ? 1.0 : 0.0, 0.0, 1.0); }");
    glBindAttribLocation(p, 1, "shift");
    glLinkProgram(p);
    glUseProgram(p);
    GLuint vao = gpu::quad();
    GLuint buffers[3] = {};
    glGenBuffers(3, buffers);
    const GLuint staging = buffers[0], vertices = buffers[1], indices = buffers[2];
    const GLbitfield flags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
    glBindBuffer(GL_COPY_READ_BUFFER, staging);
    glBufferStorage(GL_COPY_READ_BUFFER, 24, nullptr, flags);
    auto* mapped = static_cast<uint16_t*>(glMapBufferRange(GL_COPY_READ_BUFFER, 0, 24, flags));
    CHECK(mapped != nullptr);
    if (!mapped) return;
    glBindBuffer(GL_ARRAY_BUFFER, vertices);
    glBufferStorage(GL_ARRAY_BUFFER, 16, nullptr, GL_DYNAMIC_STORAGE_BIT);
    glVertexAttribIPointer(1, 2, GL_UNSIGNED_SHORT, 0, nullptr);
    glEnableVertexAttribArray(1);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indices);
    glBufferStorage(GL_ELEMENT_ARRAY_BUFFER, 8, nullptr, GL_DYNAMIC_STORAGE_BIT);
    const uint8_t red[4] = {255, 0, 0, 255}, green[4] = {0, 255, 0, 255};
    for (int frame = 0; frame < 2; ++frame) {
        // Read back since the last copy: the staging buffer is free to write.
        for (int v = 0; v < 4; ++v) {
            mapped[v * 2] = frame == 0 ? 40000 : 0;
            mapped[v * 2 + 1] = frame == 0 ? 0 : 40000;
            mapped[8 + v] = static_cast<uint16_t>(v);
        }
        glBindBuffer(GL_COPY_WRITE_BUFFER, vertices);
        glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 0, 0, 16);
        glBindBuffer(GL_COPY_WRITE_BUFFER, indices);
        glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 16, 0, 8);
        glClearColor(0, 0, 1, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glDrawElements(GL_TRIANGLE_STRIP, 4, GL_UNSIGNED_SHORT, nullptr);
        CHECK_GL();
        CHECK(gpu::near(gpu::readRGBA8(32, 32, 1, 1).data(), frame == 0 ? red : green));
    }
    glBindBuffer(GL_COPY_READ_BUFFER, staging);
    glUnmapBuffer(GL_COPY_READ_BUFFER);
    glDeleteBuffers(3, buffers);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
    CHECK_GL();
}
