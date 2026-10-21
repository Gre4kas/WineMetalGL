// GL objects. Shared objects (buffers, textures, renderbuffers, samplers,
// shaders, programs, syncs) live in a ShareGroup; container objects (vertex
// arrays, framebuffers, queries, transform feedback) belong to one context.
#pragma once

#import <Metal/Metal.h>

#include <array>
#include <cstring>
#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/format/formats.h"
#include "core/metal/command_stream.h"
#include "core/shader/compiler.h"
#include "core/gl/types.h"
#include "core/util/ref.h"

namespace wmgl {

class Program;
struct LinkedProgram;

// KHR_debug object label (ObjectLabel / GetObjectLabel).
struct Labeled {
    std::string label;
};

// ---------------------------------------------------------------- buffers

class Buffer : public RefCounted, public Labeled {
public:
    explicit Buffer(GLuint name) : name(name) {}
    GLuint name;
    id<MTLBuffer> mtl = nil;     // nil while size == 0
    GLsizeiptr size = 0;
    GLenum usage = GL_STATIC_DRAW;
    bool immutable = false;
    // GL_AMD_pinned_memory: the storage is the application's memory (never
    // replaced behind its back).
    bool pinned = false;
    GLbitfield storageFlags = 0;
    // Mapping state (glMapBuffer*).
    bool mapped = false;
    GLbitfield mapAccess = 0;
    GLintptr mapOffset = 0;
    GLsizeiptr mapLength = 0;
    void* mapPointer = nullptr;
    // A writable invalidating map of a large buffer the GPU still uses
    // writes here instead; glUnmapBuffer copies it into place, ordered on the
    // GPU (api/buffer.mm).
    id<MTLBuffer> mapStaging = nil;
    // Storage replaced while the GPU still used it (copy-on-write), and map
    // staging the GPU has copied from, with the serial of their last use:
    // reused once that completes instead of allocating (api/buffer.mm).
    struct Retired {
        id<MTLBuffer> mtl;
        uint64_t serial;
    };
    static constexpr size_t kMaxRetired = 4;
    std::vector<Retired> retired, retiredStaging;
    // DrawState::uniformBlockBuffer's copies of small uniform block ranges
    // into transient memory, so the GPU never reads the buffer and updates
    // between draws need no copy-on-write: valid for `serial` (one command
    // buffer) while `version` is unchanged.
    struct UniformSnapshot {
        id<MTLBuffer> buffer = nil;
        NSUInteger offset = 0;
        GLintptr source = -1;
        uint32_t bytes = 0;
        uint64_t version = 0, serial = 0;
    };
    UniformSnapshot uniformSnapshots[4];
    uint8_t nextUniformSnapshot = 0;
    // A CPU copy of the contents, for CPU reads of indices (draw.mm
    // drawElements) while ordered GPU copies of CPU data into the buffer
    // (large glBufferSubData, invalidating maps) are pending: started the
    // first time such a read had to wait for the GPU, then kept up to date by
    // the CPU's writes; any other GPU write or respecification invalidates it.
    std::vector<uint8_t> shadow;
    bool shadowValid = false;
    bool shadowCurrent() const { return shadowValid && shadow.size() == static_cast<size_t>(size); }
    // Starts the shadow from the contents, when they are current: no GPU
    // writes pending (small, unmapped buffers only).
    void startShadow() {
        if (size <= (64 << 20) && !mapped && !pinned && contents()) {
            shadow.assign(contents(), contents() + size);
            shadowValid = true;
        }
    }
    void shadowWrite(GLintptr offset, GLsizeiptr bytes, const void* data) {
        if (shadowValid && bytes > 0 && static_cast<size_t>(offset + bytes) <= shadow.size())
            std::memcpy(shadow.data() + offset, data, static_cast<size_t>(bytes));
    }
    // GPU serial of the last command buffer that read or wrote this buffer,
    // and of the last one that wrote it (CPU reads wait for that one).
    uint64_t lastUseSerial = 0;
    uint64_t lastWriteSerial = 0;
    // Incremented whenever the contents change (keys derived caches).
    uint64_t version = 0;
    // drawElements' scans of index ranges in this buffer (draw.mm), valid
    // while `version` is `indexScansVersion`: by range (offset, count, type
    // and restart index, all-ones for none), for a scene's worth of meshes.
    struct IndexRange {
        uintptr_t offset;
        uint64_t count, type, restartIndex;
        bool operator==(const IndexRange& o) const {
            return offset == o.offset && count == o.count && type == o.type && restartIndex == o.restartIndex;
        }
    };
    struct IndexRangeHash {
        size_t operator()(const IndexRange& r) const {
            return std::hash<uint64_t>()(r.offset * 0x9e3779b97f4a7c15ull ^ (r.count << 20) ^ (r.type << 8) ^ r.restartIndex);
        }
    };
    struct IndexScan {
        uint32_t minIndex, maxIndex;
        bool hasRestart;
    };
    static constexpr size_t kMaxIndexScans = 16384;
    std::unordered_map<IndexRange, IndexScan, IndexRangeHash> indexScans;
    uint64_t indexScansVersion = 0;

