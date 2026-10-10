// KHR_debug (GL 4.3 section 20) and GL_ARB_debug_output: message control,
// application messages, the message log, debug groups and object labels.
// Messages go to the callback when one is set, else to a bounded log
// (Context::debugMessage). GL errors produce API messages (Context::setError).

#include <algorithm>
#include <cstring>

#include "core/compat/compat_api.h"
#include "core/context.h"
#include "gl_api.gen.h"

namespace wmgl::api {
namespace {

bool validSource(GLenum source, bool dontCare) {
    switch (source) {
    case GL_DEBUG_SOURCE_API: case GL_DEBUG_SOURCE_WINDOW_SYSTEM: case GL_DEBUG_SOURCE_SHADER_COMPILER:
    case GL_DEBUG_SOURCE_THIRD_PARTY: case GL_DEBUG_SOURCE_APPLICATION: case GL_DEBUG_SOURCE_OTHER:
        return true;
    default: return dontCare && source == GL_DONT_CARE;
    }
}

bool validType(GLenum type, bool dontCare) {
    switch (type) {
    case GL_DEBUG_TYPE_ERROR: case GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR: case GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR:
    case GL_DEBUG_TYPE_PORTABILITY: case GL_DEBUG_TYPE_PERFORMANCE: case GL_DEBUG_TYPE_OTHER: case GL_DEBUG_TYPE_MARKER:
    case GL_DEBUG_TYPE_PUSH_GROUP: case GL_DEBUG_TYPE_POP_GROUP:
        return true;
    default: return dontCare && type == GL_DONT_CARE;
    }
}

bool validSeverity(GLenum severity, bool dontCare) {
    switch (severity) {
    case GL_DEBUG_SEVERITY_HIGH: case GL_DEBUG_SEVERITY_MEDIUM: case GL_DEBUG_SEVERITY_LOW: case GL_DEBUG_SEVERITY_NOTIFICATION:
        return true;
    default: return dontCare && severity == GL_DONT_CARE;
    }
}

// A message string: `length` < 0 means NUL-terminated.
bool messageText(Context& ctx, const GLchar* text, GLsizei length, std::string& out) {
    if (!text) return false;
    out = length < 0 ? std::string(text) : std::string(text, static_cast<size_t>(length));
    if (out.size() >= Context::kMaxDebugMessageLength) {
        ctx.setError(GL_INVALID_VALUE);
        return false;
    }
    return true;
}

// The label slot of the object `identifier`/`name`, creating generated
// objects that were not bound yet. Null (with the GL error set) otherwise.
std::string* labelOf(Context& ctx, GLenum identifier, GLuint name) {
    ShareGroup& share = ctx.share();
    auto shared = [&](auto& table, auto create) -> std::string* {
        auto object = table.getRef(name);
        if (!object && table.isGenerated(name)) {
            object = create();
            table.set(name, object);
        }
        if (!object) return ctx.setError(GL_INVALID_VALUE), nullptr;
        return &object->label;
    };
    switch (identifier) {
    case GL_BUFFER: return shared(share.buffers, [&] { return makeRef<Buffer>(name); });
    case GL_SAMPLER: return shared(share.samplers, [&] { return makeRef<Sampler>(name); });
    case GL_RENDERBUFFER: return shared(share.renderbuffers, [&] { return makeRef<Renderbuffer>(name); });
    case GL_TEXTURE: {
        Texture* tex = share.textures.get(name);
        // A texture gets its target when first bound; a label needs that object.
        if (!tex) return ctx.setError(GL_INVALID_VALUE), nullptr;
        return &tex->label;
    }
    case GL_FRAMEBUFFER: return shared(ctx.framebuffers, [&] { return makeRef<Framebuffer>(name); });
    case GL_VERTEX_ARRAY: return shared(ctx.vertexArrays, [&] { return makeRef<VertexArray>(name); });
    case GL_TRANSFORM_FEEDBACK: return shared(ctx.transformFeedbacks, [&] { return makeRef<TransformFeedback>(name); });
    case GL_QUERY: {
        Query* q = ctx.queries.get(name);
        if (!q) return ctx.setError(GL_INVALID_VALUE), nullptr;
        return &q->label;
    }
    case GL_SHADER: {
        Shader* s = share.shader(name);
        if (!s) return ctx.setError(GL_INVALID_VALUE), nullptr;
        return &s->label;
    }
    case GL_PROGRAM: {
        Program* p = share.program(name);
        if (!p) return ctx.setError(GL_INVALID_VALUE), nullptr;
        return &p->label;
    }
    default:
        ctx.setError(GL_INVALID_ENUM);
        return nullptr;
    }
}

void setLabel(Context& ctx, std::string* slot, GLsizei length, const GLchar* label) {
    if (!slot) return;
    if (!label) {
        slot->clear();
        return;
    }
    const std::string text = length < 0 ? std::string(label) : std::string(label, static_cast<size_t>(length));
    if (text.size() >= static_cast<size_t>(Context::kMaxLabelLength)) return ctx.setError(GL_INVALID_VALUE);
    *slot = text;
}

void getLabel(Context& ctx, const std::string* slot, GLsizei bufSize, GLsizei* length, GLchar* label) {
    if (bufSize < 0) return ctx.setError(GL_INVALID_VALUE);
    if (!slot) return;
    if (!label) {
        if (length) *length = static_cast<GLsizei>(slot->size());
        return;
    }
    const GLsizei n = bufSize > 0 ? std::min<GLsizei>(bufSize - 1, static_cast<GLsizei>(slot->size())) : 0;
    if (bufSize > 0) {
        std::memcpy(label, slot->data(), static_cast<size_t>(n));
        label[n] = 0;
    }
    if (length) *length = n;
}

Sync* syncOf(Context& ctx, const void* ptr) {
    auto* sync = static_cast<Sync*>(const_cast<void*>(ptr));
    if (!sync || !ctx.share().syncs.count(sync)) {
        ctx.setError(GL_INVALID_VALUE);
        return nullptr;
    }
    return sync;
}

}  // namespace

void DebugMessageControl(Context& ctx, GLenum source, GLenum type, GLenum severity, GLsizei count, const GLuint* ids,
                         GLboolean enabled) {
    if (!validSource(source, true) || !validType(type, true) || !validSeverity(severity, true))
        return ctx.setError(GL_INVALID_ENUM);
    if (count < 0) return ctx.setError(GL_INVALID_VALUE);
    if (count > 0 && (source == GL_DONT_CARE || type == GL_DONT_CARE || severity != GL_DONT_CARE))
        return ctx.setError(GL_INVALID_OPERATION);
    Context::DebugRule rule{source, type, severity, {}, enabled != GL_FALSE};
    if (count > 0 && ids) rule.ids.assign(ids, ids + count);
    ctx.debugGroups.back().rules.push_back(std::move(rule));
}

void DebugMessageInsert(Context& ctx, GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length,
                        const GLchar* buf) {
    if (source != GL_DEBUG_SOURCE_APPLICATION && source != GL_DEBUG_SOURCE_THIRD_PARTY) return ctx.setError(GL_INVALID_ENUM);
    if (!validType(type, false) || !validSeverity(severity, false)) return ctx.setError(GL_INVALID_ENUM);
    std::string text;
    if (!messageText(ctx, buf, length, text)) return;
    ctx.debugMessage(source, type, id, severity, text);
}

void DebugMessageCallback(Context& ctx, GLDEBUGPROC callback, const void* userParam) {
    ctx.debugCallback = callback;
    ctx.debugUserParam = userParam;
}

GLuint GetDebugMessageLog(Context& ctx, GLuint count, GLsizei bufSize, GLenum* sources, GLenum* types, GLuint* ids,
                          GLenum* severities, GLsizei* lengths, GLchar* messageLog) {
    if (bufSize < 0 && messageLog) {
        ctx.setError(GL_INVALID_VALUE);
        return 0;
    }
    GLuint fetched = 0;
    GLsizei used = 0;
    while (fetched < count && !ctx.debugLog.empty()) {
        const Context::DebugLogEntry& m = ctx.debugLog.front();
        const GLsizei size = static_cast<GLsizei>(m.message.size() + 1);
        if (messageLog) {
            if (used + size > bufSize) break;
            std::memcpy(messageLog + used, m.message.c_str(), static_cast<size_t>(size));
            used += size;
        }
        if (sources) sources[fetched] = m.source;
        if (types) types[fetched] = m.type;
        if (ids) ids[fetched] = m.id;
        if (severities) severities[fetched] = m.severity;
        if (lengths) lengths[fetched] = size;
        ctx.debugLog.pop_front();
        ++fetched;
    }
    return fetched;
}

void PushDebugGroup(Context& ctx, GLenum source, GLuint id, GLsizei length, const GLchar* message) {
    if (source != GL_DEBUG_SOURCE_APPLICATION && source != GL_DEBUG_SOURCE_THIRD_PARTY) return ctx.setError(GL_INVALID_ENUM);
    std::string text;
    if (!messageText(ctx, message, length, text)) return;
    if (ctx.debugGroups.size() >= Context::kMaxDebugGroupDepth) return ctx.setError(GL_STACK_OVERFLOW);
    Context::DebugGroup group{source, id, text, ctx.debugGroups.back().rules};
    ctx.debugGroups.push_back(std::move(group));
    ctx.debugMessage(source, GL_DEBUG_TYPE_PUSH_GROUP, id, GL_DEBUG_SEVERITY_NOTIFICATION, text);
}

void PopDebugGroup(Context& ctx) {
    if (ctx.debugGroups.size() <= 1) return ctx.setError(GL_STACK_UNDERFLOW);
    Context::DebugGroup group = std::move(ctx.debugGroups.back());
    ctx.debugGroups.pop_back();
    ctx.debugMessage(group.source, GL_DEBUG_TYPE_POP_GROUP, group.id, GL_DEBUG_SEVERITY_NOTIFICATION, group.message);
}

void ObjectLabel(Context& ctx, GLenum identifier, GLuint name, GLsizei length, const GLchar* label) {
    if (length < 0 && label == nullptr) length = 0;
    setLabel(ctx, labelOf(ctx, identifier, name), length, label);
}

void GetObjectLabel(Context& ctx, GLenum identifier, GLuint name, GLsizei bufSize, GLsizei* length, GLchar* label) {
    if (bufSize < 0) return ctx.setError(GL_INVALID_VALUE);
    getLabel(ctx, labelOf(ctx, identifier, name), bufSize, length, label);
}

void ObjectPtrLabel(Context& ctx, const void* ptr, GLsizei length, const GLchar* label) {
    if (Sync* sync = syncOf(ctx, ptr)) setLabel(ctx, &sync->label, length, label);
}

void GetObjectPtrLabel(Context& ctx, const void* ptr, GLsizei bufSize, GLsizei* length, GLchar* label) {
    if (bufSize < 0) return ctx.setError(GL_INVALID_VALUE);
    if (Sync* sync = syncOf(ctx, ptr)) getLabel(ctx, &sync->label, bufSize, length, label);
}

void GetPointerv(Context& ctx, GLenum pname, void** params) {
    switch (pname) {
    case GL_DEBUG_CALLBACK_FUNCTION: *params = reinterpret_cast<void*>(ctx.debugCallback); return;
    case GL_DEBUG_CALLBACK_USER_PARAM: *params = const_cast<void*>(ctx.debugUserParam); return;
    default:
        if (!getConventionalPointer(ctx, pname, params)) ctx.setError(GL_INVALID_ENUM);
    }
}

}  // namespace wmgl::api
