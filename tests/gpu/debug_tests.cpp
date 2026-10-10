// KHR_debug.
#include <string>
#include <vector>

#include "gpu.h"

namespace {

struct Received {
    GLenum source, type, severity;
    GLuint id;
    std::string message;
};

void callback(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei, const GLchar* message, const void* user) {
    static_cast<std::vector<Received>*>(const_cast<void*>(user))->push_back({source, type, severity, id, message});
}

}  // namespace

GPU_TEST(gpu_khr_debug) {
    std::vector<Received> got;
    glEnable(GL_DEBUG_OUTPUT);
    glDebugMessageCallback(callback, &got);
    glEnable(0xFFFFFFFFu);  // invalid enum: an API error message
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_ENUM));
    CHECK(got.size() == 1 && got[0].type == GL_DEBUG_TYPE_ERROR && got[0].id == GL_INVALID_ENUM &&
          got[0].message.find("glEnable") != std::string::npos);
    // Disabling a type filters it.
    glDebugMessageControl(GL_DONT_CARE, GL_DEBUG_TYPE_ERROR, GL_DONT_CARE, 0, nullptr, GL_FALSE);
    glEnable(0xFFFFFFFFu);
    glGetError();
    CHECK_EQ(got.size(), static_cast<size_t>(1));
    // Groups notify and scope the control state.
    glPushDebugGroup(GL_DEBUG_SOURCE_APPLICATION, 7, -1, "frame");
    glDebugMessageControl(GL_DONT_CARE, GL_DEBUG_TYPE_ERROR, GL_DONT_CARE, 0, nullptr, GL_TRUE);
    glEnable(0xFFFFFFFFu);
    glGetError();
    glPopDebugGroup();
    glEnable(0xFFFFFFFFu);  // filtered again after the pop
    glGetError();
    CHECK(got.size() == 4 && got[1].type == GL_DEBUG_TYPE_PUSH_GROUP && got[1].message == "frame" &&
          got[2].type == GL_DEBUG_TYPE_ERROR && got[3].type == GL_DEBUG_TYPE_POP_GROUP);
    // Without a callback, messages are logged.
    glDebugMessageCallback(nullptr, nullptr);
    glDebugMessageInsert(GL_DEBUG_SOURCE_APPLICATION, GL_DEBUG_TYPE_MARKER, 42, GL_DEBUG_SEVERITY_NOTIFICATION, -1, "hello");
    GLenum source = 0, type = 0, severity = 0;
    GLuint id = 0;
    GLsizei length = 0;
    char text[64] = {};
    CHECK_EQ(glGetDebugMessageLog(1, sizeof(text), &source, &type, &id, &severity, &length, text), 1u);
    CHECK(id == 42 && type == GL_DEBUG_TYPE_MARKER && std::string(text) == "hello" && length == 6);
    // Object labels.
    GLuint buffer = 0;
    glGenBuffers(1, &buffer);
    glBindBuffer(GL_ARRAY_BUFFER, buffer);
    glObjectLabel(GL_BUFFER, buffer, -1, "vertices");
    char label[32] = {};
    GLsizei labelLength = 0;
    glGetObjectLabel(GL_BUFFER, buffer, sizeof(label), &labelLength, label);
    CHECK(std::string(label) == "vertices" && labelLength == 8);
    glObjectLabel(GL_BUFFER, 12345, -1, "nothing");
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_VALUE));
    glDeleteBuffers(1, &buffer);
    glDisable(GL_DEBUG_OUTPUT);
    CHECK_GL();
}