    // bindVertexInputs' CPU conversions of attributes read from this buffer
    // (draw.mm), valid while `version` is `convertedVersion`: by the
    // attributes' layouts and the elements converted, so draws of unchanged
    // meshes convert once. Their bytes count towards kMaxConvertedBytes.
    struct ConvertedVertices {
        id<MTLBuffer> mtl = nil;
        size_t bytes = 0;
        ~ConvertedVertices() { convertedBytes() -= bytes; }
    };
    static constexpr size_t kMaxConvertedBytes = size_t{256} << 20;
    static std::atomic<size_t>& convertedBytes() {
        static std::atomic<size_t> bytes{0};
        return bytes;
    }
    std::map<std::vector<uint64_t>, std::unique_ptr<ConvertedVertices>> converted;
    uint64_t convertedVersion = 0;
    // drawElements' rewritten index lists of ranges of this element buffer
    // (draw.mm), valid while `version` is `indexListsVersion`: by offset,
    // count, type, mode, rotation and restart index; 32-bit indices.
    std::map<std::vector<uint64_t>, std::unique_ptr<ConvertedVertices>> indexLists;
    uint64_t indexListsVersion = 0;
    // DrawState::drawIndirect's scans for 16-bit and 32-bit indices of the
    // largest value (where Metal always restarts strips), valid
    // while `version` is maxIndexScanVersion[0 for 16-bit, 1 for 32-bit].
    uint64_t maxIndexScanVersion[2] = {~0ull, ~0ull};
    bool hasMaxIndex[2] = {};

