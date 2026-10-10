#include "core/metal/command_stream.h"

#import <QuartzCore/CAMetalLayer.h>

#include "core/util/log.h"

namespace wmgl {

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

uint64_t CommandStream::serial() {
    commandBuffer();
    return serial_;
}

void CommandStream::endEncoder() {
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

void CommandStream::startRender(const RenderTargetDesc& target) {
    endEncoder();
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
    if (clearsApply) pending_ = PendingClear();
    render_ = [commandBuffer() renderCommandEncoderWithDescriptor:pass];
    target_ = target;
    drawsInPass_ = false;
    ++passGeneration_;
}

id<MTLRenderCommandEncoder> CommandStream::render(const RenderTargetDesc& target) {
    if (render_ && target_ == target) return render_;
    if (pending_.any() && pendingTarget_ != target) resolvePendingClear();
    startRender(target);
    return render_;
}

bool CommandStream::clearWithLoadAction(const RenderTargetDesc& target, const PendingClear& clear) {
    if (render_ && target_ == target && drawsInPass_) return false;
    if (pending_.any() && pendingTarget_ != target) resolvePendingClear();
    if (render_ && target_ == target) endEncoder();  // pass has no draws: restart it with clears
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
    if (blit_) return blit_;
    resolvePendingClear();
    endEncoder();
    blit_ = [commandBuffer() blitCommandEncoder];
    return blit_;
}

id<MTLComputeCommandEncoder> CommandStream::compute() {
    if (compute_) return compute_;
    resolvePendingClear();
    endEncoder();
    compute_ = [commandBuffer() computeCommandEncoder];
    return compute_;
}

void CommandStream::present(id<CAMetalDrawable> drawable) {
    [commandBuffer() presentDrawable:drawable];
}

uint64_t CommandStream::flush() {
    resolvePendingClear();
    endEncoder();
    if (!buffer_) return lastCommitted_;
    queue_.commit(buffer_, serial_);
    lastCommitted_ = serial_;
    buffer_ = nil;
    serial_ = 0;
    return lastCommitted_;
}

void CommandStream::finish() {
    const uint64_t serial = flush();
    if (serial) queue_.wait(serial);
}

void CommandStream::waitFor(uint64_t serial) {
    if (!serial) return;
    if (buffer_ && serial == serial_) flush();
    queue_.wait(serial);
}

}  // namespace wmgl
