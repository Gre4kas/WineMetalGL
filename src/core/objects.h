// GL objects. Shared objects (buffers, textures, renderbuffers, samplers,
// shaders, programs, syncs) live in a ShareGroup; container objects (vertex
// arrays, framebuffers, queries, transform feedback) belong to one context.
#pragma once

#import <Metal/Metal.h>

#include <array>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "core/format/formats.h"
#include "core/shader/compiler.h"
#include "core/gl/types.h"
#include "core/util/ref.h"

namespace wmgl {

class Program;
struct LinkedProgram;

// ---------------------------------------------------------------- buffers

class Buffer : public RefCounted {
public:
    explicit Buffer(GLuint name) : name(name) {}
    GLuint name;
    id<MTLBuffer> mtl = nil;     // nil while size == 0
    GLsizeiptr size = 0;
    GLenum usage = GL_STATIC_DRAW;
    bool immutable = false;
    GLbitfield storageFlags = 0;
    // Mapping state (glMapBuffer*).
    bool mapped = false;
    GLbitfield mapAccess = 0;
    GLintptr mapOffset = 0;
    GLsizeiptr mapLength = 0;
    void* mapPointer = nullptr;
    // GPU serial of the last command buffer that read or wrote this buffer.
    uint64_t lastUseSerial = 0;
    // Incremented whenever the contents change (keys derived caches).
    uint64_t version = 0;
    std::string label;

    uint8_t* contents() const { return mtl ? static_cast<uint8_t*>(mtl.contents) : nullptr; }
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
    union {
        GLfloat f[4];
        GLint i[4];
        GLuint u[4];
    } borderColor = {{0, 0, 0, 0}};
    bool borderIsInteger = false;
    bool seamlessCubeMap = true;  // per-texture in ARB_seamless_cubemap_per_texture; GL 3.2 global
    bool operator==(const SamplerParams& o) const;
};

class Sampler : public RefCounted {
public:
    explicit Sampler(GLuint name) : name(name) {}
    GLuint name;
    SamplerParams params;
    std::string label;
};

// --------------------------------------------------------------- textures

struct TextureImage {
    GLenum internalFormat = 0;      // as requested (sized after resolution)
    const FormatInfo* format = nullptr;
    GLsizei width = 0, height = 0, depth = 0;  // depth = layers for arrays, 6 faces are separate images
    GLsizei samples = 0;
    bool fixedSampleLocations = true;
    bool defined() const { return format != nullptr; }
};

class Texture : public RefCounted {
public:
    Texture(GLuint name, GLenum target) : name(name), target(target) {}
    GLuint name;
    GLenum target;
    static constexpr int kMaxLevels = 16;
    // images[face][level]; face is 0 except for cube maps.
    std::array<std::array<TextureImage, kMaxLevels>, 6> images{};
    SamplerParams sampler;
    GLint baseLevel = 0, maxLevel = 1000;
    GLenum swizzle[4] = {GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA};
    GLenum depthStencilMode = GL_DEPTH_COMPONENT;
    bool immutable = false;
    GLsizei immutableLevels = 0;
    // Texture buffer (GL_TEXTURE_BUFFER).
    RefPtr<Buffer> buffer;
    GLenum bufferFormat = 0;
    GLintptr bufferOffset = 0;
    GLsizeiptr bufferSize = -1;

    // Metal storage: rebuilt when the defined images no longer match it.
    id<MTLTexture> mtl = nil;
    Storage storage = Storage::Invalid;
    GLsizei mtlLevels = 0;          // levels allocated (from level 0)
    GLsizei mtlWidth = 0, mtlHeight = 0, mtlDepth = 0, mtlSamples = 0;
    bool storageDirty = true;       // images changed shape; storage must be rebuilt
    // Cached views keyed by (base, max, swizzle, format) for sampling.
    struct View { uint64_t key; id<MTLTexture> view; };
    std::vector<View> views;
    uint64_t lastUseSerial = 0;
    uint64_t version = 0;
    std::string label;

    int faceCount() const { return target == GL_TEXTURE_CUBE_MAP ? 6 : 1; }
    const TextureImage& image(int face, int level) const { return images[face][level]; }
    TextureImage& image(int face, int level) { return images[face][level]; }
    const TextureImage& baseImage() const { return images[0][baseLevel < kMaxLevels ? baseLevel : 0]; }
};

// --------------------------------------------------------- renderbuffers

class Renderbuffer : public RefCounted {
public:
    explicit Renderbuffer(GLuint name) : name(name) {}
    GLuint name;
    GLenum internalFormat = GL_RGBA4;
    const FormatInfo* format = nullptr;
    GLsizei width = 0, height = 0, samples = 0;
    id<MTLTexture> mtl = nil;
    uint64_t lastUseSerial = 0;
    std::string label;
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

class Framebuffer : public RefCounted {
public:
    explicit Framebuffer(GLuint name) : name(name) {}
    GLuint name;
    static constexpr int kMaxColor = 8;
    std::array<Attachment, kMaxColor> color;
    Attachment depth, stencil;
    std::array<GLenum, kMaxColor> drawBuffers{GL_COLOR_ATTACHMENT0, GL_NONE, GL_NONE, GL_NONE,
                                              GL_NONE, GL_NONE, GL_NONE, GL_NONE};
    GLenum readBuffer = GL_COLOR_ATTACHMENT0;
    uint64_t version = 0;  // bumped on any attachment/draw-buffer change
    std::string label;
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
    GLsizei effectiveStride() const;
    uint32_t elementBytes() const;
};

class VertexArray : public RefCounted {
public:
    explicit VertexArray(GLuint name) : name(name) {}
    GLuint name;
    static constexpr int kMaxAttribs = 16;
    std::array<VertexAttrib, kMaxAttribs> attribs;
    RefPtr<Buffer> elementBuffer;
    uint64_t version = 0;
    std::string label;
};

// ------------------------------------------------------- shaders/programs

using Stage = shader::Stage;
Stage stageFromGL(GLenum type);
GLenum stageToGL(Stage stage);

class Shader : public RefCounted {
public:
    Shader(GLuint name, GLenum type) : name(name), type(type), stage(stageFromGL(type)) {}
    GLuint name;
    GLenum type;
    Stage stage;
    std::string source;
    bool compiled = false;   // glCompileShader has been called since the last source change
    bool compileStatus = false;
    std::string infoLog;
    bool deletePending = false;
    int attachCount = 0;
    std::string label;
};

class Program : public RefCounted {
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
    std::vector<GLint> samplerUnits;            // per sampler slot
    uint64_t uniformVersion = 0;
    bool deletePending = false;
    int useCount = 0;  // contexts with this program current
    std::string label;
};

// ------------------------------------------------------- queries and syncs

class Query : public RefCounted {
public:
    explicit Query(GLuint name) : name(name) {}
    GLuint name;
    GLenum target = 0;
    bool active = false;
    bool everActive = false;
    uint64_t result = 0;
    uint64_t pendingSerial = 0;   // result is final once this serial completes
    // Visibility-result slots used by occlusion queries (summed on completion).
    std::vector<std::pair<id<MTLBuffer>, uint32_t>> visibilitySlots;
    std::string label;
};

class Sync : public RefCounted {
public:
    Sync() = default;
    uint64_t serial = 0;          // signalled when this command-buffer serial completes
    void* shareGroup = nullptr;
    std::string label;
};

class TransformFeedback : public RefCounted {
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
    uint64_t primitivesWritten = 0;
    std::string label;
};

}  // namespace wmgl
