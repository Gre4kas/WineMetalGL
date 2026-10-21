#include "core/metal/pipeline_cache.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <cstring>
#include <dirent.h>
#include <mutex>
#include <cstdlib>
#include <cctype>
#include <iterator>
#include <optional>
#include <sys/sysctl.h>
#include <sys/stat.h>
#include <unistd.h>

#import <objc/message.h>

#include "core/metal/device.h"
#include "core/program.h"
#include "core/shader/compiler.h"
#include "core/util/log.h"
#include "core/stats.h"

namespace wmgl {

namespace {

bool predictionEnabled() {
    static const bool enabled = [] {
        const char* v = std::getenv("WINEMETALGL_PREDICT_PIPELINES");
        return !v || v[0] != '0';
    }();
    return enabled;
}

std::string fileSafe(const char* text) {
    std::string out;
    for (const char* c = text ? text : ""; *c; ++c)
        out += (std::isalnum(static_cast<unsigned char>(*c)) || *c == '.' || *c == '-' || *c == '_') ? *c : '_';
    return out;
}

// The render pipelines of a program, compiled, in a Metal binary archive on
// disk: ~/Library/Caches/WineMetalGL/pipelines (WINEMETALGL_PIPELINE_CACHE
// names another directory; 0 turns it off), one file per program,
// WineMetalGL build, GPU and macOS build (a compiled pipeline is only valid
// for those). A pipeline the archive has is created without compiling.
//
// The archive loaded from disk is only read, by any thread; pipelines
// compiled in this run are added on a serial queue to a second copy, which
// is written back (all of it) shortly after. Both are read from disk off
// the drawing thread: the first when a context is made (start()), the
// second on the queue when the first pipeline is added.
class PipelineArchive {
public:
    static PipelineArchive& get() {
        static PipelineArchive* archive = new PipelineArchive();  // never destroyed: queued work may run at exit
        return *archive;
    }

    // Starts reading the archive (a game's archive can be hundreds of MB).
    static void start() { get(); }

