// GL_ARB_compute_shader comes with GL 4.3: a default (highest version)
// context advertises it and creates compute shaders.
#include "gpu.h"

namespace {

bool computeAdvertised() {
    GLint n = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &n);
    bool advertised = false;
    for (GLint i = 0; i < n; ++i)
        advertised |= !std::strcmp(reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, static_cast<GLuint>(i))),
                                   "GL_ARB_compute_shader");
    return advertised;
}

}  // namespace

GPU_TEST(gpu_compute_exposed) {
    CHECK(computeAdvertised());
    GLuint shader = glCreateShader(GL_COMPUTE_SHADER);
    CHECK(shader != 0u);
    glDeleteShader(shader);
    CHECK_GL();
}
