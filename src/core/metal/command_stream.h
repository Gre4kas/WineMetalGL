// Per-context command encoding.
//
// A context records into one command buffer at a time. Render passes start
// lazily: binding a framebuffer only describes the target, and the first draw
// or clear opens the encoder. Clears issued before any draw in a pass become
// load actions, so a typical frame (clear, draw, draw, swap) is one pass.
#pragma once

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <array>
#include <cstdint>
#include <functional>

#include "core/metal/queue.h"

namespace wmgl {

struct RenderTargetDesc {
    struct Target {
        id<MTLTexture> texture = nil;
        uint32_t level = 0;
        uint32_t slice = 0;        // array layer, cube face or 3D depth plane
        bool operator==(const Target& o) const {
            return texture == o.texture && level == o.level && slice == o.slice;
        }
    };
    std::array<Target, 8> color;
    Target depth, stencil;
    uint32_t width = 0, height = 0;
    uint32_t samples = 1;
    // GL's SAMPLE_BUFFERS: multisample rasterization applies, as it does to a
    // renderbuffer or texture of one sample, which Metal renders single-sample.
    bool multisampled = false;
    uint32_t layers = 0;           // renderTargetArrayLength for layered rendering (0 = not layered)
    // A multisampled default framebuffer's resolve texture for colour 0
    // (Surface): passes carry it, so a read of the resolved image while
    // the pass is open resolves as the pass ends (CommandStream::resolveOpenPass).
    id<MTLTexture> colorResolve = nil;
    bool operator==(const RenderTargetDesc& o) const;
    bool operator!=(const RenderTargetDesc& o) const { return !(*this == o); }
    bool hasAnyAttachment() const;
};

struct PendingClear {
    uint8_t colorMask = 0;         // attachments to clear
    std::array<MTLClearColor, 8> color{};
    bool depth = false, stencil = false;
    double depthValue = 1.0;
    uint32_t stencilValue = 0;
    bool any() const { return colorMask || depth || stencil; }
};

class CommandStream {
public:
    explicit CommandStream(Queue& queue) : queue_(queue) {}
    ~CommandStream();

    id<MTLCommandBuffer> commandBuffer();
    // Serial of the command buffer currently being recorded (creates one).
    // Inline: the draw path asks for it many times per draw.
    uint64_t serial() { return buffer_ ? serial_ : serialSlow(); }

    // Returns an encoder for the target, starting a pass if needed. Any
    // pending clear for this target is applied as load actions.
    id<MTLRenderCommandEncoder> render(const RenderTargetDesc& target);
    // render() for DrawState's draws, whose textures mark what they use
    // (Texture::touchSerial): uploads to other storage still run ahead
    // (preludeUpload). Other users' passes may read any texture.
    id<MTLRenderCommandEncoder> renderDraw(const RenderTargetDesc& target);
    id<MTLRenderCommandEncoder> currentRender() const { return render_; }
    // A render or compute encoder is open (work encoded now joins a draw or dispatch in progress).
    bool encoding() const { return render_ != nil || compute_ != nil; }
    const RenderTargetDesc& currentTarget() const { return target_; }
    // Bumped every time a new render encoder starts (dynamic state must be re-sent).
    uint64_t passGeneration() const { return passGeneration_; }
    // Calls of render(): every user of the render encoder makes one first, so
    // a caller that saw the count move by only its own call knows nobody
    // else set encoder state since (DrawState's dynamic state cache).
    uint64_t renderCalls() const { return renderCalls_; }