    // Creates the pipeline from the archive, or compiles it and adds it.
    id<MTLRenderPipelineState> create(MTLRenderPipelineDescriptor* desc, NSError** error) {
        id<MTLDevice> device = Device::get().mtl();
        if (!loadedReady_.load(std::memory_order_acquire)) {
            // Still reading (a large archive takes seconds; it started when
            // the context was made, usually long before the first draw): a
            // short wait, then a compile, which Metal's own shader cache
            // often makes quick, rather than holding the draw.
            std::unique_lock<std::mutex> lock(loadMutex_);
            loadDone_.wait_for(lock, std::chrono::milliseconds(50),
                               [&] { return loadedReady_.load(std::memory_order_acquire); });
        }
        if (loadedReady_.load(std::memory_order_acquire) && loaded_) {
            desc.binaryArchives = @[ loaded_ ];
            id<MTLRenderPipelineState> state = [device newRenderPipelineStateWithDescriptor:desc
                                                                                    options:MTLPipelineOptionFailOnBinaryArchiveMiss
                                                                                 reflection:nil
                                                                                      error:nil];
            if (state) {
                stats::count(&stats::Frame::pipelineArchiveHits);
                return state;
            }
            desc.binaryArchives = nil;
        }
        id<MTLRenderPipelineState> state = [device newRenderPipelineStateWithDescriptor:desc error:error];
        if (state) add(desc);
        return state;
    }

private:
    PipelineArchive() {
        const char* v = std::getenv("WINEMETALGL_PIPELINE_CACHE");
        if (v && !std::strcmp(v, "0")) return;
        std::string dir;
        if (v && *v) dir = v;
        else if (const char* home = std::getenv("HOME")) dir = std::string(home) + "/Library/Caches/WineMetalGL/pipelines";
        else return;
        for (size_t at = 1; at <= dir.size(); ++at)
            if (at == dir.size() || dir[at] == '/') ::mkdir(dir.substr(0, at).c_str(), 0755);
        id<MTLDevice> device = Device::get().mtl();
        char build[64] = "";
        size_t size = sizeof(build);
        sysctlbyname("kern.osversion", build, &size, nullptr, 0);
#ifdef WMGL_VERSION
        const std::string version = fileSafe(WMGL_COMMIT[0] ? WMGL_COMMIT : WMGL_VERSION);
#else
        const std::string version = "dev";
#endif
        path_ = dir + "/" + logProgramName() + "-" + version + "-" + fileSafe(device.name.UTF8String) + "-" + fileSafe(build) +
                ".metallib";
        url_ = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path_.c_str()]];
        dir_ = dir;
        queue_ = dispatch_queue_create_with_target("winemetalgl.pipeline-archive", DISPATCH_QUEUE_SERIAL,
                                                  dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
        enabled_ = true;
        loadedReady_.store(false, std::memory_order_relaxed);
        // Read on a queue of its own (the archive queue may be saving).
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            @autoreleasepool {
                struct stat st;
                const bool exists = ::stat(path_.c_str(), &st) == 0 && st.st_size > 0;
                id<MTLBinaryArchive> loaded = exists ? open(true) : nil;
                if (exists && !loaded) ::unlink(path_.c_str());
                full_ = loaded && st.st_size > kMaxBytes;
                WMGL_INFO(Pipeline, "pipeline cache %s (%s)", path_.c_str(), loaded ? "loaded" : "new");
                {
                    std::lock_guard<std::mutex> lock(loadMutex_);
                    loaded_ = loaded;
                    loadedReady_.store(true, std::memory_order_release);
                }
                loadDone_.notify_all();
                removeStaleTemporaries();
            }
        });
        // Saved at exit too (a short session ends before the delayed save),
        // without the additions still queued.
        std::atexit([] { get().finish(); });
    }

    // The archive at path_, or (fromDisk false) a new one.
    id<MTLBinaryArchive> open(bool fromDisk) {
        MTLBinaryArchiveDescriptor* d = [MTLBinaryArchiveDescriptor new];
        if (fromDisk) d.url = url_;
        NSError* error = nil;
        id<MTLBinaryArchive> archive = [Device::get().mtl() newBinaryArchiveWithDescriptor:d error:&error];
        if (!archive && fromDisk)
            WMGL_WARN(Pipeline, "pipeline cache %s unreadable (%s); starting a new one", path_.c_str(),
                      error.localizedDescription.UTF8String);
        return archive;
    }

    // Temporary files of saves that never finished (a process that ended
    // while writing), an hour old or more.
    void removeStaleTemporaries() {
        DIR* d = ::opendir(dir_.c_str());
        if (!d) return;
        const time_t now = ::time(nullptr);
        while (dirent* e = ::readdir(d)) {
            const size_t n = std::strlen(e->d_name);
            if (n < 4 || std::strcmp(e->d_name + n - 4, ".tmp") != 0 || !std::strstr(e->d_name, ".metallib.")) continue;
            const std::string file = dir_ + "/" + e->d_name;
            struct stat st;
            if (::stat(file.c_str(), &st) == 0 && now - st.st_mtimespec.tv_sec > 3600) ::unlink(file.c_str());
        }
        ::closedir(d);
    }

    void finish() {
        if (!enabled_) return;
        stopping_ = true;
        // The last chance for this session's pipelines: a few tries (see save()).
        dispatch_sync(queue_, ^{
            for (int attempt = 0; attempt < 3 && unsaved_ && !save(attempt == 2); ++attempt) usleep(200000);
        });
    }

    void add(MTLRenderPipelineDescriptor* desc) {
        if (full_ || !enabled_) return;
        dispatch_async(queue_, ^{
            if (stopping_) return;
            @autoreleasepool {
                // The copy written back: the file's contents, read here the first time.
                if (!writing_) {
                    // After the read: a copy started before it would replace the file's pipelines.
                    {
                        std::unique_lock<std::mutex> lock(loadMutex_);
                        loadDone_.wait(lock, [&] { return loadedReady_.load(std::memory_order_acquire); });
                    }
                    writing_ = loaded_ ? open(true) : nil;
                    if (!writing_) writing_ = open(false);
                    if (!writing_) return;
                }
                // Compiles the pipeline's functions for the archive (a second
                // compile, off the drawing thread).
                NSError* error = nil;
                const uint64_t start = stats::nowNanos();
                const BOOL added = [writing_ addRenderPipelineFunctionsWithDescriptor:desc error:&error];
                stats::g_predictNanos.fetch_add(stats::nowNanos() - start, std::memory_order_relaxed);
                if (added) {
                    ++unsaved_;
                    stats::g_pipelinesArchived.fetch_add(1, std::memory_order_relaxed);
                } else {
                    WMGL_INFO(Pipeline, "pipeline cache: not added (%s)", error.localizedDescription.UTF8String);
                }
                if (!saveScheduled_ && unsaved_) {
                    saveScheduled_ = true;
                    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC), queue_, ^{ save(); });
                }
            }
        });
    }

    // On queue_: writes the archive (to a temporary file renamed over the
    // old one, so a reader never sees half of it). Metal's archiver fails
    // now and then while the process compiles other pipelines ("cannot
    // pack ... gpuarchiver", a different pipeline each time in DOOM (2016));
    // the same archive saves on a later try, which comes a few seconds on.
    // `last`: no try follows (a failure is a warning). False on failure.
    bool save(bool last = false) {
        @autoreleasepool {
            saveScheduled_ = false;
            if (!unsaved_) return true;
            const std::string temporary = path_ + "." + std::to_string(getpid()) + ".tmp";
            NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:temporary.c_str()]];
            NSError* error = nil;
            if ([writing_ serializeToURL:url error:&error] && ::rename(temporary.c_str(), path_.c_str()) == 0) {
                WMGL_INFO(Pipeline, "pipeline cache: saved %d new pipelines", unsaved_);
                unsaved_ = 0;
                failures_ = 0;
                struct stat st;
                if (::stat(path_.c_str(), &st) == 0 && st.st_size > kMaxBytes) {
                    full_ = true;
                    WMGL_INFO(Pipeline, "pipeline cache %s is full", path_.c_str());
                }
                return true;
            }
            ::unlink(temporary.c_str());
            // Metal had none of the additions to write (they were in the
            // archive already): nothing is lost, and trying again finds the same.
            const char* reason = error.localizedDescription.UTF8String;
            if (reason && std::strstr(reason, "Nothing to serialize")) {
                unsaved_ = 0;
                failures_ = 0;
                return true;
            }
            if (last) {
                WMGL_WARN(Pipeline, "pipeline cache: %d new pipelines not saved (%s)", unsaved_, reason ? reason : "");
                return false;
            }
            // Each try runs Metal's archiver over the whole archive (CPU time
            // the game is using): 3, 6, then 12 s apart, then not again until
            // new pipelines come (they are still in writing_ for that save).
            if (++failures_ > 3) {
                WMGL_INFO(Pipeline, "pipeline cache: not saved after %d tries (%s); saving again with the next new pipelines",
                          failures_ - 1, reason ? reason : "");
                unsaved_ = 0;
                failures_ = 0;
                return false;
            }
            WMGL_INFO(Pipeline, "pipeline cache: not saved, trying again (%s)", reason ? reason : "");
            if (!stopping_ && !saveScheduled_) {
                saveScheduled_ = true;
                dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t{3} << (failures_ - 1)) * NSEC_PER_SEC), queue_,
                               ^{ save(); });
            }
            return false;
        }
    }

    // Reading an archive takes about a second per 70 MB: past this size it
    // takes no more pipelines.
    static constexpr off_t kMaxBytes = off_t(256) << 20;
    std::string path_, dir_;
    NSURL* url_ = nil;
    bool enabled_ = false;
    id<MTLBinaryArchive> loaded_ = nil;   // read only, any thread, once loadedReady_
    std::atomic<bool> loadedReady_{true};  // false while a read is under way
    std::mutex loadMutex_;
    std::condition_variable loadDone_;
    id<MTLBinaryArchive> writing_ = nil;  // queue_ only
    dispatch_queue_t queue_ = nil;
    std::atomic<bool> full_{false}, stopping_{false};
    int unsaved_ = 0;          // queue_ only
    int failures_ = 0;         // queue_ only: saves that failed since the last success
    bool saveScheduled_ = false;  // queue_ only
};

}  // namespace

