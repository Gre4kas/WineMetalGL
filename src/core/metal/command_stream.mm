#include "core/metal/command_stream.h"

#include "core/stats.h"

#import <QuartzCore/CAMetalLayer.h>

#include <cstdlib>
#include <cstring>

#include "core/metal/device.h"
#include "core/util/log.h"

namespace wmgl {

namespace {
constexpr NSUInteger kVisibilitySlots = 4096;
}  // namespace

bool RenderTargetDesc::operator==(const RenderTargetDesc& o) const {
    return color == o.color && depth == o.depth && stencil == o.stencil && width == o.width && height == o.height &&
           samples == o.samples && layers == o.layers;
}

bool RenderTargetDesc::hasAnyAttachment() const {
    for (const Target& t : color)
        if (t.texture) return true;
    return depth.texture || stencil.texture;
}

CommandStream::~CommandStream() { flush(); }

id<MTLCommandBuffer> CommandStream::commandBuffer() {
    if (!buffer_) buffer_ = queue_.create(&serial_);
    return buffer_;
}

uint64_t CommandStream::serialSlow() {
    commandBuffer();
    return serial_;
}

static_assert(static_cast<size_t>(CommandStream::PassEnd::Count) == sizeof(stats::Frame::passEnds) / sizeof(uint64_t),
              "stats::Frame::passEnds has a counter per PassEnd");

void CommandStream::endEncoder(PassEnd cause) {
    if (render_ && stats::g_enabled) ++stats::frame().passEnds[static_cast<int>(cause)];
    if (shaderWrites_ && (render_ || compute_)) {
        if (!writesFence_) writesFence_ = [Device::get().mtl() newFence];
        if (render_) [render_ updateFence:writesFence_ afterStages:MTLRenderStageVertex | MTLRenderStageFragment];
        else [compute_ updateFence:writesFence_];
        fenced_ = true;
        fenceSerial_ = serial();
    }
    shaderWrites_ = false;
    if (render_) {
        // Every attachment is stored but those invalidated since the pass's last draw.
        for (size_t i = 0; i < target_.color.size(); ++i)
            if (target_.color[i].texture)
                [render_ setColorStoreAction:storeDiscard_.color & (1u << i) ? MTLStoreActionDontCare : MTLStoreActionStore
                                     atIndex:i];
        if (target_.depth.texture)
            [render_ setDepthStoreAction:storeDiscard_.depth ? MTLStoreActionDontCare : MTLStoreActionStore];
        if (target_.stencil.texture)
            [render_ setStencilStoreAction:storeDiscard_.stencil ? MTLStoreActionDontCare : MTLStoreActionStore];
        storeDiscard_ = Discard();
        [render_ endEncoding];
        render_ = nil;
    }
    if (blit_) {
        [blit_ endEncoding];
        blit_ = nil;
    }
    if (compute_) {
        [compute_ endEncoding];
        compute_ = nil;
    }
}

void CommandStream::waitForShaderWrites(id<MTLCommandEncoder> encoder, bool render) {
    if (!fenced_) return;
    // Writes of a completed command buffer are visible to every later one.
    if (fenceSerial_ != serial_ && queue_.isComplete(fenceSerial_)) {
        fenced_ = false;
        return;
    }
    if (render)
        [static_cast<id<MTLRenderCommandEncoder>>(encoder) waitForFence:writesFence_
                                                           beforeStages:MTLRenderStageVertex | MTLRenderStageFragment];
    else if ([encoder conformsToProtocol:@protocol(MTLBlitCommandEncoder)])
        [static_cast<id<MTLBlitCommandEncoder>>(encoder) waitForFence:writesFence_];
    else
        [static_cast<id<MTLComputeCommandEncoder>>(encoder) waitForFence:writesFence_];
}

void CommandStream::startRender(const RenderTargetDesc& target) {
    stats::Timer timer(&stats::Frame::passStartNanos);
    {
        stats::Timer end(&stats::Frame::passEndNanos);
        endEncoder(PassEnd::Target);
    }
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    const bool clearsApply = pending_.any() && pendingTarget_ == target;
    // Attachments invalidated since the last work recorded are not loaded.
    const Discard noLoad = loadDiscard_.any() && discardTarget_ == target ? loadDiscard_ : Discard();
    loadDiscard_ = Discard();
    storeDiscard_ = Discard();
    for (size_t i = 0; i < target.color.size(); ++i) {
        const auto& t = target.color[i];
        if (!t.texture) continue;
        MTLRenderPassColorAttachmentDescriptor* a = pass.colorAttachments[i];
        a.texture = t.texture;
        a.level = t.level;
        if (t.texture.textureType == MTLTextureType3D) a.depthPlane = t.slice;
        else a.slice = t.slice;
        if (clearsApply && (pending_.colorMask & (1u << i))) {
            a.loadAction = MTLLoadActionClear;
            a.clearColor = pending_.color[i];
        } else {
            a.loadAction = noLoad.color & (1u << i) ? MTLLoadActionDontCare : MTLLoadActionLoad;
        }
        a.storeAction = MTLStoreActionUnknown;  // chosen when the pass ends (endEncoder)
    }
    if (target.depth.texture) {
        MTLRenderPassDepthAttachmentDescriptor* d = pass.depthAttachment;
        d.texture = target.depth.texture;
        d.level = target.depth.level;
        d.slice = target.depth.slice;
        d.loadAction = clearsApply && pending_.depth ? MTLLoadActionClear
                       : noLoad.depth                ? MTLLoadActionDontCare
                                                     : MTLLoadActionLoad;
        d.clearDepth = pending_.depthValue;
        d.storeAction = MTLStoreActionUnknown;
    }
    if (target.stencil.texture) {
        MTLRenderPassStencilAttachmentDescriptor* s = pass.stencilAttachment;
        s.texture = target.stencil.texture;
        s.level = target.stencil.level;
        s.slice = target.stencil.slice;
        s.loadAction = clearsApply && pending_.stencil ? MTLLoadActionClear
                       : noLoad.stencil                ? MTLLoadActionDontCare
                                                       : MTLLoadActionLoad;
        s.clearStencil = pending_.stencilValue;
        s.storeAction = MTLStoreActionUnknown;
    }
    if (!target.hasAnyAttachment()) {
        // Framebuffer with no attachments (ARB_framebuffer_no_attachments).
        pass.renderTargetWidth = target.width;
        pass.renderTargetHeight = target.height;
        pass.defaultRasterSampleCount = target.samples;
    }
    if (target.layers) pass.renderTargetArrayLength = target.layers;
    passVisibility_ = false;
    // Once a context uses occlusion queries, every pass carries the buffer:
    // a query begun mid-pass then needs no new pass.
    if (visibilityWanted_ || visibilityUsedBefore_) {
        if (!visibility_) {
            visibility_ = [Device::get().mtl() newBufferWithLength:kVisibilitySlots * 8 options:MTLResourceStorageModeShared];
            std::memset(visibility_.contents, 0, kVisibilitySlots * 8);
            // Slot 0 is never a query's: every pass starts counting at offset
            // 0 (and disabled counting points there), and a later pass in the
            // command buffer resets what an earlier one counted there.
            visibilityUsed_ = 1;
        }
        pass.visibilityResultBuffer = visibility_;
        passVisibility_ = true;
    }
    uint32_t sampleStart, sampleEnd;
    if (takeSamples(sampleStart, sampleEnd)) {
        pass.sampleBufferAttachments[0].sampleBuffer = samples_;
        // From the vertex stage's start to the fragment stage's end. A
        // tile-based GPU runs a pass's vertex stage ahead, beside the
        // fragments of the pass before: queries start no earlier than that
        // pass's end (api/query.mm timedNanos).
        pass.sampleBufferAttachments[0].startOfVertexSampleIndex = sampleStart;
        pass.sampleBufferAttachments[0].endOfVertexSampleIndex = MTLCounterDontSample;
        pass.sampleBufferAttachments[0].startOfFragmentSampleIndex = MTLCounterDontSample;
        pass.sampleBufferAttachments[0].endOfFragmentSampleIndex = sampleEnd;
    }
    if (clearsApply) pending_ = PendingClear();
    id<MTLCommandBuffer> buffer;
    {
        stats::Timer t(&stats::Frame::passBufferNanos);
        buffer = commandBuffer();
    }
    {
        stats::Timer t(&stats::Frame::passEncoderNanos);
        render_ = [buffer renderCommandEncoderWithDescriptor:pass];
    }
    if (stats::g_enabled) {
        stats::Timer t(&stats::Frame::passLabelNanos);
        // Pass names in Metal System Trace: target size, colour attachments,
        // depth, and the pass number in the frame.
        int colors = 0;
        for (const auto& t : target.color) colors += t.texture != nil;
        render_.label = [NSString stringWithFormat:@"WMGL pass %llu %ux%u c%d%s%s", static_cast<unsigned long long>(stats::frame().passes),
                                                   target.width, target.height, colors, target.depth.texture ? " d" : "",
                                                   clearsApply ? " clear" : ""];
    }
    {
        stats::Timer t(&stats::Frame::passFenceNanos);
        waitForShaderWrites(render_, true);
    }
    stats::passStarted(target.width, target.height);
    target_ = target;
    drawsInPass_ = false;
    ++passGeneration_;
}

id<MTLRenderCommandEncoder> CommandStream::render(const RenderTargetDesc& target) {
    if (__builtin_expect(deferred_ != nullptr, 0)) runDeferred();
    ++renderCalls_;
    // A clear recorded for another target while this pass stayed open runs
    // first: later draws (here, or sampling that target) must see it.
    if (pending_.any() && pendingTarget_ != target) resolvePendingClear();
    if (render_ && target_ == target) {
        storeDiscard_ = Discard();  // the pass writes the attachments again
        return render_;
    }
    startRender(target);
    return render_;
}

void CommandStream::invalidate(const RenderTargetDesc& target, uint8_t colorMask, bool depth, bool stencil) {
    Discard& d = render_ && target_ == target ? storeDiscard_ : loadDiscard_;
    if (&d == &loadDiscard_ && !(discardTarget_ == target)) {
        discardTarget_ = target;
        loadDiscard_ = Discard();
    }
    d.color |= colorMask;
    d.depth = d.depth || depth;
    d.stencil = d.stencil || stencil;
}

bool CommandStream::clearWithLoadAction(const RenderTargetDesc& target, const PendingClear& clear) {
    if (render_ && target_ == target && drawsInPass_) return false;
    if (pending_.any() && pendingTarget_ != target) resolvePendingClear();
    if (render_ && target_ == target) endEncoder(PassEnd::Clear);  // pass has no draws: restart it with clears
    pendingTarget_ = target;
    for (size_t i = 0; i < 8; ++i) {
        if (clear.colorMask & (1u << i)) {
            pending_.colorMask |= 1u << i;
            pending_.color[i] = clear.color[i];
        }
    }
    if (clear.depth) {
        pending_.depth = true;
        pending_.depthValue = clear.depthValue;
    }
    if (clear.stencil) {
        pending_.stencil = true;
        pending_.stencilValue = clear.stencilValue;
    }
    return true;
}

void CommandStream::resolvePendingClear() {
    if (!pending_.any()) return;
    RenderTargetDesc target = pendingTarget_;
    startRender(target);
    endEncoder();
}

bool CommandStream::pendingClearUses(id<MTLTexture> texture) const {
    if (!pending_.any() || !texture) return false;
    auto uses = [&](id<MTLTexture> t) { return t && (t == texture || t.parentTexture == texture || texture.parentTexture == t); };
    for (const RenderTargetDesc::Target& c : pendingTarget_.color)
        if (uses(c.texture)) return true;
    return uses(pendingTarget_.depth.texture) || uses(pendingTarget_.stencil.texture);
}

namespace {

constexpr uint32_t kTimingSamples = 4096;

id<MTLCounterSet> timestampCounters() {
    static id<MTLCounterSet> set = [] {
        id<MTLDevice> device = Device::get().mtl();
        if (![device supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary]) return (id<MTLCounterSet>)nil;
        for (id<MTLCounterSet> s in device.counterSets)
            if ([s.name isEqualToString:MTLCommonCounterSetTimestamp]) return s;
        return (id<MTLCounterSet>)nil;
    }();
    return set;
}

}  // namespace

bool CommandStream::gpuTiming() {
    static const bool enabled = [] {
        const char* v = std::getenv("WINEMETALGL_GPU_TIMERS");
        return (!v || v[0] != '0') && timestampCounters() != nil;
    }();
    return enabled;
}

namespace {

id<MTLCounterSampleBuffer> newTimingSamples() {
    MTLCounterSampleBufferDescriptor* desc = [MTLCounterSampleBufferDescriptor new];
    desc.counterSet = timestampCounters();
    desc.storageMode = MTLStorageModeShared;
    desc.sampleCount = kTimingSamples;
    NSError* error = nil;
    id<MTLCounterSampleBuffer> buffer = [Device::get().mtl() newCounterSampleBufferWithDescriptor:desc error:&error];
    if (!buffer) WMGL_WARN(Gpu, "no counter sample buffer for timer queries: %s", error.localizedDescription.UTF8String);
    return buffer;
}

}  // namespace

bool CommandStream::takeSamples(uint32_t& start, uint32_t& end) {
    if (!timing_) return false;
    if (!samples_ || nextSample_ + 2 > kTimingSamples) {
        // A query's samples straddling two buffers reads as 0 (finalize).
        samples_ = newTimingSamples();
        nextSample_ = 0;
        if (!samples_) return false;
    }
    start = nextSample_;
    end = nextSample_ + 1;
    nextSample_ += 2;
    return true;
}

bool CommandStream::beginTiming(TimingMark& first) {
    if (!gpuTiming()) return false;
    // The query times the encoders opened from now on, in one sample buffer
    // with room for 256 of them.
    endEncoder(PassEnd::Other);
    if (!samples_ || nextSample_ + 512 > kTimingSamples) {
        samples_ = newTimingSamples();
        nextSample_ = 0;
        if (!samples_) return false;
    }
    // Once a context times queries every encoder takes samples: a query's
    // first pass then knows when the pass before it ended.
    timing_ = true;
    first = {samples_, nextSample_};
    return true;
}

void CommandStream::endTiming(TimingMark& last) {
    endEncoder(PassEnd::Other);
    last = {samples_, nextSample_ ? nextSample_ - 1 : 0};
}

id<MTLBlitCommandEncoder> CommandStream::uploadBlit(bool keepPendingClear) {
    loadDiscard_ = Discard();  // the copies may write the invalidated attachments
    // A clear recorded after this encoder opened must run before new commands.
    if (blit_ && (keepPendingClear || !pending_.any())) return blit_;
    if (!keepPendingClear) resolvePendingClear();
    endEncoder(PassEnd::Blit);
    uint32_t start, end;
    if (takeSamples(start, end)) {
        MTLBlitPassDescriptor* pass = [MTLBlitPassDescriptor blitPassDescriptor];
        pass.sampleBufferAttachments[0].sampleBuffer = samples_;
        pass.sampleBufferAttachments[0].startOfEncoderSampleIndex = start;
        pass.sampleBufferAttachments[0].endOfEncoderSampleIndex = end;
        blit_ = [commandBuffer() blitCommandEncoderWithDescriptor:pass];
    } else {
        blit_ = [commandBuffer() blitCommandEncoder];
    }
    waitForShaderWrites(blit_, false);
    return blit_;
}

id<MTLBlitCommandEncoder> CommandStream::blit(bool keepPendingClear) {
    if (__builtin_expect(deferred_ != nullptr, 0)) runDeferred();
    return uploadBlit(keepPendingClear);
}

id<MTLComputeCommandEncoder> CommandStream::compute(bool keepPendingClear) {
    if (__builtin_expect(deferred_ != nullptr, 0)) runDeferred();
    loadDiscard_ = Discard();  // the dispatches may write the invalidated attachments
    if (compute_ && (keepPendingClear || !pending_.any())) return compute_;
    if (!keepPendingClear) resolvePendingClear();
    endEncoder(PassEnd::Compute);
    uint32_t start, end;
    if (takeSamples(start, end)) {
        MTLComputePassDescriptor* pass = [MTLComputePassDescriptor computePassDescriptor];
        pass.sampleBufferAttachments[0].sampleBuffer = samples_;
        pass.sampleBufferAttachments[0].startOfEncoderSampleIndex = start;
        pass.sampleBufferAttachments[0].endOfEncoderSampleIndex = end;
        compute_ = [commandBuffer() computeCommandEncoderWithDescriptor:pass];
    } else {
        compute_ = [commandBuffer() computeCommandEncoder];
    }
    waitForShaderWrites(compute_, false);
    return compute_;
}

void CommandStream::setVisibilityWanted(bool wanted) {
    visibilityWanted_ = wanted;
    if (wanted) visibilityUsedBefore_ = true;
    if (wanted && render_ && !passVisibility_) endEncoder(PassEnd::Visibility);
}

bool CommandStream::visibilitySlotAvailable() const { return !visibility_ || visibilityUsed_ < kVisibilitySlots; }

bool CommandStream::allocateVisibilitySlot(__strong id<MTLBuffer>& buffer, NSUInteger& offset) {
    if (!render_ || !passVisibility_ || visibilityUsed_ >= kVisibilitySlots) return false;
    buffer = visibility_;
    offset = visibilityUsed_ * 8;
    ++visibilityUsed_;
    return true;
}

void CommandStream::present(id<CAMetalDrawable> drawable) {
    [commandBuffer() presentDrawable:drawable];
    indirectWaited_ = false;
}

id<MTLRenderCommandEncoder> CommandStream::preludeRender() {
    if (__builtin_expect(deferred_ != nullptr, 0)) runDeferred();
    if (preludeRender_) return preludeRender_;
    if (!preludeRenderBuffer_) {
        preludeRenderBuffer_ = [queue_.mtl() commandBuffer];
        preludeRenderBuffer_.label = @"WineMetalGL prelude (vertex capture)";
    }
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.renderTargetWidth = 1;
    pass.renderTargetHeight = 1;
    pass.defaultRasterSampleCount = 1;
    preludeRender_ = [preludeRenderBuffer_ renderCommandEncoderWithDescriptor:pass];
    return preludeRender_;
}

id<MTLComputeCommandEncoder> CommandStream::preludeCompute() {
    if (__builtin_expect(deferred_ != nullptr, 0)) runDeferred();
    if (preludeCompute_) return preludeCompute_;
    if (!preludeComputeBuffer_) {
        preludeComputeBuffer_ = [queue_.mtl() commandBuffer];
        preludeComputeBuffer_.label = @"WineMetalGL prelude (kernels)";
    }
    preludeCompute_ = [preludeComputeBuffer_ computeCommandEncoder];
    return preludeCompute_;
}

id<MTLComputeCommandEncoder> CommandStream::preludeStep(PreludeStep step) {
    if (__builtin_expect(deferred_ != nullptr, 0)) runDeferred();
    const int i = static_cast<int>(step);
    if (stepEncoders_[i]) return stepEncoders_[i];
    if (!stepBuffers_[i]) {
        static NSString* const labels[] = {@"WineMetalGL prelude (multi-draw scan)", @"WineMetalGL prelude (multi-draw expand)",
                                           @"WineMetalGL prelude (multi-draw control)", @"WineMetalGL prelude (multi-draw factors)"};
        stepBuffers_[i] = [queue_.mtl() commandBuffer];
        stepBuffers_[i].label = labels[i];
    }
    stepEncoders_[i] = [stepBuffers_[i] computeCommandEncoderWithDispatchType:MTLDispatchTypeConcurrent];
    return stepEncoders_[i];
}

void CommandStream::commitPrelude() {
    // In queue order ahead of the command buffer being committed: the
    // multi-draw scans and expansions, the capture pass, the kernels that
    // read it, then the multi-draw control and factor kernels (tracked
    // resources order each after what it reads, and the draws after them).
    for (__strong id<MTLComputeCommandEncoder>& e : stepEncoders_)
        if (e) [e endEncoding];
    if (preludeRender_) [preludeRender_ endEncoding];
    if (preludeCompute_) [preludeCompute_ endEncoding];
    const auto commit = [](id<MTLCommandBuffer> b) {
        if (b) [b commit];
    };
    commit(stepBuffers_[static_cast<int>(PreludeStep::Scan)]);
    commit(stepBuffers_[static_cast<int>(PreludeStep::Expand)]);
    commit(preludeRenderBuffer_);
    commit(preludeComputeBuffer_);
    commit(stepBuffers_[static_cast<int>(PreludeStep::Control)]);
    commit(stepBuffers_[static_cast<int>(PreludeStep::Factors)]);
    if (preludeComputeBuffer_ || preludeRenderBuffer_) lastPrelude_ = preludeComputeBuffer_ ?: preludeRenderBuffer_;
    for (__strong id<MTLComputeCommandEncoder>& e : stepEncoders_) e = nil;
    for (__strong id<MTLCommandBuffer>& b : stepBuffers_) b = nil;
    preludeRender_ = nil;
    preludeCompute_ = nil;
    preludeRenderBuffer_ = nil;
    preludeComputeBuffer_ = nil;
}

void CommandStream::waitForPrelude() {
    flush();
    if (!lastPrelude_) return;
    stats::count(&stats::Frame::waits);
    stats::Timer timer(&stats::Frame::waitNanos);
    [lastPrelude_ waitUntilCompleted];
    lastPrelude_ = nil;
}

uint64_t CommandStream::flush(bool keepPendingClear) {
    if (__builtin_expect(deferred_ != nullptr, 0)) runDeferred();
    if (!keepPendingClear) resolvePendingClear();
    endEncoder(PassEnd::Commit);
    commitPrelude();
    if (!buffer_) return lastCommitted_;
    stats::count(&stats::Frame::commits);
    lastCommitNanos_ = stats::nowNanos();
    drawsSinceCommit_ = 0;
    queue_.commit(buffer_, serial_);
    lastCommitted_ = serial_;
    buffer_ = nil;
    visibility_ = nil;
    serial_ = 0;
    return lastCommitted_;
}

void CommandStream::finish() {
    const uint64_t serial = flush();
    if (!serial) return;
    if (!queue_.isComplete(serial)) stats::count(&stats::Frame::waits);
    stats::Timer timer(&stats::Frame::waitNanos);
    queue_.wait(serial);
}

void CommandStream::waitFor(uint64_t serial) {
    if (!serial) return;
    if (buffer_ && serial == serial_) flush();
    if (queue_.isComplete(serial)) return;
    stats::count(&stats::Frame::waits);
    stats::Timer timer(&stats::Frame::waitNanos);
    queue_.wait(serial);
}

void CommandStream::memoryBarrier(bool attachments, bool commands) {
    if (commands && compute_ && !render_) {
        static const bool early = [] {
            const char* v = std::getenv("WINEMETALGL_BARRIER_COMMIT");
            return !v || v[0] != '0';
        }();
        if (early) {
            // Committing orders the dispatch's writes before every later
            // command buffer on the queue: no barrier needed.
            flush(true);
            return;
        }
    }
    // A render pass whose draws wrote nothing through shaders has nothing to
    // order (earlier encoders' writes are fenced): it stays open, unless
    // attachment writes are ordered (glTextureBarrier).
    if (render_ && (attachments || shaderWrites_)) {
        // Apple GPUs cannot order fragment-stage writes inside a render
        // pass (render-encoder barriers may not wait on the fragment
        // stage): end the pass; Metal orders work across encoders.
        endEncoder(PassEnd::Barrier);
    } else if (compute_) {
        [compute_ memoryBarrierWithScope:MTLBarrierScopeBuffers | MTLBarrierScopeTextures];
    }
}

}  // namespace wmgl
