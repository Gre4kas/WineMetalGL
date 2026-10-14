// Drawing commands (GL 3.3 section 2.8.3).

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/compat/immediate.h"
#include "core/context.h"
#include "core/draw.h"
#include "core/stats.h"
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
    const auto fits = [&] {
        const uint64_t size = static_cast<uint64_t>(std::max<GLsizeiptr>(0, buffer->size));
        return buffer->mtl && offset <= size && count * 4 <= size - offset;
    };
    if (!fits()) return ctx.setError(GL_INVALID_OPERATION), false;
    stats::count(&stats::Frame::indirectCpu);
    if (buffer->lastWriteSerial && !ctx.stream().queue().isComplete(buffer->lastWriteSerial)) {
        stats::count(&stats::Frame::indirectWaits);
        ctx.stream().noteIndirectWait();
    }
    if (buffer->lastWriteSerial) waitForGpuUnlocked(ctx, buffer->lastWriteSerial);
    // Another context may have specified the buffer again during the wait.
    if (!fits()) return ctx.setError(GL_INVALID_OPERATION), false;
    std::memcpy(words, buffer->contents() + offset, count * 4);
    // Both command structures start with the vertex and instance counts.
    if (!words[0] || !words[1]) stats::count(&stats::Frame::indirectCpuEmpty);
    return true;
}

