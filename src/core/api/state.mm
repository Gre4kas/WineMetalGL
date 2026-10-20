// Rasterization, fragment, pixel-store and miscellaneous state commands.

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>

#include "core/compat/compat_api.h"
#include "core/context.h"
#include "core/draw.h"
#include "core/metal/device.h"
#include "core/shader/spirv_input.h"
#include "core/util/version.h"
#include "gl_api.gen.h"

namespace wmgl::api {
namespace {

bool validCompare(GLenum func) { return func >= GL_NEVER && func <= GL_ALWAYS; }

bool validStencilOp(GLenum op) {
    switch (op) {
    case GL_KEEP: case GL_ZERO: case GL_REPLACE: case GL_INCR: case GL_DECR: case GL_INVERT: case GL_INCR_WRAP:
    case GL_DECR_WRAP: return true;
    default: return false;
    }
}

bool validBlendFactor(GLenum f) {
    switch (f) {
    case GL_ZERO: case GL_ONE: case GL_SRC_COLOR: case GL_ONE_MINUS_SRC_COLOR: case GL_DST_COLOR:
    case GL_ONE_MINUS_DST_COLOR: case GL_SRC_ALPHA: case GL_ONE_MINUS_SRC_ALPHA: case GL_DST_ALPHA:
    case GL_ONE_MINUS_DST_ALPHA: case GL_CONSTANT_COLOR: case GL_ONE_MINUS_CONSTANT_COLOR: case GL_CONSTANT_ALPHA:
    case GL_ONE_MINUS_CONSTANT_ALPHA: case GL_SRC_ALPHA_SATURATE: case GL_SRC1_COLOR: case GL_ONE_MINUS_SRC1_COLOR:
    case GL_SRC1_ALPHA: case GL_ONE_MINUS_SRC1_ALPHA: return true;
    default: return false;
    }
}

bool validBlendEquation(GLenum e) {
    return e == GL_FUNC_ADD || e == GL_FUNC_SUBTRACT || e == GL_FUNC_REVERSE_SUBTRACT || e == GL_MIN || e == GL_MAX;
}

// The capabilities OpenGL ES has (with the extensions ES contexts advertise).
// Its point size from shaders, seamless cube maps and sRGB conversion are
// always on (Context::Context).
bool esCapability(Context& ctx, GLenum cap) {
    switch (cap) {
    case GL_BLEND: case GL_CULL_FACE: case GL_DEPTH_TEST: case GL_STENCIL_TEST: case GL_SCISSOR_TEST:
    case GL_POLYGON_OFFSET_FILL: case GL_DITHER: case GL_SAMPLE_ALPHA_TO_COVERAGE: case GL_SAMPLE_COVERAGE:
    case GL_DEBUG_OUTPUT: case GL_DEBUG_OUTPUT_SYNCHRONOUS:
    case GL_DEPTH_CLAMP:  // EXT_depth_clamp
        return true;
    case GL_RASTERIZER_DISCARD: case GL_PRIMITIVE_RESTART_FIXED_INDEX: return ctx.esVersion() >= 30;
    case GL_SAMPLE_MASK: return ctx.esVersion() >= 31;
    case 0x9285:  // GL_BLEND_ADVANCED_COHERENT_KHR
        return ctx.advertises("GL_KHR_blend_equation_advanced_coherent");
    case GL_SAMPLE_SHADING: return ctx.esVersion() >= 31;  // OES_sample_shading
    default:
        // EXT_clip_cull_distance
        return ctx.esVersion() >= 30 && cap >= GL_CLIP_DISTANCE0 && cap < GL_CLIP_DISTANCE0 + 8;
    }
}

// Returns true if handled; false means the capability is unknown.
bool setCapability(Context& ctx, GLenum cap, bool on) {
    ContextState& s = ctx.state;
    if (ctx.isEs() && !esCapability(ctx, cap)) return false;
    // Framebuffer fetch is coherent either way: only the state is kept.
    if (cap == 0x9285 && ctx.isEs()) { s.blendAdvancedCoherent = on; return true; }
    switch (cap) {
    case GL_BLEND: s.blendEnable = on ? 0xff : 0; ctx.markDirty(kDirtyBlend); return true;
    case GL_CULL_FACE: s.cullFace = on; ctx.markDirty(kDirtyRaster); return true;
    case GL_DEPTH_TEST: s.depthTest = on; ctx.markDirty(kDirtyDepthStencil); return true;
    case GL_STENCIL_TEST: s.stencilTest = on; ctx.markDirty(kDirtyDepthStencil); return true;
    case GL_SCISSOR_TEST: s.scissorTests = on ? 0xffffu : 0u; ctx.markDirty(kDirtyScissor); return true;
    case GL_POLYGON_OFFSET_FILL: s.polygonOffsetFill = on; ctx.markDirty(kDirtyRaster); return true;
    case GL_POLYGON_OFFSET_LINE: s.polygonOffsetLine = on; ctx.markDirty(kDirtyRaster); return true;
    case GL_POLYGON_OFFSET_POINT: s.polygonOffsetPoint = on; ctx.markDirty(kDirtyRaster); return true;
    case GL_DEPTH_CLAMP: s.depthClamp = on; ctx.markDirty(kDirtyRaster); return true;
    case GL_RASTERIZER_DISCARD: s.rasterizerDiscard = on; ctx.markDirty(kDirtyBlend); return true;
    case GL_PROGRAM_POINT_SIZE: s.programPointSize = on; ctx.markDirty(kDirtyDriverUniforms); return true;
    case GL_LINE_SMOOTH: s.lineSmooth = on; return true;
    case GL_POLYGON_SMOOTH: s.polygonSmooth = on; return true;
    case GL_PRIMITIVE_RESTART: s.primitiveRestart = on; return true;
    case GL_TEXTURE_CUBE_MAP_SEAMLESS: s.textureCubeMapSeamless = on; return true;
    case GL_COLOR_LOGIC_OP: s.colorLogicOp = on; ctx.markDirty(kDirtyBlend); return true;
    case GL_DITHER: s.dither = on; return true;
    case GL_FRAMEBUFFER_SRGB: s.framebufferSRGB = on; ctx.markDirty(kDirtyFramebuffer); return true;
    case GL_MULTISAMPLE: s.multisample = on; ctx.markDirty(kDirtyMultisample); return true;
    case GL_SAMPLE_ALPHA_TO_COVERAGE: s.sampleAlphaToCoverage = on; ctx.markDirty(kDirtyMultisample); return true;
    case GL_SAMPLE_ALPHA_TO_ONE: s.sampleAlphaToOne = on; ctx.markDirty(kDirtyMultisample); return true;
    case GL_SAMPLE_COVERAGE: s.sampleCoverage = on; ctx.markDirty(kDirtyMultisample); return true;
    case GL_SAMPLE_MASK: s.sampleMask = on; ctx.markDirty(kDirtyMultisample); return true;
    default: break;
    }
    if (cap >= GL_CLIP_DISTANCE0 && cap < GL_CLIP_DISTANCE0 + 8) {
        const uint32_t bit = 1u << (cap - GL_CLIP_DISTANCE0);
        s.clipDistanceEnables = on ? (s.clipDistanceEnables | bit) : (s.clipDistanceEnables & ~bit);
        ctx.markDirty(kDirtyDriverUniforms);
        return true;
    }
    if (ctx.version() >= 43 && cap == GL_PRIMITIVE_RESTART_FIXED_INDEX) {
        s.primitiveRestartFixedIndex = on;
        return true;
    }
    if (ctx.sampleShading() && cap == GL_SAMPLE_SHADING) {
        s.sampleShading = on;
        ctx.markDirty(kDirtyMultisample);
        return true;
    }
    if (cap == GL_DEBUG_OUTPUT) { s.debugOutput = on; return true; }
    if (cap == GL_DEBUG_OUTPUT_SYNCHRONOUS) { s.debugOutputSynchronous = on; return true; }
    return setFixedCapability(ctx, cap, on);
}

bool getCapability(Context& ctx, GLenum cap, bool& value) {
    const ContextState& s = ctx.state;
    if (ctx.isEs() && !esCapability(ctx, cap)) return false;
    if (cap == 0x9285 && ctx.isEs()) { value = s.blendAdvancedCoherent; return true; }
    switch (cap) {
    case GL_BLEND: value = s.blendEnable & 1; return true;
    case GL_CULL_FACE: value = s.cullFace; return true;
    case GL_DEPTH_TEST: value = s.depthTest; return true;
    case GL_STENCIL_TEST: value = s.stencilTest; return true;
    case GL_SCISSOR_TEST: value = (s.scissorTests & 1u); return true;
    case GL_POLYGON_OFFSET_FILL: value = s.polygonOffsetFill; return true;
    case GL_POLYGON_OFFSET_LINE: value = s.polygonOffsetLine; return true;
    case GL_POLYGON_OFFSET_POINT: value = s.polygonOffsetPoint; return true;
    case GL_DEPTH_CLAMP: value = s.depthClamp; return true;
    case GL_RASTERIZER_DISCARD: value = s.rasterizerDiscard; return true;
    case GL_PROGRAM_POINT_SIZE: value = s.programPointSize; return true;
    case GL_LINE_SMOOTH: value = s.lineSmooth; return true;
    case GL_POLYGON_SMOOTH: value = s.polygonSmooth; return true;
    case GL_PRIMITIVE_RESTART: value = s.primitiveRestart; return true;
    case GL_TEXTURE_CUBE_MAP_SEAMLESS: value = s.textureCubeMapSeamless; return true;
    case GL_COLOR_LOGIC_OP: value = s.colorLogicOp; return true;
    case GL_DITHER: value = s.dither; return true;
    case GL_FRAMEBUFFER_SRGB: value = s.framebufferSRGB; return true;
    case GL_MULTISAMPLE: value = s.multisample; return true;
    case GL_SAMPLE_ALPHA_TO_COVERAGE: value = s.sampleAlphaToCoverage; return true;
    case GL_SAMPLE_ALPHA_TO_ONE: value = s.sampleAlphaToOne; return true;
    case GL_SAMPLE_COVERAGE: value = s.sampleCoverage; return true;
    case GL_SAMPLE_MASK: value = s.sampleMask; return true;
    case GL_DEBUG_OUTPUT: value = s.debugOutput; return true;
    case GL_DEBUG_OUTPUT_SYNCHRONOUS: value = s.debugOutputSynchronous; return true;
    default: break;
    }
    if (cap >= GL_CLIP_DISTANCE0 && cap < GL_CLIP_DISTANCE0 + 8) {
        value = s.clipDistanceEnables & (1u << (cap - GL_CLIP_DISTANCE0));
        return true;
    }
    if (ctx.version() >= 43 && cap == GL_PRIMITIVE_RESTART_FIXED_INDEX) {
        value = s.primitiveRestartFixedIndex;
        return true;
    }
    if (ctx.sampleShading() && cap == GL_SAMPLE_SHADING) {
        value = s.sampleShading;
        return true;
    }
    return getFixedCapability(ctx, cap, value) || getClientCapability(ctx, cap, value);
}

}  // namespace

void Enable(Context& ctx, GLenum cap) {
    if (!setCapability(ctx, cap, true)) ctx.setError(GL_INVALID_ENUM);
}

void Disable(Context& ctx, GLenum cap) {
    if (!setCapability(ctx, cap, false)) ctx.setError(GL_INVALID_ENUM);
}

GLboolean IsEnabled(Context& ctx, GLenum cap) {
    bool value = false;
    if (!getCapability(ctx, cap, value)) {
        ctx.setError(GL_INVALID_ENUM);
        return GL_FALSE;
    }
    return value ? GL_TRUE : GL_FALSE;
}

void Enablei(Context& ctx, GLenum target, GLuint index) {
    if (target == GL_BLEND) {
        if (index >= ContextState::kMaxDrawBuffers) return ctx.setError(GL_INVALID_VALUE);
        ctx.state.blendEnable |= static_cast<uint8_t>(1u << index);
        ctx.markDirty(kDirtyBlend);
        return;
    }
    if (target == GL_SCISSOR_TEST) {
        if (index >= ContextState::kMaxViewports) return ctx.setError(GL_INVALID_VALUE);
        ctx.state.scissorTests |= 1u << index;
        ctx.markDirty(kDirtyScissor);
        return;
    }
    ctx.setError(GL_INVALID_ENUM);
}

void Disablei(Context& ctx, GLenum target, GLuint index) {
    if (target == GL_BLEND) {
        if (index >= ContextState::kMaxDrawBuffers) return ctx.setError(GL_INVALID_VALUE);
        ctx.state.blendEnable &= static_cast<uint8_t>(~(1u << index));
        ctx.markDirty(kDirtyBlend);
        return;
    }
    if (target == GL_SCISSOR_TEST) {
        if (index >= ContextState::kMaxViewports) return ctx.setError(GL_INVALID_VALUE);
        ctx.state.scissorTests &= ~(1u << index);
        ctx.markDirty(kDirtyScissor);
        return;
    }
    ctx.setError(GL_INVALID_ENUM);
}

GLboolean IsEnabledi(Context& ctx, GLenum target, GLuint index) {
    if (target == GL_BLEND) {
        if (index >= ContextState::kMaxDrawBuffers) {
            ctx.setError(GL_INVALID_VALUE);
            return GL_FALSE;
        }
        return (ctx.state.blendEnable >> index) & 1 ? GL_TRUE : GL_FALSE;
    }
    if (target == GL_SCISSOR_TEST) {
        if (index >= ContextState::kMaxViewports) {
            ctx.setError(GL_INVALID_VALUE);
            return GL_FALSE;
        }
        return (ctx.state.scissorTests >> index) & 1 ? GL_TRUE : GL_FALSE;
    }
    ctx.setError(GL_INVALID_ENUM);
    return GL_FALSE;
}

// --- viewports, depth ranges and scissors (GL 4.1 viewport arrays; the
// non-indexed commands set every index).

namespace {

constexpr GLfloat kMaxViewportDim = 16384.0f, kViewportBoundsMin = -32768.0f, kViewportBoundsMax = 32767.0f;

void setViewport(Context& ctx, GLuint index, GLfloat x, GLfloat y, GLfloat w, GLfloat h) {
    GLfloat* v = ctx.state.viewports[index];
    v[0] = std::clamp(x, kViewportBoundsMin, kViewportBoundsMax);
    v[1] = std::clamp(y, kViewportBoundsMin, kViewportBoundsMax);
    v[2] = std::min(w, kMaxViewportDim);
    v[3] = std::min(h, kMaxViewportDim);
}

bool viewportArrays(Context& ctx, const char* command) {
    if (ctx.version() >= 41 || ctx.hasExtension("GL_ARB_viewport_array")) return true;
    ctx.unimplemented(command);
    return false;
}

}  // namespace

void Viewport(Context& ctx, GLint x, GLint y, GLsizei width, GLsizei height) {
    if (width < 0 || height < 0) return ctx.setError(GL_INVALID_VALUE);
    for (GLuint i = 0; i < ContextState::kMaxViewports; ++i)
        setViewport(ctx, i, static_cast<GLfloat>(x), static_cast<GLfloat>(y), static_cast<GLfloat>(width), static_cast<GLfloat>(height));
    ctx.markDirty(kDirtyViewport);
}

void ViewportArrayv(Context& ctx, GLuint first, GLsizei count, const GLfloat* v) {
    if (!viewportArrays(ctx, "glViewportArrayv")) return;
    if (count < 0 || first + static_cast<uint64_t>(count) > ContextState::kMaxViewports) return ctx.setError(GL_INVALID_VALUE);
    for (GLsizei i = 0; i < count; ++i)
        if (v[i * 4 + 2] < 0 || v[i * 4 + 3] < 0) return ctx.setError(GL_INVALID_VALUE);
    for (GLsizei i = 0; i < count; ++i) setViewport(ctx, first + static_cast<GLuint>(i), v[i * 4], v[i * 4 + 1], v[i * 4 + 2], v[i * 4 + 3]);
    ctx.markDirty(kDirtyViewport);
}

void ViewportIndexedf(Context& ctx, GLuint index, GLfloat x, GLfloat y, GLfloat w, GLfloat h) {
    if (!viewportArrays(ctx, "glViewportIndexedf")) return;
    if (index >= ContextState::kMaxViewports || w < 0 || h < 0) return ctx.setError(GL_INVALID_VALUE);
    setViewport(ctx, index, x, y, w, h);
    ctx.markDirty(kDirtyViewport);
}

void ViewportIndexedfv(Context& ctx, GLuint index, const GLfloat* v) {
    if (!viewportArrays(ctx, "glViewportIndexedfv")) return;
    ViewportIndexedf(ctx, index, v[0], v[1], v[2], v[3]);
}

void DepthRange(Context& ctx, GLdouble n, GLdouble f) {
    for (auto& range : ctx.state.depthRanges) {
        range[0] = std::clamp(n, 0.0, 1.0);
        range[1] = std::clamp(f, 0.0, 1.0);
    }
    ctx.markDirty(kDirtyViewport);
}

void DepthRangef(Context& ctx, GLfloat n, GLfloat f) { DepthRange(ctx, n, f); }

void DepthRangeArrayv(Context& ctx, GLuint first, GLsizei count, const GLdouble* v) {
    if (!viewportArrays(ctx, "glDepthRangeArrayv")) return;
    if (count < 0 || first + static_cast<uint64_t>(count) > ContextState::kMaxViewports) return ctx.setError(GL_INVALID_VALUE);
    for (GLsizei i = 0; i < count; ++i) {
        ctx.state.depthRanges[first + static_cast<GLuint>(i)][0] = std::clamp(v[i * 2], 0.0, 1.0);
        ctx.state.depthRanges[first + static_cast<GLuint>(i)][1] = std::clamp(v[i * 2 + 1], 0.0, 1.0);
    }
    ctx.markDirty(kDirtyViewport);
}

void DepthRangeIndexed(Context& ctx, GLuint index, GLdouble n, GLdouble f) {
    if (!viewportArrays(ctx, "glDepthRangeIndexed")) return;
    if (index >= ContextState::kMaxViewports) return ctx.setError(GL_INVALID_VALUE);
    ctx.state.depthRanges[index][0] = std::clamp(n, 0.0, 1.0);
    ctx.state.depthRanges[index][1] = std::clamp(f, 0.0, 1.0);
    ctx.markDirty(kDirtyViewport);
}

// GL_ARB_viewport_array's NV_depth_buffer_float interaction: the same ranges.
void DepthRangeArraydvNV(Context& ctx, GLuint first, GLsizei count, const GLdouble* v) { DepthRangeArrayv(ctx, first, count, v); }

void DepthRangeIndexeddNV(Context& ctx, GLuint index, GLdouble n, GLdouble f) { DepthRangeIndexed(ctx, index, n, f); }

void Scissor(Context& ctx, GLint x, GLint y, GLsizei width, GLsizei height) {
    if (width < 0 || height < 0) return ctx.setError(GL_INVALID_VALUE);
    for (auto& box : ctx.state.scissors) {
        box[0] = x;
        box[1] = y;
        box[2] = width;
        box[3] = height;
    }
    ctx.markDirty(kDirtyScissor);
}

void ScissorArrayv(Context& ctx, GLuint first, GLsizei count, const GLint* v) {
    if (!viewportArrays(ctx, "glScissorArrayv")) return;
    if (count < 0 || first + static_cast<uint64_t>(count) > ContextState::kMaxViewports) return ctx.setError(GL_INVALID_VALUE);
    for (GLsizei i = 0; i < count; ++i)
        if (v[i * 4 + 2] < 0 || v[i * 4 + 3] < 0) return ctx.setError(GL_INVALID_VALUE);
    for (GLsizei i = 0; i < count; ++i)
        for (int c = 0; c < 4; ++c) ctx.state.scissors[first + static_cast<GLuint>(i)][c] = v[i * 4 + c];
    ctx.markDirty(kDirtyScissor);
}

void ScissorIndexed(Context& ctx, GLuint index, GLint left, GLint bottom, GLsizei width, GLsizei height) {
    if (!viewportArrays(ctx, "glScissorIndexed")) return;
    if (index >= ContextState::kMaxViewports || width < 0 || height < 0) return ctx.setError(GL_INVALID_VALUE);
    GLint* box = ctx.state.scissors[index];
    box[0] = left;
    box[1] = bottom;
    box[2] = width;
    box[3] = height;
    ctx.markDirty(kDirtyScissor);
}

void ScissorIndexedv(Context& ctx, GLuint index, const GLint* v) {
    if (!viewportArrays(ctx, "glScissorIndexedv")) return;
    ScissorIndexed(ctx, index, v[0], v[1], v[2], v[3]);
}

void ClearColor(Context& ctx, GLfloat r, GLfloat g, GLfloat b, GLfloat a) {
    ctx.state.clearColor[0] = r;
    ctx.state.clearColor[1] = g;
    ctx.state.clearColor[2] = b;
    ctx.state.clearColor[3] = a;
}

// GL_EXT_texture_integer: the clear colour of integer colour buffers.
void ClearColorIiEXT(Context& ctx, GLint r, GLint g, GLint b, GLint a) {
    const GLint v[4] = {r, g, b, a};
    for (int i = 0; i < 4; ++i) ctx.state.clearColorInteger[i] = v[i];
    ctx.state.clearColorIntegerSet = true;
}

void ClearColorIuiEXT(Context& ctx, GLuint r, GLuint g, GLuint b, GLuint a) {
    const GLuint v[4] = {r, g, b, a};
    for (int i = 0; i < 4; ++i) ctx.state.clearColorInteger[i] = v[i];
    ctx.state.clearColorIntegerSet = true;
}

void ClearDepth(Context& ctx, GLdouble depth) { ctx.state.clearDepth = std::clamp(depth, 0.0, 1.0); }
void ClearDepthf(Context& ctx, GLfloat depth) { ClearDepth(ctx, depth); }
void ClearStencil(Context& ctx, GLint s) { ctx.state.clearStencil = s; }

void CullFace(Context& ctx, GLenum mode) {
    if (mode != GL_FRONT && mode != GL_BACK && mode != GL_FRONT_AND_BACK) return ctx.setError(GL_INVALID_ENUM);
    ctx.state.cullFaceMode = mode;
    ctx.markDirty(kDirtyRaster);
}

void FrontFace(Context& ctx, GLenum mode) {
    if (mode != GL_CW && mode != GL_CCW) return ctx.setError(GL_INVALID_ENUM);
    ctx.state.frontFace = mode;
    ctx.markDirty(kDirtyRaster);
}

void PolygonMode(Context& ctx, GLenum face, GLenum mode) {
    if (mode != GL_POINT && mode != GL_LINE && mode != GL_FILL) return ctx.setError(GL_INVALID_ENUM);
    if (face == GL_FRONT_AND_BACK) {
        ctx.state.polygonMode = ctx.state.polygonModeBack = mode;
    } else if (ctx.isCompat() && (face == GL_FRONT || face == GL_BACK)) {
        (face == GL_FRONT ? ctx.state.polygonMode : ctx.state.polygonModeBack) = mode;
    } else {
        return ctx.setError(GL_INVALID_ENUM);
    }
    ctx.markDirty(kDirtyRaster);
}

void PolygonOffset(Context& ctx, GLfloat factor, GLfloat units) {
    ctx.state.polygonOffsetFactor = factor;
    ctx.state.polygonOffsetUnits = units;
    ctx.state.polygonOffsetClamp = 0.0f;
    ctx.markDirty(kDirtyRaster);
}

void PolygonOffsetClamp(Context& ctx, GLfloat factor, GLfloat units, GLfloat clamp) {
    PolygonOffset(ctx, factor, units);
    ctx.state.polygonOffsetClamp = clamp;
}

void LineWidth(Context& ctx, GLfloat width) {
    if (width <= 0.0f) return ctx.setError(GL_INVALID_VALUE);
    if (ctx.config().forwardCompatible && width > 1.0f) return ctx.setError(GL_INVALID_VALUE);
    ctx.state.lineWidth = width;
}

void PointSize(Context& ctx, GLfloat size) {
    if (size <= 0.0f) return ctx.setError(GL_INVALID_VALUE);
    ctx.state.pointSize = size;
    ctx.markDirty(kDirtyDriverUniforms);
}

void PointParameterf(Context& ctx, GLenum pname, GLfloat param) {
    switch (pname) {
    case GL_POINT_FADE_THRESHOLD_SIZE:
        if (param < 0.0f) return ctx.setError(GL_INVALID_VALUE);
        ctx.state.pointFadeThresholdSize = param;
        return;
    case GL_POINT_SPRITE_COORD_ORIGIN:
        if (param != GL_LOWER_LEFT && param != GL_UPPER_LEFT) return ctx.setError(GL_INVALID_ENUM);
        ctx.state.pointSpriteCoordOrigin = static_cast<GLenum>(param);
        ctx.markDirty(kDirtyDriverUniforms);
        return;
    case GL_POINT_SIZE_MIN: case GL_POINT_SIZE_MAX:  // compatibility profile
        if (!ctx.isCompat()) break;
        if (param < 0.0f) return ctx.setError(GL_INVALID_VALUE);
        (pname == GL_POINT_SIZE_MIN ? ctx.state.fixed.pointSizeMin : ctx.state.fixed.pointSizeMax) = param;
        ++ctx.state.fixed.version;
        return;
    default: break;
    }
    ctx.setError(GL_INVALID_ENUM);
}

void PointParameterfv(Context& ctx, GLenum pname, const GLfloat* params) {
    if (!params) return;
    if (pname == GL_POINT_DISTANCE_ATTENUATION && ctx.isCompat()) {
        std::copy(params, params + 3, ctx.state.fixed.pointDistanceAttenuation);
        ++ctx.state.fixed.version;
        return;
    }
    PointParameterf(ctx, pname, params[0]);
}

void PointParameteri(Context& ctx, GLenum pname, GLint param) { PointParameterf(ctx, pname, static_cast<GLfloat>(param)); }

void PointParameteriv(Context& ctx, GLenum pname, const GLint* params) {
    if (!params) return;
    if (pname == GL_POINT_DISTANCE_ATTENUATION) {
        const GLfloat v[3] = {static_cast<GLfloat>(params[0]), static_cast<GLfloat>(params[1]), static_cast<GLfloat>(params[2])};
        return PointParameterfv(ctx, pname, v);
    }
    PointParameteri(ctx, pname, params[0]);
}

void DepthFunc(Context& ctx, GLenum func) {
    if (!validCompare(func)) return ctx.setError(GL_INVALID_ENUM);
    ctx.state.depthFunc = func;
    ctx.markDirty(kDirtyDepthStencil);
}

void DepthMask(Context& ctx, GLboolean flag) {
    ctx.state.depthMask = flag != GL_FALSE;
    ctx.markDirty(kDirtyDepthStencil);
}

void StencilFuncSeparate(Context& ctx, GLenum face, GLenum func, GLint ref, GLuint mask) {
    if (face != GL_FRONT && face != GL_BACK && face != GL_FRONT_AND_BACK) return ctx.setError(GL_INVALID_ENUM);
    if (!validCompare(func)) return ctx.setError(GL_INVALID_ENUM);
    for (StencilFace* f : {&ctx.state.stencilFront, &ctx.state.stencilBack}) {
        if ((face == GL_FRONT && f != &ctx.state.stencilFront) || (face == GL_BACK && f != &ctx.state.stencilBack)) continue;
        f->func = func;
        f->ref = ref;
        f->valueMask = mask;
    }
    ctx.markDirty(kDirtyDepthStencil | kDirtyStencilRef);
}

void StencilFunc(Context& ctx, GLenum func, GLint ref, GLuint mask) { StencilFuncSeparate(ctx, GL_FRONT_AND_BACK, func, ref, mask); }

void StencilOpSeparate(Context& ctx, GLenum face, GLenum sfail, GLenum dpfail, GLenum dppass) {
    if (face != GL_FRONT && face != GL_BACK && face != GL_FRONT_AND_BACK) return ctx.setError(GL_INVALID_ENUM);
    if (!validStencilOp(sfail) || !validStencilOp(dpfail) || !validStencilOp(dppass)) return ctx.setError(GL_INVALID_ENUM);
    for (StencilFace* f : {&ctx.state.stencilFront, &ctx.state.stencilBack}) {
        if ((face == GL_FRONT && f != &ctx.state.stencilFront) || (face == GL_BACK && f != &ctx.state.stencilBack)) continue;
        f->sfail = sfail;
        f->dpfail = dpfail;
        f->dppass = dppass;
    }
    ctx.markDirty(kDirtyDepthStencil);
}

void StencilOp(Context& ctx, GLenum sfail, GLenum dpfail, GLenum dppass) { StencilOpSeparate(ctx, GL_FRONT_AND_BACK, sfail, dpfail, dppass); }

void StencilMaskSeparate(Context& ctx, GLenum face, GLuint mask) {
    if (face != GL_FRONT && face != GL_BACK && face != GL_FRONT_AND_BACK) return ctx.setError(GL_INVALID_ENUM);
    if (face != GL_BACK) ctx.state.stencilFront.writeMask = mask;
    if (face != GL_FRONT) ctx.state.stencilBack.writeMask = mask;
    ctx.markDirty(kDirtyDepthStencil);
}

void StencilMask(Context& ctx, GLuint mask) { StencilMaskSeparate(ctx, GL_FRONT_AND_BACK, mask); }

namespace {

// `buffer` < 0: every draw buffer.
void blendFunc(Context& ctx, int buffer, GLenum srcRGB, GLenum dstRGB, GLenum srcAlpha, GLenum dstAlpha) {
    if (buffer >= ContextState::kMaxDrawBuffers) return ctx.setError(GL_INVALID_VALUE);
    if (!validBlendFactor(srcRGB) || !validBlendFactor(dstRGB) || !validBlendFactor(srcAlpha) || !validBlendFactor(dstAlpha))
        return ctx.setError(GL_INVALID_ENUM);
    for (int i = 0; i < ContextState::kMaxDrawBuffers; ++i) {
        if (buffer >= 0 && i != buffer) continue;
        ContextState::Blend& b = ctx.state.blend[i];
        b.srcRGB = srcRGB;
        b.dstRGB = dstRGB;
        b.srcAlpha = srcAlpha;
        b.dstAlpha = dstAlpha;
    }
    ctx.markDirty(kDirtyBlend);
}

void blendEquation(Context& ctx, int buffer, GLenum modeRGB, GLenum modeAlpha) {
    if (buffer >= ContextState::kMaxDrawBuffers) return ctx.setError(GL_INVALID_VALUE);
    // KHR_blend_equation_advanced: glBlendEquation(i) only (both parts).
    const bool advanced = modeRGB == modeAlpha && advancedBlendIndex(modeRGB) >= 0 && ctx.advertises("GL_KHR_blend_equation_advanced");
    if (!advanced && (!validBlendEquation(modeRGB) || !validBlendEquation(modeAlpha))) return ctx.setError(GL_INVALID_ENUM);
    for (int i = 0; i < ContextState::kMaxDrawBuffers; ++i) {
        if (buffer >= 0 && i != buffer) continue;
        ctx.state.blend[i].equationRGB = modeRGB;
        ctx.state.blend[i].equationAlpha = modeAlpha;
    }
    ctx.markDirty(kDirtyBlend);
}

}  // namespace

void BlendFuncSeparate(Context& ctx, GLenum srcRGB, GLenum dstRGB, GLenum srcAlpha, GLenum dstAlpha) {
    blendFunc(ctx, -1, srcRGB, dstRGB, srcAlpha, dstAlpha);
}

void BlendFunc(Context& ctx, GLenum sfactor, GLenum dfactor) { blendFunc(ctx, -1, sfactor, dfactor, sfactor, dfactor); }

void BlendEquationSeparate(Context& ctx, GLenum modeRGB, GLenum modeAlpha) {
    if (advancedBlendIndex(modeRGB) >= 0 || advancedBlendIndex(modeAlpha) >= 0) return ctx.setError(GL_INVALID_ENUM);
    blendEquation(ctx, -1, modeRGB, modeAlpha);
}

void BlendEquation(Context& ctx, GLenum mode) { blendEquation(ctx, -1, mode, mode); }

// GL_ARB_draw_buffers_blend (the core 4.0 names are aliases of these).
void BlendFunci(Context& ctx, GLuint buf, GLenum src, GLenum dst) {
    blendFunc(ctx, static_cast<int>(std::min<GLuint>(buf, 1024)), src, dst, src, dst);
}

void BlendFuncSeparatei(Context& ctx, GLuint buf, GLenum srcRGB, GLenum dstRGB, GLenum srcAlpha, GLenum dstAlpha) {
    blendFunc(ctx, static_cast<int>(std::min<GLuint>(buf, 1024)), srcRGB, dstRGB, srcAlpha, dstAlpha);
}

void BlendEquationi(Context& ctx, GLuint buf, GLenum mode) {
    blendEquation(ctx, static_cast<int>(std::min<GLuint>(buf, 1024)), mode, mode);
}

void BlendEquationSeparatei(Context& ctx, GLuint buf, GLenum modeRGB, GLenum modeAlpha) {
    if (advancedBlendIndex(modeRGB) >= 0 || advancedBlendIndex(modeAlpha) >= 0) return ctx.setError(GL_INVALID_ENUM);
    blendEquation(ctx, static_cast<int>(std::min<GLuint>(buf, 1024)), modeRGB, modeAlpha);
}

void BlendColor(Context& ctx, GLfloat r, GLfloat g, GLfloat b, GLfloat a) {
    // GL 3.0+ does not clamp the constant colour.
    ctx.state.blendColor[0] = r;
    ctx.state.blendColor[1] = g;
    ctx.state.blendColor[2] = b;
    ctx.state.blendColor[3] = a;
    ctx.markDirty(kDirtyBlendColor);
}

void ColorMask(Context& ctx, GLboolean r, GLboolean g, GLboolean b, GLboolean a) {
    for (auto& mask : ctx.state.colorMask) mask = {r != GL_FALSE, g != GL_FALSE, b != GL_FALSE, a != GL_FALSE};
    ctx.markDirty(kDirtyBlend);
}

void ColorMaski(Context& ctx, GLuint index, GLboolean r, GLboolean g, GLboolean b, GLboolean a) {
    if (index >= ContextState::kMaxDrawBuffers) return ctx.setError(GL_INVALID_VALUE);
    ctx.state.colorMask[index] = {r != GL_FALSE, g != GL_FALSE, b != GL_FALSE, a != GL_FALSE};
    ctx.markDirty(kDirtyBlend);
}

void LogicOp(Context& ctx, GLenum opcode) {
    if (opcode < GL_CLEAR || opcode > GL_SET) return ctx.setError(GL_INVALID_ENUM);
    ctx.state.logicOp = opcode;
    ctx.markDirty(kDirtyBlend);
}

void SampleCoverage(Context& ctx, GLfloat value, GLboolean invert) {
    ctx.state.sampleCoverageValue = std::clamp(value, 0.0f, 1.0f);
    ctx.state.sampleCoverageInvert = invert != GL_FALSE;
    ctx.markDirty(kDirtyMultisample);
}

void SampleMaski(Context& ctx, GLuint maskNumber, GLbitfield mask) {
    if (maskNumber != 0) return ctx.setError(GL_INVALID_VALUE);
    ctx.state.sampleMaskValue = mask;
    ctx.markDirty(kDirtyMultisample);
}

void ProvokingVertex(Context& ctx, GLenum mode) {
    if (mode != GL_FIRST_VERTEX_CONVENTION && mode != GL_LAST_VERTEX_CONVENTION) return ctx.setError(GL_INVALID_ENUM);
    ctx.state.provokingVertex = mode;
}

void ClipControl(Context& ctx, GLenum origin, GLenum depth) {
    if ((origin != GL_LOWER_LEFT && origin != GL_UPPER_LEFT) ||
        (depth != GL_NEGATIVE_ONE_TO_ONE && depth != GL_ZERO_TO_ONE))
        return ctx.setError(GL_INVALID_ENUM);
    ctx.state.clipOrigin = origin;
    ctx.state.clipDepthMode = depth;
}

void PrimitiveRestartIndex(Context& ctx, GLuint index) { ctx.state.primitiveRestartIndex = index; }

void ClampColor(Context& ctx, GLenum target, GLenum clamp) {
    if (target != GL_CLAMP_READ_COLOR && !(ctx.isCompat() && (target == GL_CLAMP_VERTEX_COLOR || target == GL_CLAMP_FRAGMENT_COLOR)))
        return ctx.setError(GL_INVALID_ENUM);
    if (clamp != GL_TRUE && clamp != GL_FALSE && clamp != GL_FIXED_ONLY) return ctx.setError(GL_INVALID_ENUM);
    if (target == GL_CLAMP_READ_COLOR) ctx.state.clampReadColorMode = clamp;
}

void Hint(Context& ctx, GLenum target, GLenum mode) {
    if (mode != GL_FASTEST && mode != GL_NICEST && mode != GL_DONT_CARE) return ctx.setError(GL_INVALID_ENUM);
    switch (target) {
    case GL_LINE_SMOOTH_HINT: case GL_POLYGON_SMOOTH_HINT: case GL_TEXTURE_COMPRESSION_HINT:
        if (ctx.isEs()) break;  // OpenGL ES has only the mipmap and derivative hints
        (target == GL_LINE_SMOOTH_HINT      ? ctx.state.hintLineSmooth
         : target == GL_POLYGON_SMOOTH_HINT ? ctx.state.hintPolygonSmooth
                                            : ctx.state.hintTextureCompression) = mode;
        return;
    case GL_FRAGMENT_SHADER_DERIVATIVE_HINT: ctx.state.hintFragmentShaderDerivative = mode; return;
    case GL_GENERATE_MIPMAP_HINT:
        if (ctx.isCompat() || ctx.isEs()) { ctx.state.hintGenerateMipmap = mode; return; }
        break;
    case GL_PERSPECTIVE_CORRECTION_HINT: case GL_POINT_SMOOTH_HINT: case GL_FOG_HINT:  // compatibility profile
        if (!ctx.isCompat()) break;
        (target == GL_PERSPECTIVE_CORRECTION_HINT ? ctx.state.fixed.hintPerspective
         : target == GL_POINT_SMOOTH_HINT         ? ctx.state.fixed.hintPointSmooth
                                                  : ctx.state.fixed.hintFog) = mode;
        return;
    default: break;
    }
    ctx.setError(GL_INVALID_ENUM);
}

void PixelStorei(Context& ctx, GLenum pname, GLint param) {
    PixelStore& pack = ctx.state.pack;
    PixelStore& unpack = ctx.state.unpack;
    auto alignment = [&](GLint& field) {
        if (param != 1 && param != 2 && param != 4 && param != 8) return ctx.setError(GL_INVALID_VALUE);
        field = param;
    };
    auto nonNegative = [&](GLint& field) {
        if (param < 0) return ctx.setError(GL_INVALID_VALUE);
        field = param;
    };
    switch (pname) {
    case GL_PACK_ALIGNMENT: return alignment(pack.alignment);
    case GL_UNPACK_ALIGNMENT: return alignment(unpack.alignment);
    case GL_PACK_ROW_LENGTH: return nonNegative(pack.rowLength);
    case GL_UNPACK_ROW_LENGTH: return nonNegative(unpack.rowLength);
    case GL_PACK_IMAGE_HEIGHT: return nonNegative(pack.imageHeight);
    case GL_UNPACK_IMAGE_HEIGHT: return nonNegative(unpack.imageHeight);
    case GL_PACK_SKIP_PIXELS: return nonNegative(pack.skipPixels);
    case GL_UNPACK_SKIP_PIXELS: return nonNegative(unpack.skipPixels);
    case GL_PACK_SKIP_ROWS: return nonNegative(pack.skipRows);
    case GL_UNPACK_SKIP_ROWS: return nonNegative(unpack.skipRows);
    case GL_PACK_SKIP_IMAGES: return nonNegative(pack.skipImages);
    case GL_UNPACK_SKIP_IMAGES: return nonNegative(unpack.skipImages);
    case GL_UNPACK_COMPRESSED_BLOCK_WIDTH: return nonNegative(unpack.blockWidth);
    case GL_UNPACK_COMPRESSED_BLOCK_HEIGHT: return nonNegative(unpack.blockHeight);
    case GL_UNPACK_COMPRESSED_BLOCK_DEPTH: return nonNegative(unpack.blockDepth);
    case GL_UNPACK_COMPRESSED_BLOCK_SIZE: return nonNegative(unpack.blockSize);
    case GL_PACK_COMPRESSED_BLOCK_WIDTH: return nonNegative(pack.blockWidth);
    case GL_PACK_COMPRESSED_BLOCK_HEIGHT: return nonNegative(pack.blockHeight);
    case GL_PACK_COMPRESSED_BLOCK_DEPTH: return nonNegative(pack.blockDepth);
    case GL_PACK_COMPRESSED_BLOCK_SIZE: return nonNegative(pack.blockSize);
    case GL_PACK_SWAP_BYTES: pack.swapBytes = param != 0; return;
    case GL_UNPACK_SWAP_BYTES: unpack.swapBytes = param != 0; return;
    case GL_PACK_LSB_FIRST: pack.lsbFirst = param != 0; return;
    case GL_UNPACK_LSB_FIRST: unpack.lsbFirst = param != 0; return;
    default: return ctx.setError(GL_INVALID_ENUM);
    }
}

void PixelStoref(Context& ctx, GLenum pname, GLfloat param) {
    PixelStorei(ctx, pname, static_cast<GLint>(std::lround(param)));
}

// GL 4.0 / GL_ARB_sample_shading.
void MinSampleShading(Context& ctx, GLfloat value) {
    if (!ctx.sampleShading()) return ctx.unimplemented("glMinSampleShading");
    ctx.state.minSampleShading = std::clamp(value, 0.0f, 1.0f);
    ctx.markDirty(kDirtyMultisample);
}

GLenum GetError(Context& ctx) { return ctx.takeError(); }

// Drawing to a window's front buffer shows on glFlush and glFinish, as it
// does on Windows (win32u presents with GL_FLUSH_PRESENT likewise): wined3d
// draws a DirectDraw primary surface without a flip chain that way, a movie
// player's video window for one. The default framebuffer's one colour
// buffer is both buffers, so presenting it shows what was drawn.
static bool drawsToFrontBuffer(Context& ctx) {
    Surface* surface = ctx.drawSurface();
    if (!surface || !surface->isWindow() || ctx.state.drawFramebuffer) return false;
    const GLenum buffer = ctx.state.defaultDrawBuffer;
    return buffer == GL_FRONT || buffer == GL_FRONT_LEFT || buffer == GL_FRONT_AND_BACK ||
           (buffer != GL_NONE && !surface->pixelFormat().double_buffer);
}

void Flush(Context& ctx) {
    if (drawsToFrontBuffer(ctx)) ctx.drawSurface()->swap(ctx.stream());
    // With no other context to see the commands, the commit waits for the
    // pass in progress to end (CommandStream::deferFlush): engines flush
    // mid-frame often, and each commit would end the pass.
    else if (ctx.share().currentContexts.load(std::memory_order_relaxed) <= 1) ctx.stream().deferFlush();
    else ctx.stream().flush();
}

void Finish(Context& ctx) {
    if (drawsToFrontBuffer(ctx)) ctx.drawSurface()->swap(ctx.stream());
    finishUnlocked(ctx);
}

const GLubyte* GetString(Context& ctx, GLenum name) {
    switch (name) {
    // WINEMETALGL_VENDOR / WINEMETALGL_RENDERER override the strings for
    // games that refuse unknown vendors (see docs and tests/games notes).
    case GL_VENDOR: {
        static const std::string vendor = [] {
            const char* v = std::getenv("WINEMETALGL_VENDOR");
            return std::string(v && *v ? v : "WineMetalGL");
        }();
        return reinterpret_cast<const GLubyte*>(vendor.c_str());
    }
    case GL_RENDERER: return reinterpret_cast<const GLubyte*>(rendererString().c_str());
    case GL_VERSION: return reinterpret_cast<const GLubyte*>(ctx.versionString().c_str());
    case GL_SHADING_LANGUAGE_VERSION: return reinterpret_cast<const GLubyte*>(ctx.glslVersionString().c_str());
    case GL_EXTENSIONS:
        if (ctx.isDesktopCore()) break;  // core profile: only glGetStringi
        return reinterpret_cast<const GLubyte*>(ctx.extensionString().c_str());
    case GL_PROGRAM_ERROR_STRING_ARB:  // the last glProgramStringARB's error (empty after a load)
        if (ctx.isCore()) break;
        return reinterpret_cast<const GLubyte*>(ctx.arb.errorString.c_str());
    default: break;
    }
    ctx.setError(GL_INVALID_ENUM);
    return nullptr;
}

const GLubyte* GetStringi(Context& ctx, GLenum name, GLuint index) {
    if (name == GL_EXTENSIONS) {
        if (index >= ctx.extensions().size()) {
            ctx.setError(GL_INVALID_VALUE);
            return nullptr;
        }
        return reinterpret_cast<const GLubyte*>(ctx.extensions()[index].c_str());
    }
    if (name == GL_SPIR_V_EXTENSIONS && ctx.glSpirv()) {
        const auto& extensions = shader::spirvExtensions();
        if (index >= extensions.size()) {
            ctx.setError(GL_INVALID_VALUE);
            return nullptr;
        }
        return reinterpret_cast<const GLubyte*>(extensions[index]);
    }
    if (name == GL_SHADING_LANGUAGE_VERSION && ctx.version() >= 43 && !ctx.isEs()) {
        // "4N0 core" for every 4.N up to the context's, then the 3.x and 1.x
        // versions; "" is GLSL 1.10 without a #version directive; "300 es"
        // is GLSL ES 3.00 (GL_ARB_ES3_compatibility) and "310 es" GLSL ES
        // 3.10 (GL 4.5, GL_ARB_ES3_1_compatibility).
        static const char* const versions[] = {"460 core", "450 core", "440 core", "430 core", "420 core",
                                               "410 core", "400 core", "330 core", "150 core", "140",
                                               "130",      "120",      "110",      "",         "300 es",
                                               "310 es"};
        const GLuint skip = static_cast<GLuint>(46 - ctx.version());
        const GLuint count = static_cast<GLuint>(std::size(versions)) - skip - (ctx.version() >= 45 ? 0 : 1);
        if (index >= count) {
            ctx.setError(GL_INVALID_VALUE);
            return nullptr;
        }
        return reinterpret_cast<const GLubyte*>(versions[skip + index]);
    }
    ctx.setError(GL_INVALID_ENUM);
    return nullptr;
}

}  // namespace wmgl::api
