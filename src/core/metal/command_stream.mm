#include "core/metal/command_stream.h"

#include "core/stats.h"

#import <QuartzCore/CAMetalLayer.h>

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
    }
    shaderWrites_ = false;
    if (render_) {
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
    if (render)
        [static_cast<id<MTLRenderCommandEncoder>>(encoder) waitForFence:writesFence_
                                                           beforeStages:MTLRenderStageVertex | MTLRenderStageFragment];
    else if ([encoder conformsToProtocol:@protocol(MTLBlitCommandEncoder)])
        [static_cast<id<MTLBlitCommandEncoder>>(encoder) waitForFence:writesFence_];
    else
        [static_cast<id<MTLComputeCommandEncoder>>(encoder) waitForFence:writesFence_];
}

void CommandStream::startRender(const RenderTargetDesc& target) {
    endEncoder(PassEnd::Target);
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    const bool clearsApply = pending_.any() && pendingTarget_ == target;
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
            a.loadAction = MTLLoadActionLoad;
        }
        a.storeAction = MTLStoreActionStore;
    }
    if (target.depth.texture) {
        MTLRenderPassDepthAttachmentDescriptor* d = pass.depthAttachment;
        d.texture = target.depth.texture;
        d.level = target.depth.level;
        d.slice = target.depth.slice;
        d.loadAction = clearsApply && pending_.depth ? MTLLoadActionClear : MTLLoadActionLoad;
        d.clearDepth = pending_.depthValue;
        d.storeAction = MTLStoreActionStore;
    }
    if (target.stencil.texture) {
        MTLRenderPassStencilAttachmentDescriptor* s = pass.stencilAttachment;
        s.texture = target.stencil.texture;
        s.level = target.stencil.level;
        s.slice = target.stencil.slice;
        s.loadAction = clearsApply && pending_.stencil ? MTLLoadActionClear : MTLLoadActionLoad;
        s.clearStencil = pending_.stencilValue;
        s.storeAction = MTLStoreActionStore;
    }
    if (!target.hasAnyAttachment()) {
        // Framebuffer with no attachments (ARB_framebuffer_no_attachments).
        pass.renderTargetWidth = target.width;
        pass.renderTargetHeight = target.height;
        pass.defaultRasterSampleCount = target.samples;
    }
    if (target.layers) pass.renderTargetArrayLength = target.layers;
    passVisibility_ = false;
    if (visibilityWanted_) {
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
    if (clearsApply) pending_ = PendingClear();
    render_ = [commandBuffer() renderCommandEncoderWithDescriptor:pass];
    waitForShaderWrites(render_, true);
    stats::passStarted(target.width, target.height);
    target_ = target;
    drawsInPass_ = false;
    ++passGeneration_;
}

id<MTLRenderCommandEncoder> CommandStream::render(const RenderTargetDesc& target) {
    ++renderCalls_;
    // A clear recorded for another target while this pass stayed open runs
    // first: later draws (here, or sampling that target) must see it.
    if (pending_.any() && pendingTarget_ != target) resolvePendingClear();
    if (render_ && target_ == target) return render_;
    startRender(target);
    return render_;
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

id<MTLBlitCommandEncoder> CommandStream::blit() {
    // A clear recorded after this encoder opened must run before new commands.
    if (blit_ && !pending_.any()) return blit_;
    resolvePendingClear();
    endEncoder(PassEnd::Blit);
    blit_ = [commandBuffer() blitCommandEncoder];
    waitForShaderWrites(blit_, false);
    return blit_;
}

id<MTLComputeCommandEncoder> CommandStream::compute() {
    if (compute_ && !pending_.any()) return compute_;
    resolvePendingClear();
    endEncoder(PassEnd::Compute);
    compute_ = [commandBuffer() computeCommandEncoder];
    waitForShaderWrites(compute_, false);
    return compute_;
}

void CommandStream::setVisibilityWanted(bool wanted) {
    visibilityWanted_ = wanted;
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
}

uint64_t CommandStream::flush() {
    resolvePendingClear();
    endEncoder(PassEnd::Commit);
    if (!buffer_) return lastCommitted_;
    stats::count(&stats::Frame::commits);
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

void CommandStream::memoryBarrier() {
    if (render_) {
        // Apple GPUs cannot order fragment-stage writes inside a render
        // pass (render-encoder barriers may not wait on the fragment
        // stage): end the pass; Metal orders work across encoders.
        endEncoder(PassEnd::Barrier);
    } else if (compute_) {
        [compute_ memoryBarrierWithScope:MTLBarrierScopeBuffers | MTLBarrierScopeTextures];
    }
}

}  // namespace wmgl