// Draws the command of `count` words at `indirect` in DRAW_INDIRECT_BUFFER
// with the GPU reading it (DrawState::drawIndirect), so commands that shaders
// write (GPU culling) draw without waiting for them. False when the draw
// takes the CPU path instead, which also reports the GL errors.
bool drawIndirectOnGpu(Context& ctx, GLenum mode, GLenum type, const void* indirect, size_t count) {
    const uintptr_t offset = reinterpret_cast<uintptr_t>(indirect);
    Buffer* buffer = ctx.state.drawIndirectBuffer.get();
    const uint64_t size = buffer ? static_cast<uint64_t>(std::max<GLsizeiptr>(0, buffer->size)) : 0;
    if (!buffer || !buffer->mtl || offset % 4 || buffer->mappedNonPersistent() || offset > size || count * 4 > size - offset) {
        if (stats::g_enabled) {
            char detail[128];
            std::snprintf(detail, sizeof(detail), "command buffer %u (size %llu, mapped %d), offset %llu", buffer ? buffer->name : 0,
                          static_cast<unsigned long long>(size), buffer && buffer->mapped, static_cast<unsigned long long>(offset));
            stats::indirectCpu(stats::kIndirectCommands, std::to_string(buffer ? buffer->name : 0), detail);
        }
        return false;
    }
    return ctx.draw().drawIndirect(mode, type, *buffer, offset);
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

namespace {

// The CPU path of the command at `indirect`: read, then drawn like the
// direct command. With `skipEmpty`, a command that draws nothing (no
// vertices or no instances) is skipped without validating the draw: an
// earlier command of the same multi-draw, with the same state, reported
// its errors.
void drawArraysCommand(Context& ctx, GLenum mode, const void* indirect, bool skipEmpty) {
    uint32_t command[4];  // count, instance count, first, base instance
    if (!readIndirectCommand(ctx, indirect, command, 4)) return;
    if (skipEmpty && (!command[0] || !command[1])) return;
    if (command[0] > INT32_MAX || command[1] > INT32_MAX || command[2] > INT32_MAX) return;
    DrawState::BaseInstanceScope scope(ctx.draw(), command[3]);
    ctx.draw().drawArrays(mode, static_cast<GLint>(command[2]), static_cast<GLsizei>(command[0]), static_cast<GLsizei>(command[1]));
}

// Draws a DrawElementsIndirectCommand already read.
void drawElementsRead(Context& ctx, GLenum mode, GLenum type, const uint32_t command[5]) {
    if (command[0] > INT32_MAX || command[1] > INT32_MAX) return;
    const size_t size = type == GL_UNSIGNED_BYTE ? 1 : type == GL_UNSIGNED_SHORT ? 2 : 4;
    const uintptr_t offset = static_cast<uintptr_t>(command[2]) * size;
    DrawState::BaseInstanceScope scope(ctx.draw(), command[4]);
    ctx.draw().drawElements(mode, static_cast<GLsizei>(command[0]), type, reinterpret_cast<const void*>(offset),
                            static_cast<GLsizei>(command[1]), static_cast<GLint>(command[3]));
}

void drawElementsCommand(Context& ctx, GLenum mode, GLenum type, const void* indirect, bool skipEmpty) {
    uint32_t command[5];  // count, instance count, first index, base vertex, base instance
    if (!readIndirectCommand(ctx, indirect, command, 5)) return;
    if (skipEmpty && (!command[0] || !command[1])) return;
    drawElementsRead(ctx, mode, type, command);
}

}  // namespace

void DrawArraysIndirect(Context& ctx, GLenum mode, const void* indirect) {
    if (!drawIndirect(ctx)) return ctx.unimplemented("glDrawArraysIndirect");
    if (!validIndirectMode(ctx, mode)) return;
    // Core profile: a vertex array object must be bound.
    if (ctx.isCore() && ctx.state.vertexArray == ctx.defaultVertexArray) return ctx.setError(GL_INVALID_OPERATION);
    if (drawIndirectOnGpu(ctx, mode, GL_NONE, indirect, 4)) return;
    drawArraysCommand(ctx, mode, indirect, false);
}

void DrawElementsIndirect(Context& ctx, GLenum mode, GLenum type, const void* indirect) {
    if (!drawIndirect(ctx)) return ctx.unimplemented("glDrawElementsIndirect");
    if (!validIndirectMode(ctx, mode)) return;
    if (type != GL_UNSIGNED_BYTE && type != GL_UNSIGNED_SHORT && type != GL_UNSIGNED_INT) return ctx.setError(GL_INVALID_ENUM);
    if (ctx.isCore() && ctx.state.vertexArray == ctx.defaultVertexArray) return ctx.setError(GL_INVALID_OPERATION);
    if (!ctx.state.vertexArray->elementBuffer) return ctx.setError(GL_INVALID_OPERATION);
    if (drawIndirectOnGpu(ctx, mode, type, indirect, 5)) return;
    drawElementsCommand(ctx, mode, type, indirect, false);
}

// GL 4.3 / GL_ARB_multi_draw_indirect: each command as its own indirect
// draw (read by the GPU or the CPU, as for the single-draw forms), with
// gl_DrawID its index. The commands share the draw state (validMultiDraw
// checked the rest): once one takes the CPU path, so do the others, without
// trying the GPU's, and those that draw nothing (GPU culling leaves most
// commands empty) cost a read.
namespace {

// INVALID_OPERATION, before anything draws, when `drawcount` commands of
// `size` bytes `step` apart from `indirect` don't fit in DRAW_INDIRECT_BUFFER.
bool indirectCommandsFit(Context& ctx, const void* indirect, GLsizei drawcount, uintptr_t step, uintptr_t size) {
    Buffer* buffer = ctx.state.drawIndirectBuffer.get();
    if (!buffer || drawcount <= 0) return true;  // client memory (compatibility profile), or nothing to read
    uint64_t end = 0;
    if (__builtin_mul_overflow(step, static_cast<uint64_t>(drawcount - 1), &end) ||
        __builtin_add_overflow(end, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(indirect)), &end) ||
        __builtin_add_overflow(end, static_cast<uint64_t>(size), &end) || end > static_cast<uint64_t>(buffer->size))
        return ctx.setError(GL_INVALID_OPERATION), false;
    return true;
}

void multiDrawArraysIndirect(Context& ctx, GLenum mode, const void* indirect, GLsizei drawcount, uintptr_t step) {
    bool cpu = false;
    for (GLsizei i = 0; i < drawcount; ++i) {
        DrawState::DrawIdScope id(ctx.draw(), i);
        const void* at = reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(indirect) + step * static_cast<uintptr_t>(i));
        if (cpu) stats::repeatIndirectCpu();
        else if (drawIndirectOnGpu(ctx, mode, GL_NONE, at, 4)) continue;
        drawArraysCommand(ctx, mode, at, cpu);
        cpu = true;
    }
}

