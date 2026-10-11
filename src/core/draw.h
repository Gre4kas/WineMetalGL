// The draw path: turns GL state into Metal render passes, pipelines and draws.
#pragma once

#import <Metal/Metal.h>

#include <array>
#include <memory>
#include <cstdint>
#include <vector>

#include "core/format/formats.h"
#include "core/gl/types.h"
#include "core/metal/command_stream.h"
#include "core/metal/pipeline_cache.h"
#include "core/metal/transient.h"
#include "core/shader/compiler.h"
#include "core/texture_storage.h"
#include "core/tess/tessellator.h"

namespace wmgl {

class Context;
class Buffer;
class Framebuffer;
class Program;
class VertexArray;
struct VertexAttrib;
struct LinkedProgram;

struct TargetFormats {
    MTLPixelFormat color[8] = {};
    MTLPixelFormat depth = MTLPixelFormatInvalid, stencil = MTLPixelFormatInvalid;
    bool alphaIsOne[8] = {};   // RGB formats stored with an alpha channel
    bool integer[8] = {};
    bool srgb[8] = {};
    // Depth/stencil buffers GL sees. A combined Metal format is bound to both
    // Metal attachments even when GL attaches it to one point only.
    bool depthAttached = false, stencilAttached = false;
};

class DrawState {
public:
    explicit DrawState(Context& ctx);

    // Describes the current draw framebuffer. Returns false (and sets
    // GL_INVALID_FRAMEBUFFER_OPERATION) if it is incomplete.
    bool drawTarget(RenderTargetDesc& target, TargetFormats& formats, bool reportErrors = true);
    // Read framebuffer colour/depth/stencil texture for glReadPixels and blits.
    id<MTLTexture> readColorTexture(uint32_t& level, uint32_t& slice, GLenum& internalFormat, int& width, int& height);
    // Read framebuffer source for GL_COLOR, GL_DEPTH or GL_STENCIL. For the
    // default framebuffer, colour is the resolved back buffer while `samples`
    // reports the surface's sample count. False if the buffer does not exist.
    bool readTarget(GLenum buffer, RenderTargetDesc::Target& out, const FormatInfo*& info, uint32_t& width,
                    uint32_t& height, uint32_t& samples);

    void drawArrays(GLenum mode, GLint first, GLsizei count, GLsizei instances);
    void drawElements(GLenum mode, GLsizei count, GLenum type, const void* indices, GLsizei instances, GLint baseVertex);
    void clear(GLbitfield mask);
    // glClearBuffer*: buffer is GL_COLOR (drawbuffer = draw buffer index),
    // GL_DEPTH, GL_STENCIL or GL_DEPTH_STENCIL. Colour values are converted
    // to the attachment's type.
    void clearBuffer(GLenum buffer, GLint drawbuffer, const double color[4], double depth, GLint stencil);

    // The executable that draws use: the current program's, else the bound
    // program pipeline's (GL_ARB_separate_shader_objects), else (in the
    // compatibility profile) the generated fixed-function program for the
    // draw begun with beginDraw. Null if none.
    LinkedProgram* executable();
    // Starts a draw of `mode`: the fixed-function program is chosen for it.
    void beginDraw(GLenum mode);

    // glDispatchCompute / glDispatchComputeIndirect (validated by the caller);
    // `indirect` is the DISPATCH_INDIRECT_BUFFER, or null.
    void dispatch(GLuint x, GLuint y, GLuint z, Buffer* indirect, GLintptr offset);

    TransientAllocator& transient() { return transient_; }

