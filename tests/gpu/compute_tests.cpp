// GL_ARB_compute_shader is not exposed on 3.3 contexts until storage buffers,
// images and atomic counters exist (WineMetalGL 4.x roadmap, G08): engines
// that detect it must keep their fallback paths.
#include "gpu.h"

GPU_TEST(gpu_compute_not_exposed) {
    GLint n = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &n);
    bool advertised = false;
    for (GLint i = 0; i < n; ++i)
        advertised |= !std::strcmp(reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, static_cast<GLuint>(i))),
                                   "GL_ARB_compute_shader");
    CHECK(!advertised);
    CHECK_EQ(glCreateShader(GL_COMPUTE_SHADER), 0u);
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_ENUM));
    CHECK_GL();
}
