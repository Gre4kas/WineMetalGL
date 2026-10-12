#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "gpu.h"

#define WMGL_DEFINE_FUNC(name) PFN_##name name = nullptr;
WMGL_GPU_FUNCS(WMGL_DEFINE_FUNC)
#undef WMGL_DEFINE_FUNC

namespace wmgl_test {
std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}
int failures = 0;
}  // namespace wmgl_test

namespace gpu {
namespace {

int pickFormat() {
    std::vector<wmgl_pixel_format> formats(256);
    unsigned onscreen = 0;
    const unsigned count = wmgl_get_pixel_formats(formats.data(), static_cast<unsigned>(formats.size()), &onscreen);
    for (unsigned i = 0; i < count && i < formats.size(); ++i) {
        const wmgl_pixel_format& f = formats[i];
        if (f.draw_to_pbuffer && f.red_bits == 8 && f.alpha_bits == 8 && f.depth_bits >= 24 && f.stencil_bits == 8 &&
            f.samples <= 1 && !f.srgb_capable && !f.float_components)
            return static_cast<int>(i + 1);
    }
    return 0;
}

bool loadFunctions() {
    bool ok = true;
#define WMGL_LOAD_FUNC(name)                                                     \
    name = reinterpret_cast<PFN_##name>(wmgl_get_proc_address(#name));          \
    if (!name) { std::fprintf(stderr, "missing entry point %s\n", #name); ok = false; }
    WMGL_GPU_FUNCS(WMGL_LOAD_FUNC)
#undef WMGL_LOAD_FUNC
    return ok;
}

}  // namespace

Fixture::Fixture(int width, int height, bool core, int version) {
    static const bool initialized = wmgl_initialize(WMGL_DRIVER_ABI_VERSION) == 0 && loadFunctions();
    if (!initialized) return;
    const int format = pickFormat();
    if (!format) return;
    surface = wmgl_surface_create_pbuffer(format, width, height);
    const int attribs[] = {0x2091 /* MAJOR */, version / 10, 0x2092 /* MINOR */, version % 10, 0x9126 /* PROFILE_MASK */,
                           core ? 1 : 2, 0};
    int error = 0;
    if (version > 43) {
        char value[8];
        std::snprintf(value, sizeof(value), "%d.%d", version / 10, version % 10);
        setenv("WINEMETALGL_DEV_MAX_VERSION", value, 1);
    }
    context = wmgl_context_create(format, nullptr, attribs, &error);
    if (version > 43) unsetenv("WINEMETALGL_DEV_MAX_VERSION");
    ok = surface && context && wmgl_make_current(context, surface, surface);
    if (ok) glViewport(0, 0, width, height);
}

Fixture::~Fixture() {
    wmgl_make_current(nullptr, nullptr, nullptr);
    if (context) wmgl_context_destroy(context);
    if (surface) wmgl_surface_destroy(surface);
}

GLuint program(const char* vs, const char* fs) {
    auto compile = [](GLenum type, const char* source) {
        GLuint shader = glCreateShader(type);
        glShaderSource(shader, 1, &source, nullptr);
        glCompileShader(shader);
        GLint status = 0;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
        if (!status) {
            char log[2048] = {};
            glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
            std::fprintf(stderr, "  shader compile failed:\n%s\n", log);
            ++wmgl_test::failures;
        }
        return shader;
    };
    GLuint p = glCreateProgram();
    GLuint v = compile(GL_VERTEX_SHADER, vs), f = compile(GL_FRAGMENT_SHADER, fs);
    glAttachShader(p, v);
    glAttachShader(p, f);
    glBindAttribLocation(p, 0, "position");
    glLinkProgram(p);
    GLint status = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &status);
    if (!status) {
        char log[2048] = {};
        glGetProgramInfoLog(p, sizeof(log), nullptr, log);
        std::fprintf(stderr, "  program link failed:\n%s\n", log);
        ++wmgl_test::failures;
    }
    glDeleteShader(v);
    glDeleteShader(f);
    return p;
}

GLuint quad() {
    static const float vertices[] = {-1, -1, 1, -1, -1, 1, 1, 1};
    GLuint vao = 0, vbo = 0;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glEnableVertexAttribArray(0);
    return vao;
}

std::vector<uint8_t> readRGBA8(int x, int y, int width, int height) {
    std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(x, y, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    return pixels;
}

bool near(const uint8_t* got, const uint8_t want[4], int tolerance) {
    for (int i = 0; i < 4; ++i)
        if (std::abs(static_cast<int>(got[i]) - static_cast<int>(want[i])) > tolerance) {
            std::fprintf(stderr, "  pixel (%d, %d, %d, %d), expected (%d, %d, %d, %d)\n", got[0], got[1], got[2], got[3],
                         want[0], want[1], want[2], want[3]);
            return false;
        }
    return true;
}

}  // namespace gpu

int main(int argc, char** argv) {
    int run = 0;
    for (const auto& test : wmgl_test::registry()) {
        if (argc > 1 && !std::strstr(test.name, argv[1])) continue;
        const int before = wmgl_test::failures;
        test.body();
        std::printf("%s %s\n", wmgl_test::failures == before ? "PASS" : "FAIL", test.name);
        std::fflush(stdout);
        ++run;
    }
    std::printf("%d tests, %d failed checks\n", run, wmgl_test::failures);
    return wmgl_test::failures ? 1 : 0;
}