    uint8_t* contents() const { return mtl ? static_cast<uint8_t*>(mtl.contents) : nullptr; }
    // Mapped in a way that forbids GPU use (persistent mappings allow it).
    bool mappedNonPersistent() const { return mapped && !(mapAccess & GL_MAP_PERSISTENT_BIT); }
    // Whether [offset, offset + size) overlaps a non-persistent mapping: the
    // INVALID_OPERATION of BufferSubData and ClearBufferSubData (GL 4.6
    // sections 6.2.2, 6.3).
    bool rangeMapped(GLintptr offset, GLsizeiptr size) const {
        return mappedNonPersistent() && offset < mapOffset + mapLength && mapOffset < offset + size;
    }
};

// --------------------------------------------------------------- samplers

struct SamplerParams {
    GLenum minFilter = GL_NEAREST_MIPMAP_LINEAR;
    GLenum magFilter = GL_LINEAR;
    GLenum wrapS = GL_REPEAT, wrapT = GL_REPEAT, wrapR = GL_REPEAT;
    GLfloat minLod = -1000.0f, maxLod = 1000.0f, lodBias = 0.0f;
    GLenum compareMode = GL_NONE;
    GLenum compareFunc = GL_LEQUAL;
    GLfloat maxAnisotropy = 1.0f;
    GLenum srgbDecode = GL_DECODE_EXT;  // GL_EXT_texture_sRGB_decode
    union {
        GLfloat f[4];
        GLint i[4];
        GLuint u[4];
    } borderColor = {{0, 0, 0, 0}};
    bool borderIsInteger = false;
    bool seamlessCubeMap = true;  // per-texture in ARB_seamless_cubemap_per_texture; GL 3.2 global
    bool operator==(const SamplerParams& o) const;
};

class Sampler : public RefCounted, public Labeled {
public:
    explicit Sampler(GLuint name) : name(name) {}
    GLuint name;
    SamplerParams params;
};

// --------------------------------------------------------------- textures

struct TextureImage {
    GLenum internalFormat = 0;      // sized internal format after resolution
    GLenum requestedFormat = 0;     // as the application specified it (TEXTURE_INTERNAL_FORMAT)
    const FormatInfo* format = nullptr;
    GLsizei width = 0, height = 0, depth = 0;  // depth = layers for arrays, 6 faces are separate images
    GLsizei samples = 0;
    bool fixedSampleLocations = true;
    // Contents given while the image did not fit the texture's storage (a
    // cube face re-specified in another format before the base face): its
    // stored texels, written when the storage is rebuilt (texture_storage.mm).
    std::vector<uint8_t> pending;
    bool defined() const { return format != nullptr; }
};

class Texture : public RefCounted, public Labeled {
public:
    Texture(GLuint name, GLenum target) : name(name), target(target) {}
    GLuint name;
    GLenum target;
    static constexpr int kMaxLevels = 16;
    // images[face][level]; face is 0 except for cube maps.
    std::array<std::array<TextureImage, kMaxLevels>, 6> images{};
    SamplerParams sampler;
    // Bound to an image unit (GL 4.2): storage is (re)allocated with
    // shader write (and, for 32-bit integer formats, atomic) usage.
    bool shaderWrite = false;
    // Views in another format (GL texture views, stencil sampling, image
    // units) need MTLTextureUsagePixelFormatView, which turns off Apple
    // GPUs' lossless compression (render passes into a texture take several
    // times the bandwidth): storage gains it when such a view is first
    // needed. sRGB and swizzle views do not need it.
    bool formatViews = false;
    GLint baseLevel = 0, maxLevel = 1000;
    GLenum swizzle[4] = {GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA};
    GLenum depthStencilMode = GL_DEPTH_COMPONENT;
    // GL_DEPTH_TEXTURE_MODE (compatibility profile): how depth values are
    // returned to sampling, as GL_LUMINANCE (d, d, d, 1), GL_INTENSITY
    // (d, d, d, d), GL_ALPHA (0, 0, 0, d) or GL_RED (d, 0, 0, 1, the core profile's).
    GLenum depthTextureMode = GL_LUMINANCE;
    // GL_GENERATE_MIPMAP (compatibility profile): changes to the base level
    // regenerate the levels below it.
    bool generateMipmap = false;
    // GL_GENERATE_MIPMAP levels the GPU has yet to regenerate (api/texture.mm
    // generateMipmapsAfterChange defers them to the next command that may
    // read them), for this target.
    bool mipmapsPending = false;
    GLenum pendingMipmapTarget = 0;
    bool immutable = false;
    GLsizei immutableLevels = 0;
    // GL 4.3 texture views: GL_TEXTURE_VIEW_* (TexStorage sets the levels
    // and layers; a view's minimums are relative to its root). A view's
    // storage is a Metal view of the root texture's storage (`viewOf`, never
    // itself a view), remade when the root's storage changes (`viewSource`).
    GLuint viewMinLevel = 0, viewNumLevels = 0, viewMinLayer = 0, viewNumLayers = 0;
    RefPtr<Texture> viewOf;
    id<MTLTexture> viewSource = nil;
    // Texture buffer (GL_TEXTURE_BUFFER).
    RefPtr<Buffer> buffer;
    GLenum bufferFormat = 0;
    GLintptr bufferOffset = 0;
    GLsizeiptr bufferSize = -1;
    // GL_RGB32F/I/UI buffer textures: Metal has no RGB32 formats, so the
    // texels are expanded into an RGBA32 copy, refreshed when the buffer's
    // version changes (texture_storage.mm).
    id<MTLBuffer> rgb32Shadow = nil;
    id<MTLBuffer> rgb32ShadowSource = nil;
    uint64_t rgb32ShadowVersion = 0;
    GLintptr rgb32ShadowOffset = -1;