void startPipelineArchive() { PipelineArchive::start(); }

namespace {

// The interface a pipeline's render state depends on in a program: its
// vertex inputs (location, kind, attribute) and fragment outputs. Programs
// with the same one take the same states. Empty for programs whose
// pipelines depend on more (geometry and tessellation stages, captures,
// clip and cull distances, sample masks, layers and viewports).
std::string interfaceSignature(LinkedProgram& program) {
    if (program.pipeline || !program.hasStage(shader::Stage::Vertex) || !program.hasStage(shader::Stage::Fragment) ||
        program.hasStage(shader::Stage::Geometry) || program.hasStage(shader::Stage::TessControl) ||
        program.hasStage(shader::Stage::TessEval) || program.link.geometry.active || program.link.xfbVertexCapture ||
        program.cullDistances() || program.writesLayerOrViewport() || program.writesSampleMask())
        return {};
    std::string signature = "v";
    for (const shader::StageOutput::VertexInput& in : program.stage(shader::Stage::Vertex).vertexInputs)
        signature += std::to_string(in.location) + in.kind + std::to_string(in.source()) + "." + std::to_string(in.part) + ",";
    signature += "f";
    for (const shader::StageOutput::FragOutput& out : program.stage(shader::Stage::Fragment).fragOutputs)
        signature += std::to_string(out.location) + ":" + std::to_string(out.index) + out.kind + ",";
    return signature;
}

// Whether a draw's pipeline key is a plain render state that suits any
// program of the same interface (no program- or texture-specific variant).
bool predictableState(const PipelineKey& key) {
    return key.rasterization && !key.geometryPass && !key.tessellation && !key.borders[0] && !key.borders[1] &&
           !key.sampleMask && !key.logicOp && !key.ignoreSampleMask && !key.cull && !key.topologyClass &&
           !key.countFragments;
}

constexpr size_t kMaxStates = 4096, kStatesPerSignature = 96, kMaxPredicted = 12000;

dispatch_queue_t predictionQueue(size_t index) {
    // Two serial queues: a few compiles at a time, beside the game's threads.
    static dispatch_queue_t queues[2] = {
        dispatch_queue_create_with_target("winemetalgl.predict.0", DISPATCH_QUEUE_SERIAL,
                                          dispatch_get_global_queue(QOS_CLASS_UTILITY, 0)),
        dispatch_queue_create_with_target("winemetalgl.predict.1", DISPATCH_QUEUE_SERIAL,
                                          dispatch_get_global_queue(QOS_CLASS_UTILITY, 0)),
    };
    return queues[index & 1];
}

}  // namespace

