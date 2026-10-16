// OpenGL ES commands desktop GL has no counterpart for, and OpenGL ES
// extension commands that differ from the desktop ones they mirror (most
// alias them: tools/gen/gl_gen.py).

#include <algorithm>
#include <vector>

#include "core/context.h"
#include "core/limits.h"
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

// ------------------------------------------------- GL_EXT_fragment_shading_rate
//
// Metal has no per-draw, per-primitive or attachment shading rates; the only
// supported rate is 1x1 (glGetFragmentShadingRatesEXT), which every requested
// or combined rate clamps to, so draws shade every pixel. The state, the
// combiners and the shading rate attachment are kept and validated.

namespace {

constexpr GLint kShadingRateTexelSize = 16;  // MIN/MAX_FRAGMENT_SHADING_RATE_ATTACHMENT_TEXEL_WIDTH/HEIGHT_EXT

bool shadingRates(Context& ctx, const char* command) {
    if (ctx.advertises("GL_EXT_fragment_shading_rate")) return true;
    ctx.unimplemented(command);
    return false;
}

bool isShadingRate(GLenum rate) {
    switch (rate) {
    case GL_SHADING_RATE_1X1_PIXELS_EXT: case GL_SHADING_RATE_1X2_PIXELS_EXT: case GL_SHADING_RATE_2X1_PIXELS_EXT:
    case GL_SHADING_RATE_2X2_PIXELS_EXT: case GL_SHADING_RATE_1X4_PIXELS_EXT: case GL_SHADING_RATE_4X1_PIXELS_EXT:
    case GL_SHADING_RATE_4X2_PIXELS_EXT: case GL_SHADING_RATE_2X4_PIXELS_EXT: case GL_SHADING_RATE_4X4_PIXELS_EXT:
        return true;
    default: return false;
    }
}

bool isCombinerOp(GLenum op) {
    return op == GL_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_EXT || op == GL_FRAGMENT_SHADING_RATE_COMBINER_OP_REPLACE_EXT ||
           op == GL_FRAGMENT_SHADING_RATE_COMBINER_OP_MIN_EXT || op == GL_FRAGMENT_SHADING_RATE_COMBINER_OP_MAX_EXT ||
           op == GL_FRAGMENT_SHADING_RATE_COMBINER_OP_MUL_EXT;
}

}  // namespace

void GetFragmentShadingRatesEXT(Context& ctx, GLsizei samples, GLsizei maxCount, GLsizei* count, GLenum* rates) {
    (void)samples;
    if (!shadingRates(ctx, "glGetFragmentShadingRatesEXT")) return;
    if (maxCount < 0) return ctx.setError(GL_INVALID_VALUE);
    const GLsizei written = std::min<GLsizei>(maxCount, 1);
    if (rates && written > 0) rates[0] = GL_SHADING_RATE_1X1_PIXELS_EXT;
    if (count) *count = rates ? written : 1;
}

void ShadingRateEXT(Context& ctx, GLenum rate) {
    if (!shadingRates(ctx, "glShadingRateEXT")) return;
    if (!isShadingRate(rate)) return ctx.setError(GL_INVALID_ENUM);
    ctx.state.shadingRate = rate;
}

void ShadingRateCombinerOpsEXT(Context& ctx, GLenum combinerOp0, GLenum combinerOp1) {
    if (!shadingRates(ctx, "glShadingRateCombinerOpsEXT")) return;
    if (!isCombinerOp(combinerOp0) || !isCombinerOp(combinerOp1)) return ctx.setError(GL_INVALID_ENUM);
    // Non-trivial combiners are supported (FRAGMENT_SHADING_RATE_NON_TRIVIAL_COMBINERS_SUPPORTED_EXT).
    ctx.state.shadingRateCombiners[0] = combinerOp0;
    ctx.state.shadingRateCombiners[1] = combinerOp1;
}

void FramebufferShadingRateEXT(Context& ctx, GLenum target, GLenum attachment, GLuint texture, GLint baseLayer,
                               GLsizei numLayers, GLsizei texelWidth, GLsizei texelHeight) {
    if (!shadingRates(ctx, "glFramebufferShadingRateEXT")) return;
    if (target != GL_FRAMEBUFFER && target != GL_DRAW_FRAMEBUFFER && target != GL_READ_FRAMEBUFFER)
        return ctx.setError(GL_INVALID_ENUM);
    if (attachment != GL_SHADING_RATE_ATTACHMENT_EXT) return ctx.setError(GL_INVALID_ENUM);
    Framebuffer* fb = target == GL_READ_FRAMEBUFFER ? ctx.state.readFramebuffer.get() : ctx.state.drawFramebuffer.get();
    // FRAGMENT_SHADING_RATE_ATTACHMENT_WITH_DEFAULT_FRAMEBUFFER_SUPPORTED_EXT is FALSE.
    if (!fb) return ctx.setError(GL_INVALID_OPERATION);
    RefPtr<Texture> tex;
    if (texture) {
        {
            ShareLock lock(ctx);
            tex = ctx.share().textures.getRef(texture);
        }
        if (!tex || !tex->immutable) return ctx.setError(GL_INVALID_VALUE);
        if (tex->target != GL_TEXTURE_2D && tex->target != GL_TEXTURE_2D_ARRAY) return ctx.setError(GL_INVALID_OPERATION);
        const auto pow2 = [](GLsizei v) { return v > 0 && (v & (v - 1)) == 0; };
        if (baseLayer < 0 || baseLayer >= limits::kMaxArrayLayers || numLayers < 1 || numLayers > limits::kMaxArrayLayers ||
            !pow2(texelWidth) || !pow2(texelHeight) || texelWidth < kShadingRateTexelSize || texelWidth > kShadingRateTexelSize ||
            texelHeight < kShadingRateTexelSize || texelHeight > kShadingRateTexelSize)
            return ctx.setError(GL_INVALID_VALUE);
    }
    fb->shadingRateTexture = tex;
    fb->shadingRateBaseLayer = texture ? baseLayer : 0;
    fb->shadingRateLayers = texture ? numLayers : 0;
    fb->shadingRateTexelWidth = texture ? texelWidth : 0;
    fb->shadingRateTexelHeight = texture ? texelHeight : 0;
    ++fb->version;
}

}  // namespace wmgl::api