    // Metal storage: rebuilt when the defined images no longer match it.
    id<MTLTexture> mtl = nil;
    Storage storage = Storage::Invalid;
    GLsizei mtlLevels = 0;          // levels allocated (from level 0)
    GLsizei mtlWidth = 0, mtlHeight = 0, mtlDepth = 0, mtlSamples = 0;
    bool storageDirty = true;       // images changed shape; storage must be rebuilt
    // An image was defined without data (glTexStorage*, glTexImage* with a
    // null pointer, multisample images): the next new storage starts zeroed.
    bool zeroStorage = false;
    // Cached views keyed by (base, max, swizzle, format) for sampling.
    struct View { uint64_t key; id<MTLTexture> view; };
    std::vector<View> views;
    // Recorded for the GL 4.x binding model (state.h); nothing reads them yet.
    uint64_t lastUseSerial = 0;
    uint64_t version = 0;
    // The last command buffer whose draws sample, write or render to the
    // storage, or that uploads to it in order (kept on the texture that owns
    // the storage: root()). An upload while it is not the one being recorded
    // may run ahead of that command buffer (texture_storage.mm uploadStaged).
    uint64_t touchSerial = 0;
    Texture& root() { return viewOf ? *viewOf : *this; }
    // textureComplete's last answer about the images (storage is checked
    // every time), for the version and the parameters it depends on.
    struct CompletenessMemo {
        uint64_t version = ~0ull;
        GLint baseLevel = 0, maxLevel = 0;
        GLenum minFilter = 0, magFilter = 0, depthStencilMode = 0;
        bool complete = false;
    } completeness;