PipelineCache::PipelineCache() : predictions_(dispatch_group_create()) {}

PipelineCache::~PipelineCache() {
    closing_ = true;
    dispatch_group_wait(predictions_, DISPATCH_TIME_FOREVER);
}

template <class K>
size_t PipelineCache::Hash<K>::operator()(const K& k) const {
    // Eight key bytes a step (keys are hundreds of bytes, hashed per draw).
    const auto* bytes = reinterpret_cast<const uint8_t*>(&k);
    uint64_t hash = 1469598103934665603ull ^ sizeof(K);
    size_t i = 0;
    for (; i + 8 <= sizeof(K); i += 8) {
        uint64_t word;
        std::memcpy(&word, bytes + i, 8);
        hash = (hash ^ word) * 0x9e3779b97f4a7c15ull;
        hash ^= hash >> 29;
    }
    for (; i < sizeof(K); ++i) hash = (hash ^ bytes[i]) * 1099511628211ull;
    return static_cast<size_t>(hash ^ (hash >> 32));
}

id<MTLRenderPipelineState> PipelineCache::render(const PipelineKey& key, LinkedProgram& program) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = render_.find(key);
        if (it != render_.end()) return it->second;
    }
    return build(key, program, false);
}

id<MTLRenderPipelineState> PipelineCache::build(const PipelineKey& key, LinkedProgram& program, bool predicted) {
    std::promise<id<MTLRenderPipelineState>> promise;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        auto it = render_.find(key);
        if (it != render_.end()) return it->second;
        auto building = building_.find(key);
        if (building != building_.end()) {
            // Another thread (a prediction) is creating it: wait for that.
            if (predicted) return nil;
            Future future = building->second;
            lock.unlock();
            stats::count(&stats::Frame::pipelineWaits);
            stats::Timer timer(&stats::Frame::pipelineNanos);
            if (id<MTLRenderPipelineState> state = future.get()) return state;
            lock.lock();
            it = render_.find(key);
            if (it != render_.end()) return it->second;
        }
        building_[key] = promise.get_future().share();
    }
    id<MTLRenderPipelineState> state = nil;
    @autoreleasepool {
        std::optional<stats::Timer> timer;
        if (!predicted) {
            stats::count(&stats::Frame::pipelineMisses);
            timer.emplace(&stats::Frame::pipelineNanos);
        }
        NSError* error = nil;
        MTLRenderPipelineDescriptor* desc = describe(key, program);
        state = desc ? PipelineArchive::get().create(desc, &error) : nil;
        if (desc && !state && !predicted) WMGL_ERR(Pipeline, "render pipeline creation failed: %s", error.localizedDescription.UTF8String);
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // A failed guess is not kept: the draw that needs the key (if one
        // does) tries again and reports its error.
        if (state || !predicted) render_.emplace(key, state);
        building_.erase(key);
    }
    promise.set_value(state);
    if (state && !predicted) learn(key, program);
    if (state && predicted) stats::g_pipelinesPredicted.fetch_add(1, std::memory_order_relaxed);
    return state;
}

