// Compatibility profile vertex specification (W22): immediate mode
// (glBegin/glEnd), current attribute values (glColor, glNormal,
// glTexCoord, ...), conventional client arrays (glVertexPointer, ...) and
// glArrayElement / glInterleavedArrays / glRect (GL 3.3 compatibility
// sections 2.6-2.8). Batches and display list recording: compat/immediate.h.

#include <algorithm>
#include <cmath>
#include <cstring>

#include "core/compat/compat_api.h"
#include "core/compat/display_list.h"
#include "core/compat/immediate.h"
#include "core/context.h"
#include "core/draw.h"
#include "core/objects.h"
#include "gl_api.gen.h"

namespace wmgl {

// ------------------------------------------------------------ batches

namespace {

ImmediateBatch::Value bits(float x, float y, float z, float w) {
    ImmediateBatch::Value v;
    const float f[4] = {x, y, z, w};
    std::memcpy(v.data(), f, 16);
    return v;
}

}  // namespace

void ImmediateBatch::begin(GLenum primitive, const Context* live) {
    mode = primitive;
    varying = 0;
    data.clear();
    firstSet.fill(0);
    if (live) {
        known = ~0u;
        for (int s = 0; s < kAttribSlotCount; ++s) {
            std::memcpy(current[static_cast<size_t>(s)].data(), &live->state.genericAttribs[static_cast<size_t>(s)].value, 16);
            types[static_cast<size_t>(s)] = live->state.genericAttribs[static_cast<size_t>(s)].type;
        }
    } else {
        known = 0;
        types.fill(GL_FLOAT);
    }
}

void ImmediateBatch::set(int slot, const Value& value, GLenum type) {
    const uint32_t bit = 1u << slot;
    if (!(varying & bit) && !(known & bit)) firstSet[static_cast<size_t>(slot)] = static_cast<uint32_t>(vertexCount());
    varying |= bit;
    current[static_cast<size_t>(slot)] = value;
    types[static_cast<size_t>(slot)] = type;
}

void ImmediateBatch::vertex(const Value& position) {
    // Generic attribute 0 aliases gl_Vertex in the compatibility profile.
    for (int slot : {static_cast<int>(kConvVertex), 0}) {
        known |= 1u << slot;
        varying |= 1u << slot;
        current[static_cast<size_t>(slot)] = position;
        types[static_cast<size_t>(slot)] = GL_FLOAT;
    }
    data.insert(data.end(), current.begin(), current.end());
}

namespace compat {

ImmediateState& immediate(Context& ctx) {
    if (!ctx.immediate) ctx.immediate = std::make_unique<ImmediateState>();
    return *ctx.immediate;
}

bool listCompiling(const Context& ctx) { return ctx.listCompiler != nullptr; }

bool listExecutes(const Context& ctx) {
    return !ctx.listCompiler || ctx.listCompiler->mode == GL_COMPILE_AND_EXECUTE;
}

void recordCommand(Context& ctx, std::function<void(Context&)> command) {
    if (ctx.listCompiler) ctx.listCompiler->list->commands.push_back(std::move(command));
}

namespace {

void setAttributeNow(Context& ctx, int slot, const ImmediateBatch::Value& value, GLenum type) {
    if (ctx.immediate && ctx.immediate->active) ctx.immediate->batch.set(slot, value, type);
    ContextState::GenericAttrib& g = ctx.state.genericAttribs[static_cast<size_t>(slot)];
    std::memcpy(&g.value, value.data(), 16);
    g.type = type;
    if (slot == kConvColor) api::trackColorMaterial(ctx);
}

void emitVertexRaw(Context& ctx, const ImmediateBatch::Value& position) {
    if (ListCompiler* lc = ctx.listCompiler.get()) {
        if (lc->inBegin) lc->batch.vertex(position);
        if (lc->mode == GL_COMPILE) return;
    }
    if (ctx.immediate && ctx.immediate->active) ctx.immediate->batch.vertex(position);
    // A vertex outside glBegin/glEnd has no effect.
}

}  // namespace

void setAttribute(Context& ctx, int slot, const ImmediateBatch::Value& value, GLenum type) {
    if (slot == 0 && ctx.isCompat() && ((ctx.immediate && ctx.immediate->active) || (ctx.listCompiler && ctx.listCompiler->inBegin)))
        return emitVertexRaw(ctx, value);  // generic attribute 0 inside Begin/End specifies a vertex
    if (ListCompiler* lc = ctx.listCompiler.get()) {
        if (lc->inBegin) {
            lc->batch.set(slot, value, type);
        } else {
            lc->list->commands.push_back([slot, value, type](Context& c) { setAttributeNow(c, slot, value, type); });
        }
        if (lc->mode == GL_COMPILE) return;
    }
    setAttributeNow(ctx, slot, value, type);
}

void setAttribute(Context& ctx, int slot, float x, float y, float z, float w) {
    setAttribute(ctx, slot, bits(x, y, z, w), GL_FLOAT);
}

void emitVertex(Context& ctx, float x, float y, float z, float w) { emitVertexRaw(ctx, bits(x, y, z, w)); }

void drawBatch(Context& ctx, const ImmediateBatch& batch) {
    const size_t n = batch.vertexCount();
    ImmediateState& im = immediate(ctx);
    if (n) {
        const ImmediateBatch::Value* source = batch.data.data();
        if (batch.varying & ~batch.known) {
            // A list's batch: vertices before a slot's first set take the current value.
            im.scratch = batch.data;
            for (int slot = 0; slot < kAttribSlotCount; ++slot) {
                if (!(batch.varying & ~batch.known & (1u << slot))) continue;
                ImmediateBatch::Value value;
                std::memcpy(value.data(), &ctx.state.genericAttribs[static_cast<size_t>(slot)].value, 16);
                for (size_t v = 0; v < batch.firstSet[static_cast<size_t>(slot)] && v < n; ++v)
                    im.scratch[v * kAttribSlotCount + static_cast<size_t>(slot)] = value;
            }
            source = im.scratch.data();
        }
        if (!im.vao) im.vao = makeRef<VertexArray>(0);
        VertexArray& vao = *im.vao;
        for (int slot = 0; slot < kAttribSlotCount; ++slot) {
            VertexAttrib& a = vao.attribs[static_cast<size_t>(slot)];
            a = VertexAttrib();
            if (!(batch.varying & (1u << slot))) continue;
            const GLenum type = batch.types[static_cast<size_t>(slot)];
            a.enabled = true;
            a.size = 4;
            a.type = type;
            a.integer = type != GL_FLOAT;
            a.stride = static_cast<GLsizei>(kAttribSlotCount * sizeof(ImmediateBatch::Value));
            a.offset = reinterpret_cast<GLintptr>(source + slot);
        }
        ++vao.version;
        RefPtr<VertexArray> saved = ctx.state.vertexArray;
        ctx.state.vertexArray = im.vao;
        ctx.markDirty(kDirtyVertexInput);
        ctx.draw().drawArrays(batch.mode, 0, static_cast<GLsizei>(n), 1);
        ctx.state.vertexArray = saved;
        ctx.markDirty(kDirtyVertexInput);
    }
}

namespace {

// Reads element `index` of an array as 4 components (GL 3.3 section 2.8:
// missing components default to (0, 0, 0, 1)).
bool fetch(Context& ctx, const VertexAttrib& a, uint32_t index, ImmediateBatch::Value& out, GLenum& type) {
    const uint8_t* base;
    if (a.buffer) {
        if (!a.buffer->mtl) return false;
        if (a.buffer->lastWriteSerial) ctx.stream().waitFor(a.buffer->lastWriteSerial);
        base = a.buffer->contents() + a.offset;
    } else {
        base = reinterpret_cast<const uint8_t*>(a.offset);
    }
    if (!base) return false;
    const uint8_t* p = base + static_cast<size_t>(index) * static_cast<size_t>(a.effectiveStride());
    if (a.integer) {
        int32_t v[4] = {0, 0, 0, 1};
        for (int c = 0; c < std::min(a.size, 4); ++c) {
            switch (a.type) {
            case GL_BYTE: v[c] = reinterpret_cast<const int8_t*>(p)[c]; break;
            case GL_UNSIGNED_BYTE: v[c] = p[c]; break;
            case GL_SHORT: { int16_t x; std::memcpy(&x, p + c * 2, 2); v[c] = x; break; }
            case GL_UNSIGNED_SHORT: { uint16_t x; std::memcpy(&x, p + c * 2, 2); v[c] = x; break; }
            default: std::memcpy(&v[c], p + c * 4, 4); break;
            }
        }
        std::memcpy(out.data(), v, 16);
        type = a.type == GL_UNSIGNED_INT || a.type == GL_UNSIGNED_SHORT || a.type == GL_UNSIGNED_BYTE ? GL_UNSIGNED_INT : GL_INT;
        return true;
    }
    float v[4] = {0, 0, 0, 1};
    if (a.type == GL_INT_2_10_10_10_REV || a.type == GL_UNSIGNED_INT_2_10_10_10_REV) {
        uint32_t word;
        std::memcpy(&word, p, 4);
        const int widths[4] = {10, 10, 10, 2};
        int shift = 0;
        for (int c = 0; c < 4; ++c) {
            const uint32_t raw = (word >> shift) & ((1u << widths[c]) - 1);
            shift += widths[c];
            if (a.type == GL_UNSIGNED_INT_2_10_10_10_REV) {
                v[c] = a.normalized ? raw / static_cast<float>((1u << widths[c]) - 1) : static_cast<float>(raw);
            } else {
                const int32_t value = static_cast<int32_t>(raw << (32 - widths[c])) >> (32 - widths[c]);
                const float max = static_cast<float>((1 << (widths[c] - 1)) - 1);
                v[c] = a.normalized ? std::max(-1.0f, value / max) : static_cast<float>(value);
            }
        }
    } else {
        const int size = a.size == GL_BGRA ? 4 : a.size;
        for (int c = 0; c < std::min(size, 4); ++c) {
            float x = 0;
            switch (a.type) {
            case GL_BYTE: { const int8_t s = reinterpret_cast<const int8_t*>(p)[c]; x = a.normalized ? (2.0f * s + 1.0f) / 255.0f : s; break; }
            case GL_UNSIGNED_BYTE: x = a.normalized ? p[c] / 255.0f : p[c]; break;
            case GL_SHORT: { int16_t s; std::memcpy(&s, p + c * 2, 2); x = a.normalized ? (2.0f * s + 1.0f) / 65535.0f : s; break; }
            case GL_UNSIGNED_SHORT: { uint16_t s; std::memcpy(&s, p + c * 2, 2); x = a.normalized ? s / 65535.0f : s; break; }
            case GL_INT: { int32_t s; std::memcpy(&s, p + c * 4, 4); x = a.normalized ? static_cast<float>((2.0 * s + 1.0) / 4294967295.0) : static_cast<float>(s); break; }
            case GL_UNSIGNED_INT: { uint32_t s; std::memcpy(&s, p + c * 4, 4); x = a.normalized ? static_cast<float>(s / 4294967295.0) : static_cast<float>(s); break; }
            case GL_DOUBLE: { double s; std::memcpy(&s, p + c * 8, 8); x = static_cast<float>(s); break; }
            case GL_HALF_FLOAT: {
                uint16_t h;
                std::memcpy(&h, p + c * 2, 2);
                const uint32_t e = (h >> 10) & 0x1f, m = h & 0x3ff;
                x = e == 0 ? std::ldexp(static_cast<float>(m), -24)
                    : e == 31 ? (m ? NAN : INFINITY)
                              : std::ldexp(static_cast<float>(m | 0x400), static_cast<int>(e) - 25);
                if (h & 0x8000) x = -x;
                break;
            }
            default: std::memcpy(&x, p + c * 4, 4); break;
            }
            v[c] = x;
        }
        if (a.size == GL_BGRA) std::swap(v[0], v[2]);
    }
    std::memcpy(out.data(), v, 16);
    type = GL_FLOAT;
    return true;
}

// The array that provides positions: the conventional vertex array, else
// generic attribute 0.
const VertexAttrib* positionArray(const VertexArray& vao) {
    if (vao.attribs[kConvVertex].enabled) return &vao.attribs[kConvVertex];
    if (vao.attribs[0].enabled) return &vao.attribs[0];
    return nullptr;
}

}  // namespace

bool compileArrayDraw(Context& ctx, GLenum mode, const std::vector<uint32_t>& indices) {
    ListCompiler* lc = ctx.listCompiler.get();
    if (!lc) return true;
    const VertexArray& vao = *ctx.state.vertexArray;
    const VertexAttrib* position = positionArray(vao);
    ImmediateBatch batch;
    batch.begin(mode, nullptr);
    for (uint32_t index : indices) {
        for (int slot = 0; slot < kAttribSlotCount; ++slot) {
            const VertexAttrib& a = vao.attribs[static_cast<size_t>(slot)];
            if (!a.enabled || &a == position || slot == kConvVertex) continue;
            ImmediateBatch::Value value;
            GLenum type;
            if (fetch(ctx, a, index, value, type)) batch.set(slot, value, type);
        }
        ImmediateBatch::Value value;
        GLenum type;
        if (position && fetch(ctx, *position, index, value, type)) batch.vertex(value);
    }
    lc->list->commands.push_back([batch = std::move(batch)](Context& c) { drawBatch(c, batch); });
    return lc->mode == GL_COMPILE_AND_EXECUTE;
}

}  // namespace compat

namespace api {

using compat::emitVertex;
using compat::setAttribute;

bool insideBeginEnd(const Context& ctx) { return ctx.immediate && ctx.immediate->active; }

namespace {

bool compatOnly(Context& ctx) {
    if (ctx.isCompat()) return true;
    ctx.setError(GL_INVALID_OPERATION);  // removed from the core profile
    return false;
}

// Colour and normal components: signed and unsigned integers map to
// [-1, 1] / [0, 1] (GL 3.3 compatibility table 2.10).
float cn(GLbyte v) { return (2.0f * v + 1.0f) / 255.0f; }
float cn(GLubyte v) { return v / 255.0f; }
float cn(GLshort v) { return (2.0f * v + 1.0f) / 65535.0f; }
float cn(GLushort v) { return v / 65535.0f; }
float cn(GLint v) { return static_cast<float>((2.0 * v + 1.0) / 4294967295.0); }
float cn(GLuint v) { return static_cast<float>(v / 4294967295.0); }
float cn(GLfloat v) { return v; }
float cn(GLdouble v) { return static_cast<float>(v); }
template <class T>
float fv(T v) { return static_cast<float>(v); }

int texCoordSlot(Context& ctx, GLenum target) {
    if (target < GL_TEXTURE0 || target >= GL_TEXTURE0 + FixedState::kTextureUnits) {
        ctx.setError(GL_INVALID_ENUM);
        return -1;
    }
    return kConvTexCoord0 + static_cast<int>(target - GL_TEXTURE0);
}

}  // namespace


// ------------------------------------------------------------ Begin/End

namespace {

bool validBeginMode(GLenum mode) {
    switch (mode) {
    case GL_POINTS: case GL_LINES: case GL_LINE_LOOP: case GL_LINE_STRIP: case GL_TRIANGLES: case GL_TRIANGLE_STRIP:
    case GL_TRIANGLE_FAN: case GL_QUADS: case GL_QUAD_STRIP: case GL_POLYGON: case GL_LINES_ADJACENCY:
    case GL_LINE_STRIP_ADJACENCY: case GL_TRIANGLES_ADJACENCY: case GL_TRIANGLE_STRIP_ADJACENCY:
        return true;
    default: return false;
    }
}

// Current values after a list's batch: the last value each varying slot took.
void applyBatchCurrent(Context& ctx, const ImmediateBatch& batch) {
    for (int slot = 1; slot < kAttribSlotCount; ++slot) {
        if (slot == kConvVertex || !(batch.varying & (1u << slot))) continue;
        ContextState::GenericAttrib& g = ctx.state.genericAttribs[static_cast<size_t>(slot)];
        std::memcpy(&g.value, batch.current[static_cast<size_t>(slot)].data(), 16);
        g.type = batch.types[static_cast<size_t>(slot)];
    }
    trackColorMaterial(ctx);
}

}  // namespace

void Begin(Context& ctx, GLenum mode) {
    if (!compatOnly(ctx)) return;
    if (!validBeginMode(mode)) return ctx.setError(GL_INVALID_ENUM);
    if (ListCompiler* lc = ctx.listCompiler.get()) {
        if (!lc->inBegin) {
            lc->inBegin = true;
            lc->batch.begin(mode, nullptr);
        }
        if (lc->mode == GL_COMPILE) return;
    }
    ImmediateState& im = compat::immediate(ctx);
    if (im.active) return ctx.setError(GL_INVALID_OPERATION);
    im.active = true;
    im.batch.begin(mode, &ctx);
}

void End(Context& ctx) {
    if (!compatOnly(ctx)) return;
    if (ListCompiler* lc = ctx.listCompiler.get(); lc && lc->inBegin) {
        lc->inBegin = false;
        lc->list->commands.push_back([batch = std::move(lc->batch)](Context& c) {
            compat::drawBatch(c, batch);
            applyBatchCurrent(c, batch);
        });
        lc->batch = ImmediateBatch();
        if (lc->mode == GL_COMPILE) return;
    }
    ImmediateState& im = compat::immediate(ctx);
    if (!im.active) return ctx.setError(GL_INVALID_OPERATION);
    im.active = false;
    compat::drawBatch(ctx, im.batch);
}

// ------------------------------------------------------------ current values

// glColor*
void Color3b(Context& ctx, GLbyte red, GLbyte green, GLbyte blue) { setAttribute(ctx, kConvColor, cn(red), cn(green), cn(blue), 1.0f); }
void Color3d(Context& ctx, GLdouble red, GLdouble green, GLdouble blue) { setAttribute(ctx, kConvColor, cn(red), cn(green), cn(blue), 1.0f); }
void Color3f(Context& ctx, GLfloat red, GLfloat green, GLfloat blue) { setAttribute(ctx, kConvColor, cn(red), cn(green), cn(blue), 1.0f); }
void Color3i(Context& ctx, GLint red, GLint green, GLint blue) { setAttribute(ctx, kConvColor, cn(red), cn(green), cn(blue), 1.0f); }
void Color3s(Context& ctx, GLshort red, GLshort green, GLshort blue) { setAttribute(ctx, kConvColor, cn(red), cn(green), cn(blue), 1.0f); }
void Color4b(Context& ctx, GLbyte red, GLbyte green, GLbyte blue, GLbyte alpha) { setAttribute(ctx, kConvColor, cn(red), cn(green), cn(blue), cn(alpha)); }
void Color4d(Context& ctx, GLdouble red, GLdouble green, GLdouble blue, GLdouble alpha) { setAttribute(ctx, kConvColor, cn(red), cn(green), cn(blue), cn(alpha)); }
void Color4f(Context& ctx, GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha) { setAttribute(ctx, kConvColor, cn(red), cn(green), cn(blue), cn(alpha)); }
void Color4i(Context& ctx, GLint red, GLint green, GLint blue, GLint alpha) { setAttribute(ctx, kConvColor, cn(red), cn(green), cn(blue), cn(alpha)); }
void Color4s(Context& ctx, GLshort red, GLshort green, GLshort blue, GLshort alpha) { setAttribute(ctx, kConvColor, cn(red), cn(green), cn(blue), cn(alpha)); }
void Color3bv(Context& ctx, const GLbyte * v) { if (v) setAttribute(ctx, kConvColor, cn(v[0]), cn(v[1]), cn(v[2]), 1.0f); }
void Color3dv(Context& ctx, const GLdouble * v) { if (v) setAttribute(ctx, kConvColor, cn(v[0]), cn(v[1]), cn(v[2]), 1.0f); }
void Color3fv(Context& ctx, const GLfloat * v) { if (v) setAttribute(ctx, kConvColor, cn(v[0]), cn(v[1]), cn(v[2]), 1.0f); }
void Color3iv(Context& ctx, const GLint * v) { if (v) setAttribute(ctx, kConvColor, cn(v[0]), cn(v[1]), cn(v[2]), 1.0f); }
void Color3sv(Context& ctx, const GLshort * v) { if (v) setAttribute(ctx, kConvColor, cn(v[0]), cn(v[1]), cn(v[2]), 1.0f); }
void Color3ub(Context& ctx, GLubyte red, GLubyte green, GLubyte blue) { setAttribute(ctx, kConvColor, cn(red), cn(green), cn(blue), 1.0f); }
void Color3ui(Context& ctx, GLuint red, GLuint green, GLuint blue) { setAttribute(ctx, kConvColor, cn(red), cn(green), cn(blue), 1.0f); }
void Color3us(Context& ctx, GLushort red, GLushort green, GLushort blue) { setAttribute(ctx, kConvColor, cn(red), cn(green), cn(blue), 1.0f); }
void Color4bv(Context& ctx, const GLbyte * v) { if (v) setAttribute(ctx, kConvColor, cn(v[0]), cn(v[1]), cn(v[2]), cn(v[3])); }
void Color4dv(Context& ctx, const GLdouble * v) { if (v) setAttribute(ctx, kConvColor, cn(v[0]), cn(v[1]), cn(v[2]), cn(v[3])); }
void Color4fv(Context& ctx, const GLfloat * v) { if (v) setAttribute(ctx, kConvColor, cn(v[0]), cn(v[1]), cn(v[2]), cn(v[3])); }
void Color4iv(Context& ctx, const GLint * v) { if (v) setAttribute(ctx, kConvColor, cn(v[0]), cn(v[1]), cn(v[2]), cn(v[3])); }
void Color4sv(Context& ctx, const GLshort * v) { if (v) setAttribute(ctx, kConvColor, cn(v[0]), cn(v[1]), cn(v[2]), cn(v[3])); }
void Color4ub(Context& ctx, GLubyte red, GLubyte green, GLubyte blue, GLubyte alpha) { setAttribute(ctx, kConvColor, cn(red), cn(green), cn(blue), cn(alpha)); }
void Color4ui(Context& ctx, GLuint red, GLuint green, GLuint blue, GLuint alpha) { setAttribute(ctx, kConvColor, cn(red), cn(green), cn(blue), cn(alpha)); }
void Color4us(Context& ctx, GLushort red, GLushort green, GLushort blue, GLushort alpha) { setAttribute(ctx, kConvColor, cn(red), cn(green), cn(blue), cn(alpha)); }
void Color3ubv(Context& ctx, const GLubyte * v) { if (v) setAttribute(ctx, kConvColor, cn(v[0]), cn(v[1]), cn(v[2]), 1.0f); }
void Color3uiv(Context& ctx, const GLuint * v) { if (v) setAttribute(ctx, kConvColor, cn(v[0]), cn(v[1]), cn(v[2]), 1.0f); }
void Color3usv(Context& ctx, const GLushort * v) { if (v) setAttribute(ctx, kConvColor, cn(v[0]), cn(v[1]), cn(v[2]), 1.0f); }
void Color4ubv(Context& ctx, const GLubyte * v) { if (v) setAttribute(ctx, kConvColor, cn(v[0]), cn(v[1]), cn(v[2]), cn(v[3])); }
void Color4uiv(Context& ctx, const GLuint * v) { if (v) setAttribute(ctx, kConvColor, cn(v[0]), cn(v[1]), cn(v[2]), cn(v[3])); }
void Color4usv(Context& ctx, const GLushort * v) { if (v) setAttribute(ctx, kConvColor, cn(v[0]), cn(v[1]), cn(v[2]), cn(v[3])); }
// glSecondaryColor*
void SecondaryColor3b(Context& ctx, GLbyte red, GLbyte green, GLbyte blue) { setAttribute(ctx, kConvSecondaryColor, cn(red), cn(green), cn(blue), 1.0f); }
void SecondaryColor3d(Context& ctx, GLdouble red, GLdouble green, GLdouble blue) { setAttribute(ctx, kConvSecondaryColor, cn(red), cn(green), cn(blue), 1.0f); }
void SecondaryColor3f(Context& ctx, GLfloat red, GLfloat green, GLfloat blue) { setAttribute(ctx, kConvSecondaryColor, cn(red), cn(green), cn(blue), 1.0f); }
void SecondaryColor3i(Context& ctx, GLint red, GLint green, GLint blue) { setAttribute(ctx, kConvSecondaryColor, cn(red), cn(green), cn(blue), 1.0f); }
void SecondaryColor3s(Context& ctx, GLshort red, GLshort green, GLshort blue) { setAttribute(ctx, kConvSecondaryColor, cn(red), cn(green), cn(blue), 1.0f); }
void SecondaryColor3bv(Context& ctx, const GLbyte * v) { if (v) setAttribute(ctx, kConvSecondaryColor, cn(v[0]), cn(v[1]), cn(v[2]), 1.0f); }
void SecondaryColor3dv(Context& ctx, const GLdouble * v) { if (v) setAttribute(ctx, kConvSecondaryColor, cn(v[0]), cn(v[1]), cn(v[2]), 1.0f); }
void SecondaryColor3fv(Context& ctx, const GLfloat * v) { if (v) setAttribute(ctx, kConvSecondaryColor, cn(v[0]), cn(v[1]), cn(v[2]), 1.0f); }
void SecondaryColor3iv(Context& ctx, const GLint * v) { if (v) setAttribute(ctx, kConvSecondaryColor, cn(v[0]), cn(v[1]), cn(v[2]), 1.0f); }
void SecondaryColor3sv(Context& ctx, const GLshort * v) { if (v) setAttribute(ctx, kConvSecondaryColor, cn(v[0]), cn(v[1]), cn(v[2]), 1.0f); }
void SecondaryColor3ub(Context& ctx, GLubyte red, GLubyte green, GLubyte blue) { setAttribute(ctx, kConvSecondaryColor, cn(red), cn(green), cn(blue), 1.0f); }
void SecondaryColor3ui(Context& ctx, GLuint red, GLuint green, GLuint blue) { setAttribute(ctx, kConvSecondaryColor, cn(red), cn(green), cn(blue), 1.0f); }
void SecondaryColor3us(Context& ctx, GLushort red, GLushort green, GLushort blue) { setAttribute(ctx, kConvSecondaryColor, cn(red), cn(green), cn(blue), 1.0f); }
void SecondaryColor3ubv(Context& ctx, const GLubyte * v) { if (v) setAttribute(ctx, kConvSecondaryColor, cn(v[0]), cn(v[1]), cn(v[2]), 1.0f); }
void SecondaryColor3uiv(Context& ctx, const GLuint * v) { if (v) setAttribute(ctx, kConvSecondaryColor, cn(v[0]), cn(v[1]), cn(v[2]), 1.0f); }
void SecondaryColor3usv(Context& ctx, const GLushort * v) { if (v) setAttribute(ctx, kConvSecondaryColor, cn(v[0]), cn(v[1]), cn(v[2]), 1.0f); }
// glNormal*
void Normal3b(Context& ctx, GLbyte nx, GLbyte ny, GLbyte nz) { setAttribute(ctx, kConvNormal, cn(nx), cn(ny), cn(nz), 1.0f); }
void Normal3d(Context& ctx, GLdouble nx, GLdouble ny, GLdouble nz) { setAttribute(ctx, kConvNormal, cn(nx), cn(ny), cn(nz), 1.0f); }
void Normal3f(Context& ctx, GLfloat nx, GLfloat ny, GLfloat nz) { setAttribute(ctx, kConvNormal, cn(nx), cn(ny), cn(nz), 1.0f); }
void Normal3i(Context& ctx, GLint nx, GLint ny, GLint nz) { setAttribute(ctx, kConvNormal, cn(nx), cn(ny), cn(nz), 1.0f); }
void Normal3s(Context& ctx, GLshort nx, GLshort ny, GLshort nz) { setAttribute(ctx, kConvNormal, cn(nx), cn(ny), cn(nz), 1.0f); }
void Normal3bv(Context& ctx, const GLbyte * v) { if (v) setAttribute(ctx, kConvNormal, cn(v[0]), cn(v[1]), cn(v[2]), 1.0f); }
void Normal3dv(Context& ctx, const GLdouble * v) { if (v) setAttribute(ctx, kConvNormal, cn(v[0]), cn(v[1]), cn(v[2]), 1.0f); }
void Normal3fv(Context& ctx, const GLfloat * v) { if (v) setAttribute(ctx, kConvNormal, cn(v[0]), cn(v[1]), cn(v[2]), 1.0f); }
void Normal3iv(Context& ctx, const GLint * v) { if (v) setAttribute(ctx, kConvNormal, cn(v[0]), cn(v[1]), cn(v[2]), 1.0f); }
void Normal3sv(Context& ctx, const GLshort * v) { if (v) setAttribute(ctx, kConvNormal, cn(v[0]), cn(v[1]), cn(v[2]), 1.0f); }
// glTexCoord*
void TexCoord1d(Context& ctx, GLdouble s) { setAttribute(ctx, kConvTexCoord0, fv(s), 0, 0, 1.0f); }
void TexCoord1f(Context& ctx, GLfloat s) { setAttribute(ctx, kConvTexCoord0, fv(s), 0, 0, 1.0f); }
void TexCoord1i(Context& ctx, GLint s) { setAttribute(ctx, kConvTexCoord0, fv(s), 0, 0, 1.0f); }
void TexCoord1s(Context& ctx, GLshort s) { setAttribute(ctx, kConvTexCoord0, fv(s), 0, 0, 1.0f); }
void TexCoord2d(Context& ctx, GLdouble s, GLdouble t) { setAttribute(ctx, kConvTexCoord0, fv(s), fv(t), 0, 1.0f); }
void TexCoord2f(Context& ctx, GLfloat s, GLfloat t) { setAttribute(ctx, kConvTexCoord0, fv(s), fv(t), 0, 1.0f); }
void TexCoord2i(Context& ctx, GLint s, GLint t) { setAttribute(ctx, kConvTexCoord0, fv(s), fv(t), 0, 1.0f); }
void TexCoord2s(Context& ctx, GLshort s, GLshort t) { setAttribute(ctx, kConvTexCoord0, fv(s), fv(t), 0, 1.0f); }
void TexCoord3d(Context& ctx, GLdouble s, GLdouble t, GLdouble r) { setAttribute(ctx, kConvTexCoord0, fv(s), fv(t), fv(r), 1.0f); }
void TexCoord3f(Context& ctx, GLfloat s, GLfloat t, GLfloat r) { setAttribute(ctx, kConvTexCoord0, fv(s), fv(t), fv(r), 1.0f); }
void TexCoord3i(Context& ctx, GLint s, GLint t, GLint r) { setAttribute(ctx, kConvTexCoord0, fv(s), fv(t), fv(r), 1.0f); }
void TexCoord3s(Context& ctx, GLshort s, GLshort t, GLshort r) { setAttribute(ctx, kConvTexCoord0, fv(s), fv(t), fv(r), 1.0f); }
void TexCoord4d(Context& ctx, GLdouble s, GLdouble t, GLdouble r, GLdouble q) { setAttribute(ctx, kConvTexCoord0, fv(s), fv(t), fv(r), fv(q)); }
void TexCoord4f(Context& ctx, GLfloat s, GLfloat t, GLfloat r, GLfloat q) { setAttribute(ctx, kConvTexCoord0, fv(s), fv(t), fv(r), fv(q)); }
void TexCoord4i(Context& ctx, GLint s, GLint t, GLint r, GLint q) { setAttribute(ctx, kConvTexCoord0, fv(s), fv(t), fv(r), fv(q)); }
void TexCoord4s(Context& ctx, GLshort s, GLshort t, GLshort r, GLshort q) { setAttribute(ctx, kConvTexCoord0, fv(s), fv(t), fv(r), fv(q)); }
void TexCoord1dv(Context& ctx, const GLdouble * v) { if (v) setAttribute(ctx, kConvTexCoord0, fv(v[0]), 0, 0, 1.0f); }
void TexCoord1fv(Context& ctx, const GLfloat * v) { if (v) setAttribute(ctx, kConvTexCoord0, fv(v[0]), 0, 0, 1.0f); }
void TexCoord1iv(Context& ctx, const GLint * v) { if (v) setAttribute(ctx, kConvTexCoord0, fv(v[0]), 0, 0, 1.0f); }
void TexCoord1sv(Context& ctx, const GLshort * v) { if (v) setAttribute(ctx, kConvTexCoord0, fv(v[0]), 0, 0, 1.0f); }
void TexCoord2dv(Context& ctx, const GLdouble * v) { if (v) setAttribute(ctx, kConvTexCoord0, fv(v[0]), fv(v[1]), 0, 1.0f); }
void TexCoord2fv(Context& ctx, const GLfloat * v) { if (v) setAttribute(ctx, kConvTexCoord0, fv(v[0]), fv(v[1]), 0, 1.0f); }
void TexCoord2iv(Context& ctx, const GLint * v) { if (v) setAttribute(ctx, kConvTexCoord0, fv(v[0]), fv(v[1]), 0, 1.0f); }
void TexCoord2sv(Context& ctx, const GLshort * v) { if (v) setAttribute(ctx, kConvTexCoord0, fv(v[0]), fv(v[1]), 0, 1.0f); }
void TexCoord3dv(Context& ctx, const GLdouble * v) { if (v) setAttribute(ctx, kConvTexCoord0, fv(v[0]), fv(v[1]), fv(v[2]), 1.0f); }
void TexCoord3fv(Context& ctx, const GLfloat * v) { if (v) setAttribute(ctx, kConvTexCoord0, fv(v[0]), fv(v[1]), fv(v[2]), 1.0f); }
void TexCoord3iv(Context& ctx, const GLint * v) { if (v) setAttribute(ctx, kConvTexCoord0, fv(v[0]), fv(v[1]), fv(v[2]), 1.0f); }
void TexCoord3sv(Context& ctx, const GLshort * v) { if (v) setAttribute(ctx, kConvTexCoord0, fv(v[0]), fv(v[1]), fv(v[2]), 1.0f); }
void TexCoord4dv(Context& ctx, const GLdouble * v) { if (v) setAttribute(ctx, kConvTexCoord0, fv(v[0]), fv(v[1]), fv(v[2]), fv(v[3])); }
void TexCoord4fv(Context& ctx, const GLfloat * v) { if (v) setAttribute(ctx, kConvTexCoord0, fv(v[0]), fv(v[1]), fv(v[2]), fv(v[3])); }
void TexCoord4iv(Context& ctx, const GLint * v) { if (v) setAttribute(ctx, kConvTexCoord0, fv(v[0]), fv(v[1]), fv(v[2]), fv(v[3])); }
void TexCoord4sv(Context& ctx, const GLshort * v) { if (v) setAttribute(ctx, kConvTexCoord0, fv(v[0]), fv(v[1]), fv(v[2]), fv(v[3])); }
// glMultiTexCoord*
void MultiTexCoord1d(Context& ctx, GLenum target, GLdouble s) { if (const int slot = texCoordSlot(ctx, target); slot >= 0) setAttribute(ctx, slot, fv(s), 0, 0, 1.0f); }
void MultiTexCoord1f(Context& ctx, GLenum target, GLfloat s) { if (const int slot = texCoordSlot(ctx, target); slot >= 0) setAttribute(ctx, slot, fv(s), 0, 0, 1.0f); }
void MultiTexCoord1i(Context& ctx, GLenum target, GLint s) { if (const int slot = texCoordSlot(ctx, target); slot >= 0) setAttribute(ctx, slot, fv(s), 0, 0, 1.0f); }
void MultiTexCoord1s(Context& ctx, GLenum target, GLshort s) { if (const int slot = texCoordSlot(ctx, target); slot >= 0) setAttribute(ctx, slot, fv(s), 0, 0, 1.0f); }
void MultiTexCoord2d(Context& ctx, GLenum target, GLdouble s, GLdouble t) { if (const int slot = texCoordSlot(ctx, target); slot >= 0) setAttribute(ctx, slot, fv(s), fv(t), 0, 1.0f); }
void MultiTexCoord2f(Context& ctx, GLenum target, GLfloat s, GLfloat t) { if (const int slot = texCoordSlot(ctx, target); slot >= 0) setAttribute(ctx, slot, fv(s), fv(t), 0, 1.0f); }
void MultiTexCoord2i(Context& ctx, GLenum target, GLint s, GLint t) { if (const int slot = texCoordSlot(ctx, target); slot >= 0) setAttribute(ctx, slot, fv(s), fv(t), 0, 1.0f); }
void MultiTexCoord2s(Context& ctx, GLenum target, GLshort s, GLshort t) { if (const int slot = texCoordSlot(ctx, target); slot >= 0) setAttribute(ctx, slot, fv(s), fv(t), 0, 1.0f); }
void MultiTexCoord3d(Context& ctx, GLenum target, GLdouble s, GLdouble t, GLdouble r) { if (const int slot = texCoordSlot(ctx, target); slot >= 0) setAttribute(ctx, slot, fv(s), fv(t), fv(r), 1.0f); }
void MultiTexCoord3f(Context& ctx, GLenum target, GLfloat s, GLfloat t, GLfloat r) { if (const int slot = texCoordSlot(ctx, target); slot >= 0) setAttribute(ctx, slot, fv(s), fv(t), fv(r), 1.0f); }
void MultiTexCoord3i(Context& ctx, GLenum target, GLint s, GLint t, GLint r) { if (const int slot = texCoordSlot(ctx, target); slot >= 0) setAttribute(ctx, slot, fv(s), fv(t), fv(r), 1.0f); }
void MultiTexCoord3s(Context& ctx, GLenum target, GLshort s, GLshort t, GLshort r) { if (const int slot = texCoordSlot(ctx, target); slot >= 0) setAttribute(ctx, slot, fv(s), fv(t), fv(r), 1.0f); }
void MultiTexCoord4d(Context& ctx, GLenum target, GLdouble s, GLdouble t, GLdouble r, GLdouble q) { if (const int slot = texCoordSlot(ctx, target); slot >= 0) setAttribute(ctx, slot, fv(s), fv(t), fv(r), fv(q)); }
void MultiTexCoord4f(Context& ctx, GLenum target, GLfloat s, GLfloat t, GLfloat r, GLfloat q) { if (const int slot = texCoordSlot(ctx, target); slot >= 0) setAttribute(ctx, slot, fv(s), fv(t), fv(r), fv(q)); }
void MultiTexCoord4i(Context& ctx, GLenum target, GLint s, GLint t, GLint r, GLint q) { if (const int slot = texCoordSlot(ctx, target); slot >= 0) setAttribute(ctx, slot, fv(s), fv(t), fv(r), fv(q)); }
void MultiTexCoord4s(Context& ctx, GLenum target, GLshort s, GLshort t, GLshort r, GLshort q) { if (const int slot = texCoordSlot(ctx, target); slot >= 0) setAttribute(ctx, slot, fv(s), fv(t), fv(r), fv(q)); }
void MultiTexCoord1dv(Context& ctx, GLenum target, const GLdouble * v) { if (v) if (const int slot = texCoordSlot(ctx, target); slot >= 0) setAttribute(ctx, slot, fv(v[0]), 0, 0, 1.0f); }
void MultiTexCoord1fv(Context& ctx, GLenum target, const GLfloat * v) { if (v) if (const int slot = texCoordSlot(ctx, target); slot >= 0) setAttribute(ctx, slot, fv(v[0]), 0, 0, 1.0f); }
void MultiTexCoord1iv(Context& ctx, GLenum target, const GLint * v) { if (v) if (const int slot = texCoordSlot(ctx, target); slot >= 0) setAttribute(ctx, slot, fv(v[0]), 0, 0, 1.0f); }
void MultiTexCoord1sv(Context& ctx, GLenum target, const GLshort * v) { if (v) if (const int slot = texCoordSlot(ctx, target); slot >= 0) setAttribute(ctx, slot, fv(v[0]), 0, 0, 1.0f); }
void MultiTexCoord2dv(Context& ctx, GLenum target, const GLdouble * v) { if (v) if (const int slot = texCoordSlot(ctx, target); slot >= 0) setAttribute(ctx, slot, fv(v[0]), fv(v[1]), 0, 1.0f); }
void MultiTexCoord2fv(Context& ctx, GLenum target, const GLfloat * v) { if (v) if (const int slot = texCoordSlot(ctx, target); slot >= 0) setAttribute(ctx, slot, fv(v[0]), fv(v[1]), 0, 1.0f); }
void MultiTexCoord2iv(Context& ctx, GLenum target, const GLint * v) { if (v) if (const int slot = texCoordSlot(ctx, target); slot >= 0) setAttribute(ctx, slot, fv(v[0]), fv(v[1]), 0, 1.0f); }
void MultiTexCoord2sv(Context& ctx, GLenum target, const GLshort * v) { if (v) if (const int slot = texCoordSlot(ctx, target); slot >= 0) setAttribute(ctx, slot, fv(v[0]), fv(v[1]), 0, 1.0f); }
void MultiTexCoord3dv(Context& ctx, GLenum target, const GLdouble * v) { if (v) if (const int slot = texCoordSlot(ctx, target); slot >= 0) setAttribute(ctx, slot, fv(v[0]), fv(v[1]), fv(v[2]), 1.0f); }
void MultiTexCoord3fv(Context& ctx, GLenum target, const GLfloat * v) { if (v) if (const int slot = texCoordSlot(ctx, target); slot >= 0) setAttribute(ctx, slot, fv(v[0]), fv(v[1]), fv(v[2]), 1.0f); }
void MultiTexCoord3iv(Context& ctx, GLenum target, const GLint * v) { if (v) if (const int slot = texCoordSlot(ctx, target); slot >= 0) setAttribute(ctx, slot, fv(v[0]), fv(v[1]), fv(v[2]), 1.0f); }
void MultiTexCoord3sv(Context& ctx, GLenum target, const GLshort * v) { if (v) if (const int slot = texCoordSlot(ctx, target); slot >= 0) setAttribute(ctx, slot, fv(v[0]), fv(v[1]), fv(v[2]), 1.0f); }
void MultiTexCoord4dv(Context& ctx, GLenum target, const GLdouble * v) { if (v) if (const int slot = texCoordSlot(ctx, target); slot >= 0) setAttribute(ctx, slot, fv(v[0]), fv(v[1]), fv(v[2]), fv(v[3])); }
void MultiTexCoord4fv(Context& ctx, GLenum target, const GLfloat * v) { if (v) if (const int slot = texCoordSlot(ctx, target); slot >= 0) setAttribute(ctx, slot, fv(v[0]), fv(v[1]), fv(v[2]), fv(v[3])); }
void MultiTexCoord4iv(Context& ctx, GLenum target, const GLint * v) { if (v) if (const int slot = texCoordSlot(ctx, target); slot >= 0) setAttribute(ctx, slot, fv(v[0]), fv(v[1]), fv(v[2]), fv(v[3])); }
void MultiTexCoord4sv(Context& ctx, GLenum target, const GLshort * v) { if (v) if (const int slot = texCoordSlot(ctx, target); slot >= 0) setAttribute(ctx, slot, fv(v[0]), fv(v[1]), fv(v[2]), fv(v[3])); }
// glFogCoord*
void FogCoordd(Context& ctx, GLdouble coord) { setAttribute(ctx, kConvFogCoord, fv(coord), 0, 0, 1.0f); }
void FogCoordf(Context& ctx, GLfloat coord) { setAttribute(ctx, kConvFogCoord, fv(coord), 0, 0, 1.0f); }
void FogCoorddv(Context& ctx, const GLdouble * coord) { if (coord) setAttribute(ctx, kConvFogCoord, fv(coord[0]), 0, 0, 1.0f); }
void FogCoordfv(Context& ctx, const GLfloat * coord) { if (coord) setAttribute(ctx, kConvFogCoord, fv(coord[0]), 0, 0, 1.0f); }
// glVertex*
void Vertex2d(Context& ctx, GLdouble x, GLdouble y) { emitVertex(ctx, fv(x), fv(y), 0, 1.0f); }
void Vertex2f(Context& ctx, GLfloat x, GLfloat y) { emitVertex(ctx, fv(x), fv(y), 0, 1.0f); }
void Vertex2i(Context& ctx, GLint x, GLint y) { emitVertex(ctx, fv(x), fv(y), 0, 1.0f); }
void Vertex2s(Context& ctx, GLshort x, GLshort y) { emitVertex(ctx, fv(x), fv(y), 0, 1.0f); }
void Vertex3d(Context& ctx, GLdouble x, GLdouble y, GLdouble z) { emitVertex(ctx, fv(x), fv(y), fv(z), 1.0f); }
void Vertex3f(Context& ctx, GLfloat x, GLfloat y, GLfloat z) { emitVertex(ctx, fv(x), fv(y), fv(z), 1.0f); }
void Vertex3i(Context& ctx, GLint x, GLint y, GLint z) { emitVertex(ctx, fv(x), fv(y), fv(z), 1.0f); }
void Vertex3s(Context& ctx, GLshort x, GLshort y, GLshort z) { emitVertex(ctx, fv(x), fv(y), fv(z), 1.0f); }
void Vertex4d(Context& ctx, GLdouble x, GLdouble y, GLdouble z, GLdouble w) { emitVertex(ctx, fv(x), fv(y), fv(z), fv(w)); }
void Vertex4f(Context& ctx, GLfloat x, GLfloat y, GLfloat z, GLfloat w) { emitVertex(ctx, fv(x), fv(y), fv(z), fv(w)); }
void Vertex4i(Context& ctx, GLint x, GLint y, GLint z, GLint w) { emitVertex(ctx, fv(x), fv(y), fv(z), fv(w)); }
void Vertex4s(Context& ctx, GLshort x, GLshort y, GLshort z, GLshort w) { emitVertex(ctx, fv(x), fv(y), fv(z), fv(w)); }
void Vertex2dv(Context& ctx, const GLdouble * v) { if (v) emitVertex(ctx, fv(v[0]), fv(v[1]), 0, 1.0f); }
void Vertex2fv(Context& ctx, const GLfloat * v) { if (v) emitVertex(ctx, fv(v[0]), fv(v[1]), 0, 1.0f); }
void Vertex2iv(Context& ctx, const GLint * v) { if (v) emitVertex(ctx, fv(v[0]), fv(v[1]), 0, 1.0f); }
void Vertex2sv(Context& ctx, const GLshort * v) { if (v) emitVertex(ctx, fv(v[0]), fv(v[1]), 0, 1.0f); }
void Vertex3dv(Context& ctx, const GLdouble * v) { if (v) emitVertex(ctx, fv(v[0]), fv(v[1]), fv(v[2]), 1.0f); }
void Vertex3fv(Context& ctx, const GLfloat * v) { if (v) emitVertex(ctx, fv(v[0]), fv(v[1]), fv(v[2]), 1.0f); }
void Vertex3iv(Context& ctx, const GLint * v) { if (v) emitVertex(ctx, fv(v[0]), fv(v[1]), fv(v[2]), 1.0f); }
void Vertex3sv(Context& ctx, const GLshort * v) { if (v) emitVertex(ctx, fv(v[0]), fv(v[1]), fv(v[2]), 1.0f); }
void Vertex4dv(Context& ctx, const GLdouble * v) { if (v) emitVertex(ctx, fv(v[0]), fv(v[1]), fv(v[2]), fv(v[3])); }
void Vertex4fv(Context& ctx, const GLfloat * v) { if (v) emitVertex(ctx, fv(v[0]), fv(v[1]), fv(v[2]), fv(v[3])); }
void Vertex4iv(Context& ctx, const GLint * v) { if (v) emitVertex(ctx, fv(v[0]), fv(v[1]), fv(v[2]), fv(v[3])); }
void Vertex4sv(Context& ctx, const GLshort * v) { if (v) emitVertex(ctx, fv(v[0]), fv(v[1]), fv(v[2]), fv(v[3])); }

void EdgeFlag(Context& ctx, GLboolean flag) {
    if (!compatOnly(ctx)) return;
    if (compat::listCompiling(ctx)) compat::recordCommand(ctx, [flag](Context& c) { c.state.fixed.edgeFlag = flag != GL_FALSE; });
    if (compat::listExecutes(ctx)) ctx.state.fixed.edgeFlag = flag != GL_FALSE;
}
void EdgeFlagv(Context& ctx, const GLboolean* flag) { if (flag) EdgeFlag(ctx, *flag); }

// Colour index mode is not supported: index commands are accepted and ignored.
void Indexd(Context& ctx, GLdouble) { compatOnly(ctx); }
void Indexdv(Context& ctx, const GLdouble*) { compatOnly(ctx); }
void Indexf(Context& ctx, GLfloat) { compatOnly(ctx); }
void Indexfv(Context& ctx, const GLfloat*) { compatOnly(ctx); }
void Indexi(Context& ctx, GLint) { compatOnly(ctx); }
void Indexiv(Context& ctx, const GLint*) { compatOnly(ctx); }
void Indexs(Context& ctx, GLshort) { compatOnly(ctx); }
void Indexsv(Context& ctx, const GLshort*) { compatOnly(ctx); }
void Indexub(Context& ctx, GLubyte) { compatOnly(ctx); }
void Indexubv(Context& ctx, const GLubyte*) { compatOnly(ctx); }

// glRect: a polygon in the z = 0 plane (GL 3.3 compatibility section 2.11).
namespace {
void rect(Context& ctx, float x1, float y1, float x2, float y2) {
    if (!compatOnly(ctx)) return;
    if (insideBeginEnd(ctx)) return ctx.setError(GL_INVALID_OPERATION);
    Begin(ctx, GL_POLYGON);
    emitVertex(ctx, x1, y1, 0, 1);
    emitVertex(ctx, x2, y1, 0, 1);
    emitVertex(ctx, x2, y2, 0, 1);
    emitVertex(ctx, x1, y2, 0, 1);
    End(ctx);
}
}  // namespace
void Rectd(Context& ctx, GLdouble x1, GLdouble y1, GLdouble x2, GLdouble y2) { rect(ctx, fv(x1), fv(y1), fv(x2), fv(y2)); }
void Rectdv(Context& ctx, const GLdouble* v1, const GLdouble* v2) { rect(ctx, fv(v1[0]), fv(v1[1]), fv(v2[0]), fv(v2[1])); }
void Rectf(Context& ctx, GLfloat x1, GLfloat y1, GLfloat x2, GLfloat y2) { rect(ctx, x1, y1, x2, y2); }
void Rectfv(Context& ctx, const GLfloat* v1, const GLfloat* v2) { rect(ctx, v1[0], v1[1], v2[0], v2[1]); }
void Recti(Context& ctx, GLint x1, GLint y1, GLint x2, GLint y2) { rect(ctx, fv(x1), fv(y1), fv(x2), fv(y2)); }
void Rectiv(Context& ctx, const GLint* v1, const GLint* v2) { rect(ctx, fv(v1[0]), fv(v1[1]), fv(v2[0]), fv(v2[1])); }
void Rects(Context& ctx, GLshort x1, GLshort y1, GLshort x2, GLshort y2) { rect(ctx, fv(x1), fv(y1), fv(x2), fv(y2)); }
void Rectsv(Context& ctx, const GLshort* v1, const GLshort* v2) { rect(ctx, fv(v1[0]), fv(v1[1]), fv(v2[0]), fv(v2[1])); }

// ------------------------------------------------------------ packed attributes (GL_ARB_vertex_type_2_10_10_10_rev)

namespace {

// Decodes a packed 2_10_10_10 value (GL 3.3 section 2.8.2); `n` components.
bool unpack(Context& ctx, GLenum type, bool normalized, GLuint word, float out[4]) {
    if (type != GL_INT_2_10_10_10_REV && type != GL_UNSIGNED_INT_2_10_10_10_REV) {
        ctx.setError(GL_INVALID_ENUM);
        return false;
    }
    const int widths[4] = {10, 10, 10, 2};
    int shift = 0;
    for (int c = 0; c < 4; ++c) {
        const uint32_t raw = (word >> shift) & ((1u << widths[c]) - 1);
        shift += widths[c];
        if (type == GL_UNSIGNED_INT_2_10_10_10_REV) {
            out[c] = normalized ? raw / static_cast<float>((1u << widths[c]) - 1) : static_cast<float>(raw);
        } else {
            const int32_t value = static_cast<int32_t>(raw << (32 - widths[c])) >> (32 - widths[c]);
            const float max = static_cast<float>((1 << (widths[c] - 1)) - 1);
            out[c] = normalized ? std::max(-1.0f, value / max) : static_cast<float>(value);
        }
    }
    return true;
}

void packedAttribute(Context& ctx, int slot, GLenum type, bool normalized, GLuint value, int n) {
    if (!compatOnly(ctx) || slot < 0) return;
    float v[4];
    if (!unpack(ctx, type, normalized, value, v)) return;
    for (int c = n; c < 4; ++c) v[c] = c == 3 ? 1.0f : 0.0f;
    if (slot == kConvVertex) return emitVertex(ctx, v[0], v[1], v[2], v[3]);
    setAttribute(ctx, slot, v[0], v[1], v[2], v[3]);
}

}  // namespace

void VertexP2ui(Context& ctx, GLenum type, GLuint value) { packedAttribute(ctx, kConvVertex, type, false, value, 2); }
void VertexP2uiv(Context& ctx, GLenum type, const GLuint* value) { if (value) VertexP2ui(ctx, type, *value); }
void VertexP3ui(Context& ctx, GLenum type, GLuint value) { packedAttribute(ctx, kConvVertex, type, false, value, 3); }
void VertexP3uiv(Context& ctx, GLenum type, const GLuint* value) { if (value) VertexP3ui(ctx, type, *value); }
void VertexP4ui(Context& ctx, GLenum type, GLuint value) { packedAttribute(ctx, kConvVertex, type, false, value, 4); }
void VertexP4uiv(Context& ctx, GLenum type, const GLuint* value) { if (value) VertexP4ui(ctx, type, *value); }
void NormalP3ui(Context& ctx, GLenum type, GLuint coords) { packedAttribute(ctx, kConvNormal, type, true, coords, 3); }
void NormalP3uiv(Context& ctx, GLenum type, const GLuint* coords) { if (coords) NormalP3ui(ctx, type, *coords); }
void ColorP3ui(Context& ctx, GLenum type, GLuint color) { packedAttribute(ctx, kConvColor, type, true, color, 3); }
void ColorP3uiv(Context& ctx, GLenum type, const GLuint* color) { if (color) ColorP3ui(ctx, type, *color); }
void ColorP4ui(Context& ctx, GLenum type, GLuint color) { packedAttribute(ctx, kConvColor, type, true, color, 4); }
void ColorP4uiv(Context& ctx, GLenum type, const GLuint* color) { if (color) ColorP4ui(ctx, type, *color); }
void SecondaryColorP3ui(Context& ctx, GLenum type, GLuint color) { packedAttribute(ctx, kConvSecondaryColor, type, true, color, 3); }
void SecondaryColorP3uiv(Context& ctx, GLenum type, const GLuint* color) { if (color) SecondaryColorP3ui(ctx, type, *color); }
void TexCoordP1ui(Context& ctx, GLenum type, GLuint coords) { packedAttribute(ctx, kConvTexCoord0, type, false, coords, 1); }
void TexCoordP1uiv(Context& ctx, GLenum type, const GLuint* coords) { if (coords) TexCoordP1ui(ctx, type, *coords); }
void TexCoordP2ui(Context& ctx, GLenum type, GLuint coords) { packedAttribute(ctx, kConvTexCoord0, type, false, coords, 2); }
void TexCoordP2uiv(Context& ctx, GLenum type, const GLuint* coords) { if (coords) TexCoordP2ui(ctx, type, *coords); }
void TexCoordP3ui(Context& ctx, GLenum type, GLuint coords) { packedAttribute(ctx, kConvTexCoord0, type, false, coords, 3); }
void TexCoordP3uiv(Context& ctx, GLenum type, const GLuint* coords) { if (coords) TexCoordP3ui(ctx, type, *coords); }
void TexCoordP4ui(Context& ctx, GLenum type, GLuint coords) { packedAttribute(ctx, kConvTexCoord0, type, false, coords, 4); }
void TexCoordP4uiv(Context& ctx, GLenum type, const GLuint* coords) { if (coords) TexCoordP4ui(ctx, type, *coords); }
void MultiTexCoordP1ui(Context& ctx, GLenum texture, GLenum type, GLuint coords) {
    packedAttribute(ctx, texCoordSlot(ctx, texture), type, false, coords, 1);
}
void MultiTexCoordP1uiv(Context& ctx, GLenum texture, GLenum type, const GLuint* coords) { if (coords) MultiTexCoordP1ui(ctx, texture, type, *coords); }
void MultiTexCoordP2ui(Context& ctx, GLenum texture, GLenum type, GLuint coords) {
    packedAttribute(ctx, texCoordSlot(ctx, texture), type, false, coords, 2);
}
void MultiTexCoordP2uiv(Context& ctx, GLenum texture, GLenum type, const GLuint* coords) { if (coords) MultiTexCoordP2ui(ctx, texture, type, *coords); }
void MultiTexCoordP3ui(Context& ctx, GLenum texture, GLenum type, GLuint coords) {
    packedAttribute(ctx, texCoordSlot(ctx, texture), type, false, coords, 3);
}
void MultiTexCoordP3uiv(Context& ctx, GLenum texture, GLenum type, const GLuint* coords) { if (coords) MultiTexCoordP3ui(ctx, texture, type, *coords); }
void MultiTexCoordP4ui(Context& ctx, GLenum texture, GLenum type, GLuint coords) {
    packedAttribute(ctx, texCoordSlot(ctx, texture), type, false, coords, 4);
}
void MultiTexCoordP4uiv(Context& ctx, GLenum texture, GLenum type, const GLuint* coords) { if (coords) MultiTexCoordP4ui(ctx, texture, type, *coords); }

// ------------------------------------------------------------ conventional arrays

namespace {

enum TypeSet : unsigned {
    kTByte = 1, kTUByte = 2, kTShort = 4, kTUShort = 8, kTInt = 16, kTUInt = 32, kTFloat = 64, kTDouble = 128,
    kTHalf = 256, kTPacked = 512,
};

unsigned typeBit(GLenum type) {
    switch (type) {
    case GL_BYTE: return kTByte;
    case GL_UNSIGNED_BYTE: return kTUByte;
    case GL_SHORT: return kTShort;
    case GL_UNSIGNED_SHORT: return kTUShort;
    case GL_INT: return kTInt;
    case GL_UNSIGNED_INT: return kTUInt;
    case GL_FLOAT: return kTFloat;
    case GL_DOUBLE: return kTDouble;
    case GL_HALF_FLOAT: return kTHalf;
    case GL_INT_2_10_10_10_REV: case GL_UNSIGNED_INT_2_10_10_10_REV: return kTPacked;
    default: return 0;
    }
}

// glVertexPointer and friends (GL 3.3 compatibility table 2.5).
void conventionalPointer(Context& ctx, int slot, GLint size, bool sizeOk, GLenum type, unsigned types, bool normalized,
                         GLsizei stride, const void* pointer) {
    if (!compatOnly(ctx)) return;
    if (!sizeOk || stride < 0) return ctx.setError(GL_INVALID_VALUE);
    if (!(typeBit(type) & types)) return ctx.setError(GL_INVALID_ENUM);
    if (typeBit(type) == kTPacked && size != 4 && size != 3 && size != GL_BGRA) return ctx.setError(GL_INVALID_OPERATION);
    if (size == GL_BGRA && type != GL_UNSIGNED_BYTE && typeBit(type) != kTPacked) return ctx.setError(GL_INVALID_OPERATION);
    VertexArray& vao = *ctx.state.vertexArray;
    VertexAttrib& a = vao.attribs[static_cast<size_t>(slot)];
    a.size = size;
    a.type = type;
    a.normalized = normalized;
    a.integer = false;
    a.isDouble = false;
    a.stride = stride;
    a.offset = reinterpret_cast<GLintptr>(pointer);
    a.buffer = ctx.state.arrayBuffer;
    ++vao.version;
    ctx.markDirty(kDirtyVertexInput);
}

int clientArraySlot(Context& ctx, GLenum array) {
    switch (array) {
    case GL_VERTEX_ARRAY: return kConvVertex;
    case GL_NORMAL_ARRAY: return kConvNormal;
    case GL_COLOR_ARRAY: return kConvColor;
    case GL_SECONDARY_COLOR_ARRAY: return kConvSecondaryColor;
    case GL_FOG_COORD_ARRAY: return kConvFogCoord;
    case GL_TEXTURE_COORD_ARRAY: return kConvTexCoord0 + static_cast<int>(ctx.state.fixed.clientActiveTexture);
    case GL_EDGE_FLAG_ARRAY: case GL_INDEX_ARRAY: return -2;  // accepted, no effect
    default: return -1;
    }
}

void clientState(Context& ctx, GLenum array, bool on) {
    if (!compatOnly(ctx)) return;
    const int slot = clientArraySlot(ctx, array);
    if (slot == -1) return ctx.setError(GL_INVALID_ENUM);
    if (slot < 0) return;
    VertexArray& vao = *ctx.state.vertexArray;
    vao.attribs[static_cast<size_t>(slot)].enabled = on;
    ++vao.version;
    ctx.markDirty(kDirtyVertexInput);
}

}  // namespace

void VertexPointer(Context& ctx, GLint size, GLenum type, GLsizei stride, const void* pointer) {
    conventionalPointer(ctx, kConvVertex, size, size >= 2 && size <= 4, type,
                        kTShort | kTInt | kTFloat | kTDouble | kTHalf | kTPacked, false, stride, pointer);
}
void NormalPointer(Context& ctx, GLenum type, GLsizei stride, const void* pointer) {
    conventionalPointer(ctx, kConvNormal, 3, true, type, kTByte | kTShort | kTInt | kTFloat | kTDouble | kTHalf | kTPacked,
                        true, stride, pointer);
}
void ColorPointer(Context& ctx, GLint size, GLenum type, GLsizei stride, const void* pointer) {
    conventionalPointer(ctx, kConvColor, size, size == 3 || size == 4 || size == GL_BGRA, type,
                        kTByte | kTUByte | kTShort | kTUShort | kTInt | kTUInt | kTFloat | kTDouble | kTHalf | kTPacked, true,
                        stride, pointer);
}
void SecondaryColorPointer(Context& ctx, GLint size, GLenum type, GLsizei stride, const void* pointer) {
    conventionalPointer(ctx, kConvSecondaryColor, size, size == 3 || size == GL_BGRA, type,
                        kTByte | kTUByte | kTShort | kTUShort | kTInt | kTUInt | kTFloat | kTDouble | kTHalf | kTPacked, true,
                        stride, pointer);
}
void FogCoordPointer(Context& ctx, GLenum type, GLsizei stride, const void* pointer) {
    conventionalPointer(ctx, kConvFogCoord, 1, true, type, kTFloat | kTDouble | kTHalf, false, stride, pointer);
}
void TexCoordPointer(Context& ctx, GLint size, GLenum type, GLsizei stride, const void* pointer) {
    conventionalPointer(ctx, kConvTexCoord0 + static_cast<int>(ctx.state.fixed.clientActiveTexture), size, size >= 1 && size <= 4,
                        type, kTShort | kTInt | kTFloat | kTDouble | kTHalf | kTPacked, false, stride, pointer);
}
void EdgeFlagPointer(Context& ctx, GLsizei stride, const void*) {
    if (compatOnly(ctx) && stride < 0) ctx.setError(GL_INVALID_VALUE);
}
void IndexPointer(Context& ctx, GLenum, GLsizei stride, const void*) {
    if (compatOnly(ctx) && stride < 0) ctx.setError(GL_INVALID_VALUE);
}

void EnableClientState(Context& ctx, GLenum array) { clientState(ctx, array, true); }
void DisableClientState(Context& ctx, GLenum array) { clientState(ctx, array, false); }

// GL_EXT_compiled_vertex_array: a hint that the arrays do not change; arrays
// are always read at draw time.
void LockArraysEXT(Context& ctx, GLint first, GLsizei count) {
    if (first < 0 || count <= 0) ctx.setError(GL_INVALID_VALUE);
}
void UnlockArraysEXT(Context& ctx) { (void)ctx; }

bool getClientCapability(Context& ctx, GLenum cap, bool& value) {
    if (!ctx.isCompat()) return false;
    const int slot = clientArraySlot(ctx, cap);
    if (slot == -1) return false;
    value = slot >= 0 && ctx.state.vertexArray->attribs[static_cast<size_t>(slot)].enabled;
    return true;
}

void InterleavedArrays(Context& ctx, GLenum format, GLsizei stride, const void* pointer) {
    if (!compatOnly(ctx)) return;
    if (stride < 0) return ctx.setError(GL_INVALID_VALUE);
    // GL 3.3 compatibility table 2.6: texture, colour, normal and vertex parts.
    struct Layout { GLenum format; int t, c; GLenum ctype; bool n; int v; };
    static const Layout kLayouts[] = {
        {GL_V2F, 0, 0, 0, false, 2}, {GL_V3F, 0, 0, 0, false, 3}, {GL_C4UB_V2F, 0, 4, GL_UNSIGNED_BYTE, false, 2},
        {GL_C4UB_V3F, 0, 4, GL_UNSIGNED_BYTE, false, 3}, {GL_C3F_V3F, 0, 3, GL_FLOAT, false, 3},
        {GL_N3F_V3F, 0, 0, 0, true, 3}, {GL_C4F_N3F_V3F, 0, 4, GL_FLOAT, true, 3}, {GL_T2F_V3F, 2, 0, 0, false, 3},
        {GL_T4F_V4F, 4, 0, 0, false, 4}, {GL_T2F_C4UB_V3F, 2, 4, GL_UNSIGNED_BYTE, false, 3},
        {GL_T2F_C3F_V3F, 2, 3, GL_FLOAT, false, 3}, {GL_T2F_N3F_V3F, 2, 0, 0, true, 3},
        {GL_T2F_C4F_N3F_V3F, 2, 4, GL_FLOAT, true, 3}, {GL_T4F_C4F_N3F_V4F, 4, 4, GL_FLOAT, true, 4},
    };
    const Layout* layout = nullptr;
    for (const Layout& l : kLayouts)
        if (l.format == format) layout = &l;
    if (!layout) return ctx.setError(GL_INVALID_ENUM);
    const int colorBytes = layout->c ? (layout->ctype == GL_UNSIGNED_BYTE ? 4 : layout->c * 4) : 0;
    const int pc = layout->t * 4, pn = pc + colorBytes, pv = pn + (layout->n ? 12 : 0);
    const GLsizei s = stride ? stride : pv + layout->v * 4;
    const auto* base = static_cast<const uint8_t*>(pointer);
    DisableClientState(ctx, GL_EDGE_FLAG_ARRAY);
    DisableClientState(ctx, GL_INDEX_ARRAY);
    DisableClientState(ctx, GL_SECONDARY_COLOR_ARRAY);
    DisableClientState(ctx, GL_FOG_COORD_ARRAY);
    if (layout->t) {
        EnableClientState(ctx, GL_TEXTURE_COORD_ARRAY);
        TexCoordPointer(ctx, layout->t, GL_FLOAT, s, base);
    } else {
        DisableClientState(ctx, GL_TEXTURE_COORD_ARRAY);
    }
    if (layout->c) {
        EnableClientState(ctx, GL_COLOR_ARRAY);
        ColorPointer(ctx, layout->c, layout->ctype, s, base + pc);
    } else {
        DisableClientState(ctx, GL_COLOR_ARRAY);
    }
    if (layout->n) {
        EnableClientState(ctx, GL_NORMAL_ARRAY);
        NormalPointer(ctx, GL_FLOAT, s, base + pn);
    } else {
        DisableClientState(ctx, GL_NORMAL_ARRAY);
    }
    EnableClientState(ctx, GL_VERTEX_ARRAY);
    VertexPointer(ctx, layout->v, GL_FLOAT, s, base + pv);
}

// glArrayElement: element `i` of every enabled array, as the matching
// immediate-mode commands (GL 3.3 compatibility section 2.8.3).
void ArrayElement(Context& ctx, GLint i) {
    if (!compatOnly(ctx)) return;
    if (i < 0) return ctx.setError(GL_INVALID_VALUE);
    const VertexArray& vao = *ctx.state.vertexArray;
    const VertexAttrib* position = vao.attribs[kConvVertex].enabled ? &vao.attribs[kConvVertex]
                                   : vao.attribs[0].enabled         ? &vao.attribs[0]
                                                                    : nullptr;
    for (int slot = 0; slot < kAttribSlotCount; ++slot) {
        const VertexAttrib& a = vao.attribs[static_cast<size_t>(slot)];
        if (!a.enabled || &a == position || slot == kConvVertex) continue;
        ImmediateBatch::Value value;
        GLenum type;
        if (compat::fetch(ctx, a, static_cast<uint32_t>(i), value, type)) setAttribute(ctx, slot, value, type);
    }
    ImmediateBatch::Value value;
    GLenum type;
    if (position && compat::fetch(ctx, *position, static_cast<uint32_t>(i), value, type)) {
        float v[4];
        std::memcpy(v, value.data(), 16);
        emitVertex(ctx, v[0], v[1], v[2], v[3]);
    }
}

// Conventional array pointers (glGetPointerv).
bool getConventionalPointer(Context& ctx, GLenum pname, void** params) {
    if (!ctx.isCompat()) return false;
    int slot;
    switch (pname) {
    case GL_VERTEX_ARRAY_POINTER: slot = kConvVertex; break;
    case GL_NORMAL_ARRAY_POINTER: slot = kConvNormal; break;
    case GL_COLOR_ARRAY_POINTER: slot = kConvColor; break;
    case GL_SECONDARY_COLOR_ARRAY_POINTER: slot = kConvSecondaryColor; break;
    case GL_FOG_COORD_ARRAY_POINTER: slot = kConvFogCoord; break;
    case GL_TEXTURE_COORD_ARRAY_POINTER: slot = kConvTexCoord0 + static_cast<int>(ctx.state.fixed.clientActiveTexture); break;
    case GL_EDGE_FLAG_ARRAY_POINTER: case GL_INDEX_ARRAY_POINTER: case GL_FEEDBACK_BUFFER_POINTER:
    case GL_SELECTION_BUFFER_POINTER:
        *params = nullptr;
        return true;
    default: return false;
    }
    *params = reinterpret_cast<void*>(ctx.state.vertexArray->attribs[static_cast<size_t>(slot)].offset);
    return true;
}

}  // namespace api
}  // namespace wmgl