void multiDrawElementsIndirect(Context& ctx, GLenum mode, GLenum type, const void* indirect, GLsizei drawcount, uintptr_t step) {
    bool cpu = false;
    // Patches on the CPU path: commands that draw one instance of the same
    // whole number of patches are drawn as one batch (one control dispatch
    // and tessellation draw instead of one each per command; one vertex
    // capture per base instance), in command order. Foundation gives each
    // terrain tile a base instance of its own (issue #12).
    bool batching = false, direct = false;
    std::vector<std::array<uint32_t, 5>> batch;
    auto flush = [&] {
        if (batch.empty()) return;
        ctx.draw().drawPatchCommands(type, reinterpret_cast<const uint32_t(*)[5]>(batch.data()), batch.size());
        batch.clear();
    };
    const uint32_t patchVertices = static_cast<uint32_t>(std::max(1, ctx.state.patchVertices));
    for (GLsizei i = 0; i < drawcount; ++i) {
        DrawState::DrawIdScope id(ctx.draw(), i);
        const void* at = reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(indirect) + step * static_cast<uintptr_t>(i));
        if (cpu) stats::repeatIndirectCpu();
        else if (drawIndirectOnGpu(ctx, mode, type, at, 5)) continue;
        if (batching) {
            std::array<uint32_t, 5> command;
            // After the first read (which waited for GPU writes to the
            // commands), the rest come straight from the buffer: the
            // commands were checked to fit before the loop, and the buffer
            // stays as it is while the lock is held.
            Buffer* buffer = ctx.state.drawIndirectBuffer.get();
            const uintptr_t offset = reinterpret_cast<uintptr_t>(at);
            if (direct && buffer && buffer->mtl && offset + 20 <= static_cast<uint64_t>(buffer->size)) {
                std::memcpy(command.data(), buffer->contents() + offset, 20);
                stats::count(&stats::Frame::indirectCpu);
                if (!command[0] || !command[1]) stats::count(&stats::Frame::indirectCpuEmpty);
            } else if (!readIndirectCommand(ctx, at, command.data(), 5)) {
                break;
            } else {
                direct = buffer != nullptr;
            }
            if (!command[0] || !command[1]) continue;  // draws nothing
            const bool fits = command[1] == 1 && command[0] % patchVertices == 0 && command[0] <= INT32_MAX &&
                              (batch.empty() || command[0] == batch[0][0]);
            if (fits) {
                batch.push_back(command);
                continue;
            }
            flush();
            if (command[1] == 1 && command[0] % patchVertices == 0 && command[0] <= INT32_MAX) batch.push_back(command);
            else drawElementsRead(ctx, mode, type, command.data());
            continue;
        }
        drawElementsCommand(ctx, mode, type, at, cpu);
        // After the first command (which reported the state's errors), the
        // state decides whether the rest can be batched.
        if (!cpu) batching = mode == GL_PATCHES && ctx.draw().canBatchPatchCommands();
        cpu = true;
    }
    flush();
}

// GL 4.6 / GL_ARB_indirect_parameters: the draw count is the uint at
// `offset` in PARAMETER_BUFFER (read on the CPU after any GPU write to it),
// at most `maxdrawcount`. -1 with the GL error set.
GLsizei parameterDrawCount(Context& ctx, GLintptr offset, GLsizei maxdrawcount) {
    if (offset < 0 || offset % 4 || maxdrawcount < 0) return ctx.setError(GL_INVALID_VALUE), -1;
    Buffer* buffer = ctx.state.parameterBuffer.get();
    if (!buffer || !buffer->mtl || 4 > buffer->size - offset || buffer->mappedNonPersistent())
        return ctx.setError(GL_INVALID_OPERATION), -1;
    if (buffer->lastWriteSerial) waitForGpuUnlocked(ctx, buffer->lastWriteSerial);
    // Another context may have specified the buffer again during the wait.
    if (!buffer->mtl || 4 > buffer->size - offset) return ctx.setError(GL_INVALID_OPERATION), -1;
    uint32_t count = 0;
    std::memcpy(&count, buffer->contents() + offset, 4);
    return static_cast<GLsizei>(std::min<uint32_t>(count, static_cast<uint32_t>(maxdrawcount)));
}

// The errors each command of a multi-draw would generate, checked once
// before any command is read, so that they are generated when none draws.
bool validMultiDraw(Context& ctx, GLenum mode, const GLenum* type, const void* indirect) {
    if (!validIndirectMode(ctx, mode)) return false;
    if (type && *type != GL_UNSIGNED_BYTE && *type != GL_UNSIGNED_SHORT && *type != GL_UNSIGNED_INT)
        return ctx.setError(GL_INVALID_ENUM), false;
    if (ctx.isCore() && ctx.state.vertexArray == ctx.defaultVertexArray) return ctx.setError(GL_INVALID_OPERATION), false;
    if (type && !ctx.state.vertexArray->elementBuffer) return ctx.setError(GL_INVALID_OPERATION), false;
    Buffer* buffer = ctx.state.drawIndirectBuffer.get();
    if (!buffer && ctx.isCore()) return ctx.setError(GL_INVALID_OPERATION), false;
    if (buffer && reinterpret_cast<uintptr_t>(indirect) % 4) return ctx.setError(GL_INVALID_VALUE), false;
    return true;
}

}  // namespace

void MultiDrawArraysIndirect(Context& ctx, GLenum mode, const void* indirect, GLsizei drawcount, GLsizei stride) {
    if (ctx.version() < 43 && !ctx.hasExtension("GL_ARB_multi_draw_indirect")) return ctx.unimplemented("glMultiDrawArraysIndirect");
    if (drawcount < 0 || stride < 0 || stride % 4) return ctx.setError(GL_INVALID_VALUE);
    if (!validMultiDraw(ctx, mode, nullptr, indirect)) return;
    const uintptr_t step = stride ? static_cast<uintptr_t>(stride) : 16;
    if (!indirectCommandsFit(ctx, indirect, drawcount, step, 16)) return;
    multiDrawArraysIndirect(ctx, mode, indirect, drawcount, step);
}

