// Per-context OpenGL state. Every setter marks the dirty bits the draw path
// uses to decide which Metal state needs rebuilding.
#pragma once

#include <array>
#include <cstdint>

#include "core/compat/fixed_state.h"
#include "core/gl/types.h"
#include "core/objects.h"

namespace wmgl {

enum Dirty : uint64_t {
    kDirtyViewport = 1ull << 0,
    kDirtyScissor = 1ull << 1,
    kDirtyRaster = 1ull << 2,        // cull, winding, polygon mode/offset, depth clamp
    kDirtyDepthStencil = 1ull << 3,
    kDirtyBlend = 1ull << 4,         // pipeline-affecting blend and colour-mask state
    kDirtyBlendColor = 1ull << 5,
    kDirtyStencilRef = 1ull << 6,
    kDirtyProgram = 1ull << 7,
    kDirtyVertexInput = 1ull << 8,
    kDirtyTextures = 1ull << 9,
    kDirtyUniforms = 1ull << 10,
    kDirtyUniformBuffers = 1ull << 11,
    kDirtyFramebuffer = 1ull << 12,
    kDirtyMultisample = 1ull << 13,
    kDirtyDriverUniforms = 1ull << 14,
    kDirtyAll = ~0ull,
};

struct StencilFace {
    GLenum func = GL_ALWAYS;
    GLint ref = 0;
    GLuint valueMask = ~0u;
    GLuint writeMask = ~0u;
    GLenum sfail = GL_KEEP, dpfail = GL_KEEP, dppass = GL_KEEP;
};

struct PixelStore {
    GLint alignment = 4;
    GLint rowLength = 0;
    GLint imageHeight = 0;
    GLint skipPixels = 0;
    GLint skipRows = 0;
    GLint skipImages = 0;
    bool swapBytes = false;
    bool lsbFirst = false;
    // GL_ARB_compressed_texture_pixel_storage (0 = not set).
    GLint blockWidth = 0, blockHeight = 0, blockDepth = 0, blockSize = 0;
};

struct IndexedBufferBinding {
    RefPtr<Buffer> buffer;
    GLintptr offset = 0;
    GLsizeiptr size = 0;  // 0 = whole buffer
};

struct TextureUnit {
    // Indexed by TextureTarget.
    std::array<RefPtr<Texture>, 11> bound;
    RefPtr<Sampler> sampler;
};

// Dense indices for the texture targets of GL 3.3 (plus rectangle/buffer).
enum TextureTarget : uint8_t {
    kTex1D, kTex2D, kTex3D, kTex1DArray, kTex2DArray, kTexRectangle, kTexCubeMap, kTexBuffer,
    kTex2DMultisample, kTex2DMultisampleArray, kTexCubeMapArray, kTexTargetCount
};
int textureTargetIndex(GLenum target);  // -1 if not a texture target
GLenum textureTargetEnum(int index);

struct ContextState {
    // Rasterization and fragment operations.
    GLint viewport[4] = {0, 0, 0, 0};
    GLdouble depthRange[2] = {0.0, 1.0};
    bool scissorTest = false;
    GLint scissor[4] = {0, 0, 0, 0};
    bool cullFace = false;
    GLenum cullFaceMode = GL_BACK;
    GLenum frontFace = GL_CCW;
    GLenum polygonMode = GL_FILL;  // FRONT_AND_BACK only in core
    GLenum polygonModeBack = GL_FILL;
    bool polygonOffsetFill = false, polygonOffsetLine = false, polygonOffsetPoint = false;
    GLfloat polygonOffsetFactor = 0.0f, polygonOffsetUnits = 0.0f, polygonOffsetClamp = 0.0f;
    bool depthClamp = false;
    bool rasterizerDiscard = false;
    GLfloat lineWidth = 1.0f;
    GLfloat pointSize = 1.0f;
    bool programPointSize = false;
    GLenum pointSpriteCoordOrigin = GL_UPPER_LEFT;
    GLfloat pointFadeThresholdSize = 1.0f;
    bool lineSmooth = false, polygonSmooth = false;
    GLenum provokingVertex = GL_LAST_VERTEX_CONVENTION;
    GLenum clipOrigin = GL_LOWER_LEFT, clipDepthMode = GL_NEGATIVE_ONE_TO_ONE;  // GL_ARB_clip_control
    bool primitiveRestart = false;
    GLuint primitiveRestartIndex = 0;
    bool primitiveRestartFixedIndex = false;
    bool textureCubeMapSeamless = false;
    uint32_t clipDistanceEnables = 0;

    bool depthTest = false;
    GLenum depthFunc = GL_LESS;
    bool depthMask = true;
    bool stencilTest = false;
    StencilFace stencilFront, stencilBack;