MTLRenderPipelineDescriptor* PipelineCache::describe(const PipelineKey& key, LinkedProgram& program) {
    const bool pointSizeAllowed = key.topologyClass == MTLPrimitiveTopologyClassUnspecified ||
                                  key.topologyClass == MTLPrimitiveTopologyClassPoint;
    id<MTLFunction> vertex = key.tessellation ? program.postTessellationVertex(key.tessellation & 2, key.borders[0])
                             : key.borders[0] && !key.geometryPass
                                 ? program.vertexWithBorders(key.rasterization, pointSizeAllowed, key.borders[0])
                             : !key.rasterization ? program.vertexNoRaster()
                             : !pointSizeAllowed  ? program.vertexWithoutPointSize(key.geometryPass)
                             : key.geometryPass   ? program.geometryPassthrough()
                                                  : program.function(shader::Stage::Vertex);
    id<MTLFunction> fragment =
        key.sampleMask || key.logicOp || key.sampleShading || key.ignoreSampleMask || key.cull || key.borders[1] ||
                key.countFragments
            ? program.fragmentVariant(key.sampleMask, key.logicOp, key.colorFormats, key.sampleShading, key.ignoreSampleMask,
                                      key.cull, key.borders[1], key.countFragments)
            : program.function(shader::Stage::Fragment);
    if (!vertex) return nil;
    MTLRenderPipelineDescriptor* desc = [MTLRenderPipelineDescriptor new];
    desc.vertexFunction = vertex;
    desc.fragmentFunction = key.rasterization ? fragment : nil;
    if (fragment && key.rasterization && !key.alphaToCoverage && !key.countFragments && !key.sampleMask &&
        std::all_of(std::begin(key.colorFormats), std::end(key.colorFormats), [](uint16_t f) { return f == 0; })) {
        // Depth-only pass (Foundation's shadow maps, issue #12): no fragment
        // shading for a fragment stage that can change nothing there
        // (WINEMETALGL_DEPTH_ONLY_FRAGMENT=1 keeps it).
        static const bool enabled = [] {
            const char* v = std::getenv("WINEMETALGL_DEPTH_ONLY_FRAGMENT");
            return !v || v[0] != '1';
        }();
        if (program.fragmentDepthOnlyRemovable.load() < 0) {
            const shader::StageOutput& fs = program.stage(shader::Stage::Fragment);
            bool writes = fs.discards || !program.link.images.empty() || !program.link.atomicBuffers.empty();
            for (const shader::StageOutput::StorageArg& arg : fs.storageArgs) writes |= arg.writes || arg.counterBinding >= 0;
            const std::string& m = fs.msl;
            writes |= m.find("[[depth(") != std::string::npos || m.find("[[sample_mask]]") != std::string::npos ||
                      m.find("atomic") != std::string::npos || m.find("access::write") != std::string::npos ||
                      m.find("access::read_write") != std::string::npos || m.find("discard_fragment") != std::string::npos;
            program.fragmentDepthOnlyRemovable.store(writes ? 0 : 1);
        }
        if (enabled && program.fragmentDepthOnlyRemovable.load() == 1) {
            desc.fragmentFunction = nil;
            // Nothing reads the other outputs either: a vertex function that
            // writes only the position, for depth textures never drawn with
            // colour (DrawState sets positionOnly; WINEMETALGL_POSITION_ONLY=0 keeps it).
            static const bool positionOnly = [] {
                const char* v = std::getenv("WINEMETALGL_POSITION_ONLY");
                return !v || v[0] != '0';
            }();
            if (positionOnly && key.positionOnly && !key.borders[0] && !key.geometryPass) {
                id<MTLFunction> reduced = key.tessellation ? program.postTessellationVertex(key.tessellation & 2, 0, true)
                                                           : program.positionOnlyVertex(pointSizeAllowed);
                if (reduced) {
                    desc.vertexFunction = reduced;
                    stats::count(&stats::Frame::positionOnlyPipelines);
                }
            }
        }
    }
    MTLVertexDescriptor* vd = [MTLVertexDescriptor vertexDescriptor];
    for (uint32_t i = 0; i < std::size(key.attributes); ++i) {
        if (!key.attributes[i].format) continue;
        vd.attributes[i].format = static_cast<MTLVertexFormat>(key.attributes[i].format);
        vd.attributes[i].offset = key.attributes[i].offset;
        vd.attributes[i].bufferIndex = shader::kSlotFirstVertexBuffer + key.attributes[i].layout;
    }
    for (uint32_t l = 0; l < key.layoutCount; ++l) {
        MTLVertexBufferLayoutDescriptor* layout = vd.layouts[shader::kSlotFirstVertexBuffer + l];
        layout.stride = key.layouts[l].stride;
        layout.stepFunction = static_cast<MTLVertexStepFunction>(key.layouts[l].stepFunction);
        layout.stepRate = key.layouts[l].stepFunction == MTLVertexStepFunctionConstant ? 0 : key.layouts[l].stepRate;
    }
    desc.vertexDescriptor = vd;
    desc.rasterSampleCount = key.sampleCount ? key.sampleCount : 1;
    desc.alphaToCoverageEnabled = key.alphaToCoverage;
    desc.alphaToOneEnabled = key.alphaToOne;
    desc.rasterizationEnabled = key.rasterization;
    desc.inputPrimitiveTopology = static_cast<MTLPrimitiveTopologyClass>(key.topologyClass);
    if (key.tessellation) {
        desc.tessellationPartitionMode = static_cast<MTLTessellationPartitionMode>((key.tessellation >> 2) & 3);
        desc.tessellationOutputWindingOrder = key.tessellation & 16 ? MTLWindingClockwise : MTLWindingCounterClockwise;
        desc.tessellationFactorFormat = MTLTessellationFactorFormatHalf;
        desc.tessellationFactorStepFunction = MTLTessellationFactorStepFunctionPerPatch;
        desc.tessellationControlPointIndexType = MTLTessellationControlPointIndexTypeNone;
        desc.maxTessellationFactor = 64;  // GL_MAX_TESS_GEN_LEVEL
    }
    for (int i = 0; i < 8; ++i) {
        desc.colorAttachments[i].pixelFormat = static_cast<MTLPixelFormat>(key.colorFormats[i]);
        if (!key.colorFormats[i]) continue;
        const auto& b = key.blend[i];
        desc.colorAttachments[i].writeMask = b.writeMask;
        desc.colorAttachments[i].blendingEnabled = b.enabled;
        desc.colorAttachments[i].sourceRGBBlendFactor = static_cast<MTLBlendFactor>(b.srcRGB);
        desc.colorAttachments[i].destinationRGBBlendFactor = static_cast<MTLBlendFactor>(b.dstRGB);
        desc.colorAttachments[i].sourceAlphaBlendFactor = static_cast<MTLBlendFactor>(b.srcAlpha);
        desc.colorAttachments[i].destinationAlphaBlendFactor = static_cast<MTLBlendFactor>(b.dstAlpha);
        desc.colorAttachments[i].rgbBlendOperation = static_cast<MTLBlendOperation>(b.opRGB);
        desc.colorAttachments[i].alphaBlendOperation = static_cast<MTLBlendOperation>(b.opAlpha);
    }
    desc.depthAttachmentPixelFormat = static_cast<MTLPixelFormat>(key.depthFormat);
    desc.stencilAttachmentPixelFormat = static_cast<MTLPixelFormat>(key.stencilFormat);
    return desc;
}