    // Records a clear for the target. Returns false if the clear cannot be a
    // load action (a pass on this target already has draws) — the caller then
    // clears with a draw.
    bool clearWithLoadAction(const RenderTargetDesc& target, const PendingClear& clear);
    // Forces pending load-action clears to execute (empty pass).
    void resolvePendingClear();
    // Ends a pass open on `color` (colour 0) that carries a resolve texture,
    // resolving into it; false if no such pass is open (the caller resolves).
    bool resolveOpenPass(id<MTLTexture> color);
    // glInvalidateFramebuffer of the target's attachments (colour slots in
    // colorMask, depth, stencil): while a pass on the target is open, it
    // ends without storing them unless a draw follows in it; otherwise the
    // target's next pass, if it is the next work recorded, does not load them.
    void invalidate(const RenderTargetDesc& target, uint8_t colorMask, bool depth, bool stencil);
    // GPU timing of encoders for GL_TIME_ELAPSED queries, without commits:
    // from the first query on, every render, compute and blit encoder samples
    // the GPU timestamp at its start and its end into a counter sample buffer.
    // beginTiming ends the open encoder and returns where the next encoder's
    // start sample goes; endTiming ends the open encoder and returns the
    // last sample taken since (below `first`: no encoder ran). False without
    // stage-boundary counter sampling (the caller commits instead).
    struct TimingMark {
        id<MTLCounterSampleBuffer> buffer = nil;
        uint32_t sample = 0;
    };
    bool beginTiming(TimingMark& first);
    void endTiming(TimingMark& last);
    // Whether the device samples timestamps at encoder boundaries.
    static bool gpuTiming();
    // Whether a pending load-action clear is for an attachment of `texture`
    // (or of a view of it): writes to it must let the clear run first.
    bool pendingClearUses(id<MTLTexture> texture) const;

    // keepPendingClear: the encoder's commands touch no texture of a pending
    // clear's target (buffer copies, a dispatch without textures or images),
    // so the clear stays a load action of the target's next pass instead of
    // running first as a pass of its own (which stores the attachments, for
    // the next pass to load them back).
    id<MTLBlitCommandEncoder> blit(bool keepPendingClear = false);
    // A blit that writes texture contents the API is replacing (uploads,
    // storage reallocation): deferred work (setDeferred) stays deferred.
    id<MTLBlitCommandEncoder> uploadBlit(bool keepPendingClear = false);
    // An upload to storage the command buffer being recorded has not used
    // (the caller knows: Texture::touchSerial): recorded in a command buffer
    // committed just ahead of it, so a render pass in progress goes on. Nil
    // once the command buffer has a blit or dispatch of its own (it may have
    // read or written the storage): the caller uploads in order then.
    id<MTLBlitCommandEncoder> preludeUpload();
    // A command buffer for initialising new storage (texture_storage.mm
    // initStorage), committed first at the next commit: one for all the
    // textures made until then rather than one each.
    id<MTLCommandBuffer> preludeInit();
    // Work the API defers until a command may read what it produces
    // (GL_GENERATE_MIPMAP regeneration after uploads, api/texture.mm): runs
    // before the next render, compute or blit encoder (other than an
    // upload's) and before a commit. Replaces earlier deferred work; the
    // caller's function drains everything it deferred.
    void setDeferred(std::function<void()> work) { deferred_ = std::move(work); }
    void runDeferred() {
        if (!deferred_ || runningDeferred_) return;
        std::function<void()> work = std::move(deferred_);
        deferred_ = nullptr;
        runningDeferred_ = true;
        work();
        runningDeferred_ = false;
    }
    id<MTLComputeCommandEncoder> compute(bool keepPendingClear = false);
    // Prelude: two command buffers committed just ahead of the one being
    // recorded, for work that reads nothing the GPU writes in it. Draws that
    // need such work first (tessellation: vertex capture, then the control
    // and tessellation factor kernels) record it there instead of ending the
    // render pass in progress, which on a tile-based GPU stores and reloads
    // every attachment. preludeRender: one rasterization-free pass without
    // attachments; preludeCompute: one compute pass, run after it.
    id<MTLRenderCommandEncoder> preludeRender();
    id<MTLComputeCommandEncoder> preludeCompute();
    // The GPU-driven multi-draw of patches (DrawState::drawPatchesIndirectGpu)
    // records each step of all its draws in a concurrent compute pass of its
    // own, in queue order: the scans (ahead of the capture pass), the
    // expansions, then (after the prelude's kernels) the control kernels and
    // the factor kernels. Each step reads only the previous steps' output,
    // which the command buffer boundaries order, so the draws' dispatches of
    // one step run side by side instead of one after another.
    enum class PreludeStep : uint8_t { Scan, Expand, Control, Factors, Count };
    id<MTLComputeCommandEncoder> preludeStep(PreludeStep step);
    // Commits the prelude and the command buffer being recorded, and waits
    // for the prelude's kernels (the CPU reads their results).
    void waitForPrelude();
    // Why an open render pass ends (stats: each end stores and reloads the
    // attachments on tile-based GPUs).
    enum class PassEnd : uint8_t { Other, Target, Blit, Compute, Visibility, Commit, Barrier, Clear, Count };
    void endEncoder(PassEnd cause = PassEnd::Other);
    // glMemoryBarrier: orders shader writes to buffers and textures before
    // later shader accesses: ends an open render pass, or a barrier in the
    // open compute encoder.
    // commands: the barrier orders shader writes before the GPU reads indirect
    // commands (GL_COMMAND_BARRIER_BIT). After a dispatch, the command buffer
    // holding it is committed then, so the GPU runs the dispatch (GPU culling)
    // while the CPU records what follows, and the CPU, when it reads the
    // commands, waits for that command buffer alone.
    void memoryBarrier(bool attachments = false, bool commands = false);
    // The open render or compute encoder writes through images or atomic
    // counters. Metal's hazard tracking does not order those writes (texture
    // atomics in particular) before later encoders, so the encoder updates a
    // fence when it ends and every later encoder waits for it.
    void noteShaderWrites() { shaderWrites_ = true; }

