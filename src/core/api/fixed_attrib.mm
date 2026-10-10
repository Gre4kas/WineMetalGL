// glPushAttrib/glPopAttrib and glPushClientAttrib/glPopClientAttrib
// (GL 3.3 compatibility section 6.1.21 and table 6.x "attribute" column).

#include <algorithm>

#include "core/compat/compat_api.h"
#include "core/compat/fixed_state.h"
#include "core/context.h"
#include "gl_api.gen.h"

namespace wmgl::api {
namespace {

bool compatOnly(Context& ctx) {
    if (ctx.isCompat()) return true;
    ctx.setError(GL_INVALID_OPERATION);
    return false;
}

// Restores the groups of `mask` from `from` into `to`.
void restore(GLbitfield mask, const ContextState& from, ContextState& to) {
    FixedState& f = to.fixed;
    const FixedState& g = from.fixed;
    if (mask & GL_CURRENT_BIT) {
        for (int slot = kConvVertex + 1; slot < kAttribSlotCount; ++slot)
            to.genericAttribs[static_cast<size_t>(slot)] = from.genericAttribs[static_cast<size_t>(slot)];
        f.rasterPos = g.rasterPos;
        f.edgeFlag = g.edgeFlag;
    }
    if (mask & GL_POINT_BIT) {
        to.pointSize = from.pointSize;
        to.pointFadeThresholdSize = from.pointFadeThresholdSize;
        to.pointSpriteCoordOrigin = from.pointSpriteCoordOrigin;
        f.pointSmooth = g.pointSmooth;
        f.pointSprite = g.pointSprite;
        f.pointSizeMin = g.pointSizeMin;
        f.pointSizeMax = g.pointSizeMax;
        std::copy(g.pointDistanceAttenuation, g.pointDistanceAttenuation + 3, f.pointDistanceAttenuation);
        for (int u = 0; u < FixedState::kTextureUnits; ++u)
            f.texEnv[static_cast<size_t>(u)].coordReplace = g.texEnv[static_cast<size_t>(u)].coordReplace;
    }
    if (mask & GL_LINE_BIT) {
        to.lineWidth = from.lineWidth;
        to.lineSmooth = from.lineSmooth;
        f.lineStipple = g.lineStipple;
        f.lineStippleFactor = g.lineStippleFactor;
        f.lineStipplePattern = g.lineStipplePattern;
    }
    if (mask & GL_POLYGON_BIT) {
        to.cullFace = from.cullFace;
        to.cullFaceMode = from.cullFaceMode;
        to.frontFace = from.frontFace;
        to.polygonMode = from.polygonMode;
        to.polygonModeBack = from.polygonModeBack;
        to.polygonOffsetFill = from.polygonOffsetFill;
        to.polygonOffsetLine = from.polygonOffsetLine;
        to.polygonOffsetPoint = from.polygonOffsetPoint;
        to.polygonOffsetFactor = from.polygonOffsetFactor;
        to.polygonOffsetUnits = from.polygonOffsetUnits;
        to.polygonSmooth = from.polygonSmooth;
        f.polygonStipple = g.polygonStipple;
    }
    if (mask & GL_PIXEL_MODE_BIT) {
        to.defaultReadBuffer = from.defaultReadBuffer;
        std::copy(g.pixelZoom, g.pixelZoom + 2, f.pixelZoom);
    }
    if (mask & GL_LIGHTING_BIT) {
        f.lighting = g.lighting;
        f.lights = g.lights;
        std::copy(g.lightModelAmbient, g.lightModelAmbient + 4, f.lightModelAmbient);
        f.lightModelLocalViewer = g.lightModelLocalViewer;
        f.lightModelTwoSide = g.lightModelTwoSide;
        f.lightModelColorControl = g.lightModelColorControl;
        f.material[0] = g.material[0];
        f.material[1] = g.material[1];
        f.colorMaterial = g.colorMaterial;
        f.colorMaterialFace = g.colorMaterialFace;
        f.colorMaterialMode = g.colorMaterialMode;
        f.shadeModel = g.shadeModel;
    }
    if (mask & GL_FOG_BIT) {
        f.fog = g.fog;
        f.fogMode = g.fogMode;
        f.fogCoordSource = g.fogCoordSource;
        f.fogDensity = g.fogDensity;
        f.fogStart = g.fogStart;
        f.fogEnd = g.fogEnd;
        std::copy(g.fogColor, g.fogColor + 4, f.fogColor);
    }
    if (mask & GL_DEPTH_BUFFER_BIT) {
        to.depthTest = from.depthTest;
        to.depthFunc = from.depthFunc;
        to.depthMask = from.depthMask;
        to.clearDepth = from.clearDepth;
    }
    if (mask & GL_ACCUM_BUFFER_BIT) std::copy(g.accumClear, g.accumClear + 4, f.accumClear);
    if (mask & GL_STENCIL_BUFFER_BIT) {
        to.stencilTest = from.stencilTest;
        to.stencilFront = from.stencilFront;
        to.stencilBack = from.stencilBack;
        to.clearStencil = from.clearStencil;
    }
    if (mask & GL_VIEWPORT_BIT) {
        std::copy(from.viewport, from.viewport + 4, to.viewport);
        std::copy(from.depthRange, from.depthRange + 2, to.depthRange);
    }
    if (mask & GL_TRANSFORM_BIT) {
        f.matrixMode = g.matrixMode;
        std::copy(&g.clipPlanes[0][0], &g.clipPlanes[0][0] + FixedState::kClipPlanes * 4, &f.clipPlanes[0][0]);
        to.clipDistanceEnables = from.clipDistanceEnables;
        f.normalize = g.normalize;
        f.rescaleNormal = g.rescaleNormal;
        to.depthClamp = from.depthClamp;
    }
    if (mask & GL_ENABLE_BIT) {
        f.alphaTest = g.alphaTest;
        to.blendEnable = from.blendEnable;
        to.clipDistanceEnables = from.clipDistanceEnables;
        f.colorMaterial = g.colorMaterial;
        to.cullFace = from.cullFace;
        to.depthTest = from.depthTest;
        to.dither = from.dither;
        f.fog = g.fog;
        for (int i = 0; i < FixedState::kLights; ++i)
            f.lights[static_cast<size_t>(i)].enabled = g.lights[static_cast<size_t>(i)].enabled;
        f.lighting = g.lighting;
        to.lineSmooth = from.lineSmooth;
        f.lineStipple = g.lineStipple;
        to.colorLogicOp = from.colorLogicOp;
        to.multisample = from.multisample;
        to.sampleAlphaToCoverage = from.sampleAlphaToCoverage;
        to.sampleAlphaToOne = from.sampleAlphaToOne;
        to.sampleCoverage = from.sampleCoverage;
        f.normalize = g.normalize;
        f.rescaleNormal = g.rescaleNormal;
        f.pointSmooth = g.pointSmooth;
        f.pointSprite = g.pointSprite;
        to.polygonOffsetFill = from.polygonOffsetFill;
        to.polygonOffsetLine = from.polygonOffsetLine;
        to.polygonOffsetPoint = from.polygonOffsetPoint;
        to.polygonSmooth = from.polygonSmooth;
        f.polygonStipple = g.polygonStipple;
        to.scissorTest = from.scissorTest;
        to.stencilTest = from.stencilTest;
        f.textureEnables = g.textureEnables;
        for (int u = 0; u < FixedState::kTextureUnits; ++u)
            f.texGen[static_cast<size_t>(u)].enabled = g.texGen[static_cast<size_t>(u)].enabled;
        f.colorSum = g.colorSum;
        to.programPointSize = from.programPointSize;
        to.depthClamp = from.depthClamp;
        to.framebufferSRGB = from.framebufferSRGB;
        to.primitiveRestart = from.primitiveRestart;
    }
    if (mask & GL_COLOR_BUFFER_BIT) {
        f.alphaTest = g.alphaTest;
        f.alphaFunc = g.alphaFunc;
        f.alphaRef = g.alphaRef;
        to.blendEnable = from.blendEnable;
        std::copy(from.blend, from.blend + ContextState::kMaxDrawBuffers, to.blend);
        std::copy(from.blendColor, from.blendColor + 4, to.blendColor);
        to.dither = from.dither;
        to.colorLogicOp = from.colorLogicOp;
        to.logicOp = from.logicOp;
        to.colorMask = from.colorMask;
        std::copy(from.clearColor, from.clearColor + 4, to.clearColor);
        to.defaultDrawBuffer = from.defaultDrawBuffer;
        to.framebufferSRGB = from.framebufferSRGB;
    }
    if (mask & GL_HINT_BIT) {
        f.hintPerspective = g.hintPerspective;
        f.hintPointSmooth = g.hintPointSmooth;
        f.hintFog = g.hintFog;
        to.hintGenerateMipmap = from.hintGenerateMipmap;
        to.hintLineSmooth = from.hintLineSmooth;
        to.hintPolygonSmooth = from.hintPolygonSmooth;
        to.hintTextureCompression = from.hintTextureCompression;
        to.hintFragmentShaderDerivative = from.hintFragmentShaderDerivative;
    }
    if (mask & GL_LIST_BIT) f.listBase = g.listBase;
    if (mask & GL_TEXTURE_BIT) {
        to.activeTexture = from.activeTexture;
        for (size_t u = 0; u < to.textureUnits.size(); ++u) to.textureUnits[u].bound = from.textureUnits[u].bound;
        f.textureEnables = g.textureEnables;
        f.texEnv = g.texEnv;
        f.texGen = g.texGen;
    }
    if (mask & GL_SCISSOR_BIT) {
        to.scissorTest = from.scissorTest;
        std::copy(from.scissor, from.scissor + 4, to.scissor);
    }
    if (mask & GL_MULTISAMPLE_BIT) {
        to.multisample = from.multisample;
        to.sampleAlphaToCoverage = from.sampleAlphaToCoverage;
        to.sampleAlphaToOne = from.sampleAlphaToOne;
        to.sampleCoverage = from.sampleCoverage;
        to.sampleCoverageValue = from.sampleCoverageValue;
        to.sampleCoverageInvert = from.sampleCoverageInvert;
    }
}

}  // namespace

int attribStackDepth(const Context& ctx, bool client) {
    return static_cast<int>(client ? ctx.clientAttribStack.size() : ctx.attribStack.size());
}

void PushAttrib(Context& ctx, GLbitfield mask) {
    if (!compatOnly(ctx)) return;
    if (ctx.attribStack.size() >= static_cast<size_t>(kMaxAttribStackDepth)) return ctx.setError(GL_STACK_OVERFLOW);
    Context::AttribFrame frame;
    frame.mask = mask;
    frame.state = ctx.state;
    ctx.attribStack.push_back(std::move(frame));
}

void PopAttrib(Context& ctx) {
    if (!compatOnly(ctx)) return;
    if (ctx.attribStack.empty()) return ctx.setError(GL_STACK_UNDERFLOW);
    Context::AttribFrame frame = std::move(ctx.attribStack.back());
    ctx.attribStack.pop_back();
    restore(frame.mask, frame.state, ctx.state);
    ++ctx.state.fixed.version;
    ctx.markDirty(kDirtyAll);
}

void PushClientAttrib(Context& ctx, GLbitfield mask) {
    if (!compatOnly(ctx)) return;
    if (ctx.clientAttribStack.size() >= static_cast<size_t>(kMaxAttribStackDepth)) return ctx.setError(GL_STACK_OVERFLOW);
    Context::AttribFrame frame;
    frame.mask = mask;
    if (mask & GL_CLIENT_PIXEL_STORE_BIT) {
        frame.state.pack = ctx.state.pack;
        frame.state.unpack = ctx.state.unpack;
    }
    if (mask & GL_CLIENT_VERTEX_ARRAY_BIT) {
        frame.vertexArray = ctx.state.vertexArray;
        frame.attribs = ctx.state.vertexArray->attribs;
        frame.state.arrayBuffer = ctx.state.arrayBuffer;
        frame.state.fixed.clientActiveTexture = ctx.state.fixed.clientActiveTexture;
    }
    ctx.clientAttribStack.push_back(std::move(frame));
}

void PopClientAttrib(Context& ctx) {
    if (!compatOnly(ctx)) return;
    if (ctx.clientAttribStack.empty()) return ctx.setError(GL_STACK_UNDERFLOW);
    Context::AttribFrame frame = std::move(ctx.clientAttribStack.back());
    ctx.clientAttribStack.pop_back();
    if (frame.mask & GL_CLIENT_PIXEL_STORE_BIT) {
        ctx.state.pack = frame.state.pack;
        ctx.state.unpack = frame.state.unpack;
    }
    if ((frame.mask & GL_CLIENT_VERTEX_ARRAY_BIT) && frame.vertexArray) {
        ctx.state.vertexArray = frame.vertexArray;
        ctx.state.vertexArray->attribs = frame.attribs;
        ++ctx.state.vertexArray->version;
        ctx.state.arrayBuffer = frame.state.arrayBuffer;
        ctx.state.fixed.clientActiveTexture = frame.state.fixed.clientActiveTexture;
    }
    ctx.markDirty(kDirtyVertexInput);
}

}  // namespace wmgl::api
