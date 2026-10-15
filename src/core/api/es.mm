// OpenGL ES commands desktop GL has no counterpart for, and OpenGL ES
// extension commands that differ from the desktop ones they mirror (most
// alias them: tools/gen/gl_gen.py).

#include <algorithm>
#include <vector>

#include "core/context.h"
#include "gl_api.gen.h"

namespace wmgl::api {
namespace {

// EXT_debug_label's object types, as KHR_debug's identifiers.
GLenum debugLabelIdentifier(GLenum type) {
    switch (type) {
    case 0x9151: return GL_BUFFER;            // GL_BUFFER_OBJECT_EXT
    case 0x8B48: return GL_SHADER;            // GL_SHADER_OBJECT_EXT
    case 0x8B40: return GL_PROGRAM;           // GL_PROGRAM_OBJECT_EXT
    case 0x9153: return GL_QUERY;             // GL_QUERY_OBJECT_EXT
    case 0x9154: return GL_VERTEX_ARRAY;      // GL_VERTEX_ARRAY_OBJECT_EXT
    case 0x8A4F: return GL_PROGRAM_PIPELINE;  // GL_PROGRAM_PIPELINE_OBJECT_EXT
    case GL_SAMPLER: case GL_TRANSFORM_FEEDBACK: case GL_TEXTURE: case GL_RENDERBUFFER: case GL_FRAMEBUFFER: return type;
    default: return GL_NONE;
    }
}

}  // namespace

void LabelObjectEXT(Context& ctx, GLenum type, GLuint object, GLsizei length, const GLchar* label) {
    const GLenum identifier = debugLabelIdentifier(type);
    if (!identifier) return ctx.setError(GL_INVALID_ENUM);
    if (length < 0) return ctx.setError(GL_INVALID_VALUE);
    // A length of 0: a null-terminated label.
    ObjectLabel(ctx, identifier, object, length ? length : -1, label);
}

void GetObjectLabelEXT(Context& ctx, GLenum type, GLuint object, GLsizei bufSize, GLsizei* length, GLchar* label) {
    const GLenum identifier = debugLabelIdentifier(type);
    if (!identifier) return ctx.setError(GL_INVALID_ENUM);
    GetObjectLabel(ctx, identifier, object, bufSize, length, label);
}

// EXT_debug_marker: annotations for tools, which WineMetalGL has none of.
void InsertEventMarkerEXT(Context&, GLsizei, const GLchar*) {}
void PushGroupMarkerEXT(Context&, GLsizei, const GLchar*) {}
void PopGroupMarkerEXT(Context&) {}

// EXT_discard_framebuffer: glInvalidateFramebuffer for GL_FRAMEBUFFER (its
// GL_COLOR_EXT, GL_DEPTH_EXT and GL_STENCIL_EXT are GL_COLOR, GL_DEPTH and
// GL_STENCIL).
void DiscardFramebufferEXT(Context& ctx, GLenum target, GLsizei numAttachments, const GLenum* attachments) {
    if (target != GL_FRAMEBUFFER) return ctx.setError(GL_INVALID_ENUM);
    InvalidateFramebuffer(ctx, target, numAttachments, attachments);
}

// OES_viewport_array: the single-precision depth ranges.
void DepthRangeArrayfvOES(Context& ctx, GLuint first, GLsizei count, const GLfloat* v) {
    if (count < 0) return ctx.setError(GL_INVALID_VALUE);
    std::vector<GLdouble> d(v ? static_cast<size_t>(count) * 2 : 0);
    for (size_t i = 0; i < d.size(); ++i) d[i] = v[i];
    DepthRangeArrayv(ctx, first, count, v ? d.data() : nullptr);
}

void DepthRangeIndexedfOES(Context& ctx, GLuint index, GLfloat n, GLfloat f) { DepthRangeIndexed(ctx, index, n, f); }

// OpenGL ES 3.2 / KHR_blend_equation_advanced: framebuffer fetch on Apple
// GPUs reads every earlier fragment's result, so there is nothing to wait for.
void BlendBarrier(Context& ctx) {
    if (!ctx.advertises("GL_KHR_blend_equation_advanced")) return ctx.unimplemented("glBlendBarrier");
}

// OpenGL ES 3.2 / EXT_primitive_bounding_box: where tessellation and
// geometry shaders' output lies, a hint that is only kept for queries.
void PrimitiveBoundingBox(Context& ctx, GLfloat minX, GLfloat minY, GLfloat minZ, GLfloat minW, GLfloat maxX, GLfloat maxY,
                          GLfloat maxZ, GLfloat maxW) {
    if (!ctx.isEs() || ctx.esVersion() < 31) return ctx.unimplemented("glPrimitiveBoundingBox");
    const GLfloat box[8] = {minX, minY, minZ, minW, maxX, maxY, maxZ, maxW};
    std::copy(box, box + 8, ctx.state.primitiveBoundingBox);
}

}  // namespace wmgl::api