void MultiDrawElementsIndirect(Context& ctx, GLenum mode, GLenum type, const void* indirect, GLsizei drawcount, GLsizei stride) {
    if (ctx.version() < 43 && !ctx.hasExtension("GL_ARB_multi_draw_indirect")) return ctx.unimplemented("glMultiDrawElementsIndirect");
    if (drawcount < 0 || stride < 0 || stride % 4) return ctx.setError(GL_INVALID_VALUE);
    if (!validMultiDraw(ctx, mode, &type, indirect)) return;
    const uintptr_t step = stride ? static_cast<uintptr_t>(stride) : 20;
    if (!indirectCommandsFit(ctx, indirect, drawcount, step, 20)) return;
    multiDrawElementsIndirect(ctx, mode, type, indirect, drawcount, step);
}

void MultiDrawArraysIndirectCount(Context& ctx, GLenum mode, const void* indirect, GLintptr drawcount, GLsizei maxdrawcount,
                                  GLsizei stride) {
    if (!ctx.indirectParameters()) return ctx.unimplemented("glMultiDrawArraysIndirectCount");
    if (stride < 0 || stride % 4) return ctx.setError(GL_INVALID_VALUE);
    if (!validMultiDraw(ctx, mode, nullptr, indirect)) return;
    const uintptr_t step = stride ? static_cast<uintptr_t>(stride) : 16;
    const GLsizei count = parameterDrawCount(ctx, drawcount, maxdrawcount);
    if (count < 0 || !indirectCommandsFit(ctx, indirect, maxdrawcount, step, 16)) return;
    multiDrawArraysIndirect(ctx, mode, indirect, count, step);
}

void MultiDrawElementsIndirectCount(Context& ctx, GLenum mode, GLenum type, const void* indirect, GLintptr drawcount,
                                    GLsizei maxdrawcount, GLsizei stride) {
    if (!ctx.indirectParameters()) return ctx.unimplemented("glMultiDrawElementsIndirectCount");
    if (stride < 0 || stride % 4) return ctx.setError(GL_INVALID_VALUE);
    if (!validMultiDraw(ctx, mode, &type, indirect)) return;
    const uintptr_t step = stride ? static_cast<uintptr_t>(stride) : 20;
    const GLsizei count = parameterDrawCount(ctx, drawcount, maxdrawcount);
    if (count < 0 || !indirectCommandsFit(ctx, indirect, maxdrawcount, step, 20)) return;
    multiDrawElementsIndirect(ctx, mode, type, indirect, count, step);
}

void MultiDrawArrays(Context& ctx, GLenum mode, const GLint* first, const GLsizei* count, GLsizei drawcount) {
    if (drawcount < 0) return ctx.setError(GL_INVALID_VALUE);
    for (GLsizei i = 0; i < drawcount; ++i) {
        DrawState::DrawIdScope id(ctx.draw(), i);
        if (compileArrays(ctx, mode, first[i], count[i])) ctx.draw().drawArrays(mode, first[i], count[i], 1);
    }
}

void MultiDrawElements(Context& ctx, GLenum mode, const GLsizei* count, GLenum type, const void* const* indices,
                       GLsizei drawcount) {
    if (drawcount < 0) return ctx.setError(GL_INVALID_VALUE);
    for (GLsizei i = 0; i < drawcount; ++i) {
        DrawState::DrawIdScope id(ctx.draw(), i);
        if (compileElements(ctx, mode, count[i], type, indices[i], 0)) ctx.draw().drawElements(mode, count[i], type, indices[i], 1, 0);
    }
}

void MultiDrawElementsBaseVertex(Context& ctx, GLenum mode, const GLsizei* count, GLenum type, const void* const* indices,
                                 GLsizei drawcount, const GLint* basevertex) {
    if (drawcount < 0) return ctx.setError(GL_INVALID_VALUE);
    // Errors of any draw are reported before anything draws (and before the
    // other arrays are read).
    for (GLsizei i = 0; i < drawcount; ++i)
        if (count[i] < 0) return ctx.setError(GL_INVALID_VALUE);
    if (type != GL_UNSIGNED_BYTE && type != GL_UNSIGNED_SHORT && type != GL_UNSIGNED_INT) return ctx.setError(GL_INVALID_ENUM);
    for (GLsizei i = 0; i < drawcount; ++i) {
        DrawState::DrawIdScope id(ctx.draw(), i);
        ctx.draw().drawElements(mode, count[i], type, indices[i], 1, basevertex ? basevertex[i] : 0);
    }
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