    // Commits the command buffer; returns its serial (0 if nothing was recorded).
    // keepPendingClear: a pending clear stays pending (for the next command
    // buffer's pass on its target) instead of running as a pass of its own:
    // for commits WineMetalGL makes itself, which nothing outside sees.
    uint64_t flush(bool keepPendingClear = false);
    // glFlush when nothing outside this context can see the commands yet
    // (one context in the share group, no front-buffer rendering): the
    // commit waits for the next render pass to start, a commit of its own,
    // or commitIfDeferred (an application waiting for a fence or query of
    // the commands), so a pass in progress goes on.
    void deferFlush() { flushDeferred_ = true; }
    bool flushDeferred() const { return flushDeferred_; }
    void commitIfDeferred() {
        if (flushDeferred_) flush(true);
    }
    // Nanoseconds (stats::nowNanos) of the last commit.
    uint64_t lastCommitNanos() const { return lastCommitNanos_; }
    // Commits and waits for all work from this stream to complete.
    void finish();
    // Waits for a serial, flushing first if it belongs to this stream.
    void waitFor(uint64_t serial);

    // Called when the recorded work presents a drawable.
    void present(id<CAMetalDrawable> drawable);

    // Occlusion queries. Render passes started while visibility is wanted
    // carry a visibility-result buffer; each slot counts samples for one
    // (query set, pass) pair. Wanting visibility ends a pass started without it.
    void setVisibilityWanted(bool wanted);
    bool visibilityWanted() const { return visibilityWanted_; }
    // Reserves a slot in the open pass's visibility buffer (false if none).
    bool allocateVisibilitySlot(__strong id<MTLBuffer>& buffer, NSUInteger& offset);
    // Whether a pass opened now could allocate a visibility slot.
    bool visibilitySlotAvailable() const;

    // Serial of the most recently committed command buffer of this stream.
    uint64_t lastCommitted() const { return lastCommitted_; }
    // Serial a fence created now covers: the command buffer being recorded, or,
    // when nothing is recorded or pending, the last committed one (0 if none).
    uint64_t fenceSerial() { return buffer_ || pending_.any() ? serial() : lastCommitted_; }