    static constexpr int kMaxDrawBuffers = 8;
    uint8_t blendEnable = 0;  // bit per draw buffer
    // Per draw buffer (GL_ARB_draw_buffers_blend); the non-indexed commands set all.
    struct Blend {
        GLenum srcRGB = GL_ONE, dstRGB = GL_ZERO, srcAlpha = GL_ONE, dstAlpha = GL_ZERO;
        GLenum equationRGB = GL_FUNC_ADD, equationAlpha = GL_FUNC_ADD;
    };
    Blend blend[8];
    GLfloat blendColor[4] = {0, 0, 0, 0};
    std::array<std::array<bool, 4>, kMaxDrawBuffers> colorMask;
    bool colorLogicOp = false;
    GLenum logicOp = GL_COPY;
    bool dither = true;
    bool framebufferSRGB = false;

    bool multisample = true;
    bool sampleAlphaToCoverage = false, sampleAlphaToOne = false, sampleCoverage = false;
    GLfloat sampleCoverageValue = 1.0f;
    bool sampleCoverageInvert = false;
    bool sampleMask = false;
    GLbitfield sampleMaskValue = ~0u;
    bool sampleShading = false;
    GLfloat minSampleShading = 0.0f;

    // Clear values.
    GLfloat clearColor[4] = {0, 0, 0, 0};
    GLdouble clearDepth = 1.0;
    GLint clearStencil = 0;

    // Pixel storage.
    PixelStore pack, unpack;
    bool clampReadColor = true;  // GL_FIXED_ONLY default handled at read time
    GLenum clampReadColorMode = GL_FIXED_ONLY;

    // Hints.
    GLenum hintGenerateMipmap = GL_DONT_CARE, hintLineSmooth = GL_DONT_CARE, hintPolygonSmooth = GL_DONT_CARE,
           hintTextureCompression = GL_DONT_CARE, hintFragmentShaderDerivative = GL_DONT_CARE;

    // Bindings.
    RefPtr<Buffer> arrayBuffer, copyReadBuffer, copyWriteBuffer, pixelPackBuffer, pixelUnpackBuffer,
        textureBuffer, uniformBuffer, transformFeedbackBuffer, drawIndirectBuffer, dispatchIndirectBuffer,
        shaderStorageBuffer, atomicCounterBuffer, queryBuffer;
    static constexpr int kMaxUniformBufferBindings = 36;  // = limits::kMaxUniformBufferBindings
    std::array<IndexedBufferBinding, kMaxUniformBufferBindings> uniformBufferBindings;
    static constexpr int kMaxTextureUnits = 80;
    std::array<TextureUnit, kMaxTextureUnits> textureUnits;
    GLuint activeTexture = 0;
    RefPtr<VertexArray> vertexArray;      // never null (default VAO in compatibility profile)
    RefPtr<Framebuffer> drawFramebuffer;  // null = default framebuffer
    RefPtr<Framebuffer> readFramebuffer;
    RefPtr<Renderbuffer> renderbuffer;
    RefPtr<Program> program;
    RefPtr<ProgramPipeline> programPipeline;  // used when no program is current
    RefPtr<TransformFeedback> transformFeedback;  // never null
    // Default framebuffer draw/read buffers.
    GLenum defaultDrawBuffer = GL_BACK;
    GLenum defaultReadBuffer = GL_BACK;

    // Active queries by target index.
    std::array<RefPtr<Query>, 8> activeQueries;
    struct {
        RefPtr<Query> query;
        GLenum mode = 0;
        bool active = false;
    } conditionalRender;

    // Generic (current) vertex attribute values.
    struct GenericAttrib {
        union {
            GLfloat f[4];
            GLint i[4];
            GLuint u[4];
        } value = {{0, 0, 0, 1}};
        GLenum type = GL_FLOAT;  // GL_FLOAT, GL_INT, GL_UNSIGNED_INT
    };
    // Generic attributes, then the conventional current values (current
    // colour, normal, texture coordinates, ...; compat/fixed_state.h).
    std::array<GenericAttrib, VertexArray::kAttribSlots> genericAttribs;

    // Compatibility profile fixed-function state.
    FixedState fixed;

    // Debug output (KHR_debug).
    bool debugOutput = false, debugOutputSynchronous = false;

    ContextState() {
        static_assert(kInternalAttribEnd == VertexArray::kAttribSlots, "conventional attribute slots");
        for (auto& mask : colorMask) mask = {true, true, true, true};
        // Initial current values (GL 3.3 compatibility table 6.5).
        genericAttribs[kConvColor].value = {{1, 1, 1, 1}};
        genericAttribs[kConvNormal].value = {{0, 0, 1, 1}};
        genericAttribs[kConvSecondaryColor].value = {{0, 0, 0, 1}};
        genericAttribs[kConvFogCoord].value = {{0, 0, 0, 1}};
    }
};

}  // namespace wmgl
