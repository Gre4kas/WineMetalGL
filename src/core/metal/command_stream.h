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
    uint32_t layers = 0;           // renderTargetArrayLength for layered rendering (0 = not layered)
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

    id<MTLBlitCommandEncoder> blit();
    id<MTLComputeCommandEncoder> compute();
    // Why an open render pass ends (stats: each end stores and reloads the
    // attachments on tile-based GPUs).
    enum class PassEnd : uint8_t { Other, Target, Blit, Compute, Visibility, Commit, Barrier, Clear, Count };
    void endEncoder(PassEnd cause = PassEnd::Other);
    // glMemoryBarrier: orders shader writes to buffers and textures before
    // later shader accesses: ends an open render pass, or a barrier in the
    // open compute encoder.
    void memoryBarrier(bool attachments = false);
    // The open render or compute encoder writes through images or atomic
    // counters. Metal's hazard tracking does not order those writes (texture
    // atomics in particular) before later encoders, so the encoder updates a
    // fence when it ends and every later encoder waits for it.
    void noteShaderWrites() { shaderWrites_ = true; }

    // Commits the command buffer; returns its serial (0 if nothing was recorded).
    uint64_t flush();
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

private:
    uint64_t serialSlow();
    void startRender(const RenderTargetDesc& target);

    Queue& queue_;
    id<MTLCommandBuffer> buffer_ = nil;
    uint64_t serial_ = 0;
    uint64_t lastCommitted_ = 0;
    id<MTLRenderCommandEncoder> render_ = nil;
    id<MTLBlitCommandEncoder> blit_ = nil;
    id<MTLComputeCommandEncoder> compute_ = nil;
    RenderTargetDesc target_;
    RenderTargetDesc pendingTarget_;
    PendingClear pending_;
    bool drawsInPass_ = false;
    uint32_t drawsSinceCommit_ = 0;
    uint64_t passGeneration_ = 0;
    uint64_t renderCalls_ = 0;
    bool visibilityWanted_ = false, passVisibility_ = false;
    bool visibilityUsedBefore_ = false;  // occlusion queries were used: passes keep a visibility buffer
    // noteShaderWrites: set while the open encoder writes through images;
    // fenced once such an encoder ended (later encoders wait for writesFence_).
    bool shaderWrites_ = false, fenced_ = false;
    id<MTLFence> writesFence_ = nil;
    void waitForShaderWrites(id<MTLCommandEncoder> encoder, bool render);
    id<MTLBuffer> visibility_ = nil;
    NSUInteger visibilityUsed_ = 0;
};

}  // namespace wmgl