void PipelineCache::learn(const PipelineKey& key, LinkedProgram& program) {
    if (!predictionEnabled() || !predictableState(key)) return;
    const std::string signature = interfaceSignature(program);
    if (signature.empty()) return;
    PipelineKey state = key;
    state.programId = 0;
    std::vector<std::pair<PipelineKey, std::shared_ptr<LinkedProgram>>> work;
    {
        std::lock_guard<std::mutex> lock(predictMutex_);
        if (states_.size() >= kMaxStates || !states_.insert(state).second) return;
        std::vector<PipelineKey>& states = statesBySignature_[signature];
        if (states.size() >= kStatesPerSignature) return;
        states.push_back(state);
        auto programs = programsBySignature_.find(signature);
        if (programs == programsBySignature_.end()) return;
        std::vector<std::weak_ptr<LinkedProgram>>& list = programs->second;
        for (size_t i = 0; i < list.size();) {
            std::shared_ptr<LinkedProgram> target = list[i].lock();
            if (!target) {
                list[i] = list.back();
                list.pop_back();
                continue;
            }
            if (target->uid != program.uid) {
                PipelineKey k = state;
                k.programId = target->uid;
                work.emplace_back(k, std::move(target));
            }
            ++i;
        }
    }
    for (auto& [k, target] : work) predict(k, target);
}

