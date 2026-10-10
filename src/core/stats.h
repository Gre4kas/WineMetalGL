// Per-frame performance counters (W24).
//
// WINEMETALGL_STATS=1 logs one line per presented frame: wall time since the
// previous present, CPU time spent inside GL calls, draws, render passes,
// pipeline-state misses (and their build time), program links, transient
// upload bytes and CPU waits for the GPU (which should be 0 in a steady
// state). os_signpost intervals per frame and events per render pass are
// emitted for Instruments ("org.winehq.winemetalgl", Points of Interest)
// whether or not the log is on.
//
// Counters are per thread: a context's commands run on the thread it is
// current on.
#pragma once

#include <cstdint>

namespace wmgl::stats {

extern const bool g_enabled;  // WINEMETALGL_STATS set

struct Frame {
    uint64_t calls = 0, draws = 0, passes = 0, pipelineMisses = 0, programLinks = 0, transientBytes = 0, waits = 0;
    uint64_t glNanos = 0, pipelineNanos = 0, linkNanos = 0, waitNanos = 0;
    uint64_t presentNanos = 0, drawableNanos = 0;  // SwapBuffers, and waiting for a drawable inside it
};

Frame& frame();
uint64_t nowNanos();

inline void count(uint64_t Frame::*field, uint64_t n = 1) {
    if (__builtin_expect(g_enabled, 0)) frame().*field += n;
}

// Adds the scope's duration to `field` (when enabled).
class Timer {
public:
    explicit Timer(uint64_t Frame::*field) : field_(field), start_(g_enabled ? nowNanos() : 0) {}
    ~Timer() {
        if (start_) frame().*field_ += nowNanos() - start_;
    }

private:
    uint64_t Frame::*field_;
    uint64_t start_;
};

// One GL call (the generated entry points).
class CallTimer {
public:
    CallTimer() : start_(g_enabled ? nowNanos() : 0) {}
    ~CallTimer() {
        if (!start_) return;
        Frame& f = frame();
        ++f.calls;
        f.glNanos += nowNanos() - start_;
    }

private:
    uint64_t start_;
};

// A frame was presented: logs it (when enabled), resets the counters and
// moves the frame signpost interval on.
void endFrame();
// A render pass started (signpost event).
void passStarted(uint32_t width, uint32_t height);

}  // namespace wmgl::stats
