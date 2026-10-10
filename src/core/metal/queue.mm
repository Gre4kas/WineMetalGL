#include "core/metal/queue.h"

#include "core/metal/device.h"
#include "core/util/log.h"

namespace wmgl {

Queue::Queue() {
    queue_ = [Device::get().mtl() newCommandQueueWithMaxCommandBufferCount:64];
    queue_.label = @"WineMetalGL";
}

id<MTLCommandBuffer> Queue::create(uint64_t* serial) {
    id<MTLCommandBuffer> buffer = [queue_ commandBuffer];
    std::lock_guard<std::mutex> lock(mutex_);
    *serial = next_++;
    reserved_.insert(*serial);
    return buffer;
}

void Queue::commit(id<MTLCommandBuffer> buffer, uint64_t serial) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        reserved_.erase(serial);
        inflight_.insert(serial);
    }
    [buffer addCompletedHandler:^(id<MTLCommandBuffer> done) {
        if (done.status == MTLCommandBufferStatusError)
            WMGL_ERR("command buffer %llu failed: %s", static_cast<unsigned long long>(serial),
                     done.error.localizedDescription.UTF8String);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            inflight_.erase(serial);
        }
        completed_.notify_all();
    }];
    [buffer commit];
}

void Queue::abandon(uint64_t serial) {
    std::lock_guard<std::mutex> lock(mutex_);
    reserved_.erase(serial);
}

bool Queue::isComplete(uint64_t serial) {
    std::lock_guard<std::mutex> lock(mutex_);
    return !inflight_.count(serial) && !reserved_.count(serial);
}

bool Queue::isCommitted(uint64_t serial) {
    std::lock_guard<std::mutex> lock(mutex_);
    return !reserved_.count(serial);
}

void Queue::wait(uint64_t serial) {
    std::unique_lock<std::mutex> lock(mutex_);
    completed_.wait(lock, [&] { return !inflight_.count(serial); });
}

void Queue::waitIdle() {
    std::unique_lock<std::mutex> lock(mutex_);
    completed_.wait(lock, [&] { return inflight_.empty(); });
}

uint64_t Queue::lastSerial() {
    std::lock_guard<std::mutex> lock(mutex_);
    return next_ - 1;
}

}  // namespace wmgl