void PipelineCache::linked(const std::shared_ptr<LinkedProgram>& program) {
    if (!predictionEnabled() || !program) return;
    const std::string signature = interfaceSignature(*program);
    if (signature.empty()) return;
    std::vector<PipelineKey> keys;
    {
        std::lock_guard<std::mutex> lock(predictMutex_);
        programsBySignature_[signature].push_back(program);
        auto states = statesBySignature_.find(signature);
        if (states != statesBySignature_.end()) keys = states->second;
    }
    for (PipelineKey k : keys) {
        k.programId = program->uid;
        predict(k, program);
    }
}

void PipelineCache::predict(const PipelineKey& key, const std::shared_ptr<LinkedProgram>& program) {
    size_t queue;
    {
        std::lock_guard<std::mutex> lock(predictMutex_);
        if (predicted_.size() >= kMaxPredicted || !predicted_.insert(key).second) return;
        queue = nextQueue_++;
    }
    std::shared_ptr<LinkedProgram> target = program;
    const PipelineKey k = key;
    dispatch_group_async(predictions_, predictionQueue(queue), ^{
        if (closing_) return;
        const uint64_t start = stats::nowNanos();
        build(k, *target, true);
        stats::g_predictNanos.fetch_add(stats::nowNanos() - start, std::memory_order_relaxed);
    });
}

