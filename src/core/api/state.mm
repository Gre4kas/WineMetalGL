// Rasterization, fragment, pixel-store and miscellaneous state commands.

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>

#include "core/compat/compat_api.h"
#include "core/context.h"
#include "core/draw.h"
#include "core/metal/device.h"
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

// Returns true if handled; false means the capability is unknown.
bool setCapability(Context& ctx, GLenum cap, bool on) {
    ContextState& s = ctx.state;
    switch (cap) {
    case GL_BLEND: s.blendEnable = on ? 0xff : 0; ctx.markDirty(kDirtyBlend); return true;
    case GL_CULL_FACE: s.cullFace = on; ctx.markDirty(kDirtyRaster); return true;
    case GL_DEPTH_TEST: s.depthTest = on; ctx.markDirty(kDirtyDepthStencil); return true;
    case GL_STENCIL_TEST: s.stencilTest = on; ctx.markDirty(kDirtyDepthStencil); return true;
    case GL_SCISSOR_TEST: s.scissorTest = on; ctx.markDirty(kDirtyScissor); return true;
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
    if (cap == GL_DEBUG_OUTPUT) { s.debugOutput = on; return true; }
    if (cap == GL_DEBUG_OUTPUT_SYNCHRONOUS) { s.debugOutputSynchronous = on; return true; }
    return setFixedCapability(ctx, cap, on);
}

bool getCapability(Context& ctx, GLenum cap, bool& value) {
    const ContextState& s = ctx.state;
    switch (cap) {
    case GL_BLEND: value = s.blendEnable & 1; return true;
    case GL_CULL_FACE: value = s.cullFace; return true;
    case GL_DEPTH_TEST: value = s.depthTest; return true;
    case GL_STENCIL_TEST: value = s.stencilTest; return true;
    case GL_SCISSOR_TEST: value = s.scissorTest; return true;
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
    if (target == GL_SCISSOR_TEST && index == 0) return Enable(ctx, target);
    ctx.setError(GL_INVALID_ENUM);
}

void Disablei(Context& ctx, GLenum target, GLuint index) {
    if (target == GL_BLEND) {
        if (index >= ContextState::kMaxDrawBuffers) return ctx.setError(GL_INVALID_VALUE);
        ctx.state.blendEnable &= static_cast<uint8_t>(~(1u << index));
        ctx.markDirty(kDirtyBlend);
        return;
    }
    if (target == GL_SCISSOR_TEST && index == 0) return Disable(ctx, target);
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
    if (target == GL_SCISSOR_TEST && index == 0) return IsEnabled(ctx, target);
    ctx.setError(GL_INVALID_ENUM);
    return GL_FALSE;
}

void Viewport(Context& ctx, GLint x, GLint y, GLsizei width, GLsizei height) {
    if (width < 0 || height < 0) return ctx.setError(GL_INVALID_VALUE);
    const GLsizei maxDim = 16384;
    ctx.state.viewport[0] = x;
    ctx.state.viewport[1] = y;
    ctx.state.viewport[2] = std::min(width, maxDim);
    ctx.state.viewport[3] = std::min(height, maxDim);
    ctx.markDirty(kDirtyViewport);
}

void DepthRange(Context& ctx, GLdouble n, GLdouble f) {
    ctx.state.depthRange[0] = std::clamp(n, 0.0, 1.0);
    ctx.state.depthRange[1] = std::clamp(f, 0.0, 1.0);
    ctx.markDirty(kDirtyViewport);
}

void DepthRangef(Context& ctx, GLfloat n, GLfloat f) { DepthRange(ctx, n, f); }

void Scissor(Context& ctx, GLint x, GLint y, GLsizei width, GLsizei height) {
    if (width < 0 || height < 0) return ctx.setError(GL_INVALID_VALUE);
    ctx.state.scissor[0] = x;
    ctx.state.scissor[1] = y;
    ctx.state.scissor[2] = width;
    ctx.state.scissor[3] = height;
    ctx.markDirty(kDirtyScissor);
}

void ClearColor(Context& ctx, GLfloat r, GLfloat g, GLfloat b, GLfloat a) {
    ctx.state.clearColor[0] = r;
    ctx.state.clearColor[1] = g;
    ctx.state.clearColor[2] = b;
    ctx.state.clearColor[3] = a;
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
    if (!validBlendEquation(modeRGB) || !validBlendEquation(modeAlpha)) return ctx.setError(GL_INVALID_ENUM);
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

void BlendEquationSeparate(Context& ctx, GLenum modeRGB, GLenum modeAlpha) { blendEquation(ctx, -1, modeRGB, modeAlpha); }

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
    case GL_LINE_SMOOTH_HINT: ctx.state.hintLineSmooth = mode; return;
    case GL_POLYGON_SMOOTH_HINT: ctx.state.hintPolygonSmooth = mode; return;
    case GL_TEXTURE_COMPRESSION_HINT: ctx.state.hintTextureCompression = mode; return;
    case GL_FRAGMENT_SHADER_DERIVATIVE_HINT: ctx.state.hintFragmentShaderDerivative = mode; return;
    case GL_GENERATE_MIPMAP_HINT:
        if (ctx.isCompat()) { ctx.state.hintGenerateMipmap = mode; return; }
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

GLenum GetError(Context& ctx) { return ctx.takeError(); }

void Flush(Context& ctx) { ctx.stream().flush(); }

void Finish(Context& ctx) { finishUnlocked(ctx); }

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
    case GL_RENDERER: {
        static const std::string renderer = [] {
            const char* r = std::getenv("WINEMETALGL_RENDERER");
            return r && *r ? std::string(r) : std::string(Device::get().name()) + " (WineMetalGL)";
        }();
        return reinterpret_cast<const GLubyte*>(renderer.c_str());
    }
    case GL_VERSION: return reinterpret_cast<const GLubyte*>(ctx.versionString().c_str());
    case GL_SHADING_LANGUAGE_VERSION: return reinterpret_cast<const GLubyte*>(ctx.glslVersionString().c_str());
    case GL_EXTENSIONS:
        if (ctx.isCore()) break;  // core profile: only glGetStringi
        return reinterpret_cast<const GLubyte*>(ctx.extensionString().c_str());
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
    if (name == GL_SHADING_LANGUAGE_VERSION && ctx.version() >= 43) {
        static const char* const versions[] = {"330 core", "150 core", "140", "130", "120", "110", ""};
        if (index >= 7) {
            ctx.setError(GL_INVALID_VALUE);
            return nullptr;
        }
        return reinterpret_cast<const GLubyte*>(versions[index]);
    }
    ctx.setError(GL_INVALID_ENUM);
    return nullptr;
}

}  // namespace wmgl::api