    // GL_ARB_base_instance: the base instance of the draw being issued.
    struct BaseInstanceScope {
        BaseInstanceScope(DrawState& d, GLuint base) : draw(d) { draw.baseInstance_ = base; }
        ~BaseInstanceScope() { draw.baseInstance_ = 0; }
        DrawState& draw;
    };

private:
    bool validateDraw(GLenum mode);
    id<MTLRenderCommandEncoder> prepare(GLenum mode, MTLPrimitiveType& primitive);
    // Encodes one draw. indexData (CPU, uint32) or indexBuffer select indexed drawing.
    // minVertex/vertexSpan is the vertex range attribute conversion must cover.
    bool issue(GLenum mode, GLint first, GLsizei count, GLsizei instances, const uint32_t* indexData,
               id<MTLBuffer> indexBuffer, NSUInteger indexOffset, MTLIndexType indexType, GLint baseVertex,
               GLint minVertex, GLsizei vertexSpan, bool listed = false);
    void clearWithDraw(const RenderTargetDesc& target, const TargetFormats& formats, const PendingClear& clear);
    // `gather`, when given, lists the vertex fetched for each vertex of a
    // non-indexed draw (transform feedback capture): per-vertex attributes
    // are gathered on the CPU in that order.
    bool bindVertexInputs(id<MTLRenderCommandEncoder> encoder, PipelineKey& key, LinkedProgram& program,
                          GLint firstVertex, GLsizei vertexCount, GLsizei instances,
                          const std::vector<uint32_t>* gather = nullptr);
    // Writes the transform feedback output of a draw: `vertices` are the
    // vertex numbers (gl_VertexID) in draw order.
    void captureTransformFeedback(GLenum mode, const std::vector<uint32_t>& vertices, GLsizei instances);
    // Runs the program's capture vertex function (rasterization off) over
    // `list`, writing vertex k of instance i to capture slot
    // k + i * list.size() (below `limit`) of each buffer at its base word.
    bool runVertexCapture(LinkedProgram& program, const std::vector<uint32_t>& list, GLsizei instances, uint32_t limit,
                          int bufferCount, const __strong id<MTLBuffer>* buffers, const uint32_t* baseWords,
                          const uint32_t* strides);
    // Draws `vertices` (vertex numbers in draw order, one restart-free
    // segment) through the geometry shader emulation (shader/geometry.h).
    // primitiveBase is the gl_PrimitiveIDIn of the first primitive.
    // `assembled`: `vertices` already lists each input primitive's vertices.
    void drawGeometry(GLenum mode, const std::vector<uint32_t>& vertices, GLsizei instances, uint32_t& primitiveBase,
                      bool assembled = false);
    // Fixed-function wide lines (compat/fixed_function.h): `vertices` (one
    // restart-free line stream) drawn as two triangles per segment.
    void drawWideLines(GLenum mode, const std::vector<uint32_t>& vertices, GLsizei instances);
    // Draws `vertices` (vertex numbers in draw order, one restart-free
    // segment) as GL_PATCHES through the tessellation emulation
    // (shader/tessellation.h).
    void drawPatches(const std::vector<uint32_t>& vertices, GLsizei instances);
    // Uniforms, uniform blocks and samplers of a compute function (the
    // `stage` slots: Stage::Compute for a compute program or the geometry
    // kernel, Stage::TessControl for the tessellation control kernel).
    // `kernel`: the kernel's stage output (its storage block arguments).
    void bindKernelResources(id<MTLComputeCommandEncoder> encoder, LinkedProgram& program, bool usesDefaultUniforms,
                             int stage = static_cast<int>(shader::Stage::Compute), const shader::StageOutput* kernel = nullptr);
    void bindCaptureDefaults(id<MTLRenderCommandEncoder> encoder, LinkedProgram& program);
    // Whether a draw must list its primitives with the provoking vertex first
    // (LAST_VERTEX_CONVENTION and flat fragment inputs).
    bool rotateProvoking(GLenum mode, LinkedProgram& program) const;
    // GL_POINT polygon mode for both faces: triangles are drawn as their vertices.
    bool pointPolygons(GLenum mode) const;
    // Depth/stencil, dynamic state, scissor, uniforms and textures for a draw.
    void bindDrawState(id<MTLRenderCommandEncoder> encoder, LinkedProgram& program);
    void uniformBlockBuffer(LinkedProgram& program, size_t block, __strong id<MTLBuffer>& buffer, NSUInteger& offset);
    // The buffer bound to atomic counter buffer `index`'s binding point (a
    // scratch buffer when none is), marked as written by the draw.
    void atomicCounterBuffer(LinkedProgram& program, size_t index, __strong id<MTLBuffer>& buffer, NSUInteger& offset);
    // Shader storage blocks (GL 4.3): fills stage `s`'s argument buffer
    // (shader::kSlotStorageBlocks) and makes its buffers resident. Returns
    // false if the stage has none.
    struct StorageArguments {
        id<MTLBuffer> buffer = nil;
        NSUInteger offset = 0;
        std::vector<std::pair<id<MTLBuffer>, bool>> resources;  // buffer, written
    };
    // `stage`'s storage block arguments index `owner`'s storage blocks.
    bool storageArguments(const shader::StageOutput& stage, const Program* owner, StorageArguments& out);
    void bindStorageBlocks(id<MTLRenderCommandEncoder> encoder, LinkedProgram& program, int s);
    void bindStorageBlocks(id<MTLComputeCommandEncoder> encoder, const shader::StageOutput& stage, const Program* owner);
    // The program object whose state feeds `stage` (pipelines: the stage's
    // program), and the owner and own index of a uniform block or sampler.
    Program* stageProgram(LinkedProgram& program, int stage);
    std::pair<Program*, size_t> blockSource(LinkedProgram& program, size_t block);
    std::pair<Program*, size_t> samplerSource(LinkedProgram& program, size_t index);
    std::pair<Program*, size_t> imageSource(LinkedProgram& program, size_t index);
    // The view image slot `index` reads (GL 4.2), or a dummy image.
    id<MTLTexture> imageBinding(LinkedProgram& program, size_t index);
    void samplerBinding(LinkedProgram& program, size_t index, __strong id<MTLTexture>& view,
                        __strong id<MTLSamplerState>& sampler);
    // How a draw's vertex inputs are fed (planVertexInputs): read directly
    // from buffer objects through shared layouts, converted on the CPU into
    // one interleaved buffer per divisor, or taken from current values. The
    // plan keeps the layouts within Metal's vertex buffer slots.
    struct VertexInputPlan {
        enum How : uint8_t { None, Constant, Direct, Converted, LineSide };
        struct DirectGroup {
            Buffer* buffer = nullptr;
            NSUInteger base = 0;  // lowest attribute offset in the group
            uint32_t stride = 0, divisor = 0;
        };
        std::array<How, 31> how{};      // VertexArray::kAttribSlots
        std::array<int8_t, 31> group{};  // Direct: index into groups
        std::vector<DirectGroup> groups;
    };
    void planVertexInputs(LinkedProgram& program, bool gather, VertexInputPlan& plan) const;
    // Image bindings with a shadow texture (core::ImageShadow) for the draw
    // in progress: copied in by prepareSamplerStorage, back when the draw ends.
    struct PendingShadow {
        ImageShadow shadow;
        bool write = false;
    };
    std::vector<PendingShadow> imageShadows_;
    void prepareImageShadows(LinkedProgram& program);
    void finishImageShadows();
    struct ImageShadowScope {
        DrawState& draw;
        explicit ImageShadowScope(DrawState& d) : draw(d) {}
        ~ImageShadowScope() { draw.finishImageShadows(); }
    };
    GLint imageUnit(LinkedProgram& program, size_t index);
    void applyBuiltinUniforms(Program& program);
    static size_t vertexBufferSlots(LinkedProgram& program);
    void waitForCpuVertexReads(LinkedProgram& program, bool gather);
    const VertexAttrib& attribFor(const VertexArray& vao, int location) const;
    void bindUniforms(id<MTLRenderCommandEncoder> encoder, LinkedProgram& program);
    void bindTextures(id<MTLRenderCommandEncoder> encoder, LinkedProgram& program);
    // Builds pending storage of every sampled texture before a draw opens its encoder.
    void prepareSamplerStorage(LinkedProgram& program);
    void applyDynamicState(id<MTLRenderCommandEncoder> encoder, const RenderTargetDesc& target);

