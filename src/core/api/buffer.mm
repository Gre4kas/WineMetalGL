// Buffer objects (GL 3.3 section 2.9).

#include <algorithm>
#include <cstring>

#include "core/context.h"
#include "core/draw.h"
#include "core/metal/device.h"
#include "gl_api.gen.h"

namespace wmgl::api {
namespace {

bool validUsage(GLenum usage) {
    switch (usage) {
    case GL_STREAM_DRAW: case GL_STREAM_READ: case GL_STREAM_COPY: case GL_STATIC_DRAW: case GL_STATIC_READ:
    case GL_STATIC_COPY: case GL_DYNAMIC_DRAW: case GL_DYNAMIC_READ: case GL_DYNAMIC_COPY: return true;
    default: return false;
    }
}

id<MTLBuffer> newStorage(GLsizeiptr size) {
    if (size <= 0) return nil;
    return [Device::get().mtl() newBufferWithLength:static_cast<NSUInteger>(size) options:MTLResourceStorageModeShared];
}

bool gpuBusy(Context& ctx, const Buffer& buffer) {
    return buffer.lastUseSerial && !ctx.share().queue.isComplete(buffer.lastUseSerial);
}

// Replaces the storage so in-flight work keeps the old contents. Without
// memory for new storage it waits for the GPU and keeps the old one.
void rename(Context& ctx, Buffer& buffer, bool preserveContents) {
    id<MTLBuffer> fresh = newStorage(buffer.size);
    if (!fresh) {
        ctx.stream().waitFor(buffer.lastUseSerial);
        ++buffer.version;
        return;
    }
    if (preserveContents && buffer.mtl) {
        // Queued GPU writes (copies, transform feedback) must land first.
        if (buffer.lastWriteSerial) ctx.stream().waitFor(buffer.lastWriteSerial);
        std::memcpy(fresh.contents, buffer.mtl.contents, static_cast<size_t>(buffer.size));
    }
    buffer.mtl = fresh;
    buffer.lastUseSerial = buffer.lastWriteSerial = 0;
    ++buffer.version;
}

Buffer* boundBuffer(Context& ctx, GLenum target) {
    RefPtr<Buffer>* binding = ctx.bufferBinding(target);
    if (!binding) {
        ctx.setError(GL_INVALID_ENUM);
        return nullptr;
    }
    if (!*binding) {
        ctx.setError(GL_INVALID_OPERATION);
        return nullptr;
    }
    return binding->get();
}

RefPtr<Buffer> bufferForBind(Context& ctx, GLuint name, bool& ok) {
    ok = true;
    if (!name) return nullptr;
    ShareLock lock(ctx);
    RefPtr<Buffer> buffer = ctx.share().buffers.getRef(name);
    if (buffer) return buffer;
    if (!ctx.share().buffers.isGenerated(name)) {
        if (ctx.isCore()) {
            ctx.setError(GL_INVALID_OPERATION);
            ok = false;
            return nullptr;
        }
        ctx.share().buffers.reserve(name);
    }
    buffer = makeRef<Buffer>(name);
    ctx.share().buffers.set(name, buffer);
    return buffer;
}

void unmapBuffer(Buffer& buffer) {
    buffer.mapped = false;
    buffer.mapAccess = 0;
    buffer.mapOffset = 0;
    buffer.mapLength = 0;
    buffer.mapPointer = nullptr;
}

}  // namespace

void GenBuffers(Context& ctx, GLsizei n, GLuint* buffers) {
    if (n < 0) return ctx.setError(GL_INVALID_VALUE);
    if (!buffers) return;
    ShareLock lock(ctx);
    ctx.share().buffers.generate(n, buffers);
}

void DeleteBuffers(Context& ctx, GLsizei n, const GLuint* buffers) {
    if (n < 0) return ctx.setError(GL_INVALID_VALUE);
    if (!buffers) return;
    ContextState& s = ctx.state;
    for (GLsizei i = 0; i < n; ++i) {
        if (!buffers[i]) continue;
        RefPtr<Buffer> buffer;
        {
            ShareLock lock(ctx);
            buffer = ctx.share().buffers.remove(buffers[i]);
        }
        if (!buffer) continue;
        if (buffer->mapped) unmapBuffer(*buffer);
        // Deleting a bound buffer resets the bindings of the current context.
        for (RefPtr<Buffer>* b : {&s.arrayBuffer, &s.copyReadBuffer, &s.copyWriteBuffer, &s.pixelPackBuffer,
                                  &s.pixelUnpackBuffer, &s.textureBuffer, &s.uniformBuffer, &s.transformFeedbackBuffer,
                                  &s.drawIndirectBuffer, &s.dispatchIndirectBuffer, &s.shaderStorageBuffer,
                                  &s.atomicCounterBuffer, &s.queryBuffer})
            if (*b == buffer) b->reset();
        if (s.vertexArray->elementBuffer == buffer) s.vertexArray->elementBuffer.reset();
        for (VertexAttrib& a : s.vertexArray->attribs)
            if (a.buffer == buffer) a.buffer.reset();
        for (IndexedBufferBinding& b : s.uniformBufferBindings)
            if (b.buffer == buffer) b = IndexedBufferBinding();
        for (auto& b : s.transformFeedback->buffers)
            if (b.buffer == buffer) b = TransformFeedback::Binding();
        ctx.markDirty(kDirtyVertexInput | kDirtyUniformBuffers);
    }
}

GLboolean IsBuffer(Context& ctx, GLuint buffer) {
    ShareLock lock(ctx);
    return ctx.share().buffers.exists(buffer) ? GL_TRUE : GL_FALSE;
}

void BindBuffer(Context& ctx, GLenum target, GLuint name) {
    RefPtr<Buffer>* binding = ctx.bufferBinding(target);
    if (!binding) return ctx.setError(GL_INVALID_ENUM);
    bool ok;
    RefPtr<Buffer> buffer = bufferForBind(ctx, name, ok);
    if (!ok) return;
    *binding = buffer;
    if (target == GL_ELEMENT_ARRAY_BUFFER) ++ctx.state.vertexArray->version;
    ctx.markDirty(kDirtyVertexInput);
}

void BindBufferRange(Context& ctx, GLenum target, GLuint index, GLuint name, GLintptr offset, GLsizeiptr size) {
    if (target != GL_UNIFORM_BUFFER && target != GL_TRANSFORM_FEEDBACK_BUFFER) return ctx.setError(GL_INVALID_ENUM);
    if (name) {
        if (size <= 0 || offset < 0) return ctx.setError(GL_INVALID_VALUE);
        if (target == GL_UNIFORM_BUFFER && offset % 256) return ctx.setError(GL_INVALID_VALUE);
        if (target == GL_TRANSFORM_FEEDBACK_BUFFER && (offset % 4 || size % 4)) return ctx.setError(GL_INVALID_VALUE);
    }
    bool ok;
    RefPtr<Buffer> buffer = bufferForBind(ctx, name, ok);
    if (!ok) return;
    if (target == GL_UNIFORM_BUFFER) {
        if (index >= ContextState::kMaxUniformBufferBindings) return ctx.setError(GL_INVALID_VALUE);
        ctx.state.uniformBufferBindings[index] = {buffer, offset, size};
        ctx.state.uniformBuffer = buffer;
        ctx.markDirty(kDirtyUniformBuffers);
    } else {
        if (index >= TransformFeedback::kMaxBuffers) return ctx.setError(GL_INVALID_VALUE);
        TransformFeedback& xfb = *ctx.state.transformFeedback;
        if (xfb.active) return ctx.setError(GL_INVALID_OPERATION);
        xfb.buffers[index] = {buffer, offset, size};
        ctx.state.transformFeedbackBuffer = buffer;
    }
}

void BindBufferBase(Context& ctx, GLenum target, GLuint index, GLuint name) {
    if (target != GL_UNIFORM_BUFFER && target != GL_TRANSFORM_FEEDBACK_BUFFER) return ctx.setError(GL_INVALID_ENUM);
    bool ok;
    RefPtr<Buffer> buffer = bufferForBind(ctx, name, ok);
    if (!ok) return;
    if (target == GL_UNIFORM_BUFFER) {
        if (index >= ContextState::kMaxUniformBufferBindings) return ctx.setError(GL_INVALID_VALUE);
        ctx.state.uniformBufferBindings[index] = {buffer, 0, 0};
        ctx.state.uniformBuffer = buffer;
        ctx.markDirty(kDirtyUniformBuffers);
    } else {
        if (index >= TransformFeedback::kMaxBuffers) return ctx.setError(GL_INVALID_VALUE);
        TransformFeedback& xfb = *ctx.state.transformFeedback;
        if (xfb.active) return ctx.setError(GL_INVALID_OPERATION);
        xfb.buffers[index] = {buffer, 0, 0};
        ctx.state.transformFeedbackBuffer = buffer;
    }
}

void BufferData(Context& ctx, GLenum target, GLsizeiptr size, const void* data, GLenum usage) {
    if (size < 0) return ctx.setError(GL_INVALID_VALUE);
    if (!validUsage(usage)) return ctx.setError(GL_INVALID_ENUM);
    Buffer* buffer = boundBuffer(ctx, target);
    if (!buffer) return;
    if (buffer->immutable) return ctx.setError(GL_INVALID_OPERATION);
    if (buffer->mapped) unmapBuffer(*buffer);
    // Reuse the storage only when no queued work references it (orphaning).
    if (buffer->mtl && buffer->size == size && !gpuBusy(ctx, *buffer)) {
        ++buffer->version;
    } else {
        buffer->mtl = newStorage(size);
        buffer->lastUseSerial = buffer->lastWriteSerial = 0;
        ++buffer->version;
        if (size && !buffer->mtl) {
            buffer->size = 0;
            return ctx.setError(GL_OUT_OF_MEMORY);
        }
    }
    buffer->size = size;
    buffer->usage = usage;
    if (data && size) std::memcpy(buffer->mtl.contents, data, static_cast<size_t>(size));
    ctx.markDirty(kDirtyVertexInput | kDirtyUniformBuffers);
}

void BufferSubData(Context& ctx, GLenum target, GLintptr offset, GLsizeiptr size, const void* data) {
    if (offset < 0 || size < 0) return ctx.setError(GL_INVALID_VALUE);
    Buffer* buffer = boundBuffer(ctx, target);
    if (!buffer) return;
    if (offset + size > buffer->size) return ctx.setError(GL_INVALID_VALUE);
    if (buffer->mapped && !(buffer->mapAccess & GL_MAP_PERSISTENT_BIT)) return ctx.setError(GL_INVALID_OPERATION);
    if (!size || !data || !buffer->mtl) return;
    if (gpuBusy(ctx, *buffer)) {
        if (size == buffer->size || buffer->size <= (256 << 10)) {
            // Small or whole-buffer updates: copy-on-write.
            rename(ctx, *buffer, size != buffer->size);
        } else {
            // Large partial update: ordered GPU copy from staging memory.
            TransientAllocation staging = ctx.draw().transient().allocate(static_cast<size_t>(size), ctx.stream().serial(), 16);
            std::memcpy(staging.data, data, static_cast<size_t>(size));
            [ctx.stream().blit() copyFromBuffer:staging.buffer sourceOffset:staging.offset toBuffer:buffer->mtl
                                destinationOffset:static_cast<NSUInteger>(offset) size:static_cast<NSUInteger>(size)];
            buffer->lastUseSerial = buffer->lastWriteSerial = ctx.stream().serial();
            ++buffer->version;
            return;
        }
    }
    std::memcpy(static_cast<uint8_t*>(buffer->mtl.contents) + offset, data, static_cast<size_t>(size));
    ++buffer->version;
}

void GetBufferSubData(Context& ctx, GLenum target, GLintptr offset, GLsizeiptr size, void* data) {
    if (offset < 0 || size < 0) return ctx.setError(GL_INVALID_VALUE);
    Buffer* buffer = boundBuffer(ctx, target);
    if (!buffer) return;
    if (offset + size > buffer->size) return ctx.setError(GL_INVALID_VALUE);
    if (buffer->mapped && !(buffer->mapAccess & GL_MAP_PERSISTENT_BIT)) return ctx.setError(GL_INVALID_OPERATION);
    if (!size || !data || !buffer->mtl) return;
    ctx.stream().waitFor(buffer->lastUseSerial);
    std::memcpy(data, static_cast<uint8_t*>(buffer->mtl.contents) + offset, static_cast<size_t>(size));
}

void* MapBufferRange(Context& ctx, GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield access) {
    Buffer* buffer = boundBuffer(ctx, target);
    if (!buffer) return nullptr;
    const GLbitfield known = GL_MAP_READ_BIT | GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_RANGE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT |
                             GL_MAP_FLUSH_EXPLICIT_BIT | GL_MAP_UNSYNCHRONIZED_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
    if (offset < 0 || length < 0 || offset + length > buffer->size || (access & ~known)) {
        ctx.setError(GL_INVALID_VALUE);
        return nullptr;
    }
    if (length == 0) {
        ctx.setError(GL_INVALID_VALUE);
        return nullptr;
    }
    const bool read = access & GL_MAP_READ_BIT, write = access & GL_MAP_WRITE_BIT;
    if (buffer->mapped || (!read && !write) ||
        (read && (access & (GL_MAP_INVALIDATE_RANGE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT | GL_MAP_UNSYNCHRONIZED_BIT))) ||
        ((access & GL_MAP_FLUSH_EXPLICIT_BIT) && !write)) {
        ctx.setError(GL_INVALID_OPERATION);
        return nullptr;
    }
    if (!buffer->mtl) {
        ctx.setError(GL_OUT_OF_MEMORY);
        return nullptr;
    }
    if (!(access & GL_MAP_UNSYNCHRONIZED_BIT) && gpuBusy(ctx, *buffer)) {
        if (access & GL_MAP_INVALIDATE_BUFFER_BIT) rename(ctx, *buffer, false);
        else if ((access & GL_MAP_INVALIDATE_RANGE_BIT) && !read) rename(ctx, *buffer, true);
        else ctx.stream().waitFor(buffer->lastUseSerial);
    }
    buffer->mapped = true;
    buffer->mapAccess = access;
    buffer->mapOffset = offset;
    buffer->mapLength = length;
    buffer->mapPointer = static_cast<uint8_t*>(buffer->mtl.contents) + offset;
    return buffer->mapPointer;
}

void* MapBuffer(Context& ctx, GLenum target, GLenum access) {
    Buffer* buffer = boundBuffer(ctx, target);
    if (!buffer) return nullptr;
    GLbitfield bits;
    switch (access) {
    case GL_READ_ONLY: bits = GL_MAP_READ_BIT; break;
    case GL_WRITE_ONLY: bits = GL_MAP_WRITE_BIT; break;
    case GL_READ_WRITE: bits = GL_MAP_READ_BIT | GL_MAP_WRITE_BIT; break;
    default: ctx.setError(GL_INVALID_ENUM); return nullptr;
    }
    if (buffer->size == 0) {
        // Mapping an empty buffer succeeds with a null pointer in practice; GL says INVALID_VALUE for MapBufferRange only.
        if (buffer->mapped) { ctx.setError(GL_INVALID_OPERATION); return nullptr; }
        buffer->mapped = true;
        buffer->mapAccess = bits;
        return nullptr;
    }
    return MapBufferRange(ctx, target, 0, buffer->size, bits);
}

GLboolean UnmapBuffer(Context& ctx, GLenum target) {
    Buffer* buffer = boundBuffer(ctx, target);
    if (!buffer) return GL_FALSE;
    if (!buffer->mapped) {
        ctx.setError(GL_INVALID_OPERATION);
        return GL_FALSE;
    }
    unmapBuffer(*buffer);
    ++buffer->version;
    return GL_TRUE;
}

void FlushMappedBufferRange(Context& ctx, GLenum target, GLintptr offset, GLsizeiptr length) {
    Buffer* buffer = boundBuffer(ctx, target);
    if (!buffer) return;
    if (!buffer->mapped || !(buffer->mapAccess & GL_MAP_FLUSH_EXPLICIT_BIT)) return ctx.setError(GL_INVALID_OPERATION);
    if (offset < 0 || length < 0 || offset + length > buffer->mapLength) return ctx.setError(GL_INVALID_VALUE);
    ++buffer->version;  // shared storage: the CPU writes are already visible
}

void CopyBufferSubData(Context& ctx, GLenum readTarget, GLenum writeTarget, GLintptr readOffset, GLintptr writeOffset,
                       GLsizeiptr size) {
    Buffer* src = boundBuffer(ctx, readTarget);
    if (!src) return;
    Buffer* dst = boundBuffer(ctx, writeTarget);
    if (!dst) return;
    if (readOffset < 0 || writeOffset < 0 || size < 0 || readOffset + size > src->size || writeOffset + size > dst->size)
        return ctx.setError(GL_INVALID_VALUE);
    if (src == dst && std::max(readOffset, writeOffset) < std::min(readOffset, writeOffset) + size)
        return ctx.setError(GL_INVALID_VALUE);
    if (src->mapped || dst->mapped) return ctx.setError(GL_INVALID_OPERATION);
    if (!size) return;
    [ctx.stream().blit() copyFromBuffer:src->mtl sourceOffset:static_cast<NSUInteger>(readOffset) toBuffer:dst->mtl
                        destinationOffset:static_cast<NSUInteger>(writeOffset) size:static_cast<NSUInteger>(size)];
    src->lastUseSerial = dst->lastUseSerial = dst->lastWriteSerial = ctx.stream().serial();
    ++dst->version;
}

void GetBufferParameteri64v(Context& ctx, GLenum target, GLenum pname, GLint64* params) {
    Buffer* buffer = boundBuffer(ctx, target);
    if (!buffer || !params) return;
    switch (pname) {
    case GL_BUFFER_SIZE: *params = buffer->size; return;
    case GL_BUFFER_USAGE: *params = buffer->usage; return;
    case GL_BUFFER_ACCESS:
        *params = (buffer->mapAccess & GL_MAP_READ_BIT) && (buffer->mapAccess & GL_MAP_WRITE_BIT) ? GL_READ_WRITE
                  : (buffer->mapAccess & GL_MAP_READ_BIT)                                          ? GL_READ_ONLY
                                                                                                   : GL_WRITE_ONLY;
        if (!buffer->mapped) *params = GL_READ_WRITE;
        return;
    case GL_BUFFER_ACCESS_FLAGS: *params = buffer->mapped ? buffer->mapAccess : 0; return;
    case GL_BUFFER_MAPPED: *params = buffer->mapped ? GL_TRUE : GL_FALSE; return;
    case GL_BUFFER_MAP_OFFSET: *params = buffer->mapped ? buffer->mapOffset : 0; return;
    case GL_BUFFER_MAP_LENGTH: *params = buffer->mapped ? buffer->mapLength : 0; return;
    default: break;
    }
    if (ctx.version() >= 44) {
        if (pname == GL_BUFFER_IMMUTABLE_STORAGE) { *params = buffer->immutable; return; }
        if (pname == GL_BUFFER_STORAGE_FLAGS) { *params = buffer->storageFlags; return; }
    }
    ctx.setError(GL_INVALID_ENUM);
}

void GetBufferParameteriv(Context& ctx, GLenum target, GLenum pname, GLint* params) {
    GLint64 value = 0;
    const GLenum before = ctx.takeError();
    GetBufferParameteri64v(ctx, target, pname, &value);
    const GLenum error = ctx.takeError();
    if (before) ctx.setError(before);
    if (error) return ctx.setError(error);
    if (params) *params = static_cast<GLint>(std::clamp<GLint64>(value, INT32_MIN, INT32_MAX));
}

void GetBufferPointerv(Context& ctx, GLenum target, GLenum pname, void** params) {
    if (pname != GL_BUFFER_MAP_POINTER) return ctx.setError(GL_INVALID_ENUM);
    Buffer* buffer = boundBuffer(ctx, target);
    if (!buffer || !params) return;
    *params = buffer->mapped ? buffer->mapPointer : nullptr;
}

}  // namespace wmgl::api
