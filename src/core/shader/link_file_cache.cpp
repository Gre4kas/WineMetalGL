#include "core/shader/link_file_cache.h"

#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>

namespace wmgl::shader {
namespace {

// Every field of the structs below is written. Adding a field changes a
// struct's size: these fail to compile until the field is serialized (and
// kFormat moves on).
static_assert(sizeof(StageOutput) == 184, "serialize the new StageOutput field");
static_assert(sizeof(StageOutput::VertexInput) == 24 && sizeof(StageOutput::FragOutput) == 12 &&
              sizeof(StageOutput::StorageArg) == 12, "serialize the new StageOutput member field");
static_assert(sizeof(TransformFeedbackVarying) == 112, "serialize the new TransformFeedbackVarying field");
static_assert(sizeof(GeometryEmulation) == 432 && sizeof(GeometryEmulation::Field) == 40,
              "serialize the new GeometryEmulation field");
static_assert(sizeof(TessellationEmulation) == 248, "serialize the new TessellationEmulation field");
static_assert(sizeof(SubroutineStageInfo) == 80 && sizeof(SubroutineStageInfo::Function) == 56 &&
              sizeof(SubroutineStageInfo::Uniform) == 112, "serialize the new SubroutineStageInfo field");
static_assert(sizeof(LinkResult) == 2760 && sizeof(LinkResult::BuiltinUniforms) == 28, "serialize the new LinkResult field");
static_assert(sizeof(UniformInfo) == 80 && sizeof(AtomicCounterBufferInfo) == 64 && sizeof(StorageBlockInfo) == 64 &&
              sizeof(BufferVariableInfo) == 64 && sizeof(UniformBlockInfo) == 88 && sizeof(AttribInfo) == 48 &&
              sizeof(FragOutputInfo) == 40 && sizeof(InterfaceVariableInfo) == 56 && sizeof(ImageSlotInfo) == 32 &&
              sizeof(SamplerSlotInfo) == 40,
              "serialize the new reflection field");

constexpr uint32_t kFormat = 1;
constexpr char kMagic[8] = {'W', 'M', 'G', 'L', 'L', 'N', 'K', '1'};

// ------------------------------------------------------------ the format

class Writer {
public:
    std::vector<uint8_t> bytes;
    void raw(const void* p, size_t n) {
        const auto* b = static_cast<const uint8_t*>(p);
        bytes.insert(bytes.end(), b, b + n);
    }
    void u64(uint64_t v) { raw(&v, sizeof(v)); }
    void i64(int64_t v) { raw(&v, sizeof(v)); }
};

class Reader {
public:
    Reader(const uint8_t* p, size_t n) : p_(p), end_(p + n) {}
    bool ok = true;
    void raw(void* out, size_t n) {
        if (!ok || static_cast<size_t>(end_ - p_) < n) {
            ok = false;
            std::memset(out, 0, n);
            return;
        }
        std::memcpy(out, p_, n);
        p_ += n;
    }
    uint64_t u64() {
        uint64_t v = 0;
        raw(&v, sizeof(v));
        return v;
    }
    int64_t i64() {
        int64_t v = 0;
        raw(&v, sizeof(v));
        return v;
    }
    // A count of elements of at least `minBytes` each that fit in what is left.
    size_t count(size_t minBytes) {
        const uint64_t n = u64();
        if (!ok || n > static_cast<uint64_t>(end_ - p_) / std::max<size_t>(1, minBytes)) {
            ok = false;
            return 0;
        }
        return static_cast<size_t>(n);
    }
    bool done() const { return ok && p_ == end_; }

private:
    const uint8_t* p_;
    const uint8_t* end_;
};

// Fields are written and read by one function per struct (`io`), with W the
// Writer or the Reader: scalars through `f`.
template <typename T>
void f(Writer& w, const T& v) {
    if constexpr (std::is_same_v<T, bool>) w.u64(v ? 1 : 0);
    else if constexpr (std::is_enum_v<T>) w.i64(static_cast<int64_t>(v));
    else if constexpr (std::is_floating_point_v<T>) w.raw(&v, sizeof(v));
    else if constexpr (std::is_signed_v<T>) w.i64(static_cast<int64_t>(v));
    else w.u64(static_cast<uint64_t>(v));
}
template <typename T>
void f(Reader& r, T& v) {
    if constexpr (std::is_same_v<T, bool>) v = r.u64() != 0;
    else if constexpr (std::is_enum_v<T>) v = static_cast<T>(r.i64());
    else if constexpr (std::is_floating_point_v<T>) r.raw(&v, sizeof(v));
    else if constexpr (std::is_signed_v<T>) v = static_cast<T>(r.i64());
    else v = static_cast<T>(r.u64());
}
void f(Writer& w, const std::string& s) {
    w.u64(s.size());
    w.raw(s.data(), s.size());
}
void f(Reader& r, std::string& s) {
    const size_t n = r.count(1);
    s.resize(n);
    if (n) r.raw(s.data(), n);
}
template <typename T, size_t N>
void f(Writer& w, const T (&a)[N]) {
    for (const T& v : a) f(w, v);
}
template <typename T, size_t N>
void f(Reader& r, T (&a)[N]) {
    for (T& v : a) f(r, v);
}
template <typename A, typename B>
void f(Writer& w, const std::pair<A, B>& p) {
    f(w, p.first);
    f(w, p.second);
}
template <typename A, typename B>
void f(Reader& r, std::pair<A, B>& p) {
    f(r, p.first);
    f(r, p.second);
}

// Structs: `io(w, s)` lists the fields once for both directions.
#define WMGL_IO(Type, ...)                                \
    template <typename W, typename S>                     \
    void io##Type(W& w, S& s) {                           \
        auto fields = [&](auto&... v) { (f(w, v), ...); }; \
        fields(__VA_ARGS__);                              \
    }                                                     \
    void f(Writer& w, const Type& s) { io##Type(w, s); }  \
    void f(Reader& r, Type& s) { io##Type(r, s); }

template <typename T>
void f(Writer& w, const std::vector<T>& v);
template <typename T>
void f(Reader& r, std::vector<T>& v);

using VertexInput = StageOutput::VertexInput;
using FragOutput = StageOutput::FragOutput;
using StorageArg = StageOutput::StorageArg;
using GeometryField = GeometryEmulation::Field;
using SubroutineFunction = SubroutineStageInfo::Function;
using SubroutineUniform = SubroutineStageInfo::Uniform;
using BuiltinUniforms = LinkResult::BuiltinUniforms;

WMGL_IO(VertexInput, s.location, s.components, s.kind, s.attrib, s.part, s.byteOffset)
WMGL_IO(FragOutput, s.location, s.index, s.kind)
WMGL_IO(StorageArg, s.block, s.writes, s.counterBinding)
WMGL_IO(UniformInfo, s.name, s.type, s.size, s.location, s.blockIndex, s.offset, s.arrayStride, s.matrixStride, s.rowMajor,
        s.samplerSlot, s.atomicBufferIndex, s.imageSlot, s.explicitBinding, s.stageMask)
WMGL_IO(AtomicCounterBufferInfo, s.binding, s.dataSize, s.activeUniforms, s.stageMask, s.stageSlot)
WMGL_IO(StorageBlockInfo, s.name, s.dataSize, s.activeVariables, s.stageMask, s.explicitBinding)
WMGL_IO(BufferVariableInfo, s.name, s.type, s.size, s.offset, s.arrayStride, s.matrixStride, s.rowMajor, s.blockIndex,
        s.topLevelArraySize, s.topLevelArrayStride, s.stageMask)
WMGL_IO(UniformBlockInfo, s.name, s.dataSize, s.activeUniforms, s.stageMask, s.stageSlot, s.explicitBinding)
WMGL_IO(AttribInfo, s.name, s.type, s.size, s.location, s.locationCount, s.component, s.array, s.conventional)
WMGL_IO(FragOutputInfo, s.name, s.type, s.location, s.index, s.size)
WMGL_IO(InterfaceVariableInfo, s.name, s.type, s.size, s.location, s.locationIndex, s.component, s.perPatch, s.stageMask)
WMGL_IO(ImageSlotInfo, s.type, s.format, s.stageTexture)
WMGL_IO(SamplerSlotInfo, s.type, s.textureTarget, s.shadow, s.stageTexture, s.sampled)
WMGL_IO(StageOutput, s.present, s.msl, s.mslNoRaster, s.entryPoint, s.spirv, s.usesDefaultUniforms, s.usesDriverUniforms,
        s.writesPointSize, s.discards, s.usesPointCoord, s.flatInputs, s.vertexInputs, s.fragOutputs, s.storageArgs)
WMGL_IO(TransformFeedbackVarying, s.name, s.type, s.size, s.components, s.buffer, s.offset, s.expression, s.array, s.source,
        s.stream, s.sourceOffset)
WMGL_IO(GeometryField, s.name, s.type, s.size, s.offset, s.stream)
WMGL_IO(GeometryEmulation, s.active, s.inputType, s.outputType, s.verticesIn, s.maxVertices, s.invocations, s.inputStride,
        s.outputStride, s.outputs, s.streams, s.streamOffset, s.kernel, s.passthrough)
WMGL_IO(SubroutineFunction, s.name, s.types, s.index)
WMGL_IO(SubroutineUniform, s.name, s.type, s.dimensions, s.location, s.selectionOffset, s.selectionStride, s.compatible)
WMGL_IO(SubroutineStageInfo, s.types, s.functions, s.uniforms, s.locationCount)
WMGL_IO(BuiltinUniforms, s.depthRange, s.numSamples, s.baseVertex, s.baseInstance, s.drawId)

void f(Writer& w, const LinkResult& s);
void f(Reader& r, LinkResult& s);

// The evaluation stage's own link (shared, may be null).
void f(Writer& w, const std::shared_ptr<LinkResult>& p) {
    f(w, p != nullptr);
    if (p) f(w, *p);
}
void f(Reader& r, std::shared_ptr<LinkResult>& p) {
    bool present = false;
    f(r, present);
    p.reset();
    if (!present || !r.ok) return;
    p = std::make_shared<LinkResult>();
    f(r, *p);
}

WMGL_IO(TessellationEmulation, s.active, s.hasControl, s.verticesOut, s.mode, s.spacing, s.order, s.pointMode, s.inputStride,
        s.vertexWords, s.patchWords, s.patchesPerGroup, s.readsPrimitiveId, s.readsInstanceId, s.control, s.evaluation)
WMGL_IO(LinkResult, s.ok, s.log, s.blendSupport, s.uniforms, s.locations, s.blocks, s.attribs, s.fragOutputs, s.samplers, s.images,
        s.atomicBuffers, s.storageBlocks, s.programInputs, s.programOutputs, s.bufferVariables, s.subroutines,
        s.builtinUniforms, s.transformFeedback, s.xfbStride, s.xfbStream, s.xfbBufferCount, s.xfbVertexCapture, s.spirv,
        s.spirvUnnamed, s.defaultBlockSize, s.defaultBlockInit, s.stages, s.flatPerVertex, s.flatAttribs,
        s.geometryInputType, s.geometryOutputType, s.geometryVerticesOut, s.geometryInvocations, s.geometry, s.tess,
        s.computeLocalSize, s.fixedFunctionBlock)

template <typename T>
void f(Writer& w, const std::vector<T>& v) {
    w.u64(v.size());
    if constexpr (std::is_arithmetic_v<T> && !std::is_same_v<T, bool>) {
        w.raw(v.data(), v.size() * sizeof(T));
    } else {
        for (const T& e : v) f(w, e);
    }
}
template <typename T>
void f(Reader& r, std::vector<T>& v) {
    if constexpr (std::is_arithmetic_v<T> && !std::is_same_v<T, bool>) {
        v.resize(r.count(sizeof(T)));
        if (!v.empty()) r.raw(v.data(), v.size() * sizeof(T));
    } else {
        v.resize(r.count(1));
        for (T& e : v) {
            if (!r.ok) break;
            f(r, e);
        }
    }
}

#undef WMGL_IO

// ------------------------------------------------------------- the files

// The directory, or "" (off).
const std::string& cacheDirectory() {
    static const std::string dir = [] {
        const char* v = std::getenv("WINEMETALGL_SHADER_CACHE");
        if (v && !std::strcmp(v, "0")) return std::string();
        std::string d;
        if (v && *v) d = v;
        else if (const char* home = std::getenv("HOME")) d = std::string(home) + "/Library/Caches/WineMetalGL/shaders";
        else return std::string();
        for (size_t at = 1; at <= d.size(); ++at)
            if (at == d.size() || d[at] == '/') ::mkdir(d.substr(0, at).c_str(), 0755);
        return d;
    }();
    return dir;
}

uint64_t fnv1a(const void* data, size_t n, uint64_t h = 1469598103934665603ull) {
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

// What results depend on besides the key: the compiler (this build: the
// file it is in, by its size and modification time), the format, and the
// settings the shader compiler reads.
const std::string& buildIdentity() {
    static const std::string identity = [] {
        std::string id = "format " + std::to_string(kFormat);
        Dl_info info;
        struct stat st;
        if (dladdr(reinterpret_cast<const void*>(&cacheDirectory), &info) && info.dli_fname && ::stat(info.dli_fname, &st) == 0)
            id += " " + std::string(info.dli_fname) + " " + std::to_string(st.st_size) + " " +
                  std::to_string(st.st_mtimespec.tv_sec) + "." + std::to_string(st.st_mtimespec.tv_nsec);
        else
            id += " " __DATE__ " " __TIME__;
        for (const char* name : {"WINEMETALGL_TCS_LAZY_INPUTS", "WINEMETALGL_TCS_PATCHES_PER_GROUP"}) {
            const char* v = std::getenv(name);
            id += std::string(" ") + name + "=" + (v ? v : "");
        }
        return id;
    }();
    return identity;
}

std::string pathFor(const std::string& key) {
    const std::string& id = buildIdentity();
    const uint64_t a = fnv1a(key.data(), key.size(), fnv1a(id.data(), id.size()));
    const uint64_t b = fnv1a(key.data(), key.size(), 0x9e3779b97f4a7c15ull ^ a);
    char name[48];
    std::snprintf(name, sizeof(name), "/%016llx%016llx.link", static_cast<unsigned long long>(a), static_cast<unsigned long long>(b));
    return cacheDirectory() + name;
}

bool readFile(const std::string& path, std::vector<uint8_t>& out) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    struct stat st;
    bool ok = ::fstat(fd, &st) == 0 && st.st_size > 0 && st.st_size < (64 << 20);
    if (ok) {
        out.resize(static_cast<size_t>(st.st_size));
        size_t done = 0;
        while (done < out.size()) {
            const ssize_t n = ::read(fd, out.data() + done, out.size() - done);
            if (n <= 0) break;
            done += static_cast<size_t>(n);
        }
        ok = done == out.size();
    }
    ::close(fd);
    return ok;
}

// Keeps the directory under kMaxBytes: past it, the least recently used
// files (by modification time: reads touch them) go until kKeepBytes are
// left. Once per process, before its first store.
constexpr uint64_t kMaxBytes = uint64_t{512} << 20, kKeepBytes = uint64_t{384} << 20;
void trimDirectory() {
    static std::once_flag once;
    std::call_once(once, [] {
        const std::string& dir = cacheDirectory();
        DIR* d = ::opendir(dir.c_str());
        if (!d) return;
        struct File {
            std::string path;
            uint64_t bytes;
            time_t time;
        };
        std::vector<File> files;
        uint64_t total = 0;
        while (dirent* e = ::readdir(d)) {
            const size_t n = std::strlen(e->d_name);
            if (n < 5 || std::strcmp(e->d_name + n - 5, ".link") != 0) continue;
            File file{dir + "/" + e->d_name, 0, 0};
            struct stat st;
            if (::stat(file.path.c_str(), &st) != 0) continue;
            file.bytes = static_cast<uint64_t>(st.st_size);
            file.time = st.st_mtimespec.tv_sec;
            total += file.bytes;
            files.push_back(std::move(file));
        }
        ::closedir(d);
        if (total <= kMaxBytes) return;
        std::sort(files.begin(), files.end(), [](const File& a, const File& b) { return a.time < b.time; });
        for (const File& file : files) {
            if (total <= kKeepBytes) break;
            if (::unlink(file.path.c_str()) == 0) total -= file.bytes;
        }
    });
}

}  // namespace

std::vector<uint8_t> serializeLinkResult(const LinkResult& result) {
    Writer w;
    f(w, result);
    return std::move(w.bytes);
}

bool deserializeLinkResult(const std::vector<uint8_t>& bytes, LinkResult& result) {
    Reader r(bytes.data(), bytes.size());
    result = LinkResult();
    f(r, result);
    return r.done();
}

std::shared_ptr<LinkResult> loadLinkResult(const std::string& key) {
    if (cacheDirectory().empty()) return nullptr;
    std::vector<uint8_t> file;
    const std::string path = pathFor(key);
    if (!readFile(path, file)) return nullptr;
    // magic, identity, key, result
    Reader r(file.data(), file.size());
    char magic[8];
    r.raw(magic, sizeof(magic));
    std::string identity, storedKey;
    f(r, identity);
    f(r, storedKey);
    if (!r.ok || std::memcmp(magic, kMagic, sizeof(magic)) != 0 || identity != buildIdentity() || storedKey != key)
        return nullptr;
    auto result = std::make_shared<LinkResult>();
    f(r, *result);
    if (!r.done() || !result->ok) return nullptr;
    ::utimes(path.c_str(), nullptr);  // recently used: kept by trimDirectory
    return result;
}

void storeLinkResult(const std::string& key, const LinkResult& result) {
    if (cacheDirectory().empty() || !result.ok) return;
    trimDirectory();
    Writer w;
    w.raw(kMagic, sizeof(kMagic));
    f(w, buildIdentity());
    f(w, key);
    f(w, result);
    if (w.bytes.size() >= (64u << 20)) return;
    // Written whole, then renamed into place: readers see all or nothing.
    const std::string path = pathFor(key);
    char tmp[64];
    std::snprintf(tmp, sizeof(tmp), ".tmp%d.%p", static_cast<int>(::getpid()), static_cast<const void*>(&result));
    const std::string temporary = path + tmp;
    const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return;
    size_t done = 0;
    while (done < w.bytes.size()) {
        const ssize_t n = ::write(fd, w.bytes.data() + done, w.bytes.size() - done);
        if (n <= 0) break;
        done += static_cast<size_t>(n);
    }
    ::close(fd);
    if (done != w.bytes.size() || ::rename(temporary.c_str(), path.c_str()) != 0) ::unlink(temporary.c_str());
}

}  // namespace wmgl::shader
