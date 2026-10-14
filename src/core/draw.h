// The draw path: turns GL state into Metal render passes, pipelines and draws.
#pragma once

#import <Metal/Metal.h>

#include <array>
#include <memory>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "core/format/formats.h"
#include "core/gl/types.h"
#include "core/metal/command_stream.h"
#include "core/metal/pipeline_cache.h"
#include "core/metal/transient.h"
#include "core/shader/compiler.h"
#include "core/stats.h"
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

    // One glDraw*Indirect command, read by the GPU from `commands` at
    // `offset` (validated by the caller: inside the buffer, aligned): a
    // DrawArraysIndirectCommand, or with `type` (GL_UNSIGNED_SHORT or
    // GL_UNSIGNED_INT) a DrawElementsIndirectCommand of the element buffer.
    // False, having drawn nothing, when the draw needs the command on the
    // CPU (indirectCpuReason); the caller then reads it and draws.
    bool drawIndirect(GLenum mode, GLenum type, Buffer& commands, uintptr_t offset);

    // glDispatchCompute / glDispatchComputeIndirect (validated by the caller);
    // `indirect` is the DISPATCH_INDIRECT_BUFFER, or null.
    void dispatch(GLuint x, GLuint y, GLuint z, Buffer* indirect, GLintptr offset);

    TransientAllocator& transient() { return transient_; }

    // Commits the work recorded so far once it holds WINEMETALGL_COMMIT_DRAWS
    // draws, so the GPU runs it while the CPU records the rest. Otherwise the
    // GPU starts on a frame's work only at the next wait or SwapBuffers, and
    // a wait for GPU results (indirect commands read on the CPU) waits for
    // all of it. A commit ends the open render pass, which on a tile-based
    // GPU stores its attachments and loads them again, so a draw that
    // continues the pass (`target`, null for dispatches) commits only past
    // 8 times as many draws; one that starts a pass, or a dispatch, ends it
    // anyway. Called where no transient memory is held for work still to be
    // recorded: when draws are validated and dispatches start.
    void commitLargeWork(const RenderTargetDesc* target);

    // GL_ARB_base_instance: the base instance of the draw being issued.
    struct BaseInstanceScope {
        BaseInstanceScope(DrawState& d, GLuint base) : draw(d) { draw.baseInstance_ = base; }
        ~BaseInstanceScope() { draw.baseInstance_ = 0; }
        DrawState& draw;
    };
    // gl_DrawID of the draws in scope: their index in a multi-draw command.
    struct DrawIdScope {
        DrawIdScope(DrawState& d, GLsizei id) : draw(d) { draw.drawId_ = id; }
        ~DrawIdScope() { draw.drawId_ = 0; }
        DrawState& draw;
    };

