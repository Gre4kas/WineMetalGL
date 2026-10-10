// Display lists (GL 3.3 compatibility section 5.5): glNewList, glCallList(s),
// name management, and the helpers the generated recorders use.

#include "core/compat/display_list.h"

#include <algorithm>
#include <cstring>

#include "core/compat/compat_api.h"
#include "core/compat/immediate.h"
#include "core/context.h"
#include "core/format/pixel_transfer.h"
#include "gl_api.gen.h"
#include "gl_list.gen.h"

namespace wmgl {
namespace list {

int pnameCount(GLenum pname) {
    switch (pname) {
    case GL_AMBIENT: case GL_DIFFUSE: case GL_SPECULAR: case GL_EMISSION: case GL_AMBIENT_AND_DIFFUSE:
    case GL_POSITION: case GL_LIGHT_MODEL_AMBIENT: case GL_FOG_COLOR: case GL_TEXTURE_ENV_COLOR:
    case GL_OBJECT_PLANE: case GL_EYE_PLANE: case GL_TEXTURE_BORDER_COLOR: case GL_TEXTURE_SWIZZLE_RGBA:
        return 4;
    case GL_SPOT_DIRECTION: case GL_COLOR_INDEXES: case GL_POINT_DISTANCE_ATTENUATION: return 3;
    default: return 1;
    }
}

namespace {

PixelStoreParams params(const PixelStore& s, bool threeD) {
    PixelStoreParams ps;
    ps.alignment = s.alignment;
    ps.rowLength = s.rowLength;
    ps.imageHeight = threeD ? s.imageHeight : 0;
    ps.skipPixels = s.skipPixels;
    ps.skipRows = s.skipRows;
    ps.skipImages = threeD ? s.skipImages : 0;
    ps.swapBytes = s.swapBytes;
    return ps;
}

// The client pointer, or the unpack buffer's contents at that offset.
const uint8_t* source(Context& ctx, const void* data, size_t bytes) {
    if (Buffer* pbo = ctx.state.pixelUnpackBuffer.get()) {
        const uintptr_t offset = reinterpret_cast<uintptr_t>(data);
        if (!pbo->mtl || offset + bytes > static_cast<size_t>(pbo->size)) return nullptr;
        if (pbo->lastWriteSerial) ctx.stream().waitFor(pbo->lastWriteSerial);
        return pbo->contents() + offset;
    }
    return static_cast<const uint8_t*>(data);
}

}  // namespace

Pixels capturePixels(Context& ctx, const void* data, GLenum format, GLenum type, GLsizei width, GLsizei height,
                     GLsizei depth, bool threeD) {
    Pixels p;
    p.unpack = ctx.state.unpack;
    if (!data && !ctx.state.pixelUnpackBuffer) return p;
    const TransferLayout l = transferLayout(format, type, std::max(width, 0), std::max(height, 1), std::max(depth, 1),
                                            params(ctx.state.unpack, threeD));
    if (!l.groupBytes) return p;  // invalid format/type: the command reports it when the list runs
    if (const uint8_t* src = source(ctx, data, l.bytesNeeded)) {
        p.bytes.assign(src, src + l.bytesNeeded);
        p.null = false;
    }
    return p;
}

Pixels captureBitmap(Context& ctx, const void* data, GLsizei width, GLsizei height) {
    Pixels p;
    p.unpack = ctx.state.unpack;
    if (!data && !ctx.state.pixelUnpackBuffer) return p;
    const size_t bytes = api::bitmapBytes(ctx.state.unpack, width, height);
    if (!bytes) return p;
    if (const uint8_t* src = source(ctx, data, bytes)) {
        p.bytes.assign(src, src + bytes);
        p.null = false;
    }
    return p;
}

Pixels captureBytes(Context& ctx, const void* data, GLsizei size) {
    Pixels p;
    p.unpack = ctx.state.unpack;
    if ((!data && !ctx.state.pixelUnpackBuffer) || size < 0) return p;
    if (const uint8_t* src = source(ctx, data, static_cast<size_t>(size))) {
        p.bytes.assign(src, src + size);
        p.null = false;
    }
    return p;
}

UnpackScope::UnpackScope(Context& c, const PixelStore& unpack)
    : ctx(c), saved(c.state.unpack), savedBuffer(c.state.pixelUnpackBuffer) {
    ctx.state.unpack = unpack;
    ctx.state.pixelUnpackBuffer.reset();
}

UnpackScope::~UnpackScope() {
    ctx.state.unpack = saved;
    ctx.state.pixelUnpackBuffer = savedBuffer;
}

bool isProxyTarget(GLenum target) {
    switch (target) {
    case GL_PROXY_TEXTURE_1D: case GL_PROXY_TEXTURE_2D: case GL_PROXY_TEXTURE_3D: case GL_PROXY_TEXTURE_CUBE_MAP:
    case GL_PROXY_TEXTURE_1D_ARRAY: case GL_PROXY_TEXTURE_2D_ARRAY: case GL_PROXY_TEXTURE_RECTANGLE:
    case GL_PROXY_TEXTURE_CUBE_MAP_ARRAY: case GL_PROXY_TEXTURE_2D_MULTISAMPLE: case GL_PROXY_TEXTURE_2D_MULTISAMPLE_ARRAY:
        return true;
    default: return false;
    }
}

size_t callListsBytes(GLsizei n, GLenum type) {
    const size_t count = static_cast<size_t>(std::max(n, 0));
    switch (type) {
    case GL_BYTE: case GL_UNSIGNED_BYTE: return count;
    case GL_SHORT: case GL_UNSIGNED_SHORT: case GL_2_BYTES: return count * 2;
    case GL_3_BYTES: return count * 3;
    case GL_INT: case GL_UNSIGNED_INT: case GL_FLOAT: case GL_4_BYTES: return count * 4;
    default: return 0;
    }
}

bool record(Context& ctx, std::function<void(Context&)> command) {
    compat::recordCommand(ctx, std::move(command));
    return compat::listExecutes(ctx);
}

}  // namespace list

namespace api {
namespace {

constexpr int kMaxListNesting = 64;  // GL_MAX_LIST_NESTING

bool compatOnly(Context& ctx) {
    if (ctx.isCompat()) return true;
    ctx.setError(GL_INVALID_OPERATION);
    return false;
}

void callList(Context& ctx, GLuint name) {
    if (ctx.listCallDepth >= kMaxListNesting) return;
    auto& lists = ctx.share().displayLists;
    auto it = lists.find(name);
    if (it == lists.end()) return;
    RefPtr<DisplayList> list = it->second;  // the list may delete itself
    // Commands a list runs are not compiled into a list being built
    // (glCallList itself was).
    std::unique_ptr<ListCompiler> compiling = std::move(ctx.listCompiler);
    ++ctx.listCallDepth;
    for (const auto& command : list->commands) command(ctx);
    --ctx.listCallDepth;
    ctx.listCompiler = std::move(compiling);
}

}  // namespace

void NewList(Context& ctx, GLuint list, GLenum mode) {
    if (!compatOnly(ctx)) return;
    if (list == 0) return ctx.setError(GL_INVALID_VALUE);
    if (mode != GL_COMPILE && mode != GL_COMPILE_AND_EXECUTE) return ctx.setError(GL_INVALID_ENUM);
    if (ctx.listCompiler || insideBeginEnd(ctx)) return ctx.setError(GL_INVALID_OPERATION);
    auto compiler = std::make_unique<ListCompiler>();
    compiler->name = list;
    compiler->mode = mode;
    compiler->list = makeRef<DisplayList>();
    ctx.listCompiler = std::move(compiler);
}

void EndList(Context& ctx) {
    if (!compatOnly(ctx)) return;
    if (!ctx.listCompiler || insideBeginEnd(ctx)) return ctx.setError(GL_INVALID_OPERATION);
    std::unique_ptr<ListCompiler> compiler = std::move(ctx.listCompiler);
    // A compiled glBegin whose glEnd comes later (from another list or
    // immediate mode): its vertices so far replay when the list runs.
    compat::splitBatch(*compiler);
    ctx.share().displayLists[compiler->name] = compiler->list;
}

void CallList(Context& ctx, GLuint list) {
    if (compatOnly(ctx)) callList(ctx, list);
}

void CallLists(Context& ctx, GLsizei n, GLenum type, const void* lists) {
    if (!compatOnly(ctx)) return;
    if (n < 0) return ctx.setError(GL_INVALID_VALUE);
    if (!list::callListsBytes(1, type)) return ctx.setError(GL_INVALID_ENUM);
    if (!lists) return;
    const auto* p = static_cast<const uint8_t*>(lists);
    for (GLsizei i = 0; i < n; ++i) {
        GLuint name = 0;
        switch (type) {
        case GL_BYTE: name = static_cast<GLuint>(static_cast<GLint>(reinterpret_cast<const int8_t*>(p)[i])); break;
        case GL_UNSIGNED_BYTE: name = p[i]; break;
        case GL_SHORT: { int16_t v; std::memcpy(&v, p + i * 2, 2); name = static_cast<GLuint>(static_cast<GLint>(v)); break; }
        case GL_UNSIGNED_SHORT: { uint16_t v; std::memcpy(&v, p + i * 2, 2); name = v; break; }
        case GL_INT: case GL_UNSIGNED_INT: std::memcpy(&name, p + i * 4, 4); break;
        case GL_FLOAT: { float v; std::memcpy(&v, p + i * 4, 4); name = static_cast<GLuint>(static_cast<GLint>(v)); break; }
        case GL_2_BYTES: name = (static_cast<GLuint>(p[i * 2]) << 8) | p[i * 2 + 1]; break;
        case GL_3_BYTES: name = (static_cast<GLuint>(p[i * 3]) << 16) | (static_cast<GLuint>(p[i * 3 + 1]) << 8) | p[i * 3 + 2]; break;
        case GL_4_BYTES:
            name = (static_cast<GLuint>(p[i * 4]) << 24) | (static_cast<GLuint>(p[i * 4 + 1]) << 16) |
                   (static_cast<GLuint>(p[i * 4 + 2]) << 8) | p[i * 4 + 3];
            break;
        default: break;
        }
        callList(ctx, name + ctx.state.fixed.listBase);
    }
}

GLuint GenLists(Context& ctx, GLsizei range) {
    if (!compatOnly(ctx)) return 0;
    if (range < 0) {
        ctx.setError(GL_INVALID_VALUE);
        return 0;
    }
    if (range == 0) return 0;
    auto& lists = ctx.share().displayLists;
    // The first gap of `range` unused names (lists are kept sorted).
    GLuint first = 1;
    for (const auto& entry : lists) {
        if (entry.first >= first + static_cast<GLuint>(range)) break;
        if (entry.first >= first) first = entry.first + 1;
    }
    if (first + static_cast<GLuint>(range) - 1 < first) {
        ctx.setError(GL_OUT_OF_MEMORY);
        return 0;
    }
    for (GLsizei i = 0; i < range; ++i) lists[first + static_cast<GLuint>(i)] = makeRef<DisplayList>();
    return first;
}

void DeleteLists(Context& ctx, GLuint list, GLsizei range) {
    if (!compatOnly(ctx)) return;
    if (range < 0) return ctx.setError(GL_INVALID_VALUE);
    auto& lists = ctx.share().displayLists;
    for (auto it = lists.lower_bound(list); it != lists.end() && it->first - list < static_cast<GLuint>(range);)
        it = lists.erase(it);
}

GLboolean IsList(Context& ctx, GLuint list) {
    if (!ctx.isCompat()) {
        ctx.setError(GL_INVALID_OPERATION);
        return GL_FALSE;
    }
    return ctx.share().displayLists.count(list) ? GL_TRUE : GL_FALSE;
}

void ListBase(Context& ctx, GLuint base) {
    if (compatOnly(ctx)) ctx.state.fixed.listBase = base;
}

}  // namespace api
}  // namespace wmgl