    Context& ctx_;
    TransientAllocator transient_;
    uint64_t lastPassGeneration_ = 0;
    GLuint baseInstance_ = 0;
    // First vertex of the draw being bound when issue() renumbers vertices from it.
    GLint vertexRebase_ = 0;
    // GL sample mask the current draw's fragment function writes (~0: none; prepare()).
    uint32_t sampleMask_ = ~0u;
    std::shared_ptr<LinkedProgram> current_;  // keeps a pipeline executable alive during a draw
    // The executable draws use while a tessellated draw runs its evaluation
    // stage (null otherwise), and the buffers its vertex function reads
    // (shader::kSlotTessVertices, kSlotTessPatches, kSlotTessControl).
    LinkedProgram* override_ = nullptr;
    struct {
        bool active = false;
        id<MTLBuffer> buffers[3] = {};
        NSUInteger offsets[3] = {};
    } tessInputs_;
    std::unique_ptr<tess::Tessellator> tessellator_;
    // Fixed-function program of the current draw (owned by the share group's cache).
    Program* ownerProgram();
    GLenum drawMode_ = GL_TRIANGLES;
    bool ffChosen_ = false, ffPointSize_ = false, ffWideLines_ = false;
    // Wide line draws: the other endpoint and side of each expanded vertex.
    const std::vector<uint32_t>* linePartners_ = nullptr;
    const std::vector<float>* lineSides_ = nullptr;
    Program* ffProgram_ = nullptr;
    struct {
        id<MTLBuffer> buffer = nil;
        NSUInteger offset = 0;
        uint64_t generation = 0, serial = 0;
    } ffBlock_;
    PipelineKey lastKey_{};
    TargetFormats lastFormats_{};
    MTLScissorRect lastScissors_[16] = {};  // ContextState::kMaxViewports
};

}  // namespace wmgl
