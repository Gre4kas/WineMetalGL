// Vertex arrays and vertex attribute state (GL 3.3 sections 2.8, 2.10).

#include <algorithm>
#include <cstring>

#include "core/compat/immediate.h"
#include "core/context.h"
#include "core/limits.h"
#include "gl_api.gen.h"

namespace wmgl {

GLsizei VertexAttrib::effectiveStride() const {
    if (zeroStride) return 0;
    return stride ? stride : static_cast<GLsizei>(elementBytes());
}

void VertexArray::resolveBinding(GLuint index) {
    const VertexBinding& b = bindings[index];
    for (int i = 0; i < kMaxAttribs; ++i) {
        VertexAttrib& a = attribs[static_cast<size_t>(i)];
        if (a.binding != index) continue;
        a.buffer = b.buffer;
        a.offset = b.offset + static_cast<GLintptr>(a.relativeOffset);
        a.stride = b.stride;
        a.zeroStride = b.stride == 0;
        a.divisor = b.divisor;
    }
    ++version;
}

uint32_t VertexAttrib::elementBytes() const {
    const int n = size == GL_BGRA ? 4 : size;
    switch (type) {
    case GL_BYTE: case GL_UNSIGNED_BYTE: return static_cast<uint32_t>(n);
    case GL_SHORT: case GL_UNSIGNED_SHORT: case GL_HALF_FLOAT: return static_cast<uint32_t>(2 * n);
    case GL_DOUBLE: return static_cast<uint32_t>(8 * n);
    case GL_INT_2_10_10_10_REV: case GL_UNSIGNED_INT_2_10_10_10_REV: case GL_UNSIGNED_INT_10F_11F_11F_REV: return 4;
    default: return static_cast<uint32_t>(4 * n);
    }
}

namespace api {
namespace {

VertexArray* currentVao(Context& ctx) { return ctx.state.vertexArray.get(); }

bool checkIndex(Context& ctx, GLuint index) {
    if (index >= static_cast<GLuint>(VertexArray::kMaxAttribs)) {
        ctx.setError(GL_INVALID_VALUE);
        return false;
    }
    return true;
}

// Validates a vertex format (glVertexAttrib*Pointer, glVertexAttrib*Format);
// false with the error set.
bool validFormat(Context& ctx, GLint size, GLenum type, bool normalized, bool integer, bool isDouble) {
    const bool packed = type == GL_INT_2_10_10_10_REV || type == GL_UNSIGNED_INT_2_10_10_10_REV ||
                        type == GL_UNSIGNED_INT_10F_11F_11F_REV;
    auto fail = [&](GLenum error) { ctx.setError(error); return false; };
    if (size == GL_BGRA) {
        if (integer || isDouble) return fail(GL_INVALID_VALUE);
        if (type != GL_UNSIGNED_BYTE && type != GL_INT_2_10_10_10_REV && type != GL_UNSIGNED_INT_2_10_10_10_REV)
            return fail(GL_INVALID_OPERATION);
        if (!normalized) return fail(GL_INVALID_OPERATION);
    } else if (size < 1 || size > 4) {
        return fail(GL_INVALID_VALUE);
    }
    if (integer) {
        switch (type) {
        case GL_BYTE: case GL_UNSIGNED_BYTE: case GL_SHORT: case GL_UNSIGNED_SHORT: case GL_INT: case GL_UNSIGNED_INT: break;
        default: return fail(GL_INVALID_ENUM);
        }
    } else if (isDouble) {
        if (type != GL_DOUBLE) return fail(GL_INVALID_ENUM);
    } else {
        switch (type) {
        case GL_BYTE: case GL_UNSIGNED_BYTE: case GL_SHORT: case GL_UNSIGNED_SHORT: case GL_INT: case GL_UNSIGNED_INT:
        case GL_HALF_FLOAT: case GL_FLOAT: case GL_DOUBLE: case GL_FIXED: case GL_INT_2_10_10_10_REV:
        case GL_UNSIGNED_INT_2_10_10_10_REV: break;
        case GL_UNSIGNED_INT_10F_11F_11F_REV:
            if (ctx.version() >= 44) break;
            return fail(GL_INVALID_ENUM);
        default: return fail(GL_INVALID_ENUM);
        }
        if (packed && size != 4 && size != GL_BGRA && type != GL_UNSIGNED_INT_10F_11F_11F_REV) return fail(GL_INVALID_OPERATION);
        // GL_UNSIGNED_INT_10F_11F_11F_REV holds exactly three components (GL 4.4).
        if (type == GL_UNSIGNED_INT_10F_11F_11F_REV && size != 3) return fail(GL_INVALID_OPERATION);
    }
    return true;
}

void setPointer(Context& ctx, GLuint index, GLint size, GLenum type, bool normalized, bool integer, bool isDouble,
                GLsizei stride, const void* pointer) {
    if (!checkIndex(ctx, index)) return;
    if (stride < 0) return ctx.setError(GL_INVALID_VALUE);
    if (!validFormat(ctx, size, type, normalized, integer, isDouble)) return;
    // Core profile: arrays must come from a buffer object (except offset 0 with no VAO is still an error).
    // OpenGL ES: client memory arrays in vertex array 0 only.
    const bool defaultVao = currentVao(ctx) == ctx.defaultVertexArray.get();
    if (ctx.isCore() && !ctx.state.arrayBuffer && pointer && !(ctx.isEs() && defaultVao))
        return ctx.setError(GL_INVALID_OPERATION);
    if (ctx.isDesktopCore() && defaultVao) return ctx.setError(GL_INVALID_OPERATION);
    // GL 4.3: the format, binding `index`, and that binding's buffer,
    // offset (the pointer) and stride (tightly packed for 0).
    VertexArray& vao = *currentVao(ctx);
    VertexAttrib& a = vao.attribs[index];
    a.size = size;
    a.type = type;
    a.normalized = normalized;
    a.integer = integer;
    a.isDouble = isDouble;
    a.pointerStride = stride;
    a.pointer = reinterpret_cast<GLintptr>(pointer);
    a.binding = index;
    a.relativeOffset = 0;
    VertexBinding& b = vao.bindings[index];
    b.buffer = ctx.state.arrayBuffer;
    b.offset = reinterpret_cast<GLintptr>(pointer);
    b.stride = stride ? stride : static_cast<GLsizei>(a.elementBytes());
    vao.resolveBinding(index);
    ctx.markDirty(kDirtyVertexInput);
}

}  // namespace

void GenVertexArrays(Context& ctx, GLsizei n, GLuint* arrays) {
    if (n < 0) return ctx.setError(GL_INVALID_VALUE);
    if (arrays) ctx.vertexArrays.generate(n, arrays);
}

void DeleteVertexArrays(Context& ctx, GLsizei n, const GLuint* arrays) {
    if (n < 0) return ctx.setError(GL_INVALID_VALUE);
    if (!arrays) return;
    for (GLsizei i = 0; i < n; ++i) {
        if (!arrays[i]) continue;
        RefPtr<VertexArray> vao = ctx.vertexArrays.remove(arrays[i]);
        if (vao && ctx.state.vertexArray == vao) {
            ctx.state.vertexArray = ctx.defaultVertexArray;
            ctx.markDirty(kDirtyVertexInput);
        }
    }
}

void BindVertexArray(Context& ctx, GLuint array) {
    if (!array) {
        ctx.state.vertexArray = ctx.defaultVertexArray;
        ctx.markDirty(kDirtyVertexInput);
        return;
    }
    if (!ctx.vertexArrays.isGenerated(array)) return ctx.setError(GL_INVALID_OPERATION);
    RefPtr<VertexArray> vao = ctx.vertexArrays.getRef(array);
    if (!vao) {
        vao = makeRef<VertexArray>(array);
        ctx.vertexArrays.set(array, vao);
    }
    ctx.state.vertexArray = vao;
    ctx.markDirty(kDirtyVertexInput);
}

GLboolean IsVertexArray(Context& ctx, GLuint array) { return ctx.vertexArrays.exists(array) ? GL_TRUE : GL_FALSE; }

void VertexAttribPointer(Context& ctx, GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride,
                         const void* pointer) {
    setPointer(ctx, index, size, type, normalized != GL_FALSE, false, false, stride, pointer);
}

void VertexAttribIPointer(Context& ctx, GLuint index, GLint size, GLenum type, GLsizei stride, const void* pointer) {
    setPointer(ctx, index, size, type, false, true, false, stride, pointer);
}

void VertexAttribLPointer(Context& ctx, GLuint index, GLint size, GLenum type, GLsizei stride, const void* pointer) {
    setPointer(ctx, index, size, type, false, false, true, stride, pointer);
}

// GL 3.3 core (appendix E) only makes VertexAttribPointer an error without a
// bound VAO; GL 4.5 extends that to the commands that change VAO state.
// Enabling an array is not checked: the CTS (get_uniform) enables one in a
// core context without a VAO and expects no error, as drivers behave.
static bool vaoStateWritable(Context& ctx) {
    return !(ctx.isDesktopCore() && ctx.version() >= 45 && currentVao(ctx) == ctx.defaultVertexArray.get());
}

void EnableVertexAttribArray(Context& ctx, GLuint index) {
    if (!checkIndex(ctx, index)) return;
    currentVao(ctx)->attribs[index].enabled = true;
    ++currentVao(ctx)->version;
    ctx.markDirty(kDirtyVertexInput);
}

void DisableVertexAttribArray(Context& ctx, GLuint index) {
    if (!checkIndex(ctx, index)) return;
    currentVao(ctx)->attribs[index].enabled = false;
    ++currentVao(ctx)->version;
    ctx.markDirty(kDirtyVertexInput);
}

void VertexAttribDivisor(Context& ctx, GLuint index, GLuint divisor) {
    if (!checkIndex(ctx, index)) return;
    if (!vaoStateWritable(ctx)) return ctx.setError(GL_INVALID_OPERATION);
    // GL 4.3: VertexAttribBinding(index, index) and VertexBindingDivisor(index, divisor).
    VertexArray& vao = *currentVao(ctx);
    if (vao.attribs[index].binding != index) {
        vao.attribs[index].binding = index;
        vao.resolveBinding(index);
    }
    vao.bindings[index].divisor = divisor;
    vao.resolveBinding(index);
    ctx.markDirty(kDirtyVertexInput);
}

// --- GL 4.3 vertex attribute bindings (GL_ARB_vertex_attrib_binding)

namespace {

bool attribBindings(Context& ctx, const char* command) {
    if (ctx.version() >= 43 || ctx.hasExtension("GL_ARB_vertex_attrib_binding")) return true;
    ctx.unimplemented(command);
    return false;
}

// These commands change VAO state: an error on the default VAO of a core
// context (OpenGL ES's vertex array 0 is an object like the others).
bool bindingStateWritable(Context& ctx) {
    if (ctx.isDesktopCore() && currentVao(ctx) == ctx.defaultVertexArray.get()) {
        ctx.setError(GL_INVALID_OPERATION);
        return false;
    }
    return true;
}

void setFormat(Context& ctx, GLuint index, GLint size, GLenum type, bool normalized, bool integer, bool isDouble,
               GLuint relativeOffset, const char* command) {
    if (!attribBindings(ctx, command) || !bindingStateWritable(ctx)) return;
    if (!checkIndex(ctx, index)) return;
    if (relativeOffset > static_cast<GLuint>(limits::kMaxVertexAttribRelativeOffset)) return ctx.setError(GL_INVALID_VALUE);
    if (!validFormat(ctx, size, type, normalized, integer, isDouble)) return;
    VertexArray& vao = *currentVao(ctx);
    VertexAttrib& a = vao.attribs[index];
    a.size = size;
    a.type = type;
    a.normalized = normalized;
    a.integer = integer;
    a.isDouble = isDouble;
    a.relativeOffset = relativeOffset;
    vao.resolveBinding(a.binding);
    ctx.markDirty(kDirtyVertexInput);
}

}  // namespace

void BindVertexBuffer(Context& ctx, GLuint bindingindex, GLuint buffer, GLintptr offset, GLsizei stride) {
    if (!attribBindings(ctx, "glBindVertexBuffer") || !bindingStateWritable(ctx)) return;
    if (bindingindex >= static_cast<GLuint>(VertexArray::kMaxBindings) || offset < 0 || stride < 0)
        return ctx.setError(GL_INVALID_VALUE);
    if (ctx.version() >= 44 && stride > limits::kMaxVertexAttribStride) return ctx.setError(GL_INVALID_VALUE);
    RefPtr<Buffer> object;
    if (buffer) {
        ShareLock lock(ctx);
        if (!ctx.share().buffers.isGenerated(buffer)) return ctx.setError(GL_INVALID_OPERATION);
        object = ctx.share().buffers.getRef(buffer);
        if (!object) {
            object = makeRef<Buffer>(buffer);
            ctx.share().buffers.set(buffer, object);
        }
    }
    VertexArray& vao = *currentVao(ctx);
    VertexBinding& b = vao.bindings[bindingindex];
    b.buffer = object;
    b.offset = offset;
    b.stride = stride;
    vao.resolveBinding(bindingindex);
    ctx.markDirty(kDirtyVertexInput);
}

// glBindVertexBuffers (GL 4.4, GL_ARB_multi_bind): a range of the current
// vertex array's buffer bindings. A null array unbinds them (offset 0,
// stride 16); an error in one binding leaves it alone.
void BindVertexBuffers(Context& ctx, GLuint first, GLsizei count, const GLuint* buffers, const GLintptr* offsets,
                       const GLsizei* strides) {
    if (ctx.version() < 44 && !ctx.hasExtension("GL_ARB_multi_bind")) return ctx.unimplemented("glBindVertexBuffers");
    if (!attribBindings(ctx, "glBindVertexBuffers") || !bindingStateWritable(ctx)) return;
    if (count < 0) return ctx.setError(GL_INVALID_VALUE);
    if (static_cast<uint64_t>(first) + static_cast<uint64_t>(count) > static_cast<uint64_t>(VertexArray::kMaxBindings))
        return ctx.setError(GL_INVALID_OPERATION);
    VertexArray& vao = *currentVao(ctx);
    for (GLsizei i = 0; i < count; ++i) {
        const GLuint index = first + static_cast<GLuint>(i);
        VertexBinding& b = vao.bindings[index];
        if (!buffers) {
            b.buffer = nullptr;
            b.offset = 0;
            b.stride = 16;
            vao.resolveBinding(index);
            continue;
        }
        const GLintptr offset = offsets[i];
        const GLsizei stride = strides[i];
        if (offset < 0 || stride < 0 || stride > limits::kMaxVertexAttribStride) {
            ctx.setError(GL_INVALID_VALUE);
            continue;
        }
        RefPtr<Buffer> object;
        if (buffers[i]) {
            ShareLock lock(ctx);
            object = ctx.share().buffers.getRef(buffers[i]);
            if (!object && ctx.share().buffers.isGenerated(buffers[i])) {
                object = makeRef<Buffer>(buffers[i]);
                ctx.share().buffers.set(buffers[i], object);
            }
        }
        if (buffers[i] && !object) {
            ctx.setError(GL_INVALID_OPERATION);
            continue;
        }
        b.buffer = object;
        b.offset = offset;
        b.stride = stride;
        vao.resolveBinding(index);
    }
    ctx.markDirty(kDirtyVertexInput);
}

void VertexAttribFormat(Context& ctx, GLuint attribindex, GLint size, GLenum type, GLboolean normalized, GLuint relativeoffset) {
    setFormat(ctx, attribindex, size, type, normalized != GL_FALSE, false, false, relativeoffset, "glVertexAttribFormat");
}

void VertexAttribIFormat(Context& ctx, GLuint attribindex, GLint size, GLenum type, GLuint relativeoffset) {
    setFormat(ctx, attribindex, size, type, false, true, false, relativeoffset, "glVertexAttribIFormat");
}

void VertexAttribLFormat(Context& ctx, GLuint attribindex, GLint size, GLenum type, GLuint relativeoffset) {
    setFormat(ctx, attribindex, size, type, false, false, true, relativeoffset, "glVertexAttribLFormat");
}

void VertexAttribBinding(Context& ctx, GLuint attribindex, GLuint bindingindex) {
    if (!attribBindings(ctx, "glVertexAttribBinding") || !bindingStateWritable(ctx)) return;
    if (!checkIndex(ctx, attribindex)) return;
    if (bindingindex >= static_cast<GLuint>(VertexArray::kMaxBindings)) return ctx.setError(GL_INVALID_VALUE);
    VertexArray& vao = *currentVao(ctx);
    vao.attribs[attribindex].binding = bindingindex;
    vao.resolveBinding(bindingindex);
    ctx.markDirty(kDirtyVertexInput);
}

void VertexBindingDivisor(Context& ctx, GLuint bindingindex, GLuint divisor) {
    if (!attribBindings(ctx, "glVertexBindingDivisor") || !bindingStateWritable(ctx)) return;
    if (bindingindex >= static_cast<GLuint>(VertexArray::kMaxBindings)) return ctx.setError(GL_INVALID_VALUE);
    VertexArray& vao = *currentVao(ctx);
    vao.bindings[bindingindex].divisor = divisor;
    vao.resolveBinding(bindingindex);
    ctx.markDirty(kDirtyVertexInput);
}

// --- generic attribute values

namespace {

// Current values go through compat::setAttribute: inside glBegin/glEnd
// they join the batch (generic attribute 0 specifies a vertex) and during
// display list compilation they are recorded (compat/immediate.h).
void setGenericF(Context& ctx, GLuint index, GLfloat x, GLfloat y, GLfloat z, GLfloat w) {
    if (!checkIndex(ctx, index)) return;
    compat::setAttribute(ctx, static_cast<int>(index), x, y, z, w);
    ctx.markDirty(kDirtyVertexInput);
}

template <class T>
void setGenericInteger(Context& ctx, GLuint index, T x, T y, T z, T w, GLenum type) {
    if (!checkIndex(ctx, index)) return;
    ImmediateBatch::Value value;
    const T v[4] = {x, y, z, w};
    std::memcpy(value.data(), v, 16);
    compat::setAttribute(ctx, static_cast<int>(index), value, type);
    ctx.markDirty(kDirtyVertexInput);
}

void setGenericI(Context& ctx, GLuint index, GLint x, GLint y, GLint z, GLint w) {
    setGenericInteger<GLint>(ctx, index, x, y, z, w, GL_INT);
}

void setGenericU(Context& ctx, GLuint index, GLuint x, GLuint y, GLuint z, GLuint w) {
    setGenericInteger<GLuint>(ctx, index, x, y, z, w, GL_UNSIGNED_INT);
}

// GL_ARB_vertex_attrib_64bit: the current value of a 64-bit input.
void setGenericL(Context& ctx, GLuint index, GLdouble x, GLdouble y, GLdouble z, GLdouble w) {
    if (!checkIndex(ctx, index)) return;
    ContextState::GenericAttrib& g = ctx.state.genericAttribs[index];
    g.d[0] = x;
    g.d[1] = y;
    g.d[2] = z;
    g.d[3] = w;
    g.type = GL_DOUBLE;
    ctx.markDirty(kDirtyVertexInput);
}

float norm(GLbyte v) { return std::max(-1.0f, v / 127.0f); }
float norm(GLubyte v) { return v / 255.0f; }
float norm(GLshort v) { return std::max(-1.0f, v / 32767.0f); }
float norm(GLushort v) { return v / 65535.0f; }
float norm(GLint v) { return std::max(-1.0f, static_cast<float>(v / 2147483647.0)); }
float norm(GLuint v) { return static_cast<float>(v / 4294967295.0); }

}  // namespace

void VertexAttrib1f(Context& ctx, GLuint i, GLfloat x) { setGenericF(ctx, i, x, 0, 0, 1); }
void VertexAttrib2f(Context& ctx, GLuint i, GLfloat x, GLfloat y) { setGenericF(ctx, i, x, y, 0, 1); }
void VertexAttrib3f(Context& ctx, GLuint i, GLfloat x, GLfloat y, GLfloat z) { setGenericF(ctx, i, x, y, z, 1); }
void VertexAttrib4f(Context& ctx, GLuint i, GLfloat x, GLfloat y, GLfloat z, GLfloat w) { setGenericF(ctx, i, x, y, z, w); }
void VertexAttrib1fv(Context& ctx, GLuint i, const GLfloat* v) { if (v) setGenericF(ctx, i, v[0], 0, 0, 1); }
void VertexAttrib2fv(Context& ctx, GLuint i, const GLfloat* v) { if (v) setGenericF(ctx, i, v[0], v[1], 0, 1); }
void VertexAttrib3fv(Context& ctx, GLuint i, const GLfloat* v) { if (v) setGenericF(ctx, i, v[0], v[1], v[2], 1); }
void VertexAttrib4fv(Context& ctx, GLuint i, const GLfloat* v) { if (v) setGenericF(ctx, i, v[0], v[1], v[2], v[3]); }
void VertexAttrib1d(Context& ctx, GLuint i, GLdouble x) { setGenericF(ctx, i, static_cast<float>(x), 0, 0, 1); }
void VertexAttrib2d(Context& ctx, GLuint i, GLdouble x, GLdouble y) { setGenericF(ctx, i, static_cast<float>(x), static_cast<float>(y), 0, 1); }
void VertexAttrib3d(Context& ctx, GLuint i, GLdouble x, GLdouble y, GLdouble z) {
    setGenericF(ctx, i, static_cast<float>(x), static_cast<float>(y), static_cast<float>(z), 1);
}
void VertexAttrib4d(Context& ctx, GLuint i, GLdouble x, GLdouble y, GLdouble z, GLdouble w) {
    setGenericF(ctx, i, static_cast<float>(x), static_cast<float>(y), static_cast<float>(z), static_cast<float>(w));
}
void VertexAttrib1dv(Context& ctx, GLuint i, const GLdouble* v) { if (v) VertexAttrib1d(ctx, i, v[0]); }
void VertexAttrib2dv(Context& ctx, GLuint i, const GLdouble* v) { if (v) VertexAttrib2d(ctx, i, v[0], v[1]); }
void VertexAttrib3dv(Context& ctx, GLuint i, const GLdouble* v) { if (v) VertexAttrib3d(ctx, i, v[0], v[1], v[2]); }
void VertexAttrib4dv(Context& ctx, GLuint i, const GLdouble* v) { if (v) VertexAttrib4d(ctx, i, v[0], v[1], v[2], v[3]); }
void VertexAttrib1s(Context& ctx, GLuint i, GLshort x) { setGenericF(ctx, i, x, 0, 0, 1); }
void VertexAttrib2s(Context& ctx, GLuint i, GLshort x, GLshort y) { setGenericF(ctx, i, x, y, 0, 1); }
void VertexAttrib3s(Context& ctx, GLuint i, GLshort x, GLshort y, GLshort z) { setGenericF(ctx, i, x, y, z, 1); }
void VertexAttrib4s(Context& ctx, GLuint i, GLshort x, GLshort y, GLshort z, GLshort w) { setGenericF(ctx, i, x, y, z, w); }
void VertexAttrib1sv(Context& ctx, GLuint i, const GLshort* v) { if (v) setGenericF(ctx, i, v[0], 0, 0, 1); }
void VertexAttrib2sv(Context& ctx, GLuint i, const GLshort* v) { if (v) setGenericF(ctx, i, v[0], v[1], 0, 1); }
void VertexAttrib3sv(Context& ctx, GLuint i, const GLshort* v) { if (v) setGenericF(ctx, i, v[0], v[1], v[2], 1); }
void VertexAttrib4sv(Context& ctx, GLuint i, const GLshort* v) { if (v) setGenericF(ctx, i, v[0], v[1], v[2], v[3]); }
void VertexAttrib4bv(Context& ctx, GLuint i, const GLbyte* v) { if (v) setGenericF(ctx, i, v[0], v[1], v[2], v[3]); }
void VertexAttrib4ubv(Context& ctx, GLuint i, const GLubyte* v) { if (v) setGenericF(ctx, i, v[0], v[1], v[2], v[3]); }
void VertexAttrib4usv(Context& ctx, GLuint i, const GLushort* v) { if (v) setGenericF(ctx, i, v[0], v[1], v[2], v[3]); }
void VertexAttrib4iv(Context& ctx, GLuint i, const GLint* v) {
    if (v) setGenericF(ctx, i, static_cast<float>(v[0]), static_cast<float>(v[1]), static_cast<float>(v[2]), static_cast<float>(v[3]));
}
void VertexAttrib4uiv(Context& ctx, GLuint i, const GLuint* v) {
    if (v) setGenericF(ctx, i, static_cast<float>(v[0]), static_cast<float>(v[1]), static_cast<float>(v[2]), static_cast<float>(v[3]));
}
void VertexAttrib4Nbv(Context& ctx, GLuint i, const GLbyte* v) { if (v) setGenericF(ctx, i, norm(v[0]), norm(v[1]), norm(v[2]), norm(v[3])); }
void VertexAttrib4Nubv(Context& ctx, GLuint i, const GLubyte* v) { if (v) setGenericF(ctx, i, norm(v[0]), norm(v[1]), norm(v[2]), norm(v[3])); }
void VertexAttrib4Nub(Context& ctx, GLuint i, GLubyte x, GLubyte y, GLubyte z, GLubyte w) { setGenericF(ctx, i, norm(x), norm(y), norm(z), norm(w)); }
void VertexAttrib4Nsv(Context& ctx, GLuint i, const GLshort* v) { if (v) setGenericF(ctx, i, norm(v[0]), norm(v[1]), norm(v[2]), norm(v[3])); }
void VertexAttrib4Nusv(Context& ctx, GLuint i, const GLushort* v) { if (v) setGenericF(ctx, i, norm(v[0]), norm(v[1]), norm(v[2]), norm(v[3])); }
void VertexAttrib4Niv(Context& ctx, GLuint i, const GLint* v) { if (v) setGenericF(ctx, i, norm(v[0]), norm(v[1]), norm(v[2]), norm(v[3])); }
void VertexAttrib4Nuiv(Context& ctx, GLuint i, const GLuint* v) { if (v) setGenericF(ctx, i, norm(v[0]), norm(v[1]), norm(v[2]), norm(v[3])); }
void VertexAttribI1i(Context& ctx, GLuint i, GLint x) { setGenericI(ctx, i, x, 0, 0, 1); }
void VertexAttribI2i(Context& ctx, GLuint i, GLint x, GLint y) { setGenericI(ctx, i, x, y, 0, 1); }
void VertexAttribI3i(Context& ctx, GLuint i, GLint x, GLint y, GLint z) { setGenericI(ctx, i, x, y, z, 1); }
void VertexAttribI4i(Context& ctx, GLuint i, GLint x, GLint y, GLint z, GLint w) { setGenericI(ctx, i, x, y, z, w); }
void VertexAttribI1iv(Context& ctx, GLuint i, const GLint* v) { if (v) setGenericI(ctx, i, v[0], 0, 0, 1); }
void VertexAttribI2iv(Context& ctx, GLuint i, const GLint* v) { if (v) setGenericI(ctx, i, v[0], v[1], 0, 1); }
void VertexAttribI3iv(Context& ctx, GLuint i, const GLint* v) { if (v) setGenericI(ctx, i, v[0], v[1], v[2], 1); }
void VertexAttribI4iv(Context& ctx, GLuint i, const GLint* v) { if (v) setGenericI(ctx, i, v[0], v[1], v[2], v[3]); }
void VertexAttribI4bv(Context& ctx, GLuint i, const GLbyte* v) { if (v) setGenericI(ctx, i, v[0], v[1], v[2], v[3]); }
void VertexAttribI4sv(Context& ctx, GLuint i, const GLshort* v) { if (v) setGenericI(ctx, i, v[0], v[1], v[2], v[3]); }
void VertexAttribI1ui(Context& ctx, GLuint i, GLuint x) { setGenericU(ctx, i, x, 0, 0, 1); }
void VertexAttribI2ui(Context& ctx, GLuint i, GLuint x, GLuint y) { setGenericU(ctx, i, x, y, 0, 1); }
void VertexAttribI3ui(Context& ctx, GLuint i, GLuint x, GLuint y, GLuint z) { setGenericU(ctx, i, x, y, z, 1); }
void VertexAttribI4ui(Context& ctx, GLuint i, GLuint x, GLuint y, GLuint z, GLuint w) { setGenericU(ctx, i, x, y, z, w); }
void VertexAttribI1uiv(Context& ctx, GLuint i, const GLuint* v) { if (v) setGenericU(ctx, i, v[0], 0, 0, 1); }
void VertexAttribI2uiv(Context& ctx, GLuint i, const GLuint* v) { if (v) setGenericU(ctx, i, v[0], v[1], 0, 1); }
void VertexAttribI3uiv(Context& ctx, GLuint i, const GLuint* v) { if (v) setGenericU(ctx, i, v[0], v[1], v[2], 1); }
void VertexAttribI4uiv(Context& ctx, GLuint i, const GLuint* v) { if (v) setGenericU(ctx, i, v[0], v[1], v[2], v[3]); }
void VertexAttribI4ubv(Context& ctx, GLuint i, const GLubyte* v) { if (v) setGenericU(ctx, i, v[0], v[1], v[2], v[3]); }
void VertexAttribI4usv(Context& ctx, GLuint i, const GLushort* v) { if (v) setGenericU(ctx, i, v[0], v[1], v[2], v[3]); }

void VertexAttribL1d(Context& ctx, GLuint i, GLdouble x) { setGenericL(ctx, i, x, 0, 0, 1); }
void VertexAttribL2d(Context& ctx, GLuint i, GLdouble x, GLdouble y) { setGenericL(ctx, i, x, y, 0, 1); }
void VertexAttribL3d(Context& ctx, GLuint i, GLdouble x, GLdouble y, GLdouble z) { setGenericL(ctx, i, x, y, z, 1); }
void VertexAttribL4d(Context& ctx, GLuint i, GLdouble x, GLdouble y, GLdouble z, GLdouble w) { setGenericL(ctx, i, x, y, z, w); }
void VertexAttribL1dv(Context& ctx, GLuint i, const GLdouble* v) { if (v) setGenericL(ctx, i, v[0], 0, 0, 1); }
void VertexAttribL2dv(Context& ctx, GLuint i, const GLdouble* v) { if (v) setGenericL(ctx, i, v[0], v[1], 0, 1); }
void VertexAttribL3dv(Context& ctx, GLuint i, const GLdouble* v) { if (v) setGenericL(ctx, i, v[0], v[1], v[2], 1); }
void VertexAttribL4dv(Context& ctx, GLuint i, const GLdouble* v) { if (v) setGenericL(ctx, i, v[0], v[1], v[2], v[3]); }

// --- queries

namespace {

template <class T>
void getVertexAttrib(Context& ctx, GLuint index, GLenum pname, T* params) {
    if (!params) return;
    if (pname == GL_CURRENT_VERTEX_ATTRIB) {
        if (!checkIndex(ctx, index)) return;
        const auto& g = ctx.state.genericAttribs[index];
        for (int c = 0; c < 4; ++c) {
            if (g.type == GL_DOUBLE) {
                params[c] = static_cast<T>(g.d[c]);
            } else if constexpr (std::is_floating_point_v<T>) {
                params[c] = g.type == GL_INT ? static_cast<T>(g.value.i[c])
                            : g.type == GL_UNSIGNED_INT ? static_cast<T>(g.value.u[c])
                                                        : static_cast<T>(g.value.f[c]);
            } else {
                params[c] = g.type == GL_FLOAT ? static_cast<T>(g.value.f[c]) : static_cast<T>(g.value.i[c]);
            }
        }
        return;
    }
    if (!checkIndex(ctx, index)) return;
    const VertexAttrib& a = currentVao(ctx)->attribs[index];
    switch (pname) {
    case GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING: *params = static_cast<T>(a.buffer ? a.buffer->name : 0); return;
    case GL_VERTEX_ATTRIB_ARRAY_ENABLED: *params = static_cast<T>(a.enabled); return;
    case GL_VERTEX_ATTRIB_ARRAY_SIZE: *params = static_cast<T>(a.size); return;
    case GL_VERTEX_ATTRIB_ARRAY_STRIDE: *params = static_cast<T>(a.pointerStride); return;
    case GL_VERTEX_ATTRIB_ARRAY_TYPE: *params = static_cast<T>(a.type); return;
    case GL_VERTEX_ATTRIB_ARRAY_NORMALIZED: *params = static_cast<T>(a.normalized); return;
    case GL_VERTEX_ATTRIB_ARRAY_INTEGER: *params = static_cast<T>(a.integer); return;
    case GL_VERTEX_ATTRIB_ARRAY_DIVISOR: *params = static_cast<T>(a.divisor); return;
    default: break;
    }
    if (ctx.version() >= 41 && pname == GL_VERTEX_ATTRIB_ARRAY_LONG) { *params = static_cast<T>(a.isDouble); return; }
    if (ctx.version() >= 43 || ctx.hasExtension("GL_ARB_vertex_attrib_binding")) {
        if (pname == GL_VERTEX_ATTRIB_BINDING) { *params = static_cast<T>(a.binding); return; }
        if (pname == GL_VERTEX_ATTRIB_RELATIVE_OFFSET) { *params = static_cast<T>(a.relativeOffset); return; }
    }
    ctx.setError(GL_INVALID_ENUM);
}

}  // namespace

void GetVertexAttribfv(Context& ctx, GLuint index, GLenum pname, GLfloat* params) { getVertexAttrib(ctx, index, pname, params); }
void GetVertexAttribdv(Context& ctx, GLuint index, GLenum pname, GLdouble* params) { getVertexAttrib(ctx, index, pname, params); }
void GetVertexAttribiv(Context& ctx, GLuint index, GLenum pname, GLint* params) { getVertexAttrib(ctx, index, pname, params); }
void GetVertexAttribLdv(Context& ctx, GLuint index, GLenum pname, GLdouble* params) { getVertexAttrib(ctx, index, pname, params); }

void GetVertexAttribIiv(Context& ctx, GLuint index, GLenum pname, GLint* params) {
    if (pname == GL_CURRENT_VERTEX_ATTRIB && params && checkIndex(ctx, index)) {
        std::memcpy(params, ctx.state.genericAttribs[index].value.i, 16);
        return;
    }
    getVertexAttrib(ctx, index, pname, params);
}

void GetVertexAttribIuiv(Context& ctx, GLuint index, GLenum pname, GLuint* params) {
    if (pname == GL_CURRENT_VERTEX_ATTRIB && params && checkIndex(ctx, index)) {
        std::memcpy(params, ctx.state.genericAttribs[index].value.u, 16);
        return;
    }
    getVertexAttrib(ctx, index, pname, params);
}

void GetVertexAttribPointerv(Context& ctx, GLuint index, GLenum pname, void** pointer) {
    if (pname != GL_VERTEX_ATTRIB_ARRAY_POINTER) return ctx.setError(GL_INVALID_ENUM);
    if (!checkIndex(ctx, index) || !pointer) return;
    *pointer = reinterpret_cast<void*>(currentVao(ctx)->attribs[index].pointer);
}

// --- packed generic attributes (GL_ARB_vertex_type_2_10_10_10_rev)

namespace {

void setGenericPacked(Context& ctx, GLuint index, GLenum type, GLboolean normalized, GLuint value, int size) {
    if (type != GL_INT_2_10_10_10_REV && type != GL_UNSIGNED_INT_2_10_10_10_REV) return ctx.setError(GL_INVALID_ENUM);
    const int bits[4] = {10, 10, 10, 2};
    float c[4];
    int shift = 0;
    for (int i = 0; i < 4; ++i) {
        const uint32_t raw = (value >> shift) & ((1u << bits[i]) - 1);
        shift += bits[i];
        if (type == GL_INT_2_10_10_10_REV) {
            const int32_t v = static_cast<int32_t>(raw << (32 - bits[i])) >> (32 - bits[i]);
            const float maxValue = static_cast<float>((1 << (bits[i] - 1)) - 1);
            c[i] = normalized ? std::max(-1.0f, v / maxValue) : static_cast<float>(v);
        } else {
            c[i] = normalized ? raw / static_cast<float>((1u << bits[i]) - 1) : static_cast<float>(raw);
        }
    }
    setGenericF(ctx, index, c[0], size > 1 ? c[1] : 0.0f, size > 2 ? c[2] : 0.0f, size > 3 ? c[3] : 1.0f);
}

}  // namespace

void VertexAttribP1ui(Context& ctx, GLuint index, GLenum type, GLboolean normalized, GLuint value) {
    setGenericPacked(ctx, index, type, normalized, value, 1);
}
void VertexAttribP2ui(Context& ctx, GLuint index, GLenum type, GLboolean normalized, GLuint value) {
    setGenericPacked(ctx, index, type, normalized, value, 2);
}
void VertexAttribP3ui(Context& ctx, GLuint index, GLenum type, GLboolean normalized, GLuint value) {
    setGenericPacked(ctx, index, type, normalized, value, 3);
}
void VertexAttribP4ui(Context& ctx, GLuint index, GLenum type, GLboolean normalized, GLuint value) {
    setGenericPacked(ctx, index, type, normalized, value, 4);
}
void VertexAttribP1uiv(Context& ctx, GLuint index, GLenum type, GLboolean normalized, const GLuint* value) {
    if (value) setGenericPacked(ctx, index, type, normalized, *value, 1);
}
void VertexAttribP2uiv(Context& ctx, GLuint index, GLenum type, GLboolean normalized, const GLuint* value) {
    if (value) setGenericPacked(ctx, index, type, normalized, *value, 2);
}
void VertexAttribP3uiv(Context& ctx, GLuint index, GLenum type, GLboolean normalized, const GLuint* value) {
    if (value) setGenericPacked(ctx, index, type, normalized, *value, 3);
}
void VertexAttribP4uiv(Context& ctx, GLuint index, GLenum type, GLboolean normalized, const GLuint* value) {
    if (value) setGenericPacked(ctx, index, type, normalized, *value, 4);
}

}  // namespace api
}  // namespace wmgl