    int faceCount() const { return target == GL_TEXTURE_CUBE_MAP ? 6 : 1; }
    const TextureImage& image(int face, int level) const { return images[face][level]; }
    TextureImage& image(int face, int level) { return images[face][level]; }
    // The base level's image (an immutable texture's base level is clamped to its levels).
    const TextureImage& baseImage() const {
        int base = baseLevel < kMaxLevels ? baseLevel : 0;
        if (immutable && immutableLevels > 0 && base > immutableLevels - 1) base = immutableLevels - 1;
        return images[0][base];
    }
};

// --------------------------------------------------------- renderbuffers

class Renderbuffer : public RefCounted, public Labeled {
public:
    explicit Renderbuffer(GLuint name) : name(name) {}
    GLuint name;
    GLenum internalFormat = GL_RGBA;  // GL 3.3 table 6.31 initial value
    const FormatInfo* format = nullptr;
    GLsizei width = 0, height = 0, samples = 0;
    id<MTLTexture> mtl = nil;
    id<MTLTexture> linearView = nil;  // non-sRGB view of sRGB storage (raw writes)
    uint64_t lastUseSerial = 0;       // recorded, not read yet (state.h)
};

// ----------------------------------------------------------- framebuffers

struct Attachment {
    GLenum type = GL_NONE;            // GL_NONE, GL_TEXTURE, GL_RENDERBUFFER
    RefPtr<Texture> texture;
    RefPtr<Renderbuffer> renderbuffer;
    GLint level = 0;
    GLint layer = 0;                  // cube face for cube maps (non-layered), array layer, 3D slice
    GLenum cubeFace = 0;              // GL_TEXTURE_CUBE_MAP_POSITIVE_X.. when attached with FramebufferTexture2D
    bool layered = false;             // glFramebufferTexture on an array/3D/cube texture
    bool attached() const { return type != GL_NONE; }
};

class Framebuffer : public RefCounted, public Labeled {
public:
    explicit Framebuffer(GLuint name) : name(name) {}
    GLuint name;
    static constexpr int kMaxColor = 8;
    std::array<Attachment, kMaxColor> color;
    Attachment depth, stencil;
    std::array<GLenum, kMaxColor> drawBuffers{GL_COLOR_ATTACHMENT0, GL_NONE, GL_NONE, GL_NONE,
                                              GL_NONE, GL_NONE, GL_NONE, GL_NONE};
    GLenum readBuffer = GL_COLOR_ATTACHMENT0;
    // GL 4.3 framebuffers without attachments: GL_FRAMEBUFFER_DEFAULT_*.
    GLint defaultWidth = 0, defaultHeight = 0, defaultLayers = 0, defaultSamples = 0;
    bool defaultFixedSampleLocations = false;
    // GL_EXT_fragment_shading_rate_attachment (glFramebufferShadingRateEXT).
    // Kept for the state; the 1x1 rate, the only one, ignores the texels.
    RefPtr<Texture> shadingRateTexture;
    GLint shadingRateBaseLayer = 0;
    GLsizei shadingRateLayers = 0, shadingRateTexelWidth = 0, shadingRateTexelHeight = 0;
    bool hasAttachments() const {
        for (const Attachment& a : color)
            if (a.attached()) return true;
        return depth.attached() || stencil.attached();
    }
    uint64_t version = 0;  // bumped on any attachment/draw-buffer change (not read yet; state.h)
};

// ----------------------------------------------------------- vertex arrays

struct VertexAttrib {
    bool enabled = false;
    GLint size = 4;
    GLenum type = GL_FLOAT;
    bool normalized = false;
    bool integer = false;     // glVertexAttribIPointer
    bool isDouble = false;    // glVertexAttribLPointer
    GLsizei stride = 0;       // as specified (0 = tightly packed)
    GLintptr offset = 0;      // byte offset in buffer, or client pointer when buffer is null
    RefPtr<Buffer> buffer;
    GLuint divisor = 0;
    // GL 4.3 vertex attribute bindings (generic attributes): the binding
    // supplying buffer, offset, stride and divisor, and the format's offset
    // within an element. The fields above hold the resolved values
    // (VertexArray::resolveBinding); `pointerStride` is the stride given to
    // glVertexAttribPointer (GL_VERTEX_ATTRIB_ARRAY_STRIDE), and a binding
    // with stride 0 makes every vertex read the same element (`zeroStride`).
    GLuint binding = 0;
    GLuint relativeOffset = 0;
    GLsizei pointerStride = 0;
    GLintptr pointer = 0;  // GL_VERTEX_ATTRIB_ARRAY_POINTER: glVertexAttribPointer's value
    bool zeroStride = false;
    GLsizei effectiveStride() const;
    uint32_t elementBytes() const;
};

// A GL 4.3 vertex buffer binding (glBindVertexBuffer).
struct VertexBinding {
    RefPtr<Buffer> buffer;
    GLintptr offset = 0;
    GLsizei stride = 16;
    GLuint divisor = 0;
};

class VertexArray : public RefCounted, public Labeled {
public:
    explicit VertexArray(GLuint name) : name(name) {
        for (int i = 0; i < kMaxAttribs; ++i) attribs[static_cast<size_t>(i)].binding = static_cast<GLuint>(i);
    }
    GLuint name;
    static constexpr int kMaxAttribs = 16;   // generic attributes (GL_MAX_VERTEX_ATTRIBS)
    // Generic attributes, then the compatibility profile's conventional arrays
    // (vertex, normal, colours, fog coordinate, texture coordinates; see
    // compat/fixed_state.h), which the lowered legacy built-ins read.
    static constexpr int kAttribSlots = 31;  // + 2 internal (compat/fixed_state.h InternalAttrib)
    std::array<VertexAttrib, kAttribSlots> attribs;
    static constexpr int kMaxBindings = 16;  // GL_MAX_VERTEX_ATTRIB_BINDINGS
    std::array<VertexBinding, kMaxBindings> bindings;
    // Copies binding `index`'s state into the attributes that use it.
    void resolveBinding(GLuint index);
    RefPtr<Buffer> elementBuffer;
    uint64_t version = 0;  // not read yet (state.h)
};

// ------------------------------------------------------- shaders/programs

using Stage = shader::Stage;
Stage stageFromGL(GLenum type);
GLenum stageToGL(Stage stage);

class Shader : public RefCounted, public Labeled {
public:
    Shader(GLuint name, GLenum type) : name(name), type(type), stage(stageFromGL(type)) {}
    GLuint name;
    GLenum type;
    Stage stage;
    std::string source;
    bool compiled = false;   // glCompileShader has been called since the last source change
    bool compileStatus = false;
    std::string infoLog;
    // OpenGL ES contexts: the GLSL ES source as desktop GLSL, which programs
    // link (shader::compileEs).
    std::string desktopSource;
    // GL_ARB_gl_spirv: the module glShaderBinary gave (SPIR_V_BINARY) and the
    // GLSL glSpecializeShader made of it (shader/spirv_input.h).
    bool spirv = false, specialized = false, spirvNamed = false;
    std::vector<uint32_t> spirvWords;
    std::string spirvGlsl;
    std::vector<std::pair<uint32_t, std::string>> spirvUniforms;  // (location, name in spirvGlsl)
    bool deletePending = false;
    int attachCount = 0;
};

class Program : public RefCounted, public Labeled {
public:
    explicit Program(GLuint name) : name(name) {}
    GLuint name;
    std::vector<RefPtr<Shader>> attached;
    // Link inputs that persist across links.
    std::map<std::string, GLuint> attribBindings;                      // glBindAttribLocation
    std::map<std::string, std::pair<GLuint, GLuint>> fragDataBindings;  // name -> (location, index)
    std::vector<std::string> transformFeedbackVaryings;
    GLenum transformFeedbackMode = GL_INTERLEAVED_ATTRIBS;
    bool separable = false;
    bool binaryRetrievableHint = false;
    // Link results. `linked` is the executable in use; a failed relink keeps
    // the previous executable usable until the next successful link.
    bool linkStatus = false;
    bool linkAttempted = false;
    std::string infoLog;
    bool validateStatus = false;
    std::shared_ptr<LinkedProgram> linked;
    // Default-uniform-block values (std140 layout from the linked program).
    std::vector<uint8_t> uniformData;
    std::vector<GLuint> uniformBlockBindings;   // per block index
    std::vector<GLuint> storageBlockBindings;   // per storage block index (GL 4.3)
    std::vector<GLint> samplerUnits;            // per sampler slot
    std::vector<GLint> imageUnits;              // per image slot (GL 4.2)
    uint64_t uniformVersion = 0;
    bool deletePending = false;
    int useCount = 0;  // contexts with this program current
    std::atomic<int> stageUses{0};  // program pipeline stages using it (pipelines die outside the share lock)
};

// GL_ARB_separate_shader_objects program pipeline (per context).
class ProgramPipeline : public RefCounted, public Labeled {
public:
    explicit ProgramPipeline(GLuint name) : name(name) {}
    ~ProgramPipeline() {
        for (RefPtr<Program>& stage : stages)
            if (stage) --stage->stageUses;
    }
    GLuint name;
    RefPtr<Program> stages[6];   // indexed by shader::Stage (set through setStage)
    void setStage(int stage, RefPtr<Program> program) {
        if (stages[stage]) --stages[stage]->stageUses;
        if (program) ++program->stageUses;
        stages[stage] = std::move(program);
    }
    RefPtr<Program> activeProgram;
    bool validateStatus = false;
    std::string infoLog;
    // The executable built from the stage programs (DrawState::executable).
    std::shared_ptr<LinkedProgram> executable;
    std::shared_ptr<LinkedProgram> builtFrom[6];  // the stage programs' executables it was built from
    // Executables built for earlier stage combinations, most recent first:
    // switching back to one (glUseProgramStages per draw) reuses it, and with
    // it the pipeline states keyed by its uid (program.mm pipelineExecutable).
    struct BuiltExecutable {
        std::shared_ptr<LinkedProgram> from[6];
        const Program* owner[6];
        std::shared_ptr<LinkedProgram> executable;
    };
    static constexpr size_t kMaxBuiltExecutables = 32;
    std::vector<BuiltExecutable> built;
};

// ------------------------------------------------------- queries and syncs

class Query : public RefCounted, public Labeled {
public:
    explicit Query(GLuint name) : name(name) {}
    GLuint name;
    GLenum target = 0;
    bool active = false;
    bool everActive = false;
    uint64_t result = 0;
    uint64_t pendingSerial = 0;   // result is final once this serial completes
    bool resultFinal = true;      // result holds the final value
    // Visibility-result slots used by occlusion queries, or the 32-bit
    // counters of FRAGMENT_SHADER_INVOCATIONS (one per command buffer); summed
    // on completion.
    std::vector<std::pair<id<MTLBuffer>, uint32_t>> visibilitySlots;
    uint64_t counterSerial = 0;  // the command buffer of the last counter slot
    uint64_t counterBound = 0;   // invocations the last counter slot's draws can have counted
    // GPU times (seconds, host time base) for TIME_ELAPSED and TIMESTAMP,
    // written by command-buffer completion handlers.
    struct Timer {
        std::atomic<double> start{0}, end{0};
        std::atomic<bool> startSet{false}, endSet{false};
    };
    std::shared_ptr<Timer> timer;
    // TIME_ELAPSED timed by GPU timestamps of the encoders between begin
    // and end (CommandStream::beginTiming), without commits.
    bool gpuTimed = false;
    CommandStream::TimingMark timedFirst, timedLast;
};

class Sync : public RefCounted, public Labeled {
public:
    Sync() = default;
    uint64_t serial = 0;          // signalled when this command-buffer serial completes
    void* shareGroup = nullptr;
};

class TransformFeedback : public RefCounted, public Labeled {
public:
    explicit TransformFeedback(GLuint name) : name(name) {}
    GLuint name;
    static constexpr int kMaxBuffers = 4;
    struct Binding {
        RefPtr<Buffer> buffer;
        GLintptr offset = 0;
        GLsizeiptr size = 0;   // 0 = whole buffer (BindBufferBase)
    };
    std::array<Binding, kMaxBuffers> buffers;
    bool active = false, paused = false;
    GLenum primitiveMode = 0;
    RefPtr<Program> program;
    std::array<GLsizeiptr, kMaxBuffers> written{};   // bytes written per buffer since Begin
    std::array<uint64_t, 4> primitivesWritten{};     // per vertex stream, since Begin
    // GL 4.0 (GL_ARB_transform_feedback2/3): whether EndTransformFeedback
    // has run for this object, and the vertices each vertex stream captured
    // the last time (glDrawTransformFeedback*).
    bool everEnded = false;
    std::array<uint64_t, 4> streamVertices{};
};

}  // namespace wmgl
