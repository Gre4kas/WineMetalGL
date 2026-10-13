#include "core/stats.h"

#include <mach/mach_time.h>
#include <os/signpost.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>

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

namespace {

// The process-wide profile: per command (by its name's address) and per
// thread, since the 60 frames last printed.
struct CommandProfile {
    uint64_t calls = 0, nanos = 0, lockNanos = 0;
};
struct Profile {
    std::mutex mutex;
    std::unordered_map<const char*, CommandProfile> commands;
    std::map<uint64_t, CommandProfile> threads;  // by thread number
};
Profile& profile() {
    static Profile* p = new Profile;
    return *p;
}
uint64_t threadNumber() {
    static std::atomic<uint64_t> next{1};
    static thread_local uint64_t number = next++;
    return number;
}

}  // namespace

void recordCall(const char* command, uint64_t nanos, uint64_t lockNanos) {
    Profile& p = profile();
    const uint64_t thread = threadNumber();
    std::lock_guard<std::mutex> lock(p.mutex);
    CommandProfile& c = p.commands[command];
    ++c.calls;
    c.nanos += nanos;
    c.lockNanos += lockNanos;
    CommandProfile& t = p.threads[thread];
    ++t.calls;
    t.nanos += nanos;
    t.lockNanos += lockNanos;
}

namespace {

// Every 60 presented frames: the commands that took the most time (per
// frame, all threads) and each thread's share.
void printProfile(uint64_t frames) {
    Profile& p = profile();
    std::vector<std::pair<const char*, CommandProfile>> top;
    std::map<uint64_t, CommandProfile> threads;
    {
        std::lock_guard<std::mutex> lock(p.mutex);
        top.assign(p.commands.begin(), p.commands.end());
        threads.swap(p.threads);
        p.commands.clear();
    }
    std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.second.nanos > b.second.nanos; });
    const double n = static_cast<double>(frames);
    for (const auto& [number, t] : threads)
        std::fprintf(stderr, "winemetalgl:stats: thread %llu: %.2f ms/frame in %.0f calls/frame (lock %.2f ms)\n",
                     static_cast<unsigned long long>(number), ms(t.nanos) / n, static_cast<double>(t.calls) / n,
                     ms(t.lockNanos) / n);
    for (size_t i = 0; i < top.size() && i < 15; ++i)
        std::fprintf(stderr, "winemetalgl:stats:   %-36s %7.2f ms/frame %7.1f calls/frame (lock %.2f ms)\n", top[i].first,
                     ms(top[i].second.nanos) / n, static_cast<double>(top[i].second.calls) / n,
                     ms(top[i].second.lockNanos) / n);
}

}  // namespace

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
                 "waits %llu (%.2f ms) | lock %.2f ms | converted %llu vertices, %llu reused (client %llu format %llu "
                 "[0x%llx] other %llu) | cpu indices %llu | uniforms %llu KiB | commits %llu (query %llu) | "
                 "pass ends: target %llu blit %llu compute %llu visibility %llu commit %llu barrier %llu clear %llu "
                 "other %llu | reads async %llu waiting %llu\n",
                 static_cast<unsigned long long>(t.index), t.lastPresent ? ms(now - t.lastPresent) : 0.0, ms(f.glNanos),
                 static_cast<unsigned long long>(f.calls), ms(f.presentNanos), ms(f.drawableNanos),
                 static_cast<unsigned long long>(f.draws),
                 static_cast<unsigned long long>(f.passes), static_cast<unsigned long long>(f.pipelineMisses),
                 ms(f.pipelineNanos), static_cast<unsigned long long>(f.programLinks), ms(f.linkNanos),
                 static_cast<unsigned long long>(f.transientBytes / 1024), static_cast<unsigned long long>(f.waits),
                 ms(f.waitNanos), ms(f.lockNanos), static_cast<unsigned long long>(f.convertedVertices),
                 static_cast<unsigned long long>(f.convertedReused), static_cast<unsigned long long>(f.convertedClient),
                 static_cast<unsigned long long>(f.convertedFormat), static_cast<unsigned long long>(f.convertedFormatType),
                 static_cast<unsigned long long>(f.convertedOther), static_cast<unsigned long long>(f.cpuIndexDraws),
                 static_cast<unsigned long long>(f.uniformBytes / 1024), static_cast<unsigned long long>(f.commits),
                 static_cast<unsigned long long>(f.queryCommits), static_cast<unsigned long long>(f.passEnds[1]),
                 static_cast<unsigned long long>(f.passEnds[2]), static_cast<unsigned long long>(f.passEnds[3]),
                 static_cast<unsigned long long>(f.passEnds[4]), static_cast<unsigned long long>(f.passEnds[5]),
                 static_cast<unsigned long long>(f.passEnds[6]), static_cast<unsigned long long>(f.passEnds[7]),
                 static_cast<unsigned long long>(f.passEnds[0]), static_cast<unsigned long long>(f.readsAsync),
                 static_cast<unsigned long long>(f.readsWaiting));
    if (t.index % 60 == 59) printProfile(60);
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
