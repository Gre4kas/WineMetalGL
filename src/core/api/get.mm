// glGet* state queries (GL 3.3 chapter 6) with GL's type conversions.

#include <algorithm>
#include <cmath>
#include <cstring>

#include "core/compat/compat_api.h"
#include "core/compat/fixed_function.h"
#include "core/context.h"
#include "core/draw.h"
#include "core/limits.h"
#include "core/metal/device.h"
#include "core/program.h"
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
    case GL_NUM_SHADING_LANGUAGE_VERSIONS:
        // The glGetStringi(GL_SHADING_LANGUAGE_VERSION) list (api/state.mm).
        if (ctx.version() < 43) return false;
        out = ints({ctx.version() - 43 + 11});
        return true;
    case GL_RESET_NOTIFICATION_STRATEGY: out = ints({ctx.config().resetNotification}); return true;
    case 0x90F3:  // GL_CONTEXT_ROBUST_ACCESS (GL_KHR_robustness)
        out = bools({ctx.config().robustAccess});
        return true;
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
    case GL_NUM_PROGRAM_BINARY_FORMATS: case GL_PROGRAM_BINARY_FORMATS:
        if (ctx.version() < 41 && !ctx.hasExtension("GL_ARB_get_program_binary")) return false;
        out = pname == GL_NUM_PROGRAM_BINARY_FORMATS ? ints({1}) : ints({kProgramBinaryFormat});
        return true;
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
    // Primitive restart is treated as disabled for GL_PATCHES (GL 4.3 section 10.3.5).
    case GL_PRIMITIVE_RESTART_FOR_PATCHES_SUPPORTED:
        if (!ctx.tessellationShaders()) return false;
        out = bools({false});
        return true;
    // GL 4.2 atomic counters (limits.h); vertex and tessellation stages have none.
    case GL_ATOMIC_COUNTER_BUFFER_BINDING: case GL_MAX_ATOMIC_COUNTER_BUFFER_BINDINGS: case GL_MAX_ATOMIC_COUNTER_BUFFER_SIZE:
    case GL_MAX_VERTEX_ATOMIC_COUNTER_BUFFERS: case GL_MAX_TESS_CONTROL_ATOMIC_COUNTER_BUFFERS:
    case GL_MAX_TESS_EVALUATION_ATOMIC_COUNTER_BUFFERS: case GL_MAX_GEOMETRY_ATOMIC_COUNTER_BUFFERS:
    case GL_MAX_FRAGMENT_ATOMIC_COUNTER_BUFFERS: case GL_MAX_COMBINED_ATOMIC_COUNTER_BUFFERS:
    case GL_MAX_VERTEX_ATOMIC_COUNTERS: case GL_MAX_TESS_CONTROL_ATOMIC_COUNTERS: case GL_MAX_TESS_EVALUATION_ATOMIC_COUNTERS:
    case GL_MAX_GEOMETRY_ATOMIC_COUNTERS: case GL_MAX_FRAGMENT_ATOMIC_COUNTERS: case GL_MAX_COMBINED_ATOMIC_COUNTERS:
        if (!ctx.atomicCounters()) return false;
        switch (pname) {
        case GL_ATOMIC_COUNTER_BUFFER_BINDING: out = ints({nameOf(s.atomicCounterBuffer)}); break;
        case GL_MAX_ATOMIC_COUNTER_BUFFER_BINDINGS: out = ints({kMaxAtomicCounterBufferBindings}); break;
        case GL_MAX_ATOMIC_COUNTER_BUFFER_SIZE: out = ints({kMaxAtomicCounterBufferSize}); break;
        case GL_MAX_VERTEX_ATOMIC_COUNTER_BUFFERS: case GL_MAX_TESS_CONTROL_ATOMIC_COUNTER_BUFFERS:
        case GL_MAX_TESS_EVALUATION_ATOMIC_COUNTER_BUFFERS: case GL_MAX_VERTEX_ATOMIC_COUNTERS:
        case GL_MAX_TESS_CONTROL_ATOMIC_COUNTERS: case GL_MAX_TESS_EVALUATION_ATOMIC_COUNTERS:
            out = ints({0});
            break;
        case GL_MAX_GEOMETRY_ATOMIC_COUNTER_BUFFERS: case GL_MAX_FRAGMENT_ATOMIC_COUNTER_BUFFERS:
        case GL_MAX_COMBINED_ATOMIC_COUNTER_BUFFERS:
            out = ints({kMaxStageAtomicCounterBuffers});
            break;
        default: out = ints({kMaxStageAtomicCounters}); break;
        }
        return true;
    // GL 4.2 image load/store (limits.h): every stage has image uniforms.
    case GL_MAX_IMAGE_UNITS: case GL_MAX_COMBINED_IMAGE_UNITS_AND_FRAGMENT_OUTPUTS: case GL_MAX_IMAGE_SAMPLES:
    case GL_MAX_VERTEX_IMAGE_UNIFORMS: case GL_MAX_TESS_CONTROL_IMAGE_UNIFORMS: case GL_MAX_TESS_EVALUATION_IMAGE_UNIFORMS:
    case GL_MAX_GEOMETRY_IMAGE_UNIFORMS: case GL_MAX_FRAGMENT_IMAGE_UNIFORMS: case GL_MAX_COMBINED_IMAGE_UNIFORMS:
        if (!ctx.imageLoadStore()) return false;
        out = ints({pname == GL_MAX_IMAGE_UNITS ? kMaxImageUnits
                    : pname == GL_MAX_COMBINED_IMAGE_UNITS_AND_FRAGMENT_OUTPUTS ? kMaxCombinedImageUnitsAndFragmentOutputs
                    : pname == GL_MAX_IMAGE_SAMPLES ? 0  // multisample images: Metal cannot write them
                    : pname == GL_MAX_COMBINED_IMAGE_UNIFORMS ? kMaxCombinedImageUniforms
                                                               : kMaxImageUniformsPerStage});
        return true;
    case GL_MAX_SUBROUTINES: case GL_MAX_SUBROUTINE_UNIFORM_LOCATIONS:
        if (ctx.version() < 40 && !ctx.hasExtension("GL_ARB_shader_subroutine")) return false;
        out = ints({pname == GL_MAX_SUBROUTINES ? 256 : 1024});
        return true;
    case GL_MAX_GEOMETRY_SHADER_INVOCATIONS:
        if (ctx.version() < 40 && !ctx.hasExtension("GL_ARB_gpu_shader5")) return false;
        out = ints({32});
        return true;
    // --- tessellation (GL 4.0)
    case GL_PATCH_VERTICES: case GL_PATCH_DEFAULT_OUTER_LEVEL: case GL_PATCH_DEFAULT_INNER_LEVEL:
    case GL_MAX_PATCH_VERTICES: case GL_MAX_TESS_GEN_LEVEL: case GL_MAX_TESS_CONTROL_UNIFORM_COMPONENTS:
    case GL_MAX_TESS_EVALUATION_UNIFORM_COMPONENTS: case GL_MAX_TESS_CONTROL_TEXTURE_IMAGE_UNITS:
    case GL_MAX_TESS_EVALUATION_TEXTURE_IMAGE_UNITS: case GL_MAX_TESS_CONTROL_OUTPUT_COMPONENTS:
    case GL_MAX_TESS_PATCH_COMPONENTS: case GL_MAX_TESS_CONTROL_TOTAL_OUTPUT_COMPONENTS:
    case GL_MAX_TESS_EVALUATION_OUTPUT_COMPONENTS: case GL_MAX_TESS_CONTROL_INPUT_COMPONENTS:
    case GL_MAX_TESS_EVALUATION_INPUT_COMPONENTS: case GL_MAX_TESS_CONTROL_UNIFORM_BLOCKS:
    case GL_MAX_TESS_EVALUATION_UNIFORM_BLOCKS: case GL_MAX_COMBINED_TESS_CONTROL_UNIFORM_COMPONENTS:
    case GL_MAX_COMBINED_TESS_EVALUATION_UNIFORM_COMPONENTS:
        if (!ctx.tessellationShaders()) return false;
        switch (pname) {
        case GL_PATCH_VERTICES: out = ints({s.patchVertices}); break;
        case GL_PATCH_DEFAULT_OUTER_LEVEL:
            out = floats({s.patchDefaultOuterLevel[0], s.patchDefaultOuterLevel[1], s.patchDefaultOuterLevel[2],
                          s.patchDefaultOuterLevel[3]});
            break;
        case GL_PATCH_DEFAULT_INNER_LEVEL: out = floats({s.patchDefaultInnerLevel[0], s.patchDefaultInnerLevel[1]}); break;
        case GL_MAX_PATCH_VERTICES: out = ints({kMaxPatchVertices}); break;
        case GL_MAX_TESS_GEN_LEVEL: out = ints({kMaxTessGenLevel}); break;
        case GL_MAX_TESS_CONTROL_UNIFORM_COMPONENTS: case GL_MAX_TESS_EVALUATION_UNIFORM_COMPONENTS:
            out = ints({kMaxDefaultUniformComponents});
            break;
        case GL_MAX_TESS_CONTROL_TEXTURE_IMAGE_UNITS: case GL_MAX_TESS_EVALUATION_TEXTURE_IMAGE_UNITS:
            out = ints({kMaxTextureUnitsPerStage});
            break;
        case GL_MAX_TESS_PATCH_COMPONENTS: out = ints({120}); break;
        case GL_MAX_TESS_CONTROL_TOTAL_OUTPUT_COMPONENTS: out = ints({4096}); break;
        case GL_MAX_TESS_CONTROL_UNIFORM_BLOCKS: case GL_MAX_TESS_EVALUATION_UNIFORM_BLOCKS:
            out = ints({kMaxUniformBlocksPerStage});
            break;
        case GL_MAX_COMBINED_TESS_CONTROL_UNIFORM_COMPONENTS: case GL_MAX_COMBINED_TESS_EVALUATION_UNIFORM_COMPONENTS:
            out = ints({kMaxDefaultUniformComponents + static_cast<int64_t>(kMaxUniformBlocksPerStage) * kMaxUniformBlockSize / 4});
            break;
        default: out = ints({128}); break;  // input and output components
        }
        return true;
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
    case GL_MIN_MAP_BUFFER_ALIGNMENT: out = ints({64}); return true;  // GL_ARB_map_buffer_alignment
    case GL_CLIP_ORIGIN: out = ints({s.clipOrigin}); return true;      // GL_ARB_clip_control
    // KHR_debug
    case GL_MAX_DEBUG_MESSAGE_LENGTH: out = ints({static_cast<int64_t>(Context::kMaxDebugMessageLength)}); return true;
    case GL_MAX_DEBUG_LOGGED_MESSAGES: out = ints({static_cast<int64_t>(Context::kMaxDebugLoggedMessages)}); return true;
    case GL_MAX_DEBUG_GROUP_STACK_DEPTH: out = ints({static_cast<int64_t>(Context::kMaxDebugGroupDepth)}); return true;
    case GL_MAX_LABEL_LENGTH: out = ints({Context::kMaxLabelLength}); return true;
    case GL_DEBUG_LOGGED_MESSAGES: out = ints({static_cast<int64_t>(ctx.debugLog.size())}); return true;
    case GL_DEBUG_NEXT_LOGGED_MESSAGE_LENGTH:
        out = ints({ctx.debugLog.empty() ? 0 : static_cast<int64_t>(ctx.debugLog.front().message.size() + 1)});
        return true;
    case GL_DEBUG_GROUP_STACK_DEPTH: out = ints({static_cast<int64_t>(ctx.debugGroups.size())}); return true;
    case GL_CLIP_DEPTH_MODE: out = ints({s.clipDepthMode}); return true;
    case GL_MAX_TEXTURE_IMAGE_UNITS: case GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS: case GL_MAX_GEOMETRY_TEXTURE_IMAGE_UNITS:
        out = ints({kMaxTextureUnitsPerStage});
        return true;
    case GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS: out = ints({kMaxCombinedTextureUnits}); return true;
    case GL_MAX_TRANSFORM_FEEDBACK_INTERLEAVED_COMPONENTS: out = ints({64}); return true;
    case GL_MAX_TRANSFORM_FEEDBACK_SEPARATE_ATTRIBS: out = ints({4}); return true;
    case GL_MAX_TRANSFORM_FEEDBACK_SEPARATE_COMPONENTS: out = ints({4}); return true;
    case GL_MAX_CLIP_DISTANCES: out = ints({8}); return true;
    case GL_MAX_SERVER_WAIT_TIMEOUT: out = int64v(INT64_MAX); return true;
    // GL 4.3 / GL_ARB_ES3_compatibility, which the KHR-GL32 API coverage test queries in
    // every core context. Element arrays take full 32-bit indices.
    case GL_MAX_ELEMENT_INDEX: out = int64v(UINT32_MAX); return true;
    case GL_SUBPIXEL_BITS: out = ints({8}); return true;
    case GL_MAX_PROGRAM_TEXEL_OFFSET: out = ints({7}); return true;
    case GL_MIN_PROGRAM_TEXEL_OFFSET: out = ints({-8}); return true;
    // GL 4.0 / GL_ARB_texture_gather: Metal's gather offsets span -8..7.
    case GL_MIN_PROGRAM_TEXTURE_GATHER_OFFSET: case GL_MAX_PROGRAM_TEXTURE_GATHER_OFFSET:
    case GL_MAX_PROGRAM_TEXTURE_GATHER_COMPONENTS_ARB:
        if (ctx.version() < 40 && !ctx.hasExtension("GL_ARB_texture_gather")) return false;
        out = ints({pname == GL_MIN_PROGRAM_TEXTURE_GATHER_OFFSET ? -8 : pname == GL_MAX_PROGRAM_TEXTURE_GATHER_OFFSET ? 7 : 4});
        return true;
    // Wide lines are drawn by the compatibility profile's fixed-function programs only.
    case GL_ALIASED_LINE_WIDTH_RANGE:
        out = floats({1.0, ctx.isCompat() ? static_cast<double>(compat::kMaxLineWidth) : 1.0});
        return true;
    case GL_SMOOTH_LINE_WIDTH_RANGE: out = floats({1.0, 1.0}); return true;
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
    case GL_VIEWPORT: out = floats({s.viewports[0][0], s.viewports[0][1], s.viewports[0][2], s.viewports[0][3]}); return true;
    // GL 4.1 viewport arrays.
    case GL_MAX_VIEWPORTS: case GL_VIEWPORT_SUBPIXEL_BITS: case GL_VIEWPORT_BOUNDS_RANGE: case GL_LAYER_PROVOKING_VERTEX:
    case GL_VIEWPORT_INDEX_PROVOKING_VERTEX:
        if (ctx.version() < 41 && !ctx.hasExtension("GL_ARB_viewport_array")) return false;
        switch (pname) {
        case GL_MAX_VIEWPORTS: out = ints({ContextState::kMaxViewports}); break;
        case GL_VIEWPORT_SUBPIXEL_BITS: out = ints({8}); break;
        case GL_VIEWPORT_BOUNDS_RANGE: out = floats({-32768.0, 32767.0}); break;
        // Metal takes the layer and viewport of a primitive from one of its
        // vertices without specifying which.
        default: out = ints({GL_UNDEFINED_VERTEX}); break;
        }
        return true;
    case GL_DEPTH_RANGE: out = floats({s.depthRanges[0][0], s.depthRanges[0][1]}, Value::NormFloat); return true;
    case GL_SCISSOR_BOX: out = ints({s.scissors[0][0], s.scissors[0][1], s.scissors[0][2], s.scissors[0][3]}); return true;
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
    case GL_BLEND_SRC_RGB: out = ints({s.blend[0].srcRGB}); return true;
    case GL_BLEND_DST_RGB: out = ints({s.blend[0].dstRGB}); return true;
    case GL_BLEND_SRC_ALPHA: out = ints({s.blend[0].srcAlpha}); return true;
    case GL_BLEND_DST_ALPHA: out = ints({s.blend[0].dstAlpha}); return true;
    case GL_BLEND_EQUATION_RGB: out = ints({s.blend[0].equationRGB}); return true;
    case GL_BLEND_EQUATION_ALPHA: out = ints({s.blend[0].equationAlpha}); return true;
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
    case GL_DRAW_INDIRECT_BUFFER_BINDING:
        if (ctx.version() < 40 && !ctx.hasExtension("GL_ARB_draw_indirect")) return false;
        out = ints({nameOf(s.drawIndirectBuffer)});
        return true;
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
    case GL_TRANSFORM_FEEDBACK_BUFFER_ACTIVE: case GL_TRANSFORM_FEEDBACK_BUFFER_PAUSED:
        if (ctx.version() < 40) return false;
        out = bools({pname == GL_TRANSFORM_FEEDBACK_BUFFER_ACTIVE ? s.transformFeedback->active : s.transformFeedback->paused});
        return true;
    case GL_MIN_SAMPLE_SHADING_VALUE:
        if (!ctx.sampleShading()) return false;
        out = floats({s.minSampleShading});
        return true;
    case GL_MAX_TRANSFORM_FEEDBACK_BUFFERS: case GL_MAX_VERTEX_STREAMS:
        if (ctx.version() < 40) return false;
        out = ints({4});
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
    // GL_NVX_gpu_memory_info / GL_ATI_meminfo: Metal's recommended working
    // set stands in for dedicated memory (values in KiB).
    if (pname >= GL_GPU_MEMORY_INFO_DEDICATED_VIDMEM_NVX && pname <= GL_GPU_MEMORY_INFO_EVICTED_MEMORY_NVX) {
        const int64_t total = static_cast<int64_t>(Device::get().caps().workingSetBytes / 1024);
        const int64_t used = static_cast<int64_t>(Device::get().mtl().currentAllocatedSize / 1024);
        const int64_t free = std::max<int64_t>(0, total - used);
        switch (pname) {
        case GL_GPU_MEMORY_INFO_DEDICATED_VIDMEM_NVX: case GL_GPU_MEMORY_INFO_TOTAL_AVAILABLE_MEMORY_NVX: out = ints({total}); break;
        case GL_GPU_MEMORY_INFO_CURRENT_AVAILABLE_VIDMEM_NVX: out = ints({free}); break;
        default: out = ints({0}); break;  // eviction count / evicted memory
        }
        return true;
    }
    if (pname == GL_VBO_FREE_MEMORY_ATI || pname == GL_TEXTURE_FREE_MEMORY_ATI || pname == GL_RENDERBUFFER_FREE_MEMORY_ATI) {
        const int64_t total = static_cast<int64_t>(Device::get().caps().workingSetBytes / 1024);
        const int64_t free = std::max<int64_t>(0, total - static_cast<int64_t>(Device::get().mtl().currentAllocatedSize / 1024));
        out = ints({free, free, 0, 0});  // total free, largest block, auxiliary free, largest auxiliary block
        return true;
    }
    // Compatibility profile state (api/fixed_get.mm).
    {
        double values[16];
        char kind = 'f';
        if (const int n = getFixedState(ctx, pname, values, kind)) {
            if (n < 0) return true;  // error set; nothing is written
            out.kind = kind == 'c' ? Value::NormFloat : kind == 'i' ? Value::Int : kind == 'b' ? Value::Bool : Value::Float;
            out.count = n;
            std::copy(values, values + n, out.v);
            return true;
        }
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
    case GL_BLEND_SRC_RGB: case GL_BLEND_DST_RGB: case GL_BLEND_SRC_ALPHA: case GL_BLEND_DST_ALPHA:
    case GL_BLEND_EQUATION_RGB: case GL_BLEND_EQUATION_ALPHA: {  // GL_ARB_draw_buffers_blend
        if (index >= static_cast<GLuint>(ContextState::kMaxDrawBuffers)) { badIndex = true; return true; }
        const ContextState::Blend& b = s.blend[index];
        out = ints({pname == GL_BLEND_SRC_RGB ? b.srcRGB : pname == GL_BLEND_DST_RGB ? b.dstRGB
                    : pname == GL_BLEND_SRC_ALPHA ? b.srcAlpha : pname == GL_BLEND_DST_ALPHA ? b.dstAlpha
                    : pname == GL_BLEND_EQUATION_RGB ? b.equationRGB : b.equationAlpha});
        return true;
    }
    case GL_VIEWPORT: case GL_DEPTH_RANGE: case GL_SCISSOR_BOX: case GL_SCISSOR_TEST:
        if (ctx.version() < 41 && !ctx.hasExtension("GL_ARB_viewport_array")) return false;
        if (index >= static_cast<GLuint>(ContextState::kMaxViewports)) { badIndex = true; return true; }
        if (pname == GL_VIEWPORT) {
            const GLfloat* v = s.viewports[index];
            out = floats({v[0], v[1], v[2], v[3]});
        } else if (pname == GL_DEPTH_RANGE) {
            out = floats({s.depthRanges[index][0], s.depthRanges[index][1]}, Value::NormFloat);
        } else if (pname == GL_SCISSOR_BOX) {
            const GLint* b = s.scissors[index];
            out = ints({b[0], b[1], b[2], b[3]});
        } else {
            out = bools({((s.scissorTests >> index) & 1) != 0});
        }
        return true;
    case GL_IMAGE_BINDING_NAME: case GL_IMAGE_BINDING_LEVEL: case GL_IMAGE_BINDING_LAYERED: case GL_IMAGE_BINDING_LAYER:
    case GL_IMAGE_BINDING_ACCESS: case GL_IMAGE_BINDING_FORMAT: {
        if (!ctx.imageLoadStore()) return false;
        if (index >= static_cast<GLuint>(ContextState::kMaxImageUnits)) { badIndex = true; return true; }
        const ImageUnit& u = s.imageUnits[index];
        switch (pname) {
        case GL_IMAGE_BINDING_NAME: out = ints({u.texture ? u.texture->name : 0}); break;
        case GL_IMAGE_BINDING_LEVEL: out = ints({u.level}); break;
        case GL_IMAGE_BINDING_LAYERED: out = bools({u.layered}); break;
        case GL_IMAGE_BINDING_LAYER: out = ints({u.layer}); break;
        case GL_IMAGE_BINDING_ACCESS: out = ints({u.access}); break;
        default: out = ints({u.format}); break;
        }
        return true;
    }
    case GL_ATOMIC_COUNTER_BUFFER_BINDING: case GL_ATOMIC_COUNTER_BUFFER_START: case GL_ATOMIC_COUNTER_BUFFER_SIZE: {
        if (!ctx.atomicCounters()) return false;
        if (index >= static_cast<GLuint>(ContextState::kMaxAtomicCounterBufferBindings)) { badIndex = true; return true; }
        const IndexedBufferBinding& b = s.atomicCounterBufferBindings[index];
        out = pname == GL_ATOMIC_COUNTER_BUFFER_BINDING ? ints({nameOf(b.buffer)})
              : pname == GL_ATOMIC_COUNTER_BUFFER_START ? int64v(b.offset)
                                                        : int64v(b.size);
        return true;
    }
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
