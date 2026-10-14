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
#include <string>

namespace wmgl::stats {

// Counting: WINEMETALGL_STATS set, or turned on by wmgl_stats_enable (tests).
extern bool g_enabled;
// Logging the counters and descriptions: WINEMETALGL_STATS set.
extern const bool g_reporting;

// Why an indirect draw command is read on the CPU instead of by the GPU:
// the first condition that holds of api/draw.mm drawIndirectOnGpu and
// DrawState::indirectCpuReason.
enum IndirectCpu : uint8_t {
    kIndirectCommands,         // the command buffer: none, misaligned, mapped or too small
    kIndirectStage,            // an emulated stage: patches, a geometry shader, wide lines
    kIndirectCapture,          // transform feedback
    kIndirectQuery,            // a query counting the draw's primitives or samples
    kIndirectPrimitive,        // fans, loops, quads, point polygons, provoking-vertex rotation
    kIndirectParameters,       // gl_BaseVertex or gl_BaseInstance
    kIndirectConverted,        // a vertex attribute converted on the CPU
    kIndirectIndexType,        // 8-bit indices
    kIndirectRestart,          // a restart index other than the type's largest
    kIndirectElementsMapped,   // the element buffer is mapped or pinned (written behind GL's back)
    kIndirectElementsWriting,  // GPU writes to the element buffer have not completed
    kIndirectMaxIndex,         // the element buffer holds the type's largest index
    kIndirectCpuReasons
};

struct Frame {
    uint64_t calls = 0, draws = 0, passes = 0, pipelineMisses = 0, programLinks = 0, transientBytes = 0, waits = 0;
    uint64_t glNanos = 0, pipelineNanos = 0, linkNanos = 0, waitNanos = 0;
    uint64_t lockNanos = 0;  // waiting for the share group's lock inside GL calls
    uint64_t presentNanos = 0, drawableNanos = 0;  // SwapBuffers, and waiting for a drawable inside it
    // Vertex attributes converted on the CPU (draw.mm bindVertexInputs):
    // vertices converted, groups reused from a buffer's cache, and converted
    // attributes by cause (client memory, unfetchable format, other).
    uint64_t convertedVertices = 0, convertedReused = 0, convertedClient = 0, convertedFormat = 0, convertedOther = 0;
    uint64_t convertedFormatType = 0;  // GL type of the last unfetchable one (| 0x10000: normalized)
    uint64_t cpuIndexDraws = 0;        // draws whose indices were rewritten on the CPU
    // Patch draws tessellated on the CPU (after reading the levels back), by
    // Metal's tessellator, and of those, draws with flat fragment inputs:
    // they take the value of the vertex Metal provokes from. GL leaves the
    // order of a tessellated triangle's vertices to the implementation, so
    // that is as conforming as the CPU path's last vertex; a game that relies
    // on one shows here.
    uint64_t tessCpu = 0, tessMetal = 0, tessMetalFlat = 0, tessPrelude = 0;
    // Indirect draw commands read by the GPU, and read on the CPU (after
    // waiting for GPU writes to the command buffer).
    uint64_t indirectGpu = 0, indirectCpu = 0;
    // The CPU-read commands by IndirectCpu reason, those that draw nothing
    // (no vertices or no instances), and the waits for GPU writes to the
    // commands before reading them.
    uint64_t indirectCpuReasons[kIndirectCpuReasons] = {};
    uint64_t indirectCpuEmpty = 0, indirectWaits = 0;
    uint64_t uniformBytes = 0;         // default uniforms copied to transient memory
    // Command buffers committed, those committed for timer queries, and
    // render passes ended by cause (CommandStream::PassEnd order).
    uint64_t commits = 0, queryCommits = 0;
    uint64_t passEnds[8] = {};
    // glReadPixels copied by the GPU into a pack buffer, and reads that wait for the GPU.
    uint64_t readsAsync = 0, readsWaiting = 0;
    // Where draws spend CPU time (DrawState::issue and drawElements).
    uint64_t drawSamplersNanos = 0, drawVertexNanos = 0, drawPrepareNanos = 0, drawPipelineNanos = 0,
             drawBindNanos = 0, drawEncodeNanos = 0, drawIndexNanos = 0;
    uint64_t drawOtherNanos = 0;  // not reported: where a phase timer goes after its phase
};

// GPU time of completed command buffers (any thread; reported and reset per frame).
void addGpuNanos(uint64_t nanos);

Frame& frame();
// The calling thread's counter `name` (a Frame field, "passEnds.<cause>" or
// "indirectCpuReasons.<n>"), or "deviceAllocatedBytes" (the Metal device's
// allocations), in `value`; false for an unknown name.
bool counter(const char* name, uint64_t& value);
uint64_t nowNanos();

inline void count(uint64_t Frame::*field, uint64_t n = 1) {
    if (__builtin_expect(g_enabled, 0)) frame().*field += n;
}

// Logs `format` (when enabled) the first time `key` is seen.
void describeOnce(const std::string& key, const char* format, ...) __attribute__((format(printf, 2, 3)));
// An indirect command read on the CPU for `reason`; `detail` is logged the
// first time the reason is seen with `key` (a program and cause).
void indirectCpu(IndirectCpu reason, const std::string& key, const char* detail);
// Another command of the same multi-draw read on the CPU for the same reason.
void repeatIndirectCpu();

// Adds the scope's duration to `field` (when enabled).
class Timer {
public:
    explicit Timer(uint64_t Frame::*field) : field_(field), start_(g_enabled ? nowNanos() : 0) {}
    ~Timer() {
        if (start_) frame().*field_ += nowNanos() - start_;
    }
    // Ends the current field's time and goes on timing into `field`.
    void switchTo(uint64_t Frame::*field) {
        if (start_) {
            const uint64_t now = nowNanos();
            frame().*field_ += now - start_;
            start_ = now;
        }
        field_ = field;
    }

private:
    uint64_t Frame::*field_;
    uint64_t start_;
};

// Adds one call of `command` to the process-wide profile (every thread's
// calls), which endFrame prints every 60 frames.
void recordCall(const char* command, uint64_t nanos, uint64_t lockNanos);

// One GL call (the generated entry points): `locked()` once the share
// group's lock is held.
class CallTimer {
public:
    explicit CallTimer(const char* command) : command_(command), start_(g_enabled ? nowNanos() : 0) {}
    void locked() {
        if (start_) locked_ = nowNanos();
    }
    ~CallTimer() {
        if (!start_) return;
        Frame& f = frame();
        const uint64_t nanos = nowNanos() - start_, lock = locked_ ? locked_ - start_ : 0;
        ++f.calls;
        f.glNanos += nanos;
        f.lockNanos += lock;
        recordCall(command_, nanos, lock);
    }

private:
    const char* command_;
    uint64_t start_, locked_ = 0;
};

// A frame was presented: logs it (when enabled), resets the counters and
// moves the frame signpost interval on.
void endFrame();
// A render pass started (signpost event).
void passStarted(uint32_t width, uint32_t height);

}  // namespace wmgl::stats