private:
    bool validateDraw(GLenum mode);
    id<MTLRenderCommandEncoder> prepare(GLenum mode, MTLPrimitiveType& primitive);
    // An indirect draw command in a buffer, read by the GPU.
    struct IndirectSource {
        id<MTLBuffer> buffer;
        NSUInteger offset;
    };
    // Encodes one draw. indexData (CPU, uint32) or indexBuffer select indexed drawing.
    // minVertex/vertexSpan is the vertex range attribute conversion must cover.
    // With `indirect`, the GPU reads the command (count, instances, first and
    // base vertex) from it instead (draws indirectCpuReason allows).
    bool issue(GLenum mode, GLint first, GLsizei count, GLsizei instances, const uint32_t* indexData,
               id<MTLBuffer> indexBuffer, NSUInteger indexOffset, MTLIndexType indexType, GLint baseVertex,
               GLint minVertex, GLsizei vertexSpan, bool listed = false, const IndirectSource* indirect = nullptr);
    // Why a draw of `mode` (`type`: of indices, or GL_NONE) with the current
    // state cannot take its command from the GPU: something needs the
    // command's values on the CPU (vertex ranges to convert, indices to
    // rewrite, emulated stages, counting queries, gl_BaseVertex/BaseInstance).
    // stats::kIndirectCpuReasons when it can.
    stats::IndirectCpu indirectCpuReason(GLenum mode, GLenum type, LinkedProgram& program);
    // WINEMETALGL_STATS: counts and describes `reason` for the draw.
    void noteIndirectCpu(stats::IndirectCpu reason, GLenum mode, GLenum type, LinkedProgram& program);
    // No 16-bit (`type` GL_UNSIGNED_SHORT) or 32-bit index in the buffer is
    // the type's largest value: Metal always restarts strips there (lists
    // never), GL only with restart enabled (lists too). Scanned once per
    // contents; false for buffers the CPU cannot read now or that the
    // application writes behind GL's back.
    bool noMetalRestartIndex(Buffer& buffer, GLenum type);
    void clearWithDraw(const RenderTargetDesc& target, const TargetFormats& formats, const PendingClear& clear);
    // `gather`, when given, lists the vertex fetched for each vertex of a
    // non-indexed draw (transform feedback capture): per-vertex attributes
    // are gathered on the CPU in that order.
    bool bindVertexInputs(id<MTLRenderCommandEncoder> encoder, PipelineKey& key, LinkedProgram& program,
                          GLint firstVertex, GLsizei vertexCount, GLsizei instances,
                          const std::vector<uint32_t>* gather = nullptr);
    // Writes the transform feedback output of a draw: `vertices` are the
    // vertex numbers (gl_VertexID) in draw order. Returns kNotCaptured (the
    // draw still has to run its vertex stage), kCaptured (a capture pass ran
    // it for every vertex) or kCapturedAndDrawn (the draw is done: a vertex
    // stage that writes memory captures while rasterizing, so it runs once).
    enum Capture { kNotCaptured, kCaptured, kCapturedAndDrawn };
    Capture captureTransformFeedback(GLenum mode, const std::vector<uint32_t>& vertices, GLsizei instances);
    // Runs the program's capture vertex function (rasterization off) over
    // `list`, writing vertex k of instance i to capture slot
    // k + i * list.size() (below `limit`) of each buffer at its base word.
    // With `drawMode` (GL_POINTS, GL_LINES or GL_TRIANGLES) the list is drawn
    // as those primitives with the current state instead, rasterized, and
    // *rasterized says whether it was (not when the state draws nothing).
    bool runVertexCapture(LinkedProgram& program, const std::vector<uint32_t>& list, GLsizei instances, uint32_t limit,
                          int bufferCount, const __strong id<MTLBuffer>* buffers, const uint32_t* baseWords,
                          const uint32_t* strides, GLenum drawMode = GL_NONE, bool* rasterized = nullptr);
    // The vertex stage writes memory (storage blocks, atomic counters or images).
    static bool vertexWritesMemory(LinkedProgram& program);
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
    // Whether a draw of `program`'s patches can use Metal's tessellator
    // instead of reading the levels back for the CPU's: triangles or quads
    // (not point mode), rasterized, and nothing that needs the tessellated
    // primitives on the CPU (a geometry shader, transform feedback, queries).
    bool metalTessellation(LinkedProgram& program, LinkedProgram& evaluation);
    // Draws `patches` patches (patch records at `records`) with Metal's
    // tessellator, the levels' factors at `factors` (wmgl_tess_factors).
    // False, having drawn nothing, when its pipeline cannot be built: the
    // caller tessellates on the CPU, as later draws of the program do.
    bool drawMetalPatches(LinkedProgram& program, LinkedProgram& evaluation, id<MTLBuffer> records, NSUInteger recordsOffset,
                          id<MTLBuffer> factors, NSUInteger factorsOffset, uint64_t patches, uint32_t patchesPerInstance,
                          uint32_t patchVertices);
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
    // The clamp-to-border emulation variant the textures and samplers bound
    // for stage `stage`'s samplers need (shader::borderVariantId; 0: none).
    uint16_t borderVariant(LinkedProgram& program, int stage);
    std::pair<Program*, size_t> imageSource(LinkedProgram& program, size_t index);
    // The view image slot `index` reads (GL 4.2), or a dummy image.
    id<MTLTexture> imageBinding(LinkedProgram& program, size_t index);
    // `used`: the texture sampled (null: a dummy), whose lastUseSerial the binding sets.
    void samplerBinding(LinkedProgram& program, size_t index, __strong id<MTLTexture>& view,
                        __strong id<MTLSamplerState>& sampler, Texture** used = nullptr);
    void markSampled(Texture& texture);
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
    // The plan waitForCpuVertexReads made, for the same draw's bindVertexInputs
    // (plannedFor_: drawCount_ of that draw, 0: none).
    VertexInputPlan plannedInputs_;
    uint64_t plannedFor_ = 0;
    const LinkedProgram* plannedProgram_ = nullptr;
    bool plannedGather_ = false;
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
    void bindUniforms(id<MTLRenderCommandEncoder> encoder, LinkedProgram& program, bool known);
    // `known`: encoderState_ holds what the encoder has bound (encoderStateValid_).
    void bindTextures(id<MTLRenderCommandEncoder> encoder, LinkedProgram& program, bool known);
    // Builds pending storage of every sampled texture before a draw opens its encoder.
    void prepareSamplerStorage(LinkedProgram& program);
    // What prepareSamplerStorage found for each sampler of `preparedProgram_`,
    // bound by the same draw's bindTextures (nothing changes in between).
    struct PreparedSampler {
        id<MTLTexture> view;
        id<MTLSamplerState> sampler;
        Texture* texture;
    };
    std::vector<PreparedSampler> preparedSamplers_;
    const LinkedProgram* preparedProgram_ = nullptr;
    void applyDynamicState(id<MTLRenderCommandEncoder> encoder, const RenderTargetDesc& target);

    Context& ctx_;
    TransientAllocator transient_;
    uint64_t lastPassGeneration_ = 0;
    GLuint baseInstance_ = 0;
    GLint glBaseVertex_ = 0;  // the command's base vertex (gl_BaseVertex), 0 for array draws
    GLsizei drawId_ = 0;
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
    // The order a capture pass draws its list in (provokingOrder), or none.
    const std::vector<uint32_t>* captureOrder_ = nullptr;
    const std::vector<float>* lineSides_ = nullptr;
    Program* ffProgram_ = nullptr;
    struct {
        id<MTLBuffer> buffer = nil;
        NSUInteger offset = 0;
        uint64_t generation = 0, serial = 0;
    } ffBlock_;
    PipelineKey lastKey_{};
    // The pipeline of the last draw through issue() and its key (programs carry
    // their link's uid in the key, so a relinked program misses).
    // Array draws whose numbering is rewritten (fans, quads, loops,
    // provoking-vertex rotation): the rewritten indices of 0..count-1 per
    // (mode, rotation, count), drawn with the draw's first vertex as base.
    struct IndexPattern {
        id<MTLBuffer> buffer = nil;
        NSUInteger count = 0;
    };
    std::unordered_map<uint64_t, IndexPattern> indexPatterns_;
    const IndexPattern* indexPattern(GLenum mode, bool rotate, GLsizei count);
    // The draw target validateDraw built, for the same draw's first prepare()
    // (validatedFor_: drawCount_ of that draw, 0: none).
    uint64_t drawCount_ = 0, validatedFor_ = 0;
    RenderTargetDesc validatedTarget_;
    TargetFormats validatedFormats_{};
    DepthStencilKey lastDepthStencilKey_{};
    id<MTLDepthStencilState> lastDepthStencil_ = nil;
    PipelineKey lastPipelineKey_{};
    id<MTLRenderPipelineState> lastPipeline_ = nil;
    // The dynamic state applyDynamicState last set (and the scissors), on the
    // encoder of pass `pass` when the stream's render() count was `calls`:
    // unchanged values are not set again while nothing else used the encoder.
    static constexpr int kTrackedTextures = 32;
    struct EncoderState {
        uint64_t pass = 0, calls = 0;
        MTLViewport viewports[16];  // ContextState::kMaxViewports
        MTLScissorRect scissors[16];
        MTLCullMode cull;
        MTLWinding winding;
        MTLTriangleFillMode fill;
        float bias[3];
        MTLDepthClipMode clip;
        uint32_t stencil[2];
        float blend[4];
        id<MTLDepthStencilState> depthStencil = nil;
        // Driver uniforms and each stage's (vertex, fragment) default uniforms
        // set by bindUniforms: the owner program, its link, uniformVersion and size.
        uint8_t driver[16];
        uint32_t fragmentDriver[2];
        bool driverSet = false, fragmentDriverSet = false;
        struct Uniforms {
            const Program* owner = nullptr;
            uint64_t uid = 0, version = 0;
            uint32_t size = 0;
        } uniforms[2];
        // Textures and samplers set per stage (vertex, fragment) by bindTextures.
        id<MTLTexture> textures[2][kTrackedTextures];
        id<MTLSamplerState> samplers[2][kTrackedTextures];
    } encoderState_;
    bool encoderStateValid_ = false;
    TargetFormats lastFormats_{};
    MTLScissorRect lastScissors_[16] = {};  // ContextState::kMaxViewports
};

}  // namespace wmgl