    Queue& queue() { return queue_; }
    bool hasDraws() const { return drawsInPass_; }
    void noteDraw() {
        drawsInPass_ = true;
        ++drawsSinceCommit_;
    }
    // Draws recorded in the command buffer being recorded.
    uint32_t drawsSinceCommit() const { return drawsSinceCommit_; }
    // The CPU waited this frame for indirect commands the GPU wrote: such a
    // wait drains the GPU, which then idles while the CPU records
    // (DrawState::commitLargeWork commits sooner). Until the next present.
    void noteIndirectWait() { indirectWaited_ = true; }
    bool indirectWaited() const { return indirectWaited_; }
    // Whether a draw to `target` continues the open render pass.
    bool continuesPass(const RenderTargetDesc& target) const { return render_ && target_ == target; }

private:
    uint64_t serialSlow();
    void commitPrelude();
    void startRender(const RenderTargetDesc& target);

    Queue& queue_;
    std::function<void()> deferred_;
    bool runningDeferred_ = false;
    bool flushDeferred_ = false;
    bool resolveAtEnd_ = false;  // resolveOpenPass: the open pass's colour 0 resolves
    // Timing (beginTiming): encoders opened while timing_ sample into
    // samples_ (two samples each, from nextSample_; a full buffer is replaced).
    bool timing_ = false;
    id<MTLCounterSampleBuffer> samples_ = nil;
    uint32_t nextSample_ = 0;
    // The two sample indices for a new encoder, or false (not timing).
    bool takeSamples(uint32_t& start, uint32_t& end);
    // invalidate(): attachments of the open pass it ends without storing,
    // and of the next pass on discardTarget_ it starts without loading.
    struct Discard {
        uint8_t color = 0;
        bool depth = false, stencil = false;
        bool any() const { return color || depth || stencil; }
    } storeDiscard_, loadDiscard_;
    RenderTargetDesc discardTarget_;
    id<MTLCommandBuffer> buffer_ = nil;
    uint64_t serial_ = 0;
    uint64_t lastCommitted_ = 0;
    uint64_t lastCommitNanos_ = 0;
    id<MTLRenderCommandEncoder> render_ = nil;
    id<MTLBlitCommandEncoder> blit_ = nil;
    id<MTLComputeCommandEncoder> compute_ = nil;
    id<MTLCommandBuffer> preludeRenderBuffer_ = nil, preludeComputeBuffer_ = nil;
    // preludeUpload's command buffer and encoder; hoistBlocked_: the command
    // buffer being recorded has a blit, dispatch or pass other than an
    // upload's or DrawState's draws.
    id<MTLCommandBuffer> preludeUploadBuffer_ = nil, preludeInitBuffer_ = nil;
    id<MTLBlitCommandEncoder> preludeUpload_ = nil;
    bool hoistBlocked_ = false;
    id<MTLCommandBuffer> stepBuffers_[static_cast<int>(PreludeStep::Count)] = {};
    id<MTLComputeCommandEncoder> stepEncoders_[static_cast<int>(PreludeStep::Count)] = {};
    id<MTLRenderCommandEncoder> preludeRender_ = nil;
    id<MTLComputeCommandEncoder> preludeCompute_ = nil;
    id<MTLCommandBuffer> lastPrelude_ = nil;  // the last prelude committed (waitForPrelude)
    RenderTargetDesc target_;
    RenderTargetDesc pendingTarget_;
    PendingClear pending_;
    bool drawsInPass_ = false;
    uint32_t drawsSinceCommit_ = 0;
    bool indirectWaited_ = false;
    uint64_t passGeneration_ = 0;
    uint64_t renderCalls_ = 0;
    bool visibilityWanted_ = false, passVisibility_ = false;
    bool visibilityUsedBefore_ = false;  // occlusion queries were used: passes keep a visibility buffer
    // noteShaderWrites: set while the open encoder writes through images;
    // fenced once such an encoder ended (later encoders wait for writesFence_).
    bool shaderWrites_ = false, fenced_ = false;
    uint64_t fenceSerial_ = 0;  // command buffer of the last fence update (it retires once that completes)
    id<MTLFence> writesFence_ = nil;
    void waitForShaderWrites(id<MTLCommandEncoder> encoder, bool render);
    id<MTLBuffer> visibility_ = nil;
    NSUInteger visibilityUsed_ = 0;
};

}  // namespace wmgl
