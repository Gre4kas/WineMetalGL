// Drawing commands (GL 3.3 section 2.8.3).

#include <cstring>
#include <vector>

#include "core/compat/immediate.h"
#include "core/context.h"
#include "core/draw.h"
#include "gl_api.gen.h"

namespace wmgl::api {
namespace {

// A compiled draw keeps every attribute of every vertex (464 bytes each):
// larger draws are refused with GL_OUT_OF_MEMORY rather than exhausting memory.
constexpr GLsizei kMaxCompiledDrawVertices = 1 << 20;

// Vertex array draws compiled into a display list read the arrays now
// (compat/immediate.h). True if the draw must also run now.
bool compileArrays(Context& ctx, GLenum mode, GLint first, GLsizei count) {
    if (!compat::listCompiling(ctx) || first < 0 || count < 0) return true;
    if (count > kMaxCompiledDrawVertices) return ctx.setError(GL_OUT_OF_MEMORY), false;
    std::vector<uint32_t> indices(static_cast<size_t>(count));
    for (GLsizei i = 0; i < count; ++i) indices[static_cast<size_t>(i)] = static_cast<uint32_t>(first + i);
    return compat::compileArrayDraw(ctx, mode, indices);
}

bool compileElements(Context& ctx, GLenum mode, GLsizei count, GLenum type, const void* indices, GLint baseVertex) {
    if (!compat::listCompiling(ctx) || count < 0) return true;
    if (count > kMaxCompiledDrawVertices) return ctx.setError(GL_OUT_OF_MEMORY), false;
    const size_t size = type == GL_UNSIGNED_BYTE ? 1 : type == GL_UNSIGNED_SHORT ? 2 : type == GL_UNSIGNED_INT ? 4 : 0;
    if (!size) return true;  // the draw reports the error
    const uint8_t* source = static_cast<const uint8_t*>(indices);
    if (Buffer* elements = ctx.state.vertexArray->elementBuffer.get()) {
        // Indices past the end of the buffer draw nothing, as in DrawState::drawElements.
        const uintptr_t offset = reinterpret_cast<uintptr_t>(indices);
        if (!elements->mtl || offset + static_cast<size_t>(count) * size > static_cast<size_t>(elements->size))
            return false;
        if (elements->lastWriteSerial) ctx.stream().waitFor(elements->lastWriteSerial);
        source = elements->contents() + offset;
    }
    if (!source) return true;
    std::vector<uint32_t> list(static_cast<size_t>(count));
    for (GLsizei i = 0; i < count; ++i) {
        uint32_t v = 0;
        std::memcpy(&v, source + static_cast<size_t>(i) * size, size);
        list[static_cast<size_t>(i)] = v + static_cast<uint32_t>(baseVertex);
    }
    return compat::compileArrayDraw(ctx, mode, list);
}

}  // namespace

void DrawArrays(Context& ctx, GLenum mode, GLint first, GLsizei count) {
    if (compileArrays(ctx, mode, first, count)) ctx.draw().drawArrays(mode, first, count, 1);
}

void DrawArraysInstanced(Context& ctx, GLenum mode, GLint first, GLsizei count, GLsizei instancecount) {
    ctx.draw().drawArrays(mode, first, count, instancecount);
}

void DrawElements(Context& ctx, GLenum mode, GLsizei count, GLenum type, const void* indices) {
    if (compileElements(ctx, mode, count, type, indices, 0)) ctx.draw().drawElements(mode, count, type, indices, 1, 0);
}

void DrawElementsInstanced(Context& ctx, GLenum mode, GLsizei count, GLenum type, const void* indices, GLsizei instancecount) {
    ctx.draw().drawElements(mode, count, type, indices, instancecount, 0);
}

void DrawElementsBaseVertex(Context& ctx, GLenum mode, GLsizei count, GLenum type, const void* indices, GLint basevertex) {
    ctx.draw().drawElements(mode, count, type, indices, 1, basevertex);
}

void DrawElementsInstancedBaseVertex(Context& ctx, GLenum mode, GLsizei count, GLenum type, const void* indices,
                                     GLsizei instancecount, GLint basevertex) {
    ctx.draw().drawElements(mode, count, type, indices, instancecount, basevertex);
}

void DrawRangeElements(Context& ctx, GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const void* indices) {
    if (end < start) return ctx.setError(GL_INVALID_VALUE);
    if (compileElements(ctx, mode, count, type, indices, 0)) ctx.draw().drawElements(mode, count, type, indices, 1, 0);
}

void DrawRangeElementsBaseVertex(Context& ctx, GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type,
                                 const void* indices, GLint basevertex) {
    if (end < start) return ctx.setError(GL_INVALID_VALUE);
    ctx.draw().drawElements(mode, count, type, indices, 1, basevertex);
}

// GL_ARB_base_instance
void DrawArraysInstancedBaseInstance(Context& ctx, GLenum mode, GLint first, GLsizei count, GLsizei instancecount,
                                     GLuint baseinstance) {
    DrawState::BaseInstanceScope scope(ctx.draw(), baseinstance);
    ctx.draw().drawArrays(mode, first, count, instancecount);
}

void DrawElementsInstancedBaseInstance(Context& ctx, GLenum mode, GLsizei count, GLenum type, const void* indices,
                                       GLsizei instancecount, GLuint baseinstance) {
    DrawState::BaseInstanceScope scope(ctx.draw(), baseinstance);
    ctx.draw().drawElements(mode, count, type, indices, instancecount, 0);
}

void DrawElementsInstancedBaseVertexBaseInstance(Context& ctx, GLenum mode, GLsizei count, GLenum type, const void* indices,
                                                 GLsizei instancecount, GLint basevertex, GLuint baseinstance) {
    DrawState::BaseInstanceScope scope(ctx.draw(), baseinstance);
    ctx.draw().drawElements(mode, count, type, indices, instancecount, basevertex);
}

// GL 4.0 / GL_ARB_draw_indirect. The command is read from DRAW_INDIRECT_BUFFER
// on the CPU, after any GPU write to it completes, and drawn like the direct
// command with the same parameters.
namespace {

bool drawIndirect(Context& ctx) { return ctx.version() >= 40 || ctx.hasExtension("GL_ARB_draw_indirect"); }

// The `words` 32-bit words of the command at `indirect`, or false with the GL error set.
bool readIndirectCommand(Context& ctx, const void* indirect, uint32_t* words, size_t count) {
    const uintptr_t offset = reinterpret_cast<uintptr_t>(indirect);
    Buffer* buffer = ctx.state.drawIndirectBuffer.get();
    if (!buffer) {
        // The compatibility profile may source the command from client memory.
        if (ctx.isCore() || !indirect) return ctx.setError(GL_INVALID_OPERATION), false;
        std::memcpy(words, indirect, count * 4);
        return true;
    }
    if (offset % 4) return ctx.setError(GL_INVALID_VALUE), false;
    if (buffer->mappedNonPersistent()) return ctx.setError(GL_INVALID_OPERATION), false;
    if (!buffer->mtl || offset + count * 4 > static_cast<uint64_t>(buffer->size)) return ctx.setError(GL_INVALID_OPERATION), false;
    if (buffer->lastWriteSerial) waitForGpuUnlocked(ctx, buffer->lastWriteSerial);
    std::memcpy(words, buffer->contents() + offset, count * 4);
    return true;
}

bool validIndirectMode(Context& ctx, GLenum mode) {
    switch (mode) {
    case GL_POINTS: case GL_LINES: case GL_LINE_LOOP: case GL_LINE_STRIP: case GL_TRIANGLES: case GL_TRIANGLE_STRIP:
    case GL_TRIANGLE_FAN: case GL_LINES_ADJACENCY: case GL_LINE_STRIP_ADJACENCY: case GL_TRIANGLES_ADJACENCY:
    case GL_TRIANGLE_STRIP_ADJACENCY: case GL_PATCHES:
        return true;
    default:
        ctx.setError(GL_INVALID_ENUM);
        return false;
    }
}

}  // namespace

void DrawArraysIndirect(Context& ctx, GLenum mode, const void* indirect) {
    if (!drawIndirect(ctx)) return ctx.unimplemented("glDrawArraysIndirect");
    if (!validIndirectMode(ctx, mode)) return;
    // Core profile: a vertex array object must be bound.
    if (ctx.isCore() && ctx.state.vertexArray == ctx.defaultVertexArray) return ctx.setError(GL_INVALID_OPERATION);
    uint32_t command[4];  // count, instance count, first, base instance
    if (!readIndirectCommand(ctx, indirect, command, 4)) return;
    if (command[0] > INT32_MAX || command[1] > INT32_MAX || command[2] > INT32_MAX) return;
    DrawState::BaseInstanceScope scope(ctx.draw(), command[3]);
    ctx.draw().drawArrays(mode, static_cast<GLint>(command[2]), static_cast<GLsizei>(command[0]), static_cast<GLsizei>(command[1]));
}

void DrawElementsIndirect(Context& ctx, GLenum mode, GLenum type, const void* indirect) {
    if (!drawIndirect(ctx)) return ctx.unimplemented("glDrawElementsIndirect");
    if (!validIndirectMode(ctx, mode)) return;
    if (type != GL_UNSIGNED_BYTE && type != GL_UNSIGNED_SHORT && type != GL_UNSIGNED_INT) return ctx.setError(GL_INVALID_ENUM);
    if (ctx.isCore() && ctx.state.vertexArray == ctx.defaultVertexArray) return ctx.setError(GL_INVALID_OPERATION);
    if (!ctx.state.vertexArray->elementBuffer) return ctx.setError(GL_INVALID_OPERATION);
    uint32_t command[5];  // count, instance count, first index, base vertex, base instance
    if (!readIndirectCommand(ctx, indirect, command, 5)) return;
    if (command[0] > INT32_MAX || command[1] > INT32_MAX) return;
    const size_t size = type == GL_UNSIGNED_BYTE ? 1 : type == GL_UNSIGNED_SHORT ? 2 : 4;
    const uintptr_t offset = static_cast<uintptr_t>(command[2]) * size;
    DrawState::BaseInstanceScope scope(ctx.draw(), command[4]);
    ctx.draw().drawElements(mode, static_cast<GLsizei>(command[0]), type, reinterpret_cast<const void*>(offset),
                            static_cast<GLsizei>(command[1]), static_cast<GLint>(command[3]));
}

void MultiDrawArrays(Context& ctx, GLenum mode, const GLint* first, const GLsizei* count, GLsizei drawcount) {
    if (drawcount < 0) return ctx.setError(GL_INVALID_VALUE);
    for (GLsizei i = 0; i < drawcount; ++i)
        if (compileArrays(ctx, mode, first[i], count[i])) ctx.draw().drawArrays(mode, first[i], count[i], 1);
}

void MultiDrawElements(Context& ctx, GLenum mode, const GLsizei* count, GLenum type, const void* const* indices,
                       GLsizei drawcount) {
    if (drawcount < 0) return ctx.setError(GL_INVALID_VALUE);
    for (GLsizei i = 0; i < drawcount; ++i)
        if (compileElements(ctx, mode, count[i], type, indices[i], 0)) ctx.draw().drawElements(mode, count[i], type, indices[i], 1, 0);
}

void MultiDrawElementsBaseVertex(Context& ctx, GLenum mode, const GLsizei* count, GLenum type, const void* const* indices,
                                 GLsizei drawcount, const GLint* basevertex) {
    if (drawcount < 0) return ctx.setError(GL_INVALID_VALUE);
    // Errors of any draw are reported before anything draws (and before the
    // other arrays are read).
    for (GLsizei i = 0; i < drawcount; ++i)
        if (count[i] < 0) return ctx.setError(GL_INVALID_VALUE);
    if (type != GL_UNSIGNED_BYTE && type != GL_UNSIGNED_SHORT && type != GL_UNSIGNED_INT) return ctx.setError(GL_INVALID_ENUM);
    for (GLsizei i = 0; i < drawcount; ++i)
        ctx.draw().drawElements(mode, count[i], type, indices[i], 1, basevertex ? basevertex[i] : 0);
}

void Clear(Context& ctx, GLbitfield mask) { ctx.draw().clear(mask); }

namespace {

bool checkDrawBuffer(Context& ctx, GLenum buffer, GLint drawbuffer) {
    const bool color = buffer == GL_COLOR;
    if (drawbuffer < 0 || (color && drawbuffer >= ContextState::kMaxDrawBuffers) || (!color && drawbuffer != 0)) {
        ctx.setError(GL_INVALID_VALUE);
        return false;
    }
    return true;
}

}  // namespace

void ClearBufferiv(Context& ctx, GLenum buffer, GLint drawbuffer, const GLint* value) {
    if (buffer != GL_COLOR && buffer != GL_STENCIL) return ctx.setError(GL_INVALID_ENUM);
    if (!checkDrawBuffer(ctx, buffer, drawbuffer)) return;
    if (buffer == GL_STENCIL) return ctx.draw().clearBuffer(GL_STENCIL, 0, nullptr, 0.0, value[0]);
    const double color[4] = {static_cast<double>(value[0]), static_cast<double>(value[1]), static_cast<double>(value[2]),
                             static_cast<double>(value[3])};
    ctx.draw().clearBuffer(GL_COLOR, drawbuffer, color, 0.0, 0);
}

void ClearBufferuiv(Context& ctx, GLenum buffer, GLint drawbuffer, const GLuint* value) {
    if (buffer != GL_COLOR) return ctx.setError(GL_INVALID_ENUM);
    if (!checkDrawBuffer(ctx, buffer, drawbuffer)) return;
    const double color[4] = {static_cast<double>(value[0]), static_cast<double>(value[1]), static_cast<double>(value[2]),
                             static_cast<double>(value[3])};
    ctx.draw().clearBuffer(GL_COLOR, drawbuffer, color, 0.0, 0);
}

void ClearBufferfv(Context& ctx, GLenum buffer, GLint drawbuffer, const GLfloat* value) {
    if (buffer != GL_COLOR && buffer != GL_DEPTH) return ctx.setError(GL_INVALID_ENUM);
    if (!checkDrawBuffer(ctx, buffer, drawbuffer)) return;
    if (buffer == GL_DEPTH) return ctx.draw().clearBuffer(GL_DEPTH, 0, nullptr, value[0], 0);
    const double color[4] = {value[0], value[1], value[2], value[3]};
    ctx.draw().clearBuffer(GL_COLOR, drawbuffer, color, 0.0, 0);
}

void ClearBufferfi(Context& ctx, GLenum buffer, GLint drawbuffer, GLfloat depth, GLint stencil) {
    if (buffer != GL_DEPTH_STENCIL) return ctx.setError(GL_INVALID_ENUM);
    if (!checkDrawBuffer(ctx, buffer, drawbuffer)) return;
    ctx.draw().clearBuffer(GL_DEPTH_STENCIL, 0, nullptr, depth, stencil);
}

}  // namespace wmgl::api
