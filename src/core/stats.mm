#include "core/stats.h"

#include <mach/mach_time.h>
#include <os/signpost.h>

#include <cstdio>
#include <cstdlib>

namespace wmgl::stats {
namespace {

bool enabledFromEnvironment() {
    const char* v = std::getenv("WINEMETALGL_STATS");
    return v && *v && *v != '0';
}

os_log_t signpostLog() {
    static os_log_t log = os_log_create("org.winehq.winemetalgl", OS_LOG_CATEGORY_POINTS_OF_INTEREST);
    return log;
}

struct ThreadState {
    Frame frame;
    uint64_t index = 0, lastPresent = 0;
    os_signpost_id_t interval = OS_SIGNPOST_ID_NULL;
};

ThreadState& thread() {
    static thread_local ThreadState state;
    return state;
}

double ms(uint64_t nanos) { return static_cast<double>(nanos) / 1e6; }

}  // namespace

const bool g_enabled = enabledFromEnvironment();

Frame& frame() { return thread().frame; }

uint64_t nowNanos() {
    static mach_timebase_info_data_t timebase = [] {
        mach_timebase_info_data_t t;
        mach_timebase_info(&t);
        return t;
    }();
    return mach_absolute_time() * timebase.numer / timebase.denom;
}

void endFrame() {
    ThreadState& t = thread();
    os_log_t log = signpostLog();
    if (os_signpost_enabled(log)) {
        if (t.interval != OS_SIGNPOST_ID_NULL) os_signpost_interval_end(log, t.interval, "frame");
        t.interval = os_signpost_id_generate(log);
        os_signpost_interval_begin(log, t.interval, "frame");
    }
    if (!g_enabled) return;
    const uint64_t now = nowNanos();
    const Frame& f = t.frame;
    std::fprintf(stderr,
                 "winemetalgl:stats: frame %llu %.2f ms | gl %.2f ms in %llu calls | present %.2f ms (drawable %.2f ms) | "
                 "draws %llu passes %llu | pipelines %llu (%.2f ms) links %llu (%.2f ms) | transient %llu KiB | "
                 "waits %llu (%.2f ms)\n",
                 static_cast<unsigned long long>(t.index), t.lastPresent ? ms(now - t.lastPresent) : 0.0, ms(f.glNanos),
                 static_cast<unsigned long long>(f.calls), ms(f.presentNanos), ms(f.drawableNanos),
                 static_cast<unsigned long long>(f.draws),
                 static_cast<unsigned long long>(f.passes), static_cast<unsigned long long>(f.pipelineMisses),
                 ms(f.pipelineNanos), static_cast<unsigned long long>(f.programLinks), ms(f.linkNanos),
                 static_cast<unsigned long long>(f.transientBytes / 1024), static_cast<unsigned long long>(f.waits),
                 ms(f.waitNanos));
    t.frame = Frame();
    t.lastPresent = now;
    ++t.index;
}

void passStarted(uint32_t width, uint32_t height) {
    count(&Frame::passes);
    os_log_t log = signpostLog();
    if (os_signpost_enabled(log)) os_signpost_event_emit(log, OS_SIGNPOST_ID_EXCLUSIVE, "render pass", "%ux%u", width, height);
}

}  // namespace wmgl::stats
