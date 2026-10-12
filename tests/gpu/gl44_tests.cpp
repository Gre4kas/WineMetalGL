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
