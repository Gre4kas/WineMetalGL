// Direct state access (GL 4.5, GL_ARB_direct_state_access): glCreate* and
// the commands that name their object instead of using a binding.
//
// Each named command runs the bind-to-edit implementation with the object
// swapped into a binding for the duration of the call (BindingSwap), so the
// two share their validation and behaviour; the binding is restored before
// returning and no binding visible to the application changes. A name that
// is not an existing object of the kind is INVALID_OPERATION.
#include <cstring>

#include "core/context.h"
#include "core/query.h"
#include "core/draw.h"
#include "core/format/formats.h"
#include "core/format/pixel_transfer.h"
#include "core/limits.h"
#include "gl_api.gen.h"

namespace wmgl::api {
namespace {

// Puts `value` in `slot` until the end of the scope.
template <class T>
class BindingSwap {
public:
    BindingSwap(T& slot, T value) : slot_(slot), saved_(slot) { slot_ = std::move(value); }
    ~BindingSwap() { slot_ = std::move(saved_); }
    BindingSwap(const BindingSwap&) = delete;
    BindingSwap& operator=(const BindingSwap&) = delete;

private:
    T& slot_;
    T saved_;
};

bool dsa(Context& ctx, const char* command) {
    if (ctx.version() >= 45 || ctx.hasExtension("GL_ARB_direct_state_access")) return true;
    ctx.unimplemented(command);
    return false;
}

RefPtr<Buffer> namedBuffer(Context& ctx, GLuint name) {
    if (!name) return nullptr;
    ShareLock lock(ctx);
    return ctx.share().buffers.getRef(name);
}

RefPtr<Texture> namedTexture(Context& ctx, GLuint name) {
    if (!name) return nullptr;
    ShareLock lock(ctx);
    return ctx.share().textures.getRef(name);
}

RefPtr<Renderbuffer> namedRenderbuffer(Context& ctx, GLuint name) {
    if (!name) return nullptr;
    ShareLock lock(ctx);
    return ctx.share().renderbuffers.getRef(name);
}

// A texture's binding on the active unit, swapped for the call.
struct TextureScope {
    TextureScope(Context& ctx, const RefPtr<Texture>& tex)
        : swap(ctx.state.textureUnits[ctx.state.activeTexture].bound[static_cast<size_t>(textureTargetIndex(tex->target))], tex) {}
    BindingSwap<RefPtr<Texture>> swap;
};

}  // namespace

// ------------------------------------------------------------ creation

void CreateBuffers(Context& ctx, GLsizei n, GLuint* buffers) {
    if (!dsa(ctx, "glCreateBuffers")) return;
    if (n < 0) return ctx.setError(GL_INVALID_VALUE);
    if (!buffers) return;
    ShareLock lock(ctx);
    ctx.share().buffers.generate(n, buffers);
    for (GLsizei i = 0; i < n; ++i) ctx.share().buffers.set(buffers[i], makeRef<Buffer>(buffers[i]));
}

void CreateTextures(Context& ctx, GLenum target, GLsizei n, GLuint* textures) {
    if (!dsa(ctx, "glCreateTextures")) return;
    if (textureTargetIndex(target) < 0) return ctx.setError(GL_INVALID_ENUM);
    if (n < 0) return ctx.setError(GL_INVALID_VALUE);
    if (!textures) return;
    ShareLock lock(ctx);
    ctx.share().textures.generate(n, textures);
    for (GLsizei i = 0; i < n; ++i) {
        RefPtr<Texture> tex = makeRef<Texture>(textures[i], target);
        // Rectangle textures default to linear filtering without mipmaps and edge clamping.
        if (target == GL_TEXTURE_RECTANGLE) {
            tex->sampler.minFilter = GL_LINEAR;
            tex->sampler.wrapS = tex->sampler.wrapT = tex->sampler.wrapR = GL_CLAMP_TO_EDGE;
        }
        ctx.share().textures.set(textures[i], tex);
    }
}

void CreateFramebuffers(Context& ctx, GLsizei n, GLuint* framebuffers) {
    if (!dsa(ctx, "glCreateFramebuffers")) return;
    if (n < 0) return ctx.setError(GL_INVALID_VALUE);
    if (!framebuffers) return;
    ShareLock lock(ctx);
    ctx.share().framebuffers.generate(n, framebuffers);
    for (GLsizei i = 0; i < n; ++i) ctx.share().framebuffers.set(framebuffers[i], makeRef<Framebuffer>(framebuffers[i]));
}

void CreateRenderbuffers(Context& ctx, GLsizei n, GLuint* renderbuffers) {
    if (!dsa(ctx, "glCreateRenderbuffers")) return;
    if (n < 0) return ctx.setError(GL_INVALID_VALUE);
    if (!renderbuffers) return;
    ShareLock lock(ctx);
    ctx.share().renderbuffers.generate(n, renderbuffers);
    for (GLsizei i = 0; i < n; ++i) ctx.share().renderbuffers.set(renderbuffers[i], makeRef<Renderbuffer>(renderbuffers[i]));
}

void CreateVertexArrays(Context& ctx, GLsizei n, GLuint* arrays) {
    if (!dsa(ctx, "glCreateVertexArrays")) return;
    if (n < 0) return ctx.setError(GL_INVALID_VALUE);
    if (!arrays) return;
    ctx.vertexArrays.generate(n, arrays);
    for (GLsizei i = 0; i < n; ++i) ctx.vertexArrays.set(arrays[i], makeRef<VertexArray>(arrays[i]));
}

void CreateSamplers(Context& ctx, GLsizei n, GLuint* samplers) {
    if (!dsa(ctx, "glCreateSamplers")) return;
    GenSamplers(ctx, n, samplers);  // sampler objects exist from glGenSamplers
}

void CreateProgramPipelines(Context& ctx, GLsizei n, GLuint* pipelines) {
    if (!dsa(ctx, "glCreateProgramPipelines")) return;
    if (n < 0) return ctx.setError(GL_INVALID_VALUE);
    if (!pipelines) return;
    ctx.programPipelines.generate(n, pipelines);
    for (GLsizei i = 0; i < n; ++i) ctx.programPipelines.set(pipelines[i], makeRef<ProgramPipeline>(pipelines[i]));
}

void CreateTransformFeedbacks(Context& ctx, GLsizei n, GLuint* ids) {
    if (!dsa(ctx, "glCreateTransformFeedbacks")) return;
    if (n < 0) return ctx.setError(GL_INVALID_VALUE);
    if (!ids) return;
    ctx.transformFeedbacks.generate(n, ids);
    for (GLsizei i = 0; i < n; ++i) ctx.transformFeedbacks.set(ids[i], makeRef<TransformFeedback>(ids[i]));
}

void CreateQueries(Context& ctx, GLenum target, GLsizei n, GLuint* ids) {
    if (!dsa(ctx, "glCreateQueries")) return;
    switch (target) {
    case GL_SAMPLES_PASSED: case GL_ANY_SAMPLES_PASSED: case GL_ANY_SAMPLES_PASSED_CONSERVATIVE: case GL_TIME_ELAPSED:
    case GL_TIMESTAMP: case GL_PRIMITIVES_GENERATED: case GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN:
        break;
    case GL_TRANSFORM_FEEDBACK_OVERFLOW: case GL_TRANSFORM_FEEDBACK_STREAM_OVERFLOW: case GL_VERTICES_SUBMITTED:
    case GL_PRIMITIVES_SUBMITTED: case GL_VERTEX_SHADER_INVOCATIONS: case GL_TESS_CONTROL_SHADER_PATCHES:
    case GL_TESS_EVALUATION_SHADER_INVOCATIONS: case GL_GEOMETRY_SHADER_INVOCATIONS:
    case GL_GEOMETRY_SHADER_PRIMITIVES_EMITTED: case GL_FRAGMENT_SHADER_INVOCATIONS: case GL_COMPUTE_SHADER_INVOCATIONS:
    case GL_CLIPPING_INPUT_PRIMITIVES: case GL_CLIPPING_OUTPUT_PRIMITIVES:
        if (queryTargetIndex(ctx, target) < 0) return ctx.setError(GL_INVALID_ENUM);
        break;
    default: return ctx.setError(GL_INVALID_ENUM);
    }
    if (n < 0) return ctx.setError(GL_INVALID_VALUE);
    if (!ids) return;
    ctx.queries.generate(n, ids);
    for (GLsizei i = 0; i < n; ++i) {
        RefPtr<Query> q = makeRef<Query>(ids[i]);
        q->target = target;  // a created query only begins with its target
        ctx.queries.set(ids[i], q);
    }
}

// ------------------------------------------------------------ buffers

#define WMGL_NAMED_BUFFER(command, name)                                         \
    if (!dsa(ctx, command)) return;                                              \
    RefPtr<Buffer> buffer_ = namedBuffer(ctx, name);                              \
    if (!buffer_) return ctx.setError(GL_INVALID_OPERATION);                     \
    BindingSwap<RefPtr<Buffer>> bound_(ctx.state.copyWriteBuffer, buffer_)

void NamedBufferStorage(Context& ctx, GLuint buffer, GLsizeiptr size, const void* data, GLbitfield flags) {
    WMGL_NAMED_BUFFER("glNamedBufferStorage", buffer);
    BufferStorage(ctx, GL_COPY_WRITE_BUFFER, size, data, flags);
}

void NamedBufferData(Context& ctx, GLuint buffer, GLsizeiptr size, const void* data, GLenum usage) {
    WMGL_NAMED_BUFFER("glNamedBufferData", buffer);
    BufferData(ctx, GL_COPY_WRITE_BUFFER, size, data, usage);
}

void NamedBufferSubData(Context& ctx, GLuint buffer, GLintptr offset, GLsizeiptr size, const void* data) {
    WMGL_NAMED_BUFFER("glNamedBufferSubData", buffer);
    BufferSubData(ctx, GL_COPY_WRITE_BUFFER, offset, size, data);
}

void CopyNamedBufferSubData(Context& ctx, GLuint readBuffer, GLuint writeBuffer, GLintptr readOffset, GLintptr writeOffset,
                            GLsizeiptr size) {
    if (!dsa(ctx, "glCopyNamedBufferSubData")) return;
    RefPtr<Buffer> read = namedBuffer(ctx, readBuffer), write = namedBuffer(ctx, writeBuffer);
    if (!read || !write) return ctx.setError(GL_INVALID_OPERATION);
    BindingSwap<RefPtr<Buffer>> r(ctx.state.copyReadBuffer, read), w(ctx.state.copyWriteBuffer, write);
    CopyBufferSubData(ctx, GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, readOffset, writeOffset, size);
}

void ClearNamedBufferData(Context& ctx, GLuint buffer, GLenum internalformat, GLenum format, GLenum type, const void* data) {
    WMGL_NAMED_BUFFER("glClearNamedBufferData", buffer);
    ClearBufferData(ctx, GL_COPY_WRITE_BUFFER, internalformat, format, type, data);
}

void ClearNamedBufferSubData(Context& ctx, GLuint buffer, GLenum internalformat, GLintptr offset, GLsizeiptr size, GLenum format,
                             GLenum type, const void* data) {
    WMGL_NAMED_BUFFER("glClearNamedBufferSubData", buffer);
    ClearBufferSubData(ctx, GL_COPY_WRITE_BUFFER, internalformat, offset, size, format, type, data);
}

void* MapNamedBuffer(Context& ctx, GLuint buffer, GLenum access) {
    if (!dsa(ctx, "glMapNamedBuffer")) return nullptr;
    RefPtr<Buffer> object = namedBuffer(ctx, buffer);
    if (!object) return ctx.setError(GL_INVALID_OPERATION), nullptr;
    BindingSwap<RefPtr<Buffer>> bound(ctx.state.copyWriteBuffer, object);
    return MapBuffer(ctx, GL_COPY_WRITE_BUFFER, access);
}

void* MapNamedBufferRange(Context& ctx, GLuint buffer, GLintptr offset, GLsizeiptr length, GLbitfield access) {
    if (!dsa(ctx, "glMapNamedBufferRange")) return nullptr;
    RefPtr<Buffer> object = namedBuffer(ctx, buffer);
    if (!object) return ctx.setError(GL_INVALID_OPERATION), nullptr;
    if (length == 0) return ctx.setError(GL_INVALID_OPERATION), nullptr;  // GL 4.5 section 6.3
    BindingSwap<RefPtr<Buffer>> bound(ctx.state.copyWriteBuffer, object);
    return MapBufferRange(ctx, GL_COPY_WRITE_BUFFER, offset, length, access);
}

GLboolean UnmapNamedBuffer(Context& ctx, GLuint buffer) {
    if (!dsa(ctx, "glUnmapNamedBuffer")) return GL_FALSE;
    RefPtr<Buffer> object = namedBuffer(ctx, buffer);
    if (!object) return ctx.setError(GL_INVALID_OPERATION), GL_FALSE;
    BindingSwap<RefPtr<Buffer>> bound(ctx.state.copyWriteBuffer, object);
    return UnmapBuffer(ctx, GL_COPY_WRITE_BUFFER);
}

void FlushMappedNamedBufferRange(Context& ctx, GLuint buffer, GLintptr offset, GLsizeiptr length) {
    WMGL_NAMED_BUFFER("glFlushMappedNamedBufferRange", buffer);
    FlushMappedBufferRange(ctx, GL_COPY_WRITE_BUFFER, offset, length);
}

void GetNamedBufferParameteriv(Context& ctx, GLuint buffer, GLenum pname, GLint* params) {
    WMGL_NAMED_BUFFER("glGetNamedBufferParameteriv", buffer);
    GetBufferParameteriv(ctx, GL_COPY_WRITE_BUFFER, pname, params);
}

void GetNamedBufferParameteri64v(Context& ctx, GLuint buffer, GLenum pname, GLint64* params) {
    WMGL_NAMED_BUFFER("glGetNamedBufferParameteri64v", buffer);
    GetBufferParameteri64v(ctx, GL_COPY_WRITE_BUFFER, pname, params);
}

void GetNamedBufferPointerv(Context& ctx, GLuint buffer, GLenum pname, void** params) {
    WMGL_NAMED_BUFFER("glGetNamedBufferPointerv", buffer);
    GetBufferPointerv(ctx, GL_COPY_WRITE_BUFFER, pname, params);
}

void GetNamedBufferSubData(Context& ctx, GLuint buffer, GLintptr offset, GLsizeiptr size, void* data) {
    WMGL_NAMED_BUFFER("glGetNamedBufferSubData", buffer);
    GetBufferSubData(ctx, GL_COPY_WRITE_BUFFER, offset, size, data);
}

#undef WMGL_NAMED_BUFFER

// ------------------------------------------------------------ textures

namespace {

bool oneOf(GLenum target, std::initializer_list<GLenum> targets) {
    for (GLenum t : targets)
        if (t == target) return true;
    return false;
}

// The texture for a named texture command whose target must be one of
// `targets`: INVALID_OPERATION for a missing texture or another target.
RefPtr<Texture> textureFor(Context& ctx, const char* command, GLuint name, std::initializer_list<GLenum> targets) {
    if (!dsa(ctx, command)) return nullptr;
    RefPtr<Texture> tex = namedTexture(ctx, name);
    if (!tex || (targets.size() && !oneOf(tex->target, targets))) {
        ctx.setError(GL_INVALID_OPERATION);
        return nullptr;
    }
    return tex;
}

// Bytes between consecutive images of client pixels (TextureSubImage3D of a
// cube map: one face each).
size_t imageStride(Context& ctx, GLenum format, GLenum type, GLsizei w, GLsizei h) {
    PixelStoreParams ps = pixelStoreParams(ctx.state.unpack, true);
    return transferLayout(format, type, w, h, 2, ps).imageStride;
}

}  // namespace

void BindTextureUnit(Context& ctx, GLuint unit, GLuint texture) {
    if (!dsa(ctx, "glBindTextureUnit")) return;
    if (unit >= static_cast<GLuint>(limits::kMaxCombinedTextureUnits)) return ctx.setError(GL_INVALID_OPERATION);
    TextureUnit& u = ctx.state.textureUnits[unit];
    if (!texture) {
        for (size_t t = 0; t < u.bound.size(); ++t) u.bound[t] = ctx.defaultTextures[t];
    } else {
        RefPtr<Texture> tex = namedTexture(ctx, texture);
        if (!tex) return ctx.setError(GL_INVALID_OPERATION);
        u.bound[static_cast<size_t>(textureTargetIndex(tex->target))] = tex;
    }
    ctx.markDirty(kDirtyTextures);
}

void TextureStorage1D(Context& ctx, GLuint texture, GLsizei levels, GLenum internalformat, GLsizei width) {
    RefPtr<Texture> tex = textureFor(ctx, "glTextureStorage1D", texture, {GL_TEXTURE_1D});
    if (!tex) return;
    TextureScope scope(ctx, tex);
    TexStorage1D(ctx, tex->target, levels, internalformat, width);
}

void TextureStorage2D(Context& ctx, GLuint texture, GLsizei levels, GLenum internalformat, GLsizei width, GLsizei height) {
    RefPtr<Texture> tex = textureFor(ctx, "glTextureStorage2D", texture,
                                     {GL_TEXTURE_2D, GL_TEXTURE_1D_ARRAY, GL_TEXTURE_RECTANGLE, GL_TEXTURE_CUBE_MAP});
    if (!tex) return;
    TextureScope scope(ctx, tex);
    TexStorage2D(ctx, tex->target, levels, internalformat, width, height);
}

void TextureStorage3D(Context& ctx, GLuint texture, GLsizei levels, GLenum internalformat, GLsizei width, GLsizei height,
                      GLsizei depth) {
    RefPtr<Texture> tex =
        textureFor(ctx, "glTextureStorage3D", texture, {GL_TEXTURE_3D, GL_TEXTURE_2D_ARRAY, GL_TEXTURE_CUBE_MAP_ARRAY});
    if (!tex) return;
    TextureScope scope(ctx, tex);
    TexStorage3D(ctx, tex->target, levels, internalformat, width, height, depth);
}

void TextureStorage2DMultisample(Context& ctx, GLuint texture, GLsizei samples, GLenum internalformat, GLsizei width,
                                 GLsizei height, GLboolean fixedsamplelocations) {
    RefPtr<Texture> tex = textureFor(ctx, "glTextureStorage2DMultisample", texture, {GL_TEXTURE_2D_MULTISAMPLE});
    if (!tex) return;
    TextureScope scope(ctx, tex);
    TexStorage2DMultisample(ctx, tex->target, samples, internalformat, width, height, fixedsamplelocations);
}

void TextureStorage3DMultisample(Context& ctx, GLuint texture, GLsizei samples, GLenum internalformat, GLsizei width,
                                 GLsizei height, GLsizei depth, GLboolean fixedsamplelocations) {
    RefPtr<Texture> tex = textureFor(ctx, "glTextureStorage3DMultisample", texture, {GL_TEXTURE_2D_MULTISAMPLE_ARRAY});
    if (!tex) return;
    TextureScope scope(ctx, tex);
    TexStorage3DMultisample(ctx, tex->target, samples, internalformat, width, height, depth, fixedsamplelocations);
}

void TextureSubImage1D(Context& ctx, GLuint texture, GLint level, GLint xoffset, GLsizei width, GLenum format, GLenum type,
                       const void* pixels) {
    RefPtr<Texture> tex = textureFor(ctx, "glTextureSubImage1D", texture, {GL_TEXTURE_1D});
    if (!tex) return;
    TextureScope scope(ctx, tex);
    TexSubImage1D(ctx, tex->target, level, xoffset, width, format, type, pixels);
}

void TextureSubImage2D(Context& ctx, GLuint texture, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height,
                       GLenum format, GLenum type, const void* pixels) {
    RefPtr<Texture> tex =
        textureFor(ctx, "glTextureSubImage2D", texture, {GL_TEXTURE_2D, GL_TEXTURE_1D_ARRAY, GL_TEXTURE_RECTANGLE});
    if (!tex) return;
    TextureScope scope(ctx, tex);
    TexSubImage2D(ctx, tex->target, level, xoffset, yoffset, width, height, format, type, pixels);
}

void TextureSubImage3D(Context& ctx, GLuint texture, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width,
                       GLsizei height, GLsizei depth, GLenum format, GLenum type, const void* pixels) {
    RefPtr<Texture> tex = textureFor(ctx, "glTextureSubImage3D", texture,
                                     {GL_TEXTURE_3D, GL_TEXTURE_2D_ARRAY, GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_CUBE_MAP});
    if (!tex) return;
    TextureScope scope(ctx, tex);
    if (tex->target != GL_TEXTURE_CUBE_MAP)
        return TexSubImage3D(ctx, tex->target, level, xoffset, yoffset, zoffset, width, height, depth, format, type, pixels);
    // A cube map's faces are its layers: each a two-dimensional image (rows
    // and pixels skipped) at its place in the three-dimensional one.
    if (zoffset < 0 || depth < 0 || zoffset > 6 || depth > 6 - zoffset) return ctx.setError(GL_INVALID_VALUE);
    const size_t stride = imageStride(ctx, format, type, width, height);
    const size_t first = static_cast<size_t>(std::max(0, ctx.state.unpack.skipImages)) * stride;
    for (GLsizei i = 0; i < depth; ++i)
        TexSubImage2D(ctx, GL_TEXTURE_CUBE_MAP_POSITIVE_X + static_cast<GLenum>(zoffset + i), level, xoffset, yoffset, width,
                      height, format, type, static_cast<const uint8_t*>(pixels) + first + i * stride);
}

void CompressedTextureSubImage1D(Context& ctx, GLuint texture, GLint level, GLint xoffset, GLsizei width, GLenum format,
                                 GLsizei imageSize, const void* data) {
    RefPtr<Texture> tex = textureFor(ctx, "glCompressedTextureSubImage1D", texture, {GL_TEXTURE_1D});
    if (!tex) return;
    TextureScope scope(ctx, tex);
    CompressedTexSubImage1D(ctx, tex->target, level, xoffset, width, format, imageSize, data);
}

void CompressedTextureSubImage2D(Context& ctx, GLuint texture, GLint level, GLint xoffset, GLint yoffset, GLsizei width,
                                 GLsizei height, GLenum format, GLsizei imageSize, const void* data) {
    RefPtr<Texture> tex =
        textureFor(ctx, "glCompressedTextureSubImage2D", texture, {GL_TEXTURE_2D, GL_TEXTURE_1D_ARRAY, GL_TEXTURE_RECTANGLE});
    if (!tex) return;
    TextureScope scope(ctx, tex);
    CompressedTexSubImage2D(ctx, tex->target, level, xoffset, yoffset, width, height, format, imageSize, data);
}

void CompressedTextureSubImage3D(Context& ctx, GLuint texture, GLint level, GLint xoffset, GLint yoffset, GLint zoffset,
                                 GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLsizei imageSize, const void* data) {
    RefPtr<Texture> tex = textureFor(ctx, "glCompressedTextureSubImage3D", texture,
                                     {GL_TEXTURE_3D, GL_TEXTURE_2D_ARRAY, GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_CUBE_MAP});
    if (!tex) return;
    TextureScope scope(ctx, tex);
    if (tex->target != GL_TEXTURE_CUBE_MAP)
        return CompressedTexSubImage3D(ctx, tex->target, level, xoffset, yoffset, zoffset, width, height, depth, format,
                                       imageSize, data);
    if (zoffset < 0 || depth < 0 || zoffset > 6 || depth > 6 - zoffset) return ctx.setError(GL_INVALID_VALUE);
    if (!depth) return;
    if (imageSize < 0 || imageSize % depth) return ctx.setError(GL_INVALID_VALUE);
    const GLsizei faceSize = imageSize / depth;
    for (GLsizei i = 0; i < depth; ++i)
        CompressedTexSubImage2D(ctx, GL_TEXTURE_CUBE_MAP_POSITIVE_X + static_cast<GLenum>(zoffset + i), level, xoffset, yoffset,
                                width, height, format, faceSize, static_cast<const uint8_t*>(data) + i * faceSize);
}

void CopyTextureSubImage1D(Context& ctx, GLuint texture, GLint level, GLint xoffset, GLint x, GLint y, GLsizei width) {
    RefPtr<Texture> tex = textureFor(ctx, "glCopyTextureSubImage1D", texture, {GL_TEXTURE_1D});
    if (!tex) return;
    TextureScope scope(ctx, tex);
    CopyTexSubImage1D(ctx, tex->target, level, xoffset, x, y, width);
}

void CopyTextureSubImage2D(Context& ctx, GLuint texture, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y,
                           GLsizei width, GLsizei height) {
    RefPtr<Texture> tex =
        textureFor(ctx, "glCopyTextureSubImage2D", texture, {GL_TEXTURE_2D, GL_TEXTURE_1D_ARRAY, GL_TEXTURE_RECTANGLE});
    if (!tex) return;
    TextureScope scope(ctx, tex);
    CopyTexSubImage2D(ctx, tex->target, level, xoffset, yoffset, x, y, width, height);
}

void CopyTextureSubImage3D(Context& ctx, GLuint texture, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLint x,
                           GLint y, GLsizei width, GLsizei height) {
    RefPtr<Texture> tex = textureFor(ctx, "glCopyTextureSubImage3D", texture,
                                     {GL_TEXTURE_3D, GL_TEXTURE_2D_ARRAY, GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_CUBE_MAP});
    if (!tex) return;
    TextureScope scope(ctx, tex);
    if (tex->target != GL_TEXTURE_CUBE_MAP)
        return CopyTexSubImage3D(ctx, tex->target, level, xoffset, yoffset, zoffset, x, y, width, height);
    if (zoffset < 0 || zoffset > 5) return ctx.setError(GL_INVALID_VALUE);
    CopyTexSubImage2D(ctx, GL_TEXTURE_CUBE_MAP_POSITIVE_X + static_cast<GLenum>(zoffset), level, xoffset, yoffset, x, y, width,
                      height);
}

#define WMGL_NAMED_TEXTURE(command, name)                   \
    RefPtr<Texture> tex = textureFor(ctx, command, name, {}); \
    if (!tex) return;                                        \
    TextureScope scope(ctx, tex)

namespace {

// Texture parameters: none on buffer textures; no sampler state on
// multisample textures (INVALID_OPERATION, GL 4.5 section 8.10).
bool parameterTarget(Context& ctx, const Texture& tex, GLenum pname) {
    if (tex.target == GL_TEXTURE_BUFFER) return ctx.setError(GL_INVALID_OPERATION), false;
    const bool multisample = tex.target == GL_TEXTURE_2D_MULTISAMPLE || tex.target == GL_TEXTURE_2D_MULTISAMPLE_ARRAY;
    switch (pname) {
    case GL_TEXTURE_MIN_FILTER: case GL_TEXTURE_MAG_FILTER: case GL_TEXTURE_WRAP_S: case GL_TEXTURE_WRAP_T:
    case GL_TEXTURE_WRAP_R: case GL_TEXTURE_MIN_LOD: case GL_TEXTURE_MAX_LOD: case GL_TEXTURE_LOD_BIAS:
    case GL_TEXTURE_COMPARE_MODE: case GL_TEXTURE_COMPARE_FUNC: case GL_TEXTURE_BORDER_COLOR: case GL_TEXTURE_MAX_ANISOTROPY:
    case GL_TEXTURE_SRGB_DECODE_EXT:
        if (multisample) return ctx.setError(GL_INVALID_OPERATION), false;
        break;
    default: break;
    }
    return true;
}

bool getParameterTarget(Context& ctx, const Texture& tex) {
    if (tex.target == GL_TEXTURE_BUFFER) return ctx.setError(GL_INVALID_OPERATION), false;
    return true;
}

}  // namespace

void TextureParameterf(Context& ctx, GLuint texture, GLenum pname, GLfloat param) {
    WMGL_NAMED_TEXTURE("glTextureParameterf", texture);
    if (!parameterTarget(ctx, *tex, pname)) return;
    TexParameterf(ctx, tex->target, pname, param);
}

void TextureParameterfv(Context& ctx, GLuint texture, GLenum pname, const GLfloat* param) {
    WMGL_NAMED_TEXTURE("glTextureParameterfv", texture);
    if (!parameterTarget(ctx, *tex, pname)) return;
    TexParameterfv(ctx, tex->target, pname, param);
}

void TextureParameteri(Context& ctx, GLuint texture, GLenum pname, GLint param) {
    WMGL_NAMED_TEXTURE("glTextureParameteri", texture);
    if (!parameterTarget(ctx, *tex, pname)) return;
    TexParameteri(ctx, tex->target, pname, param);
}

void TextureParameteriv(Context& ctx, GLuint texture, GLenum pname, const GLint* param) {
    WMGL_NAMED_TEXTURE("glTextureParameteriv", texture);
    if (!parameterTarget(ctx, *tex, pname)) return;
    TexParameteriv(ctx, tex->target, pname, param);
}

void TextureParameterIiv(Context& ctx, GLuint texture, GLenum pname, const GLint* params) {
    WMGL_NAMED_TEXTURE("glTextureParameterIiv", texture);
    if (!parameterTarget(ctx, *tex, pname)) return;
    TexParameterIiv(ctx, tex->target, pname, params);
}

void TextureParameterIuiv(Context& ctx, GLuint texture, GLenum pname, const GLuint* params) {
    WMGL_NAMED_TEXTURE("glTextureParameterIuiv", texture);
    if (!parameterTarget(ctx, *tex, pname)) return;
    TexParameterIuiv(ctx, tex->target, pname, params);
}

void GetTextureParameterfv(Context& ctx, GLuint texture, GLenum pname, GLfloat* params) {
    WMGL_NAMED_TEXTURE("glGetTextureParameterfv", texture);
    if (!getParameterTarget(ctx, *tex)) return;
    GetTexParameterfv(ctx, tex->target, pname, params);
}

void GetTextureParameteriv(Context& ctx, GLuint texture, GLenum pname, GLint* params) {
    WMGL_NAMED_TEXTURE("glGetTextureParameteriv", texture);
    if (!getParameterTarget(ctx, *tex)) return;
    GetTexParameteriv(ctx, tex->target, pname, params);
}

void GetTextureParameterIiv(Context& ctx, GLuint texture, GLenum pname, GLint* params) {
    WMGL_NAMED_TEXTURE("glGetTextureParameterIiv", texture);
    if (!getParameterTarget(ctx, *tex)) return;
    GetTexParameterIiv(ctx, tex->target, pname, params);
}

void GetTextureParameterIuiv(Context& ctx, GLuint texture, GLenum pname, GLuint* params) {
    WMGL_NAMED_TEXTURE("glGetTextureParameterIuiv", texture);
    if (!getParameterTarget(ctx, *tex)) return;
    GetTexParameterIuiv(ctx, tex->target, pname, params);
}

void GetTextureLevelParameterfv(Context& ctx, GLuint texture, GLint level, GLenum pname, GLfloat* params) {
    WMGL_NAMED_TEXTURE("glGetTextureLevelParameterfv", texture);
    // A cube map answers for its faces (all have the same shape).
    GetTexLevelParameterfv(ctx, tex->target == GL_TEXTURE_CUBE_MAP ? GL_TEXTURE_CUBE_MAP_POSITIVE_X : tex->target, level, pname,
                           params);
}

void GetTextureLevelParameteriv(Context& ctx, GLuint texture, GLint level, GLenum pname, GLint* params) {
    WMGL_NAMED_TEXTURE("glGetTextureLevelParameteriv", texture);
    GetTexLevelParameteriv(ctx, tex->target == GL_TEXTURE_CUBE_MAP ? GL_TEXTURE_CUBE_MAP_POSITIVE_X : tex->target, level, pname,
                           params);
}

void GenerateTextureMipmap(Context& ctx, GLuint texture) {
    WMGL_NAMED_TEXTURE("glGenerateTextureMipmap", texture);
    GenerateMipmap(ctx, tex->target);
}

void TextureBuffer(Context& ctx, GLuint texture, GLenum internalformat, GLuint buffer) {
    RefPtr<Texture> tex = textureFor(ctx, "glTextureBuffer", texture, {GL_TEXTURE_BUFFER});
    if (!tex) return;
    TextureScope scope(ctx, tex);
    TexBuffer(ctx, GL_TEXTURE_BUFFER, internalformat, buffer);
}

void TextureBufferRange(Context& ctx, GLuint texture, GLenum internalformat, GLuint buffer, GLintptr offset, GLsizeiptr size) {
    RefPtr<Texture> tex = textureFor(ctx, "glTextureBufferRange", texture, {GL_TEXTURE_BUFFER});
    if (!tex) return;
    TextureScope scope(ctx, tex);
    TexBufferRange(ctx, GL_TEXTURE_BUFFER, internalformat, buffer, offset, size);
}

void GetTextureImage(Context& ctx, GLuint texture, GLint level, GLenum format, GLenum type, GLsizei bufSize, void* pixels) {
    WMGL_NAMED_TEXTURE("glGetTextureImage", texture);
    if (tex->target == GL_TEXTURE_BUFFER || tex->target == GL_TEXTURE_2D_MULTISAMPLE ||
        tex->target == GL_TEXTURE_2D_MULTISAMPLE_ARRAY)
        return ctx.setError(GL_INVALID_OPERATION);
    if (tex->target != GL_TEXTURE_CUBE_MAP) return GetnTexImage(ctx, tex->target, level, format, type, bufSize, pixels);
    // Every face must have the level, alike (cube complete at that level).
    if (level < 0 || level >= Texture::kMaxLevels) return ctx.setError(GL_INVALID_VALUE);
    for (int f = 1; f < 6; ++f) {
        const TextureImage& a = tex->image(0, level), &b = tex->image(f, level);
        if (!a.defined() || !b.defined() || a.width != b.width || a.height != b.height || a.internalFormat != b.internalFormat)
            return ctx.setError(GL_INVALID_OPERATION);
    }
    // A cube map: its six faces as the images of a three-dimensional image
    // (pack parameters included); bufSize bounds client memory only.
    const TextureImage& image = tex->image(0, level);
    const PixelStoreParams ps = pixelStoreParams(ctx.state.pack, true);
    const TransferLayout layout = transferLayout(format, type, image.width, image.height, 6, ps);
    if (bufSize < 0) return ctx.setError(GL_INVALID_VALUE);
    if (!ctx.state.pixelPackBuffer && layout.bytesNeeded > static_cast<size_t>(bufSize)) return ctx.setError(GL_INVALID_OPERATION);
    // Each face is a two-dimensional image (rows and pixels skipped) at its place.
    const size_t first = static_cast<size_t>(ps.skipImages) * layout.imageStride;
    for (int f = 0; f < 6; ++f)
        GetTexImage(ctx, GL_TEXTURE_CUBE_MAP_POSITIVE_X + static_cast<GLenum>(f), level, format, type,
                    static_cast<uint8_t*>(pixels) + first + f * layout.imageStride);
}

void GetCompressedTextureImage(Context& ctx, GLuint texture, GLint level, GLsizei bufSize, void* pixels) {
    WMGL_NAMED_TEXTURE("glGetCompressedTextureImage", texture);
    if (tex->target != GL_TEXTURE_CUBE_MAP) return GetnCompressedTexImage(ctx, tex->target, level, bufSize, pixels);
    GLint size = 0;
    GetTexLevelParameteriv(ctx, GL_TEXTURE_CUBE_MAP_POSITIVE_X, level, GL_TEXTURE_COMPRESSED_IMAGE_SIZE, &size);
    if (bufSize < 0) return ctx.setError(GL_INVALID_VALUE);
    if (!ctx.state.pixelPackBuffer && static_cast<int64_t>(bufSize) < 6 * static_cast<int64_t>(size))
        return ctx.setError(GL_INVALID_OPERATION);
    for (int f = 0; f < 6; ++f)
        GetnCompressedTexImage(ctx, GL_TEXTURE_CUBE_MAP_POSITIVE_X + static_cast<GLenum>(f), level, size,
                               static_cast<uint8_t*>(pixels) + f * size);
}

#undef WMGL_NAMED_TEXTURE

// ------------------------------------------------------------ framebuffers and renderbuffers

namespace {

// framebuffer 0 names the default framebuffer where the command allows it.
bool namedFramebuffer(Context& ctx, const char* command, GLuint name, bool allowDefault, RefPtr<Framebuffer>& out) {
    if (!dsa(ctx, command)) return false;
    if (!name) {
        if (!allowDefault) {
            ctx.setError(GL_INVALID_OPERATION);
            return false;
        }
        out = nullptr;
        return true;
    }
    {
        ShareLock lock(ctx);
        out = ctx.share().framebuffers.getRef(name);
    }
    if (!out) ctx.setError(GL_INVALID_OPERATION);
    return out != nullptr;
}

}  // namespace

#define WMGL_DRAW_FRAMEBUFFER(command, name, allowDefault)                         \
    RefPtr<Framebuffer> fb_;                                                     \
    if (!namedFramebuffer(ctx, command, name, allowDefault, fb_)) return;          \
    BindingSwap<RefPtr<Framebuffer>> drawSwap_(ctx.state.drawFramebuffer, fb_)

namespace {

// COLOR_ATTACHMENTm with m at or past MAX_COLOR_ATTACHMENTS is INVALID_OPERATION (GL 4.5 section 9.2).
bool colorAttachmentInRange(Context& ctx, GLenum attachment) {
    if (attachment >= GL_COLOR_ATTACHMENT0 + limits::kMaxDrawBuffers && attachment <= GL_COLOR_ATTACHMENT0 + 31)
        return ctx.setError(GL_INVALID_OPERATION), false;
    return true;
}

}  // namespace

void NamedFramebufferRenderbuffer(Context& ctx, GLuint framebuffer, GLenum attachment, GLenum renderbuffertarget,
                                  GLuint renderbuffer) {
    WMGL_DRAW_FRAMEBUFFER("glNamedFramebufferRenderbuffer", framebuffer, false);
    if (!colorAttachmentInRange(ctx, attachment)) return;
    FramebufferRenderbuffer(ctx, GL_DRAW_FRAMEBUFFER, attachment, renderbuffertarget, renderbuffer);
}

void NamedFramebufferTexture(Context& ctx, GLuint framebuffer, GLenum attachment, GLuint texture, GLint level) {
    WMGL_DRAW_FRAMEBUFFER("glNamedFramebufferTexture", framebuffer, false);
    FramebufferTexture(ctx, GL_DRAW_FRAMEBUFFER, attachment, texture, level);
}

void NamedFramebufferTextureLayer(Context& ctx, GLuint framebuffer, GLenum attachment, GLuint texture, GLint level, GLint layer) {
    WMGL_DRAW_FRAMEBUFFER("glNamedFramebufferTextureLayer", framebuffer, false);
    FramebufferTextureLayer(ctx, GL_DRAW_FRAMEBUFFER, attachment, texture, level, layer);
}

namespace {

// A draw buffer token of GL 4.5 tables 17.5 and 17.6.
bool drawBufferToken(GLenum buf) {
    switch (buf) {
    case GL_NONE: case GL_FRONT: case GL_BACK: case GL_LEFT: case GL_RIGHT: case GL_FRONT_AND_BACK: case GL_FRONT_LEFT:
    case GL_FRONT_RIGHT: case GL_BACK_LEFT: case GL_BACK_RIGHT:
        return true;
    default: return buf >= GL_COLOR_ATTACHMENT0 && buf <= GL_COLOR_ATTACHMENT0 + 31;
    }
}

}  // namespace

void NamedFramebufferDrawBuffer(Context& ctx, GLuint framebuffer, GLenum buf) {
    WMGL_DRAW_FRAMEBUFFER("glNamedFramebufferDrawBuffer", framebuffer, true);
    if (!drawBufferToken(buf)) return ctx.setError(GL_INVALID_ENUM);
    DrawBuffer(ctx, buf);
}

void NamedFramebufferDrawBuffers(Context& ctx, GLuint framebuffer, GLsizei n, const GLenum* bufs) {
    WMGL_DRAW_FRAMEBUFFER("glNamedFramebufferDrawBuffers", framebuffer, true);
    if (n < 0 || n > limits::kMaxDrawBuffers) return ctx.setError(GL_INVALID_VALUE);
    // FRONT, LEFT, RIGHT and FRONT_AND_BACK name several buffers, which DrawBuffers rejects.
    for (GLsizei i = 0; bufs && i < n; ++i)
        if (!drawBufferToken(bufs[i]) || bufs[i] == GL_FRONT || bufs[i] == GL_LEFT || bufs[i] == GL_RIGHT ||
            bufs[i] == GL_FRONT_AND_BACK)
            return ctx.setError(GL_INVALID_ENUM);
    DrawBuffers(ctx, n, bufs);
}

void NamedFramebufferReadBuffer(Context& ctx, GLuint framebuffer, GLenum src) {
    RefPtr<Framebuffer> fb;
    if (!namedFramebuffer(ctx, "glNamedFramebufferReadBuffer", framebuffer, true, fb)) return;
    // A framebuffer object reads only its colour attachments (GL 4.5 section 18.2.1).
    if (fb && src != GL_NONE && (!drawBufferToken(src) || src < GL_COLOR_ATTACHMENT0 ||
                                 src >= GL_COLOR_ATTACHMENT0 + limits::kMaxDrawBuffers))
        return ctx.setError(drawBufferToken(src) ? GL_INVALID_OPERATION : GL_INVALID_ENUM);
    BindingSwap<RefPtr<Framebuffer>> readSwap(ctx.state.readFramebuffer, fb);
    ReadBuffer(ctx, src);
}

void NamedFramebufferParameteri(Context& ctx, GLuint framebuffer, GLenum pname, GLint param) {
    WMGL_DRAW_FRAMEBUFFER("glNamedFramebufferParameteri", framebuffer, false);
    FramebufferParameteri(ctx, GL_DRAW_FRAMEBUFFER, pname, param);
}

void GetNamedFramebufferParameteriv(Context& ctx, GLuint framebuffer, GLenum pname, GLint* param) {
    WMGL_DRAW_FRAMEBUFFER("glGetNamedFramebufferParameteriv", framebuffer, true);
    GetFramebufferParameteriv(ctx, GL_DRAW_FRAMEBUFFER, pname, param);
}

void GetNamedFramebufferAttachmentParameteriv(Context& ctx, GLuint framebuffer, GLenum attachment, GLenum pname,
                                              GLint* params) {
    WMGL_DRAW_FRAMEBUFFER("glGetNamedFramebufferAttachmentParameteriv", framebuffer, true);
    if (framebuffer && !colorAttachmentInRange(ctx, attachment)) return;
    GetFramebufferAttachmentParameteriv(ctx, GL_DRAW_FRAMEBUFFER, attachment, pname, params);
}

void InvalidateNamedFramebufferData(Context& ctx, GLuint framebuffer, GLsizei numAttachments, const GLenum* attachments) {
    WMGL_DRAW_FRAMEBUFFER("glInvalidateNamedFramebufferData", framebuffer, true);
    InvalidateFramebuffer(ctx, GL_DRAW_FRAMEBUFFER, numAttachments, attachments);
}

void InvalidateNamedFramebufferSubData(Context& ctx, GLuint framebuffer, GLsizei numAttachments, const GLenum* attachments,
                                       GLint x, GLint y, GLsizei width, GLsizei height) {
    WMGL_DRAW_FRAMEBUFFER("glInvalidateNamedFramebufferSubData", framebuffer, true);
    InvalidateSubFramebuffer(ctx, GL_DRAW_FRAMEBUFFER, numAttachments, attachments, x, y, width, height);
}

void ClearNamedFramebufferiv(Context& ctx, GLuint framebuffer, GLenum buffer, GLint drawbuffer, const GLint* value) {
    WMGL_DRAW_FRAMEBUFFER("glClearNamedFramebufferiv", framebuffer, true);
    ClearBufferiv(ctx, buffer, drawbuffer, value);
}

void ClearNamedFramebufferuiv(Context& ctx, GLuint framebuffer, GLenum buffer, GLint drawbuffer, const GLuint* value) {
    WMGL_DRAW_FRAMEBUFFER("glClearNamedFramebufferuiv", framebuffer, true);
    ClearBufferuiv(ctx, buffer, drawbuffer, value);
}

void ClearNamedFramebufferfv(Context& ctx, GLuint framebuffer, GLenum buffer, GLint drawbuffer, const GLfloat* value) {
    WMGL_DRAW_FRAMEBUFFER("glClearNamedFramebufferfv", framebuffer, true);
    ClearBufferfv(ctx, buffer, drawbuffer, value);
}

void ClearNamedFramebufferfi(Context& ctx, GLuint framebuffer, GLenum buffer, GLint drawbuffer, GLfloat depth, GLint stencil) {
    WMGL_DRAW_FRAMEBUFFER("glClearNamedFramebufferfi", framebuffer, true);
    ClearBufferfi(ctx, buffer, drawbuffer, depth, stencil);
}

#undef WMGL_DRAW_FRAMEBUFFER

void BlitNamedFramebuffer(Context& ctx, GLuint readFramebuffer, GLuint drawFramebuffer, GLint srcX0, GLint srcY0, GLint srcX1,
                          GLint srcY1, GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1, GLbitfield mask, GLenum filter) {
    RefPtr<Framebuffer> read, draw;
    if (!namedFramebuffer(ctx, "glBlitNamedFramebuffer", readFramebuffer, true, read) ||
        !namedFramebuffer(ctx, "glBlitNamedFramebuffer", drawFramebuffer, true, draw))
        return;
    BindingSwap<RefPtr<Framebuffer>> r(ctx.state.readFramebuffer, read), d(ctx.state.drawFramebuffer, draw);
    BlitFramebuffer(ctx, srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1, mask, filter);
}

GLenum CheckNamedFramebufferStatus(Context& ctx, GLuint framebuffer, GLenum target) {
    if (target != GL_FRAMEBUFFER && target != GL_DRAW_FRAMEBUFFER && target != GL_READ_FRAMEBUFFER) {
        if (!dsa(ctx, "glCheckNamedFramebufferStatus")) return 0;
        ctx.setError(GL_INVALID_ENUM);
        return 0;
    }
    RefPtr<Framebuffer> fb;
    if (!namedFramebuffer(ctx, "glCheckNamedFramebufferStatus", framebuffer, true, fb)) return 0;
    BindingSwap<RefPtr<Framebuffer>> r(ctx.state.readFramebuffer, fb), d(ctx.state.drawFramebuffer, fb);
    return CheckFramebufferStatus(ctx, target);
}

void NamedRenderbufferStorage(Context& ctx, GLuint renderbuffer, GLenum internalformat, GLsizei width, GLsizei height) {
    if (!dsa(ctx, "glNamedRenderbufferStorage")) return;
    RefPtr<Renderbuffer> rb = namedRenderbuffer(ctx, renderbuffer);
    if (!rb) return ctx.setError(GL_INVALID_OPERATION);
    BindingSwap<RefPtr<Renderbuffer>> bound(ctx.state.renderbuffer, rb);
    RenderbufferStorage(ctx, GL_RENDERBUFFER, internalformat, width, height);
}

void NamedRenderbufferStorageMultisample(Context& ctx, GLuint renderbuffer, GLsizei samples, GLenum internalformat,
                                         GLsizei width, GLsizei height) {
    if (!dsa(ctx, "glNamedRenderbufferStorageMultisample")) return;
    RefPtr<Renderbuffer> rb = namedRenderbuffer(ctx, renderbuffer);
    if (!rb) return ctx.setError(GL_INVALID_OPERATION);
    // More samples than the format allows is INVALID_OPERATION (GL 4.5 section 9.2.4).
    GLint maxSamples = 0, maxIntegerSamples = 0;
    GetIntegerv(ctx, GL_MAX_SAMPLES, &maxSamples);
    GetIntegerv(ctx, GL_MAX_INTEGER_SAMPLES, &maxIntegerSamples);
    const FormatInfo* info = formatInfo(internalformat);
    if (samples > maxSamples || (info && info->isInteger() && samples > maxIntegerSamples))
        return ctx.setError(GL_INVALID_OPERATION);
    BindingSwap<RefPtr<Renderbuffer>> bound(ctx.state.renderbuffer, rb);
    RenderbufferStorageMultisample(ctx, GL_RENDERBUFFER, samples, internalformat, width, height);
}

void GetNamedRenderbufferParameteriv(Context& ctx, GLuint renderbuffer, GLenum pname, GLint* params) {
    if (!dsa(ctx, "glGetNamedRenderbufferParameteriv")) return;
    RefPtr<Renderbuffer> rb = namedRenderbuffer(ctx, renderbuffer);
    if (!rb) return ctx.setError(GL_INVALID_OPERATION);
    BindingSwap<RefPtr<Renderbuffer>> bound(ctx.state.renderbuffer, rb);
    GetRenderbufferParameteriv(ctx, GL_RENDERBUFFER, pname, params);
}

// ------------------------------------------------------------ vertex arrays

// vaobj 0 is the default vertex array of a compatibility context.
#define WMGL_NAMED_VAO(command, name)                                             \
    if (!dsa(ctx, command)) return;                                               \
    RefPtr<VertexArray> vao_ = (name)          ? ctx.vertexArrays.getRef(name)     \
                               : ctx.isCore() ? nullptr                            \
                                              : ctx.defaultVertexArray;            \
    if (!vao_) return ctx.setError(GL_INVALID_OPERATION);                        \
    BindingSwap<RefPtr<VertexArray>> vaoSwap_(ctx.state.vertexArray, vao_);        \
    ctx.markDirty(kDirtyVertexInput)

void VertexArrayElementBuffer(Context& ctx, GLuint vaobj, GLuint buffer) {
    WMGL_NAMED_VAO("glVertexArrayElementBuffer", vaobj);
    if (buffer && !namedBuffer(ctx, buffer)) return ctx.setError(GL_INVALID_OPERATION);
    BindingSwap<RefPtr<Buffer>> keep(ctx.state.arrayBuffer, ctx.state.arrayBuffer);  // only the element binding changes
    BindBuffer(ctx, GL_ELEMENT_ARRAY_BUFFER, buffer);
}

void VertexArrayVertexBuffer(Context& ctx, GLuint vaobj, GLuint bindingindex, GLuint buffer, GLintptr offset, GLsizei stride) {
    WMGL_NAMED_VAO("glVertexArrayVertexBuffer", vaobj);
    BindVertexBuffer(ctx, bindingindex, buffer, offset, stride);
}

void VertexArrayVertexBuffers(Context& ctx, GLuint vaobj, GLuint first, GLsizei count, const GLuint* buffers,
                              const GLintptr* offsets, const GLsizei* strides) {
    WMGL_NAMED_VAO("glVertexArrayVertexBuffers", vaobj);
    BindVertexBuffers(ctx, first, count, buffers, offsets, strides);
}

void VertexArrayAttribBinding(Context& ctx, GLuint vaobj, GLuint attribindex, GLuint bindingindex) {
    WMGL_NAMED_VAO("glVertexArrayAttribBinding", vaobj);
    VertexAttribBinding(ctx, attribindex, bindingindex);
}

void VertexArrayAttribFormat(Context& ctx, GLuint vaobj, GLuint attribindex, GLint size, GLenum type, GLboolean normalized,
                             GLuint relativeoffset) {
    WMGL_NAMED_VAO("glVertexArrayAttribFormat", vaobj);
    VertexAttribFormat(ctx, attribindex, size, type, normalized, relativeoffset);
}

void VertexArrayAttribIFormat(Context& ctx, GLuint vaobj, GLuint attribindex, GLint size, GLenum type, GLuint relativeoffset) {
    WMGL_NAMED_VAO("glVertexArrayAttribIFormat", vaobj);
    VertexAttribIFormat(ctx, attribindex, size, type, relativeoffset);
}

void VertexArrayAttribLFormat(Context& ctx, GLuint vaobj, GLuint attribindex, GLint size, GLenum type, GLuint relativeoffset) {
    WMGL_NAMED_VAO("glVertexArrayAttribLFormat", vaobj);
    VertexAttribLFormat(ctx, attribindex, size, type, relativeoffset);
}

void VertexArrayBindingDivisor(Context& ctx, GLuint vaobj, GLuint bindingindex, GLuint divisor) {
    WMGL_NAMED_VAO("glVertexArrayBindingDivisor", vaobj);
    VertexBindingDivisor(ctx, bindingindex, divisor);
}

void EnableVertexArrayAttrib(Context& ctx, GLuint vaobj, GLuint index) {
    WMGL_NAMED_VAO("glEnableVertexArrayAttrib", vaobj);
    EnableVertexAttribArray(ctx, index);
}

void DisableVertexArrayAttrib(Context& ctx, GLuint vaobj, GLuint index) {
    WMGL_NAMED_VAO("glDisableVertexArrayAttrib", vaobj);
    DisableVertexAttribArray(ctx, index);
}

void GetVertexArrayiv(Context& ctx, GLuint vaobj, GLenum pname, GLint* param) {
    WMGL_NAMED_VAO("glGetVertexArrayiv", vaobj);
    if (pname != GL_ELEMENT_ARRAY_BUFFER_BINDING) return ctx.setError(GL_INVALID_ENUM);
    *param = vao_->elementBuffer ? static_cast<GLint>(vao_->elementBuffer->name) : 0;
}

void GetVertexArrayIndexediv(Context& ctx, GLuint vaobj, GLuint index, GLenum pname, GLint* param) {
    WMGL_NAMED_VAO("glGetVertexArrayIndexediv", vaobj);
    switch (pname) {
    case GL_VERTEX_ATTRIB_ARRAY_ENABLED: case GL_VERTEX_ATTRIB_ARRAY_SIZE: case GL_VERTEX_ATTRIB_ARRAY_STRIDE:
    case GL_VERTEX_ATTRIB_ARRAY_TYPE: case GL_VERTEX_ATTRIB_ARRAY_NORMALIZED: case GL_VERTEX_ATTRIB_ARRAY_INTEGER:
    case GL_VERTEX_ATTRIB_ARRAY_LONG: case GL_VERTEX_ATTRIB_ARRAY_DIVISOR: case GL_VERTEX_ATTRIB_RELATIVE_OFFSET:
        GetVertexAttribiv(ctx, index, pname, param);
        return;
    default: return ctx.setError(GL_INVALID_ENUM);
    }
}

void GetVertexArrayIndexed64iv(Context& ctx, GLuint vaobj, GLuint index, GLenum pname, GLint64* param) {
    WMGL_NAMED_VAO("glGetVertexArrayIndexed64iv", vaobj);
    if (pname != GL_VERTEX_BINDING_OFFSET) return ctx.setError(GL_INVALID_ENUM);
    if (index >= static_cast<GLuint>(VertexArray::kMaxBindings)) return ctx.setError(GL_INVALID_VALUE);
    *param = static_cast<GLint64>(vao_->bindings[index].offset);
}

#undef WMGL_NAMED_VAO

// ------------------------------------------------------------ transform feedback and queries

namespace {

void transformFeedbackBuffer(Context& ctx, const char* command, GLuint xfb, GLuint index, GLuint buffer, GLintptr offset,
                             GLsizeiptr size, bool range) {
    if (!dsa(ctx, command)) return;
    RefPtr<TransformFeedback> object = xfb ? ctx.transformFeedbacks.getRef(xfb) : ctx.defaultTransformFeedback;
    if (!object) return ctx.setError(GL_INVALID_OPERATION);
    // Not an existing buffer: INVALID_VALUE (GL 4.6 section 13.2.2).
    if (buffer && !namedBuffer(ctx, buffer)) return ctx.setError(GL_INVALID_VALUE);
    BindingSwap<RefPtr<TransformFeedback>> bound(ctx.state.transformFeedback, object);
    BindingSwap<RefPtr<Buffer>> generic(ctx.state.transformFeedbackBuffer, ctx.state.transformFeedbackBuffer);
    if (range) BindBufferRange(ctx, GL_TRANSFORM_FEEDBACK_BUFFER, index, buffer, offset, size);
    else BindBufferBase(ctx, GL_TRANSFORM_FEEDBACK_BUFFER, index, buffer);
}

template <class T>
void queryBufferObject(Context& ctx, const char* command, GLuint id, GLuint buffer, GLenum pname, GLintptr offset,
                       void (*get)(Context&, GLuint, GLenum, T*)) {
    if (!dsa(ctx, command)) return;
    RefPtr<Buffer> object = namedBuffer(ctx, buffer);
    if (!object) return ctx.setError(GL_INVALID_OPERATION);
    if (offset < 0) return ctx.setError(GL_INVALID_VALUE);
    BindingSwap<RefPtr<Buffer>> bound(ctx.state.queryBuffer, object);
    get(ctx, id, pname, reinterpret_cast<T*>(offset));
}

}  // namespace

void TransformFeedbackBufferBase(Context& ctx, GLuint xfb, GLuint index, GLuint buffer) {
    transformFeedbackBuffer(ctx, "glTransformFeedbackBufferBase", xfb, index, buffer, 0, 0, false);
}

void TransformFeedbackBufferRange(Context& ctx, GLuint xfb, GLuint index, GLuint buffer, GLintptr offset, GLsizeiptr size) {
    transformFeedbackBuffer(ctx, "glTransformFeedbackBufferRange", xfb, index, buffer, offset, size, true);
}

void GetQueryBufferObjectiv(Context& ctx, GLuint id, GLuint buffer, GLenum pname, GLintptr offset) {
    queryBufferObject<GLint>(ctx, "glGetQueryBufferObjectiv", id, buffer, pname, offset, &GetQueryObjectiv);
}

void GetQueryBufferObjectuiv(Context& ctx, GLuint id, GLuint buffer, GLenum pname, GLintptr offset) {
    queryBufferObject<GLuint>(ctx, "glGetQueryBufferObjectuiv", id, buffer, pname, offset, &GetQueryObjectuiv);
}

void GetQueryBufferObjecti64v(Context& ctx, GLuint id, GLuint buffer, GLenum pname, GLintptr offset) {
    queryBufferObject<GLint64>(ctx, "glGetQueryBufferObjecti64v", id, buffer, pname, offset, &GetQueryObjecti64v);
}

void GetQueryBufferObjectui64v(Context& ctx, GLuint id, GLuint buffer, GLenum pname, GLintptr offset) {
    queryBufferObject<GLuint64>(ctx, "glGetQueryBufferObjectui64v", id, buffer, pname, offset, &GetQueryObjectui64v);
}

// glMemoryBarrierByRegion (GL 4.5, GL_ARB_ES3_1_compatibility): the
// framebuffer-local subset of glMemoryBarrier, which orders at least as much.
void MemoryBarrierByRegion(Context& ctx, GLbitfield barriers) {
    if (ctx.version() < 45 && !ctx.hasExtension("GL_ARB_ES3_1_compatibility")) return ctx.unimplemented("glMemoryBarrierByRegion");
    constexpr GLbitfield kRegionBits = GL_ATOMIC_COUNTER_BARRIER_BIT | GL_FRAMEBUFFER_BARRIER_BIT |
                                       GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT |
                                       GL_TEXTURE_FETCH_BARRIER_BIT | GL_UNIFORM_BARRIER_BIT;
    if (barriers != GL_ALL_BARRIER_BITS && (barriers & ~kRegionBits)) return ctx.setError(GL_INVALID_VALUE);
    MemoryBarrier(ctx, barriers);
}

}  // namespace wmgl::api
