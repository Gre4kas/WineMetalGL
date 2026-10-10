// One Metal command queue per share group, with command-buffer serials.
//
// Every command buffer gets a serial when it is created. Serials can complete
// out of order across contexts, so "complete" is tracked per serial: a serial
// is complete once its command buffer finished, and isComplete(s) also holds
// for serials that were never used.
#pragma once

#import <Metal/Metal.h>

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <set>

namespace wmgl {

class Queue {
public:
    Queue();
    id<MTLCommandQueue> mtl() const { return queue_; }

    // Creates a command buffer and reserves its serial.
    id<MTLCommandBuffer> create(uint64_t* serial);
    // Commits a command buffer created by create().
    void commit(id<MTLCommandBuffer> buffer, uint64_t serial);
    // Drops a reserved serial whose command buffer was never committed.
    void abandon(uint64_t serial);

    bool isComplete(uint64_t serial);
    bool isCommitted(uint64_t serial);
    // Blocks until the serial completes. Returns immediately for serials that
    // are not committed (their work is not visible to other contexts yet).
    void wait(uint64_t serial);
    // As wait(), giving up after timeoutNs (UINT64_MAX waits forever). True if complete.
    bool wait(uint64_t serial, uint64_t timeoutNs);
    // Fence waits: also waits while the serial is reserved but not yet
    // committed (a fence signals once its command buffer is flushed and done).
    bool waitSignaled(uint64_t serial, uint64_t timeoutNs);
    // Waits until everything committed so far has completed.
    void waitIdle();
    uint64_t lastSerial();

private:
    id<MTLCommandQueue> queue_;
    std::mutex mutex_;
    std::condition_variable completed_;
    uint64_t next_ = 1;
    std::set<uint64_t> reserved_;   // created, not committed
    std::set<uint64_t> inflight_;   // committed, not complete
};

}  // namespace wmgl
