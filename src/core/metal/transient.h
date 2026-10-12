// Transient GPU memory for per-draw data: uniforms larger than setBytes
// allows, converted index buffers, client-array uploads, staging for texture
// uploads. Chunks are recycled once the command buffer that used them has
// completed.
#pragma once

#import <Metal/Metal.h>

#include <cstdint>
#include <vector>

#include "core/metal/queue.h"

namespace wmgl {

struct TransientAllocation {
    id<MTLBuffer> buffer = nil;
    NSUInteger offset = 0;
    void* data = nullptr;
};

class TransientAllocator {
public:
    explicit TransientAllocator(Queue& queue) : queue_(queue) {}
    // Allocates `size` bytes for work recorded in command buffer `serial`;
    // an empty allocation (nil buffer) when the device has no memory for it.
    TransientAllocation allocate(size_t size, uint64_t serial, size_t alignment = 256);

private:
    struct Chunk {
        id<MTLBuffer> buffer;
        size_t used = 0;
        uint64_t lastSerial = 0;
    };
    Queue& queue_;
    std::vector<Chunk> chunks_;
    size_t current_ = SIZE_MAX;
};

}  // namespace wmgl
