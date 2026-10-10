// glGet* state queries (GL 3.3 chapter 6) with GL's type conversions.

#include <algorithm>
#include <cmath>
#include <cstring>

#include "core/context.h"
#include "core/draw.h"
#include "core/limits.h"
#include "core/metal/device.h"
#include "gl_api.gen.h"

namespace wmgl::api {

namespace limits = wmgl::limits;

namespace {

struct Value {
    // UInt: 32-bit unsigned state, returned bit-for-bit by GetIntegerv.
    enum Kind { Int, UInt, Int64, Bool, Float, Double, NormFloat } kind = Int;
    int count = 0;
    double v[16] = {};
};

Value ints(std::initializer_list<int64_t> list) {
    Value r;
    r.kind = Value::Int;
    for (int64_t x : list) r.v[r.count++] = static_cast<double>(x);
    return r;
}
Value uints(std::initializer_list<uint32_t> list) {
    Value r;
    r.kind = Value::UInt;
    for (uint32_t x : list) r.v[r.count++] = static_cast<double>(x);
    return r;
}
Value int64v(int64_t x) {
    Value r;
    r.kind = Value::Int64;
    r.v[r.count++] = static_cast<double>(x);
    return r;
}
Value floats(std::initializer_list<double> list, Value::Kind kind = Value::Float) {
    Value r;
    r.kind = kind;
    for (double x : list) r.v[r.count++] = x;
    return r;
}
Value bools(std::initializer_list<bool> list) {
    Value r;
    r.kind = Value::Bool;
    for (bool b : list) r.v[r.count++] = b ? 1.0 : 0.0;
    return r;
}

GLuint nameOf(const RefPtr<Buffer>& b) { return b ? b->name : 0; }

// The preferred (format, type) for reading the current read buffer.
bool implementationReadFormat(Context& ctx, GLenum& format, GLenum& type) {
    if (CheckFramebufferStatus(ctx, GL_READ_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) return false;
    uint32_t level, slice;
    GLenum internalFormat = 0;
    int w, h;
    if (!ctx.draw().readColorTexture(level, slice, internalFormat, w, h)) return false;
    const FormatInfo* info = formatInfo(internalFormat);
    if (!info) return false;
    format = info->isInteger() ? GL_RGBA_INTEGER : GL_RGBA;
    type = info->isSignedInteger() ? GL_INT : info->isInteger() ? GL_UNSIGNED_INT
           : info->isFloat() ? GL_FLOAT : GL_UNSIGNED_BYTE;
    if (info->defaultTransferFormat && info->defaultTransferType && !info->isInteger() && !info->isFloat()) {
        format = info->defaultTransferFormat;
        type = info->defaultTransferType;
    }
    return true;
}

GLint samples(Context& ctx) {
    Framebuffer* fb = ctx.state.drawFramebuffer.get();
    if (!fb) {
        Surface* s = ctx.drawSurface();
        return s && s->samples() > 1 ? static_cast<GLint>(s->samples()) : 0;
    }
    // A complete framebuffer's attachments all have the same sample count.
    auto attachmentSamples = [](const Attachment& a) -> GLint {
        if (a.type == GL_RENDERBUFFER && a.renderbuffer) return a.renderbuffer->samples > 1 ? a.renderbuffer->samples : 0;
        if (a.type == GL_TEXTURE && a.texture) {
            const GLsizei s = a.texture->image(0, a.level).samples;
            return s > 1 ? s : 0;
        }
        return -1;
    };
    for (const Attachment& a : fb->color)
        if (GLint s = attachmentSamples(a); s >= 0) return s;
    for (const Attachment* a : {&fb->depth, &fb->stencil})
        if (GLint s = attachmentSamples(*a); s >= 0) return s;
    return 0;
}

int textureBindingTarget(GLenum pname) {
    switch (pname) {
    case GL_TEXTURE_BINDING_1D: return kTex1D;
    case GL_TEXTURE_BINDING_2D: return kTex2D;
    case GL_TEXTURE_BINDING_3D: return kTex3D;
    case GL_TEXTURE_BINDING_1D_ARRAY: return kTex1DArray;
    case GL_TEXTURE_BINDING_2D_ARRAY: return kTex2DArray;
    case GL_TEXTURE_BINDING_RECTANGLE: return kTexRectangle;
    case GL_TEXTURE_BINDING_CUBE_MAP: return kTexCubeMap;
    case GL_TEXTURE_BINDING_BUFFER: return kTexBuffer;
    case GL_TEXTURE_BINDING_2D_MULTISAMPLE: return kTex2DMultisample;
    case GL_TEXTURE_BINDING_2D_MULTISAMPLE_ARRAY: return kTex2DMultisampleArray;
    default: return -1;
    }
}

bool getState(Context& ctx, GLenum pname, Value& out) {
    const ContextState& s = ctx.state;
    const DeviceCaps& caps = Device::get().caps();
    using namespace limits;
    switch (pname) {
    // --- version and context
    case GL_MAJOR_VERSION: out = ints({ctx.versionMajor()}); return true;
    case GL_MINOR_VERSION: out = ints({ctx.versionMinor()}); return true;
    case GL_CONTEXT_FLAGS:
        out = ints({(ctx.config().forwardCompatible ? GL_CONTEXT_FLAG_FORWARD_COMPATIBLE_BIT : 0) |
                    (ctx.config().debug ? GL_CONTEXT_FLAG_DEBUG_BIT : 0) |
                    (ctx.config().robustAccess ? GL_CONTEXT_FLAG_ROBUST_ACCESS_BIT : 0) |
                    (ctx.config().noError ? GL_CONTEXT_FLAG_NO_ERROR_BIT : 0)});
        return true;
    case GL_CONTEXT_PROFILE_MASK:
        if (ctx.version() < 32) return false;
        out = ints({ctx.isCore() ? GL_CONTEXT_CORE_PROFILE_BIT : GL_CONTEXT_COMPATIBILITY_PROFILE_BIT});
        return true;
    case GL_NUM_EXTENSIONS: out = ints({static_cast<int64_t>(ctx.extensions().size())}); return true;
    case GL_RESET_NOTIFICATION_STRATEGY: out = ints({ctx.config().resetNotification}); return true;
    // --- limits
    case GL_MAX_TEXTURE_SIZE: case GL_MAX_CUBE_MAP_TEXTURE_SIZE: case GL_MAX_RECTANGLE_TEXTURE_SIZE:
    case GL_MAX_RENDERBUFFER_SIZE:
        out = ints({kMaxTextureSize});
        return true;
    case GL_MAX_3D_TEXTURE_SIZE: out = ints({kMax3DTextureSize}); return true;
    case GL_MAX_ARRAY_TEXTURE_LAYERS: out = ints({kMaxArrayLayers}); return true;
    case GL_MAX_TEXTURE_BUFFER_SIZE: out = ints({kMaxTextureBufferSize}); return true;
    case GL_MAX_TEXTURE_LOD_BIAS: out = floats({kMaxTextureLodBias}); return true;
    case GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS: case GL_MAX_COMPUTE_UNIFORM_BLOCKS: case GL_MAX_COMPUTE_TEXTURE_IMAGE_UNITS:
    case GL_MAX_COMPUTE_UNIFORM_COMPONENTS: case GL_MAX_COMBINED_COMPUTE_UNIFORM_COMPONENTS:
    case GL_MAX_COMPUTE_SHARED_MEMORY_SIZE: case GL_MAX_COMPUTE_ATOMIC_COUNTERS: case GL_MAX_COMPUTE_ATOMIC_COUNTER_BUFFERS:
    case GL_MAX_COMPUTE_IMAGE_UNIFORMS: case GL_DISPATCH_INDIRECT_BUFFER_BINDING:
        if (!ctx.computeShaders()) return false;
        switch (pname) {
        case GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS: out = ints({kMaxComputeWorkGroupInvocations}); break;
        case GL_MAX_COMPUTE_UNIFORM_BLOCKS: out = ints({kMaxUniformBlocksPerStage}); break;
        case GL_MAX_COMPUTE_TEXTURE_IMAGE_UNITS: out = ints({kMaxTextureUnitsPerStage}); break;
        case GL_MAX_COMPUTE_UNIFORM_COMPONENTS: out = ints({kMaxDefaultUniformComponents}); break;
        case GL_MAX_COMBINED_COMPUTE_UNIFORM_COMPONENTS:
            out = ints({kMaxDefaultUniformComponents + static_cast<int64_t>(kMaxUniformBlocksPerStage) * kMaxUniformBlockSize / 4});
            break;
        case GL_MAX_COMPUTE_SHARED_MEMORY_SIZE: out = ints({kMaxComputeSharedMemorySize}); break;
        case GL_DISPATCH_INDIRECT_BUFFER_BINDING: out = ints({nameOf(s.dispatchIndirectBuffer)}); break;
        default: out = ints({0}); break;  // atomic counters and images are separate extensions
        }
        return true;
    case GL_MAX_VIEWPORT_DIMS: out = ints({kMaxTextureSize, kMaxTextureSize}); return true;
    case GL_MAX_DRAW_BUFFERS: case GL_MAX_COLOR_ATTACHMENTS: out = ints({kMaxDrawBuffers}); return true;
    case GL_MAX_DUAL_SOURCE_DRAW_BUFFERS: out = ints({1}); return true;
    case GL_MAX_SAMPLES: case GL_MAX_COLOR_TEXTURE_SAMPLES: case GL_MAX_DEPTH_TEXTURE_SAMPLES: case GL_MAX_INTEGER_SAMPLES:
        out = ints({static_cast<int64_t>(caps.maxSamples)});
        return true;
    case GL_MAX_SAMPLE_MASK_WORDS: out = ints({1}); return true;
    case GL_MAX_ELEMENTS_INDICES: case GL_MAX_ELEMENTS_VERTICES: out = ints({1 << 24}); return true;
    case GL_MAX_VERTEX_ATTRIBS: out = ints({kMaxVertexAttribs}); return true;
    case GL_MAX_VERTEX_UNIFORM_COMPONENTS: case GL_MAX_FRAGMENT_UNIFORM_COMPONENTS: case GL_MAX_GEOMETRY_UNIFORM_COMPONENTS:
        out = ints({kMaxDefaultUniformComponents});
        return true;
    case GL_MAX_VERTEX_UNIFORM_VECTORS: case GL_MAX_FRAGMENT_UNIFORM_VECTORS:
        out = ints({kMaxDefaultUniformComponents / 4});
        return true;
    case GL_MAX_VARYING_COMPONENTS: out = ints({64}); return true;
    case GL_MAX_VARYING_VECTORS: out = ints({16}); return true;
    // GL_ARB_ES2_compatibility.
    case GL_SHADER_COMPILER: out = bools({true}); return true;
    case GL_NUM_SHADER_BINARY_FORMATS: out = ints({0}); return true;
    case GL_SHADER_BINARY_FORMATS: out = ints({}); return true;
    case GL_IMPLEMENTATION_COLOR_READ_FORMAT:
    case GL_IMPLEMENTATION_COLOR_READ_TYPE: {
        GLenum format = GL_RGBA, type = GL_UNSIGNED_BYTE;
        if (!implementationReadFormat(ctx, format, type)) {
            ctx.setError(GL_INVALID_OPERATION);
            out = ints({0});
            return true;
        }
        out = ints({pname == GL_IMPLEMENTATION_COLOR_READ_FORMAT ? format : type});
        return true;
    }
    case GL_MAX_VERTEX_OUTPUT_COMPONENTS: out = ints({64}); return true;
    case GL_MAX_GEOMETRY_INPUT_COMPONENTS: out = ints({64}); return true;
    case GL_MAX_GEOMETRY_OUTPUT_COMPONENTS: out = ints({128}); return true;
    case GL_MAX_FRAGMENT_INPUT_COMPONENTS: out = ints({128}); return true;
    case GL_MAX_GEOMETRY_OUTPUT_VERTICES: out = ints({256}); return true;
    case GL_MAX_GEOMETRY_TOTAL_OUTPUT_COMPONENTS: out = ints({1024}); return true;
    case GL_MAX_VERTEX_UNIFORM_BLOCKS: case GL_MAX_GEOMETRY_UNIFORM_BLOCKS: case GL_MAX_FRAGMENT_UNIFORM_BLOCKS:
        out = ints({kMaxUniformBlocksPerStage});
        return true;
    case GL_MAX_COMBINED_UNIFORM_BLOCKS: out = ints({kMaxCombinedUniformBlocks}); return true;
    case GL_MAX_UNIFORM_BUFFER_BINDINGS:
        static_assert(kMaxUniformBufferBindings == ContextState::kMaxUniformBufferBindings);
        out = ints({kMaxUniformBufferBindings});
        return true;
    case GL_MAX_UNIFORM_BLOCK_SIZE: out = ints({kMaxUniformBlockSize}); return true;
    case GL_MAX_COMBINED_VERTEX_UNIFORM_COMPONENTS: case GL_MAX_COMBINED_GEOMETRY_UNIFORM_COMPONENTS:
    case GL_MAX_COMBINED_FRAGMENT_UNIFORM_COMPONENTS:
        out = ints({kMaxDefaultUniformComponents + static_cast<int64_t>(kMaxUniformBlocksPerStage) * kMaxUniformBlockSize / 4});
        return true;
    case GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT: out = ints({256}); return true;
    case GL_MAX_TEXTURE_IMAGE_UNITS: case GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS: case GL_MAX_GEOMETRY_TEXTURE_IMAGE_UNITS:
        out = ints({kMaxTextureUnitsPerStage});
        return true;
    case GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS: out = ints({kMaxCombinedTextureUnits}); return true;
    case GL_MAX_TRANSFORM_FEEDBACK_INTERLEAVED_COMPONENTS: out = ints({64}); return true;
    case GL_MAX_TRANSFORM_FEEDBACK_SEPARATE_ATTRIBS: out = ints({4}); return true;
    case GL_MAX_TRANSFORM_FEEDBACK_SEPARATE_COMPONENTS: out = ints({4}); return true;
    case GL_MAX_CLIP_DISTANCES: out = ints({8}); return true;
    case GL_MAX_SERVER_WAIT_TIMEOUT: out = int64v(INT64_MAX); return true;
    case GL_SUBPIXEL_BITS: out = ints({8}); return true;
    case GL_MAX_PROGRAM_TEXEL_OFFSET: out = ints({7}); return true;
    case GL_MIN_PROGRAM_TEXEL_OFFSET: out = ints({-8}); return true;
    case GL_ALIASED_LINE_WIDTH_RANGE: case GL_SMOOTH_LINE_WIDTH_RANGE: out = floats({1.0, 1.0}); return true;
    case GL_SMOOTH_LINE_WIDTH_GRANULARITY: out = floats({0.125}); return true;
    case GL_POINT_SIZE_RANGE: out = floats({1.0, 511.0}); return true;
    case GL_POINT_SIZE_GRANULARITY: out = floats({0.125}); return true;
    case GL_NUM_COMPRESSED_TEXTURE_FORMATS: out = ints({caps.bcTextures ? 4 : 0}); return true;
    case GL_COMPRESSED_TEXTURE_FORMATS:
        // Generic/RGTC formats are not listed (GL 3.3 excludes formats with restrictions).
        out = caps.bcTextures ? ints({GL_COMPRESSED_RGB_S3TC_DXT1_EXT, GL_COMPRESSED_RGBA_S3TC_DXT1_EXT,
                                      GL_COMPRESSED_RGBA_S3TC_DXT3_EXT, GL_COMPRESSED_RGBA_S3TC_DXT5_EXT})
                              : ints({});
        return true;
    case GL_TIMESTAMP: {
        uint64_t cpu = 0, gpu = 0;
        [Device::get().mtl() sampleTimestamps:&cpu gpuTimestamp:&gpu];
        out = int64v(static_cast<int64_t>(clock_gettime_nsec_np(CLOCK_UPTIME_RAW)));
        return true;
    }
    case GL_MAX_TEXTURE_MAX_ANISOTROPY: out = floats({16.0}); return true;
    // --- rasterization and fragment state
    case GL_VIEWPORT: out = ints({s.viewport[0], s.viewport[1], s.viewport[2], s.viewport[3]}); return true;
    case GL_DEPTH_RANGE: out = floats({s.depthRange[0], s.depthRange[1]}, Value::NormFloat); return true;
    case GL_SCISSOR_BOX: out = ints({s.scissor[0], s.scissor[1], s.scissor[2], s.scissor[3]}); return true;
    case GL_COLOR_CLEAR_VALUE:
        out = floats({s.clearColor[0], s.clearColor[1], s.clearColor[2], s.clearColor[3]}, Value::NormFloat);
        return true;
    case GL_DEPTH_CLEAR_VALUE: out = floats({s.clearDepth}, Value::NormFloat); return true;
    case GL_STENCIL_CLEAR_VALUE: out = ints({s.clearStencil}); return true;
    case GL_COLOR_WRITEMASK:
        out = bools({s.colorMask[0][0], s.colorMask[0][1], s.colorMask[0][2], s.colorMask[0][3]});
        return true;
    case GL_DEPTH_WRITEMASK: out = bools({s.depthMask}); return true;
    case GL_STENCIL_WRITEMASK: out = uints({s.stencilFront.writeMask}); return true;
    case GL_STENCIL_BACK_WRITEMASK: out = uints({s.stencilBack.writeMask}); return true;
    case GL_DEPTH_FUNC: out = ints({s.depthFunc}); return true;
    case GL_STENCIL_FUNC: out = ints({s.stencilFront.func}); return true;
    case GL_STENCIL_REF: out = ints({s.stencilFront.ref}); return true;
    case GL_STENCIL_VALUE_MASK: out = uints({s.stencilFront.valueMask}); return true;
    case GL_STENCIL_FAIL: out = ints({s.stencilFront.sfail}); return true;
    case GL_STENCIL_PASS_DEPTH_FAIL: out = ints({s.stencilFront.dpfail}); return true;
    case GL_STENCIL_PASS_DEPTH_PASS: out = ints({s.stencilFront.dppass}); return true;
    case GL_STENCIL_BACK_FUNC: out = ints({s.stencilBack.func}); return true;
    case GL_STENCIL_BACK_REF: out = ints({s.stencilBack.ref}); return true;
    case GL_STENCIL_BACK_VALUE_MASK: out = uints({s.stencilBack.valueMask}); return true;
    case GL_STENCIL_BACK_FAIL: out = ints({s.stencilBack.sfail}); return true;
    case GL_STENCIL_BACK_PASS_DEPTH_FAIL: out = ints({s.stencilBack.dpfail}); return true;
    case GL_STENCIL_BACK_PASS_DEPTH_PASS: out = ints({s.stencilBack.dppass}); return true;
    case GL_BLEND_SRC_RGB: out = ints({s.blendSrcRGB}); return true;
    case GL_BLEND_DST_RGB: out = ints({s.blendDstRGB}); return true;
    case GL_BLEND_SRC_ALPHA: out = ints({s.blendSrcAlpha}); return true;
    case GL_BLEND_DST_ALPHA: out = ints({s.blendDstAlpha}); return true;
    case GL_BLEND_EQUATION_RGB: out = ints({s.blendEquationRGB}); return true;
    case GL_BLEND_EQUATION_ALPHA: out = ints({s.blendEquationAlpha}); return true;
    case GL_BLEND_COLOR: out = floats({s.blendColor[0], s.blendColor[1], s.blendColor[2], s.blendColor[3]}, Value::NormFloat); return true;
    case GL_CULL_FACE_MODE: out = ints({s.cullFaceMode}); return true;
    case GL_FRONT_FACE: out = ints({s.frontFace}); return true;
    case GL_POLYGON_MODE: out = ints({s.polygonMode, s.polygonModeBack}); return true;
    case GL_POLYGON_OFFSET_FACTOR: out = floats({s.polygonOffsetFactor}); return true;
    case GL_POLYGON_OFFSET_UNITS: out = floats({s.polygonOffsetUnits}); return true;
    case GL_LINE_WIDTH: out = floats({s.lineWidth}); return true;
    case GL_POINT_SIZE: out = floats({s.pointSize}); return true;
    case GL_POINT_FADE_THRESHOLD_SIZE: out = floats({s.pointFadeThresholdSize}); return true;
    case GL_POINT_SPRITE_COORD_ORIGIN: out = ints({s.pointSpriteCoordOrigin}); return true;
    case GL_PROVOKING_VERTEX: out = ints({s.provokingVertex}); return true;
    case GL_PRIMITIVE_RESTART_INDEX: out = uints({s.primitiveRestartIndex}); return true;
    case GL_LOGIC_OP_MODE: out = ints({s.logicOp}); return true;
    case GL_SAMPLE_COVERAGE_VALUE: out = floats({s.sampleCoverageValue}); return true;
    case GL_SAMPLE_COVERAGE_INVERT: out = bools({s.sampleCoverageInvert}); return true;
    case GL_SAMPLE_BUFFERS: out = ints({samples(ctx) > 0 ? 1 : 0}); return true;
    case GL_SAMPLES: out = ints({samples(ctx)}); return true;
    case GL_CLAMP_READ_COLOR: out = ints({s.clampReadColorMode}); return true;
    // --- pixel store
    case GL_PACK_ALIGNMENT: out = ints({s.pack.alignment}); return true;
    case GL_UNPACK_ALIGNMENT: out = ints({s.unpack.alignment}); return true;
    case GL_PACK_ROW_LENGTH: out = ints({s.pack.rowLength}); return true;
    case GL_UNPACK_ROW_LENGTH: out = ints({s.unpack.rowLength}); return true;
    case GL_PACK_IMAGE_HEIGHT: out = ints({s.pack.imageHeight}); return true;
    case GL_UNPACK_IMAGE_HEIGHT: out = ints({s.unpack.imageHeight}); return true;
    case GL_PACK_SKIP_PIXELS: out = ints({s.pack.skipPixels}); return true;
    case GL_UNPACK_SKIP_PIXELS: out = ints({s.unpack.skipPixels}); return true;
    case GL_PACK_SKIP_ROWS: out = ints({s.pack.skipRows}); return true;
    case GL_UNPACK_SKIP_ROWS: out = ints({s.unpack.skipRows}); return true;
    case GL_PACK_SKIP_IMAGES: out = ints({s.pack.skipImages}); return true;
    case GL_UNPACK_SKIP_IMAGES: out = ints({s.unpack.skipImages}); return true;
    case GL_UNPACK_COMPRESSED_BLOCK_WIDTH: out = ints({s.unpack.blockWidth}); return true;
    case GL_UNPACK_COMPRESSED_BLOCK_HEIGHT: out = ints({s.unpack.blockHeight}); return true;
    case GL_UNPACK_COMPRESSED_BLOCK_DEPTH: out = ints({s.unpack.blockDepth}); return true;
    case GL_UNPACK_COMPRESSED_BLOCK_SIZE: out = ints({s.unpack.blockSize}); return true;
    case GL_PACK_COMPRESSED_BLOCK_WIDTH: out = ints({s.pack.blockWidth}); return true;
    case GL_PACK_COMPRESSED_BLOCK_HEIGHT: out = ints({s.pack.blockHeight}); return true;
    case GL_PACK_COMPRESSED_BLOCK_DEPTH: out = ints({s.pack.blockDepth}); return true;
    case GL_PACK_COMPRESSED_BLOCK_SIZE: out = ints({s.pack.blockSize}); return true;
    case GL_PACK_SWAP_BYTES: out = bools({s.pack.swapBytes}); return true;
    case GL_UNPACK_SWAP_BYTES: out = bools({s.unpack.swapBytes}); return true;
    case GL_PACK_LSB_FIRST: out = bools({s.pack.lsbFirst}); return true;
    case GL_UNPACK_LSB_FIRST: out = bools({s.unpack.lsbFirst}); return true;
    // --- bindings
    case GL_ARRAY_BUFFER_BINDING: out = ints({nameOf(s.arrayBuffer)}); return true;
    case GL_ELEMENT_ARRAY_BUFFER_BINDING: out = ints({nameOf(s.vertexArray->elementBuffer)}); return true;
    case GL_COPY_READ_BUFFER_BINDING: out = ints({nameOf(s.copyReadBuffer)}); return true;
    case GL_COPY_WRITE_BUFFER_BINDING: out = ints({nameOf(s.copyWriteBuffer)}); return true;
    case GL_PIXEL_PACK_BUFFER_BINDING: out = ints({nameOf(s.pixelPackBuffer)}); return true;
    case GL_PIXEL_UNPACK_BUFFER_BINDING: out = ints({nameOf(s.pixelUnpackBuffer)}); return true;
    case GL_UNIFORM_BUFFER_BINDING: out = ints({nameOf(s.uniformBuffer)}); return true;
    case GL_TRANSFORM_FEEDBACK_BUFFER_BINDING: out = ints({nameOf(s.transformFeedbackBuffer)}); return true;
    case GL_TEXTURE_BUFFER: out = ints({nameOf(s.textureBuffer)}); return true;
    case GL_VERTEX_ARRAY_BINDING: out = ints({s.vertexArray->name}); return true;
    case GL_DRAW_FRAMEBUFFER_BINDING: out = ints({s.drawFramebuffer ? s.drawFramebuffer->name : 0}); return true;
    case GL_READ_FRAMEBUFFER_BINDING: out = ints({s.readFramebuffer ? s.readFramebuffer->name : 0}); return true;
    case GL_RENDERBUFFER_BINDING: out = ints({s.renderbuffer ? s.renderbuffer->name : 0}); return true;
    case GL_CURRENT_PROGRAM: out = ints({s.program ? s.program->name : 0}); return true;
    case GL_ACTIVE_TEXTURE: out = ints({GL_TEXTURE0 + static_cast<int64_t>(s.activeTexture)}); return true;
    case GL_SAMPLER_BINDING: {
        const auto& unit = s.textureUnits[s.activeTexture];
        out = ints({unit.sampler ? unit.sampler->name : 0});
        return true;
    }
    case GL_TRANSFORM_FEEDBACK_BINDING:
        if (ctx.version() < 40) return false;
        out = ints({s.transformFeedback->name});
        return true;
    case GL_DRAW_BUFFER: case GL_DRAW_BUFFER0:
        out = ints({s.drawFramebuffer ? s.drawFramebuffer->drawBuffers[0] : s.defaultDrawBuffer});
        return true;
    case GL_READ_BUFFER: out = ints({s.readFramebuffer ? s.readFramebuffer->readBuffer : s.defaultReadBuffer}); return true;
    case GL_DOUBLEBUFFER: out = bools({ctx.drawSurface() && ctx.drawSurface()->pixelFormat().double_buffer}); return true;
    case GL_STEREO: out = bools({false}); return true;
    // --- hints
    case GL_LINE_SMOOTH_HINT: out = ints({s.hintLineSmooth}); return true;
    case GL_POLYGON_SMOOTH_HINT: out = ints({s.hintPolygonSmooth}); return true;
    case GL_TEXTURE_COMPRESSION_HINT: out = ints({s.hintTextureCompression}); return true;
    case GL_FRAGMENT_SHADER_DERIVATIVE_HINT: out = ints({s.hintFragmentShaderDerivative}); return true;
    default: break;
    }
    if (pname > GL_DRAW_BUFFER0 && pname < GL_DRAW_BUFFER0 + 8) {
        const int i = static_cast<int>(pname - GL_DRAW_BUFFER0);
        out = ints({s.drawFramebuffer ? s.drawFramebuffer->drawBuffers[i] : GL_NONE});
        return true;
    }
    const int target = textureBindingTarget(pname);
    if (target >= 0) {
        const RefPtr<Texture>& t = s.textureUnits[s.activeTexture].bound[target];
        out = ints({t ? t->name : 0});
        return true;
    }
    // Capabilities can be queried with glGet as well.
    const GLenum before = ctx.takeError();
    const GLboolean enabled = IsEnabled(ctx, pname);
    const GLenum error = ctx.takeError();
    if (before) ctx.setError(before);
    if (error) return false;
    out = bools({enabled == GL_TRUE});
    return true;
}

bool getIndexed(Context& ctx, GLenum pname, GLuint index, Value& out, bool& badIndex) {
    const ContextState& s = ctx.state;
    badIndex = false;
    switch (pname) {
    case GL_MAX_COMPUTE_WORK_GROUP_COUNT: case GL_MAX_COMPUTE_WORK_GROUP_SIZE:
        if (!ctx.computeShaders()) return false;
        if (index >= 3) { badIndex = true; return true; }
        out = ints({pname == GL_MAX_COMPUTE_WORK_GROUP_COUNT ? limits::kMaxComputeWorkGroupCount
                                                             : limits::kMaxComputeWorkGroupSize[index]});
        return true;
    case GL_UNIFORM_BUFFER_BINDING: case GL_UNIFORM_BUFFER_START: case GL_UNIFORM_BUFFER_SIZE: {
        if (index >= static_cast<GLuint>(limits::kMaxUniformBufferBindings)) { badIndex = true; return true; }
        const IndexedBufferBinding& b = s.uniformBufferBindings[index];
        out = pname == GL_UNIFORM_BUFFER_BINDING ? ints({nameOf(b.buffer)})
              : pname == GL_UNIFORM_BUFFER_START ? int64v(b.offset)
                                                 : int64v(b.size);
        return true;
    }
    case GL_TRANSFORM_FEEDBACK_BUFFER_BINDING: case GL_TRANSFORM_FEEDBACK_BUFFER_START:
    case GL_TRANSFORM_FEEDBACK_BUFFER_SIZE: {
        if (index >= TransformFeedback::kMaxBuffers) { badIndex = true; return true; }
        const TransformFeedback::Binding& b = s.transformFeedback->buffers[index];
        out = pname == GL_TRANSFORM_FEEDBACK_BUFFER_BINDING ? ints({nameOf(b.buffer)})
              : pname == GL_TRANSFORM_FEEDBACK_BUFFER_START ? int64v(b.offset)
                                                            : int64v(b.size);
        return true;
    }
    case GL_COLOR_WRITEMASK:
        if (index >= static_cast<GLuint>(ContextState::kMaxDrawBuffers)) { badIndex = true; return true; }
        out = bools({s.colorMask[index][0], s.colorMask[index][1], s.colorMask[index][2], s.colorMask[index][3]});
        return true;
    case GL_BLEND:
        if (index >= static_cast<GLuint>(ContextState::kMaxDrawBuffers)) { badIndex = true; return true; }
        out = bools({((s.blendEnable >> index) & 1) != 0});
        return true;
    case GL_SAMPLE_MASK_VALUE:
        if (index >= 1) { badIndex = true; return true; }
        out = uints({s.sampleMaskValue});
        return true;
    default: return false;
    }
}

template <class T>
void convert(const Value& v, T* params) {
    for (int i = 0; i < v.count; ++i) {
        const double x = v.v[i];
        if constexpr (std::is_same_v<T, GLboolean>) {
            params[i] = x != 0.0 ? GL_TRUE : GL_FALSE;
        } else if constexpr (std::is_floating_point_v<T>) {
            params[i] = static_cast<T>(x);
        } else {
            if (v.kind == Value::NormFloat) {
                // Normalized values map [-1, 1] linearly to the integer range (GL 3.3 section 6.1.2).
                const double scaled = std::clamp(x, -1.0, 1.0) * (sizeof(T) == 8 ? 9223372036854775807.0 : 2147483647.0);
                params[i] = static_cast<T>(std::llround(scaled));
            } else if (v.kind == Value::Float || v.kind == Value::Double) {
                params[i] = static_cast<T>(std::llround(x));
            } else if (v.kind == Value::UInt && sizeof(T) == 4) {
                params[i] = static_cast<T>(static_cast<uint32_t>(x));
            } else if constexpr (sizeof(T) == 4) {
                params[i] = static_cast<T>(std::clamp(x, -2147483648.0, 2147483647.0));
            } else {
                params[i] = static_cast<T>(x);
            }
        }
    }
}

template <class T>
void get(Context& ctx, GLenum pname, T* params) {
    Value v;
    if (!getState(ctx, pname, v)) return ctx.setError(GL_INVALID_ENUM);
    if (params) convert(v, params);
}

template <class T>
void getIndexedT(Context& ctx, GLenum pname, GLuint index, T* params) {
    Value v;
    bool badIndex = false;
    if (!getIndexed(ctx, pname, index, v, badIndex)) return ctx.setError(GL_INVALID_ENUM);
    if (badIndex) return ctx.setError(GL_INVALID_VALUE);
    if (params) convert(v, params);
}

}  // namespace

void GetBooleanv(Context& ctx, GLenum pname, GLboolean* data) { get(ctx, pname, data); }
void GetIntegerv(Context& ctx, GLenum pname, GLint* data) { get(ctx, pname, data); }
void GetInteger64v(Context& ctx, GLenum pname, GLint64* data) { get(ctx, pname, data); }
void GetFloatv(Context& ctx, GLenum pname, GLfloat* data) { get(ctx, pname, data); }
void GetDoublev(Context& ctx, GLenum pname, GLdouble* data) { get(ctx, pname, data); }
void GetBooleani_v(Context& ctx, GLenum target, GLuint index, GLboolean* data) { getIndexedT(ctx, target, index, data); }
void GetIntegeri_v(Context& ctx, GLenum target, GLuint index, GLint* data) { getIndexedT(ctx, target, index, data); }
void GetInteger64i_v(Context& ctx, GLenum target, GLuint index, GLint64* data) { getIndexedT(ctx, target, index, data); }
void GetFloati_v(Context& ctx, GLenum target, GLuint index, GLfloat* data) { getIndexedT(ctx, target, index, data); }
void GetDoublei_v(Context& ctx, GLenum target, GLuint index, GLdouble* data) { getIndexedT(ctx, target, index, data); }

}  // namespace wmgl::api