id<MTLDepthStencilState> PipelineCache::depthStencil(const DepthStencilKey& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = depth_.find(key);
    if (it != depth_.end()) return it->second;
    MTLDepthStencilDescriptor* desc = [MTLDepthStencilDescriptor new];
    desc.depthCompareFunction = static_cast<MTLCompareFunction>(key.depthCompare);
    desc.depthWriteEnabled = key.depthWrite;
    if (key.stencilEnabled) {
        auto face = [](const DepthStencilKey::Face& f) {
            MTLStencilDescriptor* s = [MTLStencilDescriptor new];
            s.stencilCompareFunction = static_cast<MTLCompareFunction>(f.compare);
            s.stencilFailureOperation = static_cast<MTLStencilOperation>(f.fail);
            s.depthFailureOperation = static_cast<MTLStencilOperation>(f.depthFail);
            s.depthStencilPassOperation = static_cast<MTLStencilOperation>(f.pass);
            s.readMask = f.readMask;
            s.writeMask = f.writeMask;
            return s;
        };
        desc.frontFaceStencil = face(key.front);
        desc.backFaceStencil = face(key.back);
    }
    id<MTLDepthStencilState> state = [Device::get().mtl() newDepthStencilStateWithDescriptor:desc];
    depth_.emplace(key, state);
    return state;
}

id<MTLSamplerState> PipelineCache::sampler(const SamplerKey& key) {
    // Draws look sampler states up for every sampler: recent ones per thread,
    // before the lock. A state depends on its key only, so one made for
    // another share group serves as well.
    struct Recent {
        SamplerKey key;
        id<MTLSamplerState> state;
    };
    thread_local Recent recent[16];
    Recent& slot = recent[Hash<SamplerKey>()(key) & 15];
    if (slot.state && std::memcmp(&slot.key, &key, sizeof(key)) == 0) return slot.state;
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = samplers_.find(key);
    if (it != samplers_.end()) {
        slot = {key, it->second};
        return it->second;
    }
    MTLSamplerDescriptor* desc = [MTLSamplerDescriptor new];
    desc.minFilter = static_cast<MTLSamplerMinMagFilter>(key.minFilter);
    desc.magFilter = static_cast<MTLSamplerMinMagFilter>(key.magFilter);
    desc.mipFilter = static_cast<MTLSamplerMipFilter>(key.mipFilter);
    desc.sAddressMode = static_cast<MTLSamplerAddressMode>(key.sAddress);
    desc.tAddressMode = static_cast<MTLSamplerAddressMode>(key.tAddress);
    desc.rAddressMode = static_cast<MTLSamplerAddressMode>(key.rAddress);
    desc.compareFunction = static_cast<MTLCompareFunction>(key.compare);
    desc.borderColor = static_cast<MTLSamplerBorderColor>(key.border);
    desc.maxAnisotropy = std::max<uint8_t>(1, key.maxAnisotropy);
    desc.normalizedCoordinates = key.normalized;
    desc.lodMinClamp = key.lodMin;
    desc.lodMaxClamp = key.lodMax;
    // MTLSamplerDescriptor.lodBias is macOS 26 API; set it through the
    // runtime so builds with older SDKs (CI) keep it on systems that have it
    // (without it, the shader applies the bias: DrawState::borderVariant).
    static const SEL setLodBias = NSSelectorFromString(@"setLodBias:");
    if (key.lodBias != 0.0f && Device::get().caps().samplerLodBias)
        reinterpret_cast<void (*)(id, SEL, float)>(objc_msgSend)(desc, setLodBias, key.lodBias);
    desc.supportArgumentBuffers = NO;
    id<MTLSamplerState> state = [Device::get().mtl() newSamplerStateWithDescriptor:desc];
    samplers_.emplace(key, state);
    if (state) slot = {key, state};
    return state;
}

}  // namespace wmgl
