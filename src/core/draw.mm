#include "core/draw.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/compat/fixed_function.h"
#include "core/format/vertex_decode.h"
#include "core/shader/glsl_upgrade.h"
#include "core/stats.h"
#include "core/context.h"
#include "core/metal/device.h"
#include "core/metal/enums.h"
#include "core/metal/pipeline_cache.h"
#include "core/metal/pixel_format.h"
#include "core/program.h"
#include "core/query.h"
#include "core/texture_storage.h"
#include "core/util/log.h"

namespace wmgl {


DrawState::DrawState(Context& ctx) : ctx_(ctx), transient_(ctx.share().queue) {}

// --------------------------------------------------------- render targets

namespace {

// Vertices one draw converts on the CPU at most (16M: 256 MB of float4s).
constexpr size_t kMaxConvertedVertices = size_t{1} << 24;

// sRGB storage is written (and blitted) raw unless GL_FRAMEBUFFER_SRGB is
// enabled: attachments then use a non-sRGB view of the same texture.
MTLPixelFormat linearFormat(MTLPixelFormat format) {
    switch (format) {
    case MTLPixelFormatRGBA8Unorm_sRGB: return MTLPixelFormatRGBA8Unorm;
    case MTLPixelFormatBGRA8Unorm_sRGB: return MTLPixelFormatBGRA8Unorm;
    default: return MTLPixelFormatInvalid;
    }
}

constexpr uint64_t kLinearRenderViewKey = ~0ull;

bool attachmentTarget(Context& ctx, const Attachment& a, RenderTargetDesc::Target& out, MTLPixelFormat& format,
                      uint32_t& width, uint32_t& height, uint32_t& samples, const FormatInfo*& info) {
    const bool linear = !ctx.state.framebufferSRGB;
    if (a.type == GL_RENDERBUFFER) {
        Renderbuffer* rb = a.renderbuffer.get();
        if (!rb || !rb->mtl) return false;
        out.texture = rb->mtl;
        if (linear && linearFormat(rb->mtl.pixelFormat)) {
            if (!rb->linearView) rb->linearView = [rb->mtl newTextureViewWithPixelFormat:linearFormat(rb->mtl.pixelFormat)];
            out.texture = rb->linearView;
        }
        out.level = 0;
        out.slice = 0;
        width = rb->width;
        height = rb->height;
        samples = std::max(1, rb->samples);
        format = out.texture.pixelFormat;
        info = rb->format;
        return true;
    }
    if (a.type == GL_TEXTURE) {
        Texture* tex = a.texture.get();
        if (!tex || !ensureTextureStorage(ctx, *tex)) return false;
        const int face = a.cubeFace ? static_cast<int>(a.cubeFace - GL_TEXTURE_CUBE_MAP_POSITIVE_X) : 0;
        const TextureImage& image = tex->image(face, a.level);
        // A level defined off the mip chain has no storage of its size.
        if (!image.defined() || !imageHasStorage(ctx, *tex, face, a.level)) return false;
        out.texture = tex->mtl;
        if (linear && linearFormat(tex->mtl.pixelFormat)) {
            id<MTLTexture> view = nil;
            for (const Texture::View& v : tex->views)
                if (v.key == kLinearRenderViewKey) view = v.view;
            if (!view) {
                view = [tex->mtl newTextureViewWithPixelFormat:linearFormat(tex->mtl.pixelFormat)];
                tex->views.push_back({kLinearRenderViewKey, view});
            }
            out.texture = view;
        }
        out.level = static_cast<uint32_t>(a.level);
        out.slice = a.layered ? 0u : tex->target == GL_TEXTURE_CUBE_MAP ? static_cast<uint32_t>(face) : static_cast<uint32_t>(a.layer);
        width = static_cast<uint32_t>(image.width);
        // A 1D array image's height is its layer count; each layer is one row.
        height = tex->target == GL_TEXTURE_1D_ARRAY ? 1u : static_cast<uint32_t>(image.height);
        samples = std::max(1, image.samples);
        format = out.texture.pixelFormat;
        info = image.format;
        return true;
    }
    return false;
}

// Layers of a layered attachment (glFramebufferTexture on an array, cube or
// 3D texture): the render target array length it gives a pass.
uint32_t attachmentLayers(const Attachment& a) {
    const Texture* tex = a.texture.get();
    if (a.type != GL_TEXTURE || !a.layered || !tex) return 0;
    if (tex->target == GL_TEXTURE_CUBE_MAP) return 6;
    return static_cast<uint32_t>(std::max(1, tex->image(0, a.level).depth));
}

}  // namespace

bool DrawState::drawTarget(RenderTargetDesc& target, TargetFormats& formats, bool reportErrors) {
    target = RenderTargetDesc();
    formats = TargetFormats();
    Framebuffer* fb = ctx_.state.drawFramebuffer.get();
    if (!fb) {
        Surface* surface = ctx_.drawSurface();
        if (!surface) return false;
        surface->describe(target);
        if (!ctx_.state.framebufferSRGB) target.color[0].texture = surface->linearColor();
        formats.color[0] = target.color[0].texture.pixelFormat;
        formats.srgb[0] = surface->srgbCapable() && ctx_.state.framebufferSRGB;
        if (target.depth.texture) formats.depth = target.depth.texture.pixelFormat;
        if (target.stencil.texture) formats.stencil = target.stencil.texture.pixelFormat;
        formats.depthAttached = surface->hasDepth();
        formats.stencilAttached = surface->hasStencil();
        // GL_NONE / GL_FRONT_AND_BACK draw buffer selection on the default framebuffer.
        if (ctx_.state.defaultDrawBuffer == GL_NONE) {
            target.color[0] = {};
            formats.color[0] = MTLPixelFormatInvalid;
        }
        return true;
    }
    uint32_t width = UINT32_MAX, height = UINT32_MAX, samples = 0, layers = UINT32_MAX;
    bool any = false;
    for (int i = 0; i < Framebuffer::kMaxColor; ++i) {
        const GLenum buffer = fb->drawBuffers[i];
        if (buffer == GL_NONE) continue;
        const int index = static_cast<int>(buffer - GL_COLOR_ATTACHMENT0);
        if (index < 0 || index >= Framebuffer::kMaxColor) continue;
        const Attachment& a = fb->color[index];
        if (!a.attached()) continue;
        uint32_t w, h, s;
        MTLPixelFormat format;
        const FormatInfo* info = nullptr;
        if (!attachmentTarget(ctx_, a, target.color[i], format, w, h, s, info)) {
            if (reportErrors) ctx_.setError(GL_INVALID_FRAMEBUFFER_OPERATION);
            return false;
        }
        if (const uint32_t n = attachmentLayers(a)) layers = std::min(layers, n);
        formats.color[i] = format;
        formats.alphaIsOne[i] = info && info->storedAlphaIsOne;
        formats.integer[i] = info && info->isInteger();
        formats.srgb[i] = info && info->srgb;
        width = std::min(width, w);
        height = std::min(height, h);
        samples = s;
        any = true;
    }
    for (int which = 0; which < 2; ++which) {
        const Attachment& a = which == 0 ? fb->depth : fb->stencil;
        if (!a.attached()) continue;
        RenderTargetDesc::Target& t = which == 0 ? target.depth : target.stencil;
        uint32_t w, h, s;
        MTLPixelFormat format;
        const FormatInfo* info = nullptr;
        if (!attachmentTarget(ctx_, a, t, format, w, h, s, info)) {
            if (reportErrors) ctx_.setError(GL_INVALID_FRAMEBUFFER_OPERATION);
            return false;
        }
        if (const uint32_t n = attachmentLayers(a)) layers = std::min(layers, n);
        if (which == 0) formats.depth = format;
        else formats.stencil = format;
        width = std::min(width, w);
        height = std::min(height, h);
        samples = s;
        any = true;
    }
    if (!any) {
        if (reportErrors) ctx_.setError(GL_INVALID_FRAMEBUFFER_OPERATION);
        return false;
    }
    formats.depthAttached = target.depth.texture != nil;
    formats.stencilAttached = target.stencil.texture != nil;
    // Combined depth/stencil textures must be bound to both attachment points in Metal.
    if (target.depth.texture && formats.depth == MTLPixelFormatDepth32Float_Stencil8 && !target.stencil.texture) {
        target.stencil = target.depth;
        formats.stencil = formats.depth;
    }
    if (target.stencil.texture && formats.stencil == MTLPixelFormatDepth32Float_Stencil8 && !target.depth.texture) {
        target.depth = target.stencil;
        formats.depth = formats.stencil;
    }
    target.width = width;
    target.height = height;
    target.samples = std::max(1u, samples);
    // A layered framebuffer (every attachment is layered, checked by
    // completeness) renders to the layers its attachments have in common.
    if (layers != UINT32_MAX) target.layers = layers;
    return true;
}

id<MTLTexture> DrawState::readColorTexture(uint32_t& level, uint32_t& slice, GLenum& internalFormat, int& width,
                                           int& height) {
    Framebuffer* fb = ctx_.state.readFramebuffer.get();
    level = slice = 0;
    if (!fb) {
        Surface* surface = ctx_.readSurface();
        if (!surface || ctx_.state.defaultReadBuffer == GL_NONE) return nil;
        surface->resolveForRead(ctx_.stream());
        internalFormat = surface->srgbCapable() ? GL_SRGB8_ALPHA8 : GL_RGBA8;
        width = surface->width();
        height = surface->height();
        return surface->resolvedColor();
    }
    if (fb->readBuffer == GL_NONE) return nil;
    const int index = static_cast<int>(fb->readBuffer - GL_COLOR_ATTACHMENT0);
    if (index < 0 || index >= Framebuffer::kMaxColor || !fb->color[index].attached()) return nil;
    RenderTargetDesc::Target t;
    MTLPixelFormat format;
    uint32_t w, h, s;
    const FormatInfo* info = nullptr;
    if (!attachmentTarget(ctx_, fb->color[index], t, format, w, h, s, info)) return nil;
    level = t.level;
    slice = t.slice;
    internalFormat = info ? info->internalFormat : GL_RGBA8;
    width = static_cast<int>(w);
    height = static_cast<int>(h);
    return t.texture;
}

bool DrawState::readTarget(GLenum buffer, RenderTargetDesc::Target& out, const FormatInfo*& info, uint32_t& width,
                           uint32_t& height, uint32_t& samples) {
    out = RenderTargetDesc::Target();
    info = nullptr;
    Framebuffer* fb = ctx_.state.readFramebuffer.get();
    if (!fb) {
        Surface* surface = ctx_.readSurface();
        if (!surface) return false;
        width = static_cast<uint32_t>(surface->width());
        height = static_cast<uint32_t>(surface->height());
        samples = surface->samples();
        if (buffer == GL_COLOR) {
            if (ctx_.state.defaultReadBuffer == GL_NONE) return false;
            surface->resolveForRead(ctx_.stream());
            out.texture = surface->resolvedColor();
            if (!ctx_.state.framebufferSRGB && linearFormat(out.texture.pixelFormat))
                out.texture = [out.texture newTextureViewWithPixelFormat:linearFormat(out.texture.pixelFormat)];
            info = formatInfo(surface->srgbCapable() ? GL_SRGB8_ALPHA8 : GL_RGBA8);
            return true;
        }
        if ((buffer == GL_DEPTH && !surface->hasDepth()) || (buffer == GL_STENCIL && !surface->hasStencil())) return false;
        out.texture = surface->depthStencil();
        info = formatInfo(surface->hasStencil() ? (surface->hasDepth() ? GL_DEPTH24_STENCIL8 : GL_STENCIL_INDEX8)
                          : surface->pixelFormat().depth_bits == 16 ? GL_DEPTH_COMPONENT16
                                                                    : GL_DEPTH_COMPONENT24);
        return out.texture != nil;
    }
    const Attachment* a = nullptr;
    if (buffer == GL_COLOR) {
        if (fb->readBuffer == GL_NONE) return false;
        const int index = static_cast<int>(fb->readBuffer - GL_COLOR_ATTACHMENT0);
        if (index < 0 || index >= Framebuffer::kMaxColor) return false;
        a = &fb->color[index];
    } else {
        a = buffer == GL_DEPTH ? &fb->depth : &fb->stencil;
    }
    if (!a->attached()) return false;
    MTLPixelFormat format;
    return attachmentTarget(ctx_, *a, out, format, width, height, samples, info);
}

// ------------------------------------------------------------- validation

namespace {
GLenum geometryInputFor(GLenum mode);
}  // namespace

bool DrawState::validateDraw(GLenum mode) {
    // GL_SELECT / GL_FEEDBACK render modes draw nothing (no records are produced).
    if (ctx_.state.fixed.renderMode != GL_RENDER) return false;
    switch (mode) {
    case GL_POINTS: case GL_LINES: case GL_LINE_LOOP: case GL_LINE_STRIP: case GL_TRIANGLES: case GL_TRIANGLE_STRIP:
    case GL_TRIANGLE_FAN: case GL_LINES_ADJACENCY: case GL_LINE_STRIP_ADJACENCY: case GL_TRIANGLES_ADJACENCY:
    case GL_TRIANGLE_STRIP_ADJACENCY:
        break;
    case GL_QUADS: case GL_QUAD_STRIP: case GL_POLYGON:
        if (ctx_.isCompat()) break;
        [[fallthrough]];
    case GL_PATCHES:
        if (mode == GL_PATCHES && ctx_.tessellationShaders()) break;
        [[fallthrough]];
    default:
        ctx_.setError(GL_INVALID_ENUM);
        return false;
    }
    Program* program = ctx_.state.program.get();
    if (!program && ctx_.state.programPipeline) {
        // GL_ARB_separate_shader_objects: an invalid pipeline cannot draw.
        if (!api::validateProgramPipeline(*ctx_.state.programPipeline)) {
            ctx_.setError(GL_INVALID_OPERATION);
            return false;
        }
        LinkedProgram* lp = executable();
        if (!lp || !lp->link.stages[static_cast<int>(Stage::Vertex)].present) return false;
    } else if (!program) {
        // Without a program, core profile draws are undefined but not an
        // error; the compatibility profile draws with fixed-function
        // processing (compat/fixed_function.h).
        if (ctx_.isCore() || !executable()) return false;
    } else if (!program->linked) {
        ctx_.setError(GL_INVALID_OPERATION);
        return false;
    }
    if (ctx_.isCore() && ctx_.state.vertexArray == ctx_.defaultVertexArray) {
        ctx_.setError(GL_INVALID_OPERATION);
        return false;
    }
    // Mapped buffers may not be used by draws.
    for (const VertexAttrib& a : ctx_.state.vertexArray->attribs)
        if (a.enabled && a.buffer && a.buffer->mappedNonPersistent()) {
            ctx_.setError(GL_INVALID_OPERATION);
            return false;
        }
    // GL_PATCHES draws exactly when a tessellation evaluation shader is
    // active (GL 4.0 section 2.12); tessellation needs a program object.
    LinkedProgram* executableNow = executable();
    const bool tessellated = executableNow && executableNow->hasStage(Stage::TessEval);
    if ((mode == GL_PATCHES) != tessellated) {
        ctx_.setError(GL_INVALID_OPERATION);
        return false;
    }
    if (tessellated && !executableNow->link.tess.active) return false;  // a pipeline without the vertex program's emulation
    // Active transform feedback restricts the draw mode (GL 3.3 section 2.15.2).
    const TransformFeedback& xfb = *ctx_.state.transformFeedback;
    if (xfb.active && !xfb.paused) {
        GLenum family = 0;
        switch (mode) {
        case GL_POINTS: family = GL_POINTS; break;
        case GL_LINES: case GL_LINE_LOOP: case GL_LINE_STRIP: family = GL_LINES; break;
        case GL_TRIANGLES: case GL_TRIANGLE_STRIP: case GL_TRIANGLE_FAN: family = GL_TRIANGLES; break;
        case GL_QUADS: case GL_QUAD_STRIP: case GL_POLYGON: family = GL_TRIANGLES; break;  // compatibility profile
        default: break;
        }
        // With a geometry shader, its output type must match instead; with
        // tessellation, the tessellator's output.
        LinkedProgram* lp = program ? program->linked.get() : nullptr;
        if (tessellated) {
            const shader::TessellationEmulation& t = executableNow->link.tess;
            family = t.pointMode ? GL_POINTS : t.mode == GL_ISOLINES ? GL_LINES : GL_TRIANGLES;
            if (executableNow->hasStage(Stage::Geometry)) {
                const GLenum out = executableNow->link.geometryOutputType;
                family = out == GL_POINTS ? GL_POINTS : out == GL_LINE_STRIP ? GL_LINES : GL_TRIANGLES;
            }
        } else if (lp && lp->link.geometry.active) {
            const GLenum out = lp->link.geometry.outputType;
            family = out == GL_POINTS ? GL_POINTS : out == GL_LINE_STRIP ? GL_LINES : GL_TRIANGLES;
        }
        if (family != xfb.primitiveMode) {
            ctx_.setError(GL_INVALID_OPERATION);
            return false;
        }
    }
    // A geometry shader accepts only draw modes of its input type (GL 3.3
    // section 2.12.1); after tessellation, the tessellator's output type.
    if (LinkedProgram* lp = program ? program->linked.get() : nullptr;
        lp && lp->link.geometry.active && geometryInputFor(mode) != lp->link.geometry.inputType) {
        ctx_.setError(GL_INVALID_OPERATION);
        return false;
    }
    if (tessellated && executableNow->hasStage(Stage::Geometry)) {
        const shader::TessellationEmulation& t = executableNow->link.tess;
        const GLenum produced = t.pointMode ? GL_POINTS : t.mode == GL_ISOLINES ? GL_LINES : GL_TRIANGLES;
        if (executableNow->link.geometryInputType != produced) {
            ctx_.setError(GL_INVALID_OPERATION);
            return false;
        }
    }
    RenderTargetDesc target;
    TargetFormats formats;
    if (!drawTarget(target, formats)) return false;
    return true;
}

// ---------------------------------------------------------- vertex input

namespace {

struct ConvertedAttribute {
    bool needed = false;
};

}  // namespace

// The array feeding attribute `location`. In the compatibility profile
// generic attribute 0 aliases the conventional vertex array (gl_Vertex).
const VertexAttrib& DrawState::attribFor(const VertexArray& vao, int location) const {
    if (location == kConvVertex && !vao.attribs[kConvVertex].enabled && vao.attribs[0].enabled) return vao.attribs[0];
    return vao.attribs[static_cast<size_t>(location)];
}

namespace {

// How a vertex input reads its attribute's element: the Metal format, the
// byte offset of the first byte read and the bytes read. 64-bit inputs
// (kind 'd') read raw 32-bit words, 16 bytes per location (shader/fp64.h).
struct VertexFetch {
    MTLVertexFormat format = MTLVertexFormatInvalid;
    NSUInteger offset = 0;
    uint32_t bytes = 0;
};

VertexFetch vertexFetch(const shader::StageOutput::VertexInput& input, const VertexAttrib& a) {
    if (input.kind == 'd') {
        const uint32_t element = a.isDouble ? a.elementBytes() : 0, start = 16u * static_cast<uint32_t>(input.part);
        if (element <= start) return {MTLVertexFormatInvalid, static_cast<NSUInteger>(a.offset), 0};
        const uint32_t bytes = std::min(16u, element - start);
        return {bytes == 8 ? MTLVertexFormatUInt2 : MTLVertexFormatUInt4, static_cast<NSUInteger>(a.offset) + start, bytes};
    }
    return {a.isDouble ? MTLVertexFormatInvalid : mtlVertexFormat(a.type, a.size, a.normalized, a.integer),
            static_cast<NSUInteger>(a.offset), a.elementBytes()};
}

}  // namespace

size_t DrawState::vertexBufferSlots(LinkedProgram& program) {
    // A vertex function's atomic counter buffer takes the last vertex buffer slot.
    const size_t vertex = static_cast<size_t>(Stage::Vertex);
    // Its storage block argument buffer takes slot 28 (and leaves 29 for a counter buffer).
    if (!program.link.stages[vertex].storageArgs.empty())
        return shader::kSlotStorageBlocks - shader::kSlotFirstVertexBuffer;
    for (const shader::AtomicCounterBufferInfo& b : program.link.atomicBuffers)
        if (b.stageSlot[vertex] >= 0) return shader::kMaxVertexBufferSlots - shader::kMaxVertexAtomicCounterBuffers;
    return shader::kMaxVertexBufferSlots;
}

void DrawState::planVertexInputs(LinkedProgram& program, bool gather, VertexInputPlan& plan) const {
    using Plan = VertexInputPlan;
    const VertexArray& vao = *ctx_.state.vertexArray;
    plan = Plan();
    struct Window {
        size_t group;
        NSUInteger end;  // one past the highest byte the group's attributes read, within one element
    };
    std::vector<Window> windows;
    bool constants = false, sides = false, convertedPerVertex = false;
    std::vector<uint32_t> convertedDivisors;
    auto convert = [&](int location, uint32_t divisor) {
        plan.how[static_cast<size_t>(location)] = Plan::Converted;
        if (!divisor) convertedPerVertex = true;
        else if (std::find(convertedDivisors.begin(), convertedDivisors.end(), divisor) == convertedDivisors.end())
            convertedDivisors.push_back(divisor);
    };
    struct Direct {
        int location;
        const VertexAttrib* a;
        VertexFetch fetch;
    };
    std::vector<Direct> direct;
    for (const shader::StageOutput::VertexInput& input : program.stage(Stage::Vertex).vertexInputs) {
        const int location = input.location;
        if (location < 0 || location >= VertexArray::kAttribSlots) continue;
        if (lineSides_ && location == kLineSide) {
            plan.how[static_cast<size_t>(location)] = Plan::LineSide;
            sides = true;
            continue;
        }
        // The other endpoint of a wide line segment reads the position array in partner order.
        const bool partner = linePartners_ && location == kLinePartner;
        const VertexAttrib& a = attribFor(vao, partner ? static_cast<int>(kConvVertex) : input.source());
        if (!a.enabled) {
            plan.how[static_cast<size_t>(location)] = Plan::Constant;
            constants = true;
            continue;
        }
        const uint32_t stride = static_cast<uint32_t>(a.effectiveStride());
        const VertexFetch fetch = vertexFetch(input, a);
        const bool aligned = (fetch.offset % 4) == 0 && (stride % 4) == 0 && stride > 0;
        const bool gathered = (gather || partner) && !a.divisor;
        if (a.buffer && a.buffer->mtl && fetch.format != MTLVertexFormatInvalid && aligned && !gathered)
            direct.push_back({location, &a, fetch});
        else
            convert(location, a.divisor);
    }
    // Attributes of one buffer, stride and divisor share a layout when they
    // all fit in one element window, whatever their order.
    std::vector<std::vector<int>> members;
    for (const Direct& d : direct) {
        const int location = d.location;
        const VertexAttrib* a = d.a;
        const uint32_t stride = static_cast<uint32_t>(a->effectiveStride());
        const NSUInteger offset = d.fetch.offset, end = offset + d.fetch.bytes;
        size_t chosen = plan.groups.size();
        for (Window& w : windows) {
            Plan::DirectGroup& g = plan.groups[w.group];
            if (g.buffer != a->buffer.get() || g.stride != stride || g.divisor != a->divisor) continue;
            const NSUInteger base = std::min(g.base, offset), top = std::max(w.end, end);
            if (top - base > stride) continue;
            g.base = base;
            w.end = top;
            chosen = w.group;
            break;
        }
        if (chosen == plan.groups.size()) {
            plan.groups.push_back({a->buffer.get(), offset, stride, a->divisor});
            windows.push_back({chosen, end});
            members.emplace_back();
        }
        members[chosen].push_back(location);
    }
    // Keep within the vertex buffer slots: the last direct groups fall back to conversion.
    auto layouts = [&] {
        return plan.groups.size() + (convertedPerVertex ? 1 : 0) + convertedDivisors.size() + (constants ? 1 : 0) +
               (sides ? 1 : 0);
    };
    const size_t slots = vertexBufferSlots(program);
    while (layouts() > slots && !plan.groups.empty()) {
        for (int location : members.back()) convert(location, plan.groups.back().divisor);
        plan.groups.pop_back();
        members.pop_back();
    }
    for (size_t g = 0; g < plan.groups.size(); ++g)
        for (int location : members[g]) {
            plan.how[static_cast<size_t>(location)] = Plan::Direct;
            plan.group[static_cast<size_t>(location)] = static_cast<int8_t>(g);
        }
}

void DrawState::waitForCpuVertexReads(LinkedProgram& program, bool gather) {
    // Waiting flushes the command buffer, so it must happen before a draw opens its encoder.
    Queue& queue = ctx_.stream().queue();
    const VertexArray& vao = *ctx_.state.vertexArray;
    VertexInputPlan plan;
    planVertexInputs(program, gather, plan);
    for (const shader::StageOutput::VertexInput& input : program.stage(Stage::Vertex).vertexInputs) {
        if (input.location < 0 || input.location >= VertexArray::kAttribSlots) continue;
        if (plan.how[static_cast<size_t>(input.location)] != VertexInputPlan::Converted) continue;
        const VertexAttrib& a = attribFor(vao, input.source());
        if (a.buffer && a.buffer->lastWriteSerial && !queue.isComplete(a.buffer->lastWriteSerial))
            ctx_.stream().waitFor(a.buffer->lastWriteSerial);
    }
}

bool DrawState::bindVertexInputs(id<MTLRenderCommandEncoder> encoder, PipelineKey& key, LinkedProgram& program,
                                 GLint firstVertex, GLsizei vertexCount, GLsizei instances,
                                 const std::vector<uint32_t>* gather) {
    using Plan = VertexInputPlan;
    VertexArray& vao = *ctx_.state.vertexArray;
    const uint64_t serial = ctx_.stream().serial();
    Plan plan;
    planVertexInputs(program, gather != nullptr, plan);
    struct Layout {
        id<MTLBuffer> buffer;
        NSUInteger offset;
        uint32_t stride;
        uint32_t divisor;
        bool constant;
    };
    std::vector<Layout> layouts;
    const size_t slots = vertexBufferSlots(program);
    auto addLayout = [&](id<MTLBuffer> buffer, NSUInteger offset, uint32_t stride, uint32_t divisor, bool constant) -> int {
        if (layouts.size() >= slots) return -1;
        layouts.push_back({buffer, offset, stride, divisor, constant});
        return static_cast<int>(layouts.size() - 1);
    };
    // Direct groups first: layout g is group g. Renumbered vertices (issue())
    // read from the draw's first vertex on.
    for (const Plan::DirectGroup& g : plan.groups) {
        const NSUInteger base = g.base + (g.divisor ? 0 : static_cast<NSUInteger>(vertexRebase_) * g.stride);
        if (addLayout(g.buffer->mtl, base, g.stride, g.divisor, false) < 0) return false;
        g.buffer->lastUseSerial = serial;
    }
    struct Converted {
        int location;
        const VertexAttrib* attrib;
        const std::vector<uint32_t>* order;  // gathered: the vertex fetched for each draw vertex
        char kind;
        int part;  // 'd': 16-byte part of the element
    };
    std::vector<Converted> converted;
    // Current values, one constant element each, packed into one buffer:
    // the GL attribute and (64-bit inputs) the 16-byte part of its value.
    struct ConstantInput {
        int location, attrib, part;
        char kind;
    };
    std::vector<ConstantInput> constants;
    for (const shader::StageOutput::VertexInput& input : program.stage(Stage::Vertex).vertexInputs) {
        const int location = input.location;
        if (location < 0 || location >= VertexArray::kAttribSlots) continue;
        VertexAttributeKey& out = key.attributes[location];
        const bool partner = linePartners_ && location == kLinePartner;
        const VertexAttrib& a = attribFor(vao, partner ? static_cast<int>(kConvVertex) : input.source());
        switch (plan.how[static_cast<size_t>(location)]) {
        case Plan::LineSide: {
            TransientAllocation alloc = transient_.allocate(lineSides_->size() * 16, serial, 16);
            auto* dst = static_cast<float*>(alloc.data);
            if (!dst) return false;
            for (size_t v = 0; v < lineSides_->size(); ++v) {
                dst[v * 4] = (*lineSides_)[v];
                dst[v * 4 + 1] = dst[v * 4 + 2] = 0;
                dst[v * 4 + 3] = 1;
            }
            const int layout = addLayout(alloc.buffer, alloc.offset, 16, 0, false);
            if (layout < 0) return false;
            out.format = static_cast<uint8_t>(MTLVertexFormatFloat4);
            out.layout = static_cast<uint8_t>(layout);
            out.offset = 0;
            break;
        }
        case Plan::Constant:
            out.format = static_cast<uint8_t>(input.kind == 'i' ? MTLVertexFormatInt4
                                              : input.kind == 'u' || input.kind == 'd' ? MTLVertexFormatUInt4
                                                                                       : MTLVertexFormatFloat4);
            out.offset = static_cast<uint16_t>(constants.size() * 16);
            constants.push_back({location, input.source(), input.part, input.kind});
            break;
        case Plan::Direct: {
            const size_t g = static_cast<size_t>(plan.group[static_cast<size_t>(location)]);
            const VertexFetch fetch = vertexFetch(input, a);
            out.format = static_cast<uint8_t>(fetch.format);
            out.layout = static_cast<uint8_t>(g);
            out.offset = static_cast<uint16_t>(fetch.offset - plan.groups[g].base);
            break;
        }
        case Plan::Converted:
            if (a.buffer && !a.buffer->mtl) {
                ctx_.setError(GL_INVALID_OPERATION);
                return false;
            }
            converted.push_back({location, &a, partner ? linePartners_ : gather, input.kind, input.part});
            break;
        case Plan::None: break;
        }
    }
    // Converted attributes: expanded to 4 x 32-bit components, interleaved in
    // one buffer per divisor (GPU writes to their buffers were waited for in
    // waitForCpuVertexReads).
    std::vector<uint32_t> divisors;
    for (const Converted& c : converted)
        if (std::find(divisors.begin(), divisors.end(), c.attrib->divisor) == divisors.end()) divisors.push_back(c.attrib->divisor);
    for (uint32_t divisor : divisors) {
        std::vector<const Converted*> group;
        for (const Converted& c : converted)
            if (c.attrib->divisor == divisor) group.push_back(&c);
        // Per-vertex members are all gathered or none (gathering is per draw).
        const bool gathered = !divisor && group.front()->order;
        uint32_t first = 0, count = 0;
        if (gathered) {
            count = static_cast<uint32_t>(group.front()->order->size());
        } else if (divisor) {
            // One element per `divisor` instances, from the base instance.
            count = baseInstance_ + static_cast<uint32_t>((std::max<GLsizei>(1, instances) + divisor - 1) / divisor);
        } else {
            first = static_cast<uint32_t>(std::max(0, firstVertex));
            count = static_cast<uint32_t>(std::max(0, vertexCount));
        }
        // Element v of the draw is at v - rebase (vertices renumbered by issue()).
        const uint32_t rebase = gathered || divisor ? 0 : static_cast<uint32_t>(vertexRebase_);
        const size_t elements = static_cast<size_t>(first - rebase) + count;
        if (elements > kMaxConvertedVertices) {
            // An index range this wide is almost certainly garbage indices.
            logOnce(LogLevel::Warn, "converted-span", "draw skipped: %zu vertices to convert", elements);
            return false;
        }
        const size_t stride = 16 * group.size();
        TransientAllocation alloc = transient_.allocate(std::max<size_t>(1, elements) * stride, serial, 16);
        auto* dst = static_cast<uint8_t*>(alloc.data);
        if (!dst) return false;
        for (size_t m = 0; m < group.size(); ++m) {
            const VertexAttrib& a = *group[m]->attrib;
            const uint32_t elementBytes = a.elementBytes(), attribStride = static_cast<uint32_t>(a.effectiveStride());
            // Bytes readable from `source` (a buffer object's size; client arrays are unbounded).
            size_t readable = SIZE_MAX;
            const uint8_t* source = nullptr;
            if (a.buffer) {
                readable = a.buffer->size > a.offset ? static_cast<size_t>(a.buffer->size - a.offset) : 0;
                source = a.buffer->contents() + std::min<size_t>(static_cast<size_t>(a.offset), static_cast<size_t>(a.buffer->size));
            } else {
                source = reinterpret_cast<const uint8_t*>(a.offset);
            }
            if (!source) return false;
            for (uint32_t v = first; v < first + count; ++v) {
                const uint32_t fetch = gathered ? (*group[m]->order)[v] : v;
                const size_t at = static_cast<size_t>(fetch) * attribStride;
                uint8_t* to = dst + static_cast<size_t>(v - rebase) * stride + m * 16;
                if (readable != SIZE_MAX && (at > readable || readable - at < elementBytes)) {
                    // Past the end of the buffer: GPUs read zeros (robust access), never past the allocation.
                    const int32_t one = 1;
                    const float oneF = 1.0f;
                    std::memset(to, 0, 16);
                    if (group[m]->kind != 'd')
                        std::memcpy(to + 12, a.integer ? static_cast<const void*>(&one) : static_cast<const void*>(&oneF), 4);
                    continue;
                }
                const uint8_t* element = source + at;
                if (group[m]->kind == 'd') {
                    // Raw 64-bit components (shader/fp64.h); anything but an L array reads zeros.
                    std::memset(to, 0, 16);
                    const uint32_t start = 16u * static_cast<uint32_t>(group[m]->part);
                    if (a.isDouble && elementBytes > start) std::memcpy(to, element + start, std::min(16u, elementBytes - start));
                } else if (a.integer) {
                    int32_t values[4];
                    decodeVertexInt(element, a.type, a.size, values);
                    std::memcpy(to, values, 16);
                } else {
                    float values[4];
                    decodeVertexFloat(element, a.type, a.size, a.normalized, values);
                    std::memcpy(to, values, 16);
                }
            }
            if (a.buffer) a.buffer->lastUseSerial = serial;
        }
        const int layout = addLayout(alloc.buffer, alloc.offset, static_cast<uint32_t>(stride), divisor, false);
        if (layout < 0) return false;
        for (size_t m = 0; m < group.size(); ++m) {
            VertexAttributeKey& out = key.attributes[group[m]->location];
            out.format = static_cast<uint8_t>(group[m]->kind == 'd' ? MTLVertexFormatUInt4
                                              : group[m]->attrib->integer
                                                  ? (group[m]->kind == 'u' ? MTLVertexFormatUInt4 : MTLVertexFormatInt4)
                                                  : MTLVertexFormatFloat4);
            out.layout = static_cast<uint8_t>(layout);
            out.offset = static_cast<uint16_t>(m * 16);
        }
    }
    if (!constants.empty()) {
        TransientAllocation alloc = transient_.allocate(constants.size() * 16, serial, 16);
        if (!alloc.data) return false;
        for (size_t c = 0; c < constants.size(); ++c) {
            const ContextState::GenericAttrib& g = ctx_.state.genericAttribs[static_cast<size_t>(constants[c].attrib)];
            uint8_t* dst = static_cast<uint8_t*>(alloc.data) + c * 16;
            if (constants[c].kind == 'd') std::memcpy(dst, reinterpret_cast<const uint8_t*>(g.d) + 16 * constants[c].part, 16);
            else std::memcpy(dst, &g.value, 16);
        }
        const int layout = addLayout(alloc.buffer, alloc.offset, static_cast<uint32_t>(constants.size() * 16), 0, true);
        if (layout < 0) return false;
        for (const ConstantInput& c : constants) key.attributes[c.location].layout = static_cast<uint8_t>(layout);
    }
    key.layoutCount = static_cast<uint8_t>(layouts.size());
    for (size_t l = 0; l < layouts.size(); ++l) {
        const Layout& layout = layouts[l];
        key.layouts[l].stride = layout.stride;
        key.layouts[l].stepFunction = static_cast<uint8_t>(layout.constant ? MTLVertexStepFunctionConstant
                                                           : layout.divisor ? MTLVertexStepFunctionPerInstance
                                                                            : MTLVertexStepFunctionPerVertex);
        key.layouts[l].stepRate = layout.constant ? 0 : std::max(1u, layout.divisor);
        [encoder setVertexBuffer:layout.buffer offset:layout.offset atIndex:shader::kSlotFirstVertexBuffer + l];
    }
    return true;
}

Program* DrawState::ownerProgram() {
    return ctx_.state.program ? ctx_.state.program.get() : ffProgram_;
}

Program* DrawState::stageProgram(LinkedProgram& program, int stage) {
    return program.pipeline ? program.stageOwner[stage] : ownerProgram();
}

std::pair<Program*, size_t> DrawState::blockSource(LinkedProgram& program, size_t block) {
    if (!program.pipeline) return {ownerProgram(), block};
    return {program.blockOwner[block].first, static_cast<size_t>(program.blockOwner[block].second)};
}

std::pair<Program*, size_t> DrawState::samplerSource(LinkedProgram& program, size_t index) {
    if (!program.pipeline) return {ownerProgram(), index};
    return {program.samplerOwner[index].first, static_cast<size_t>(program.samplerOwner[index].second)};
}

std::pair<Program*, size_t> DrawState::imageSource(LinkedProgram& program, size_t index) {
    if (!program.pipeline) return {ownerProgram(), index};
    return {program.imageOwner[index].first, static_cast<size_t>(program.imageOwner[index].second)};
}

void DrawState::applyBuiltinUniforms(Program& program) {
    // gl_DepthRange (viewport 0's) and gl_NumSamples (glsl_upgrade.h).
    if (!program.linked) return;
    const shader::LinkResult::BuiltinUniforms& b = program.linked->link.builtinUniforms;
    auto store = [&](int offset, const void* value) {
        if (offset < 0 || static_cast<size_t>(offset) + 4 > program.uniformData.size()) return;
        uint8_t* at = program.uniformData.data() + offset;
        if (std::memcmp(at, value, 4) == 0) return;
        std::memcpy(at, value, 4);
        ++program.uniformVersion;
    };
    if (b.depthRange[0] >= 0) {
        const float n = static_cast<float>(ctx_.state.depthRanges[0][0]), f = static_cast<float>(ctx_.state.depthRanges[0][1]);
        const float values[3] = {n, f, f - n};
        for (int i = 0; i < 3; ++i) store(b.depthRange[i], &values[i]);
    }
    if (b.numSamples >= 0) {
        const int32_t samples = std::max<int32_t>(1, lastKey_.sampleCount);
        store(b.numSamples, &samples);
    }
}

GLint DrawState::imageUnit(LinkedProgram& program, size_t index) {
    const auto [owner, ownIndex] = imageSource(program, index);
    return owner && ownIndex < owner->imageUnits.size() ? owner->imageUnits[ownIndex] : 0;
}

id<MTLTexture> DrawState::imageBinding(LinkedProgram& program, size_t index) {
    const shader::ImageSlotInfo& slot = program.link.images[index];
    const GLint unit = imageUnit(program, index);
    id<MTLTexture> view = nil;
    if (unit >= 0 && unit < ContextState::kMaxImageUnits) {
        const ImageUnit& u = ctx_.state.imageUnits[unit];
        view = imageView(ctx_, u, slot.type, slot.format);
        if (view && u.texture) {
            const uint64_t serial = ctx_.stream().serial();
            u.texture->lastUseSerial = serial;
            if (u.access != GL_READ_ONLY) ctx_.stream().noteShaderWrites();
            // Stores to a buffer image change the buffer: CPU reads and later uploads wait for this draw.
            if (Buffer* buffer = u.texture->buffer.get(); buffer && u.access != GL_READ_ONLY) {
                buffer->lastUseSerial = buffer->lastWriteSerial = serial;
                ++buffer->version;
            } else if (buffer) {
                buffer->lastUseSerial = serial;
            }
        }
        if (!view && u.texture) WMGL_TRACE("image unit %d: no view for image type 0x%x, format 0x%x", unit, slot.type, slot.format);
    }
    return view ? view : dummyImage(slot.type);
}

LinkedProgram* DrawState::executable() {
    if (override_) return override_;
    if (Program* p = ctx_.state.program.get()) return p->linked.get();
    if (ProgramPipeline* pp = ctx_.state.programPipeline.get()) {
        current_ = pipelineExecutable(*pp);
        return current_.get();
    }
    if (ctx_.isCore()) return nullptr;
    // Fixed-function processing: the generated program for the state at
    // this draw (chosen once per draw, see beginDraw).
    if (!ffChosen_) {
        ffProgram_ = compat::fixedFunctionProgram(ctx_, drawMode_);
        ffPointSize_ = drawMode_ == GL_POINTS && compat::fixedFunctionWritesPointSize(ctx_);
        ffWideLines_ = compat::fixedFunctionWideLines(ctx_, drawMode_);
        ffChosen_ = true;
    }
    return ffProgram_ && ffProgram_->linked ? ffProgram_->linked.get() : nullptr;
}

void DrawState::beginDraw(GLenum mode) {
    drawMode_ = mode;
    ffChosen_ = false;
    ffProgram_ = nullptr;
    ffPointSize_ = false;
    ffWideLines_ = false;
}

void DrawState::bindUniforms(id<MTLRenderCommandEncoder> encoder, LinkedProgram& program) {
    if (tessInputs_.active) {
        // The tessellation evaluation vertex function's domain points, patch records and control block.
        static const NSUInteger kSlots[3] = {shader::kSlotTessVertices, shader::kSlotTessPatches, shader::kSlotTessControl};
        for (int i = 0; i < 3; ++i)
            [encoder setVertexBuffer:tessInputs_.buffers[i] offset:tessInputs_.offsets[i] atIndex:kSlots[i]];
    }
    // Driver uniforms read by the generated vertex code (see compiler.cpp addPointSize).
    const struct {
        float pointSize;
        uint32_t programPointSize;
        uint32_t clipFlags;  // see compiler.cpp addPointSize
        uint32_t clipEnables;
    } driver = {std::clamp(ctx_.state.pointSize, 1.0f, 511.0f), ctx_.state.programPointSize || ffPointSize_ ? 1u : 0u,
                (ctx_.state.clipDepthMode == GL_ZERO_TO_ONE ? 1u : 0u) | (ctx_.state.clipOrigin == GL_UPPER_LEFT ? 2u : 0u),
                ctx_.state.clipDistanceEnables};
    [encoder setVertexBytes:&driver length:sizeof(driver) atIndex:shader::kSlotDriverUniforms];
    const uint64_t serial = ctx_.stream().serial();
    // Subroutine selections and built-in uniforms live in the stage programs' default uniforms.
    for (int s : {static_cast<int>(Stage::Vertex), static_cast<int>(Stage::Fragment)})
        if (Program* owner = program.link.stages[s].present ? stageProgram(program, s) : nullptr) {
            applySubroutineSelections(ctx_, *owner);
            applyBuiltinUniforms(*owner);
        }
    static id<MTLBuffer> zero = [Device::get().mtl() newBufferWithLength:65536 options:MTLResourceStorageModePrivate];
    for (int s : {static_cast<int>(Stage::Vertex), static_cast<int>(Stage::Fragment)}) {
        const shader::StageOutput& stage = program.link.stages[s];
        if (!stage.present) continue;
        const bool vertex = s == static_cast<int>(Stage::Vertex);
        if (!vertex && (stage.usesPointCoord || sampleMask_ != ~0u)) {
            // Fragment driver uniforms: the gl_PointCoord flip and the sample mask (compiler.cpp).
            const uint32_t fragmentDriver[2] = {ctx_.state.pointSpriteCoordOrigin == GL_UPPER_LEFT ? 1u : 0u, sampleMask_};
            [encoder setFragmentBytes:fragmentDriver length:sizeof(fragmentDriver) atIndex:shader::kSlotDriverUniforms];
        }
        // Default uniforms of the program that owns the stage.
        Program* owner = stageProgram(program, s);
        const uint32_t size = owner && owner->linked ? (owner->linked->link.defaultBlockSize + 15) & ~15u : 0;
        if (stage.usesDefaultUniforms && size) {
            Program& p = *owner;
            if (p.uniformData.size() < size) p.uniformData.resize(size, 0);
            if (size <= 4096) {
                if (vertex) [encoder setVertexBytes:p.uniformData.data() length:size atIndex:shader::kSlotDefaultUniforms];
                else [encoder setFragmentBytes:p.uniformData.data() length:size atIndex:shader::kSlotDefaultUniforms];
            } else {
                TransientAllocation alloc = transient_.allocate(size, serial, 256);
                std::memcpy(alloc.data, p.uniformData.data(), size);
                if (vertex) [encoder setVertexBuffer:alloc.buffer offset:alloc.offset atIndex:shader::kSlotDefaultUniforms];
                else [encoder setFragmentBuffer:alloc.buffer offset:alloc.offset atIndex:shader::kSlotDefaultUniforms];
            }
        }
        for (size_t b = 0; b < program.link.blocks.size(); ++b) {
            const int slot = program.link.blocks[b].stageSlot[s];
            if (slot < 0) continue;
            id<MTLBuffer> buffer = zero;
            NSUInteger offset = 0;
            uniformBlockBuffer(program, b, buffer, offset);
            if (vertex) [encoder setVertexBuffer:buffer offset:offset atIndex:static_cast<NSUInteger>(slot)];
            else [encoder setFragmentBuffer:buffer offset:offset atIndex:static_cast<NSUInteger>(slot)];
        }
        for (size_t b = 0; b < program.link.atomicBuffers.size(); ++b) {
            const int slot = program.link.atomicBuffers[b].stageSlot[s];
            if (slot < 0) continue;
            id<MTLBuffer> buffer = nil;
            NSUInteger offset = 0;
            atomicCounterBuffer(program, b, buffer, offset);
            if (vertex) [encoder setVertexBuffer:buffer offset:offset atIndex:static_cast<NSUInteger>(slot)];
            else [encoder setFragmentBuffer:buffer offset:offset atIndex:static_cast<NSUInteger>(slot)];
        }
        bindStorageBlocks(encoder, program, s);
    }
}

void DrawState::uniformBlockBuffer(LinkedProgram& program, size_t index, __strong id<MTLBuffer>& buffer,
                                   NSUInteger& offset) {
    if (static_cast<int>(index) == program.link.fixedFunctionBlock ||
        (program.pipeline && program.link.blocks[index].name == shader::kFixedFunctionBlockName)) {
        // Lowered legacy built-in uniforms (compat/fixed_function.h).
        uint64_t generation = 0;
        const std::vector<uint8_t>& data = compat::fixedFunctionBlock(ctx_, generation);
        const uint64_t serial = ctx_.stream().serial();
        if (!ffBlock_.buffer || ffBlock_.generation != generation || ffBlock_.serial != serial) {
            TransientAllocation alloc = transient_.allocate(data.size(), serial, 256);
            std::memcpy(alloc.data, data.data(), data.size());
            ffBlock_ = {alloc.buffer, alloc.offset, generation, serial};
        }
        buffer = ffBlock_.buffer;
        offset = ffBlock_.offset;
        return;
    }
    const auto [owner, block] = blockSource(program, index);
    if (!owner) return;
    const Program& p = *owner;
    const GLuint binding = block < p.uniformBlockBindings.size() ? p.uniformBlockBindings[block] : 0;
    if (binding >= ContextState::kMaxUniformBufferBindings) return;
    const IndexedBufferBinding& ib = ctx_.state.uniformBufferBindings[binding];
    if (!ib.buffer || !ib.buffer->mtl) return;
    // A range starting past the end (the buffer shrank after binding) reads as unbound.
    if (ib.offset < 0 || ib.offset >= ib.buffer->size) return;
    buffer = ib.buffer->mtl;
    offset = static_cast<NSUInteger>(ib.offset);
    ib.buffer->lastUseSerial = ctx_.stream().serial();
}

void DrawState::atomicCounterBuffer(LinkedProgram& program, size_t index, __strong id<MTLBuffer>& buffer, NSUInteger& offset) {
    static id<MTLBuffer> scratch = [Device::get().mtl() newBufferWithLength:65536 options:MTLResourceStorageModePrivate];
    buffer = scratch;
    offset = 0;
    const GLuint binding = program.link.atomicBuffers[index].binding;
    if (binding >= static_cast<GLuint>(ContextState::kMaxAtomicCounterBufferBindings)) return;
    const IndexedBufferBinding& ib = ctx_.state.atomicCounterBufferBindings[binding];
    if (!ib.buffer || !ib.buffer->mtl || ib.offset < 0 || ib.offset >= ib.buffer->size) return;
    buffer = ib.buffer->mtl;
    offset = static_cast<NSUInteger>(ib.offset);
    // Counters change the buffer: CPU reads and later uploads wait for this draw.
    ib.buffer->lastUseSerial = ib.buffer->lastWriteSerial = ctx_.stream().serial();
    ctx_.stream().noteShaderWrites();
    ++ib.buffer->version;
}

bool DrawState::storageArguments(const shader::StageOutput& stage, const Program* owner, StorageArguments& out) {
    if (stage.storageArgs.empty()) return false;
    // Unbound elements point at scratch memory (GL leaves their accesses undefined).
    static id<MTLBuffer> scratch = [Device::get().mtl() newBufferWithLength:65536 options:MTLResourceStorageModePrivate];
    const size_t n = stage.storageArgs.size();
    const uint64_t serial = ctx_.stream().serial();
    // Layout (shader/compiler.h): n device pointers, the pointer to the
    // sizes, then the n sizes in bytes.
    TransientAllocation alloc = transient_.allocate((n + 1) * 8 + n * 4, serial, 16);
    uint64_t* pointers = static_cast<uint64_t*>(alloc.data);
    uint32_t* sizes = reinterpret_cast<uint32_t*>(pointers + n + 1);
    out.resources.clear();
    for (size_t i = 0; i < n; ++i) {
        const shader::StageOutput::StorageArg& arg = stage.storageArgs[i];
        id<MTLBuffer> buffer = scratch;
        NSUInteger offset = 0;
        uint32_t size = 0;
        const GLuint binding = owner && arg.block >= 0 && static_cast<size_t>(arg.block) < owner->storageBlockBindings.size()
                                   ? owner->storageBlockBindings[static_cast<size_t>(arg.block)]
                                   : 0;
        if (binding < static_cast<GLuint>(ContextState::kMaxShaderStorageBufferBindings)) {
            const IndexedBufferBinding& ib = ctx_.state.shaderStorageBufferBindings[binding];
            if (ib.buffer && ib.buffer->mtl && ib.offset >= 0 && ib.offset < ib.buffer->size) {
                buffer = ib.buffer->mtl;
                offset = static_cast<NSUInteger>(ib.offset);
                const GLsizeiptr available = ib.buffer->size - ib.offset;
                size = static_cast<uint32_t>(std::min<GLsizeiptr>(ib.size ? std::min(ib.size, available) : available, UINT32_MAX));
                ib.buffer->lastUseSerial = serial;
                if (arg.writes) {
                    // Stores change the buffer: CPU reads and later uploads wait for this work.
                    ib.buffer->lastWriteSerial = serial;
                    ++ib.buffer->version;
                    ctx_.stream().noteShaderWrites();
                }
            }
        }
        pointers[i] = buffer.gpuAddress + offset;
        sizes[i] = size;
        out.resources.emplace_back(buffer, arg.writes);
    }
    pointers[n] = alloc.buffer.gpuAddress + alloc.offset + (n + 1) * 8;
    out.buffer = alloc.buffer;
    out.offset = alloc.offset;
    return true;
}

void DrawState::bindStorageBlocks(id<MTLRenderCommandEncoder> encoder, LinkedProgram& program, int s) {
    StorageArguments args;
    if (!storageArguments(program.link.stages[s], stageProgram(program, s), args)) return;
    const bool vertex = s == static_cast<int>(Stage::Vertex);
    const MTLRenderStages stages = vertex ? MTLRenderStageVertex : MTLRenderStageFragment;
    for (const auto& [buffer, writes] : args.resources)
        [encoder useResource:buffer usage:writes ? MTLResourceUsageRead | MTLResourceUsageWrite : MTLResourceUsageRead stages:stages];
    if (vertex) [encoder setVertexBuffer:args.buffer offset:args.offset atIndex:shader::kSlotStorageBlocks];
    else [encoder setFragmentBuffer:args.buffer offset:args.offset atIndex:shader::kSlotStorageBlocks];
}

void DrawState::bindStorageBlocks(id<MTLComputeCommandEncoder> encoder, const shader::StageOutput& stage, const Program* owner) {
    StorageArguments args;
    if (!storageArguments(stage, owner, args)) return;
    for (const auto& [buffer, writes] : args.resources)
        [encoder useResource:buffer usage:writes ? MTLResourceUsageRead | MTLResourceUsageWrite : MTLResourceUsageRead];
    [encoder setBuffer:args.buffer offset:args.offset atIndex:shader::kSlotStorageBlocks];
}

void DrawState::samplerBinding(LinkedProgram& program, size_t index, __strong id<MTLTexture>& view,
                               __strong id<MTLSamplerState>& sampler) {
    const shader::SamplerSlotInfo& slot = program.link.samplers[index];
    const auto [owner, ownIndex] = samplerSource(program, index);
    const GLint unit = owner && ownIndex < owner->samplerUnits.size() ? owner->samplerUnits[ownIndex] : 0;
    Texture* texture = nullptr;
    if (unit >= 0 && unit < ContextState::kMaxTextureUnits && slot.textureTarget >= 0)
        texture = ctx_.state.textureUnits[unit].bound[slot.textureTarget].get();
    Sampler* samplerObject = unit >= 0 && unit < ContextState::kMaxTextureUnits
                                 ? ctx_.state.textureUnits[unit].sampler.get()
                                 : nullptr;
    if (texture && textureComplete(ctx_, *texture, samplerObject ? &samplerObject->params : &texture->sampler)) {
        view = samplingView(ctx_, *texture);
        SamplerParams params = samplerObject ? samplerObject->params : texture->sampler;
        params.lodBias += compat::textureUnitLodBias(ctx_, unit);  // glTexEnv(GL_TEXTURE_FILTER_CONTROL)
        sampler = samplerState(ctx_, *texture, params, slot.shadow);
        texture->lastUseSerial = ctx_.stream().serial();
        // A buffer texture reads its buffer: writes to the buffer must wait for this draw.
        if (texture->target == GL_TEXTURE_BUFFER && texture->buffer) texture->buffer->lastUseSerial = texture->lastUseSerial;
    } else {
        view = dummyTexture(slot.type);
        sampler = samplerState(ctx_, SamplerParams(), slot.shadow);
    }
}

void DrawState::prepareSamplerStorage(LinkedProgram& program) {
    // textureComplete builds pending storage with the blit encoder, which
    // would end the render or compute encoder of a draw in progress. Image
    // units (re)allocate writable storage the same way.
    for (size_t i = 0; i < program.link.samplers.size(); ++i) {
        id<MTLTexture> view = nil;
        id<MTLSamplerState> sampler = nil;
        samplerBinding(program, i, view, sampler);
    }
    for (size_t i = 0; i < program.link.images.size(); ++i) imageBinding(program, i);
    prepareImageShadows(program);
}

void DrawState::prepareImageShadows(LinkedProgram& program) {
    // Bindings with a shadow texture (core::ImageShadow): the shadows take
    // their texels before the draw, once per draw (an earlier pass of the
    // draw may have written a shadow already).
    for (size_t i = 0; i < program.link.images.size(); ++i) {
        const GLint unit = imageUnit(program, i);
        if (unit < 0 || unit >= ContextState::kMaxImageUnits) continue;
        const ImageUnit& u = ctx_.state.imageUnits[unit];
        const shader::ImageSlotInfo& slot = program.link.images[i];
        PendingShadow entry;
        if (!imageView(ctx_, u, slot.type, slot.format, &entry.shadow) || !entry.shadow.shadow) continue;
        bool known = false;
        for (PendingShadow& other : imageShadows_)
            if (other.shadow.shadow == entry.shadow.shadow) {
                other.write = other.write || u.access != GL_READ_ONLY;
                known = true;
            }
        if (known) continue;
        entry.write = u.access != GL_READ_ONLY;
        copyImageShadow(ctx_, entry.shadow, true);
        imageShadows_.push_back(entry);
    }
}

void DrawState::finishImageShadows() {
    // The written shadows give their texels back to their textures.
    for (const PendingShadow& entry : imageShadows_)
        if (entry.write) copyImageShadow(ctx_, entry.shadow, false);
    imageShadows_.clear();
}

void DrawState::bindTextures(id<MTLRenderCommandEncoder> encoder, LinkedProgram& program) {
    for (size_t i = 0; i < program.link.samplers.size(); ++i) {
        const shader::SamplerSlotInfo& slot = program.link.samplers[i];
        id<MTLTexture> view = nil;
        id<MTLSamplerState> sampler = nil;
        samplerBinding(program, i, view, sampler);
        for (int s : {static_cast<int>(Stage::Vertex), static_cast<int>(Stage::Fragment)}) {
            const int index = slot.stageTexture[s];
            if (index < 0) continue;
            if (s == static_cast<int>(Stage::Vertex)) {
                [encoder setVertexTexture:view atIndex:static_cast<NSUInteger>(index)];
                [encoder setVertexSamplerState:sampler atIndex:static_cast<NSUInteger>(index)];
            } else {
                [encoder setFragmentTexture:view atIndex:static_cast<NSUInteger>(index)];
                [encoder setFragmentSamplerState:sampler atIndex:static_cast<NSUInteger>(index)];
            }
        }
    }
    for (size_t i = 0; i < program.link.images.size(); ++i) {
        const shader::ImageSlotInfo& slot = program.link.images[i];
        const int vertex = slot.stageTexture[static_cast<int>(Stage::Vertex)];
        const int fragment = slot.stageTexture[static_cast<int>(Stage::Fragment)];
        if (vertex < 0 && fragment < 0) continue;
        id<MTLTexture> view = imageBinding(program, i);
        if (vertex >= 0) [encoder setVertexTexture:view atIndex:static_cast<NSUInteger>(vertex)];
        if (fragment >= 0) [encoder setFragmentTexture:view atIndex:static_cast<NSUInteger>(fragment)];
    }
}

void DrawState::applyDynamicState(id<MTLRenderCommandEncoder> encoder, const RenderTargetDesc& target) {
    const ContextState& st = ctx_.state;
    // Every viewport of GL 4.1's array: shaders select one with gl_ViewportIndex.
    MTLViewport viewports[ContextState::kMaxViewports];
    for (int i = 0; i < ContextState::kMaxViewports; ++i) {
        viewports[i].originX = st.viewports[i][0];
        viewports[i].originY = st.viewports[i][1];
        viewports[i].width = std::max(0.0f, st.viewports[i][2]);
        viewports[i].height = std::max(0.0f, st.viewports[i][3]);
        viewports[i].znear = st.depthRanges[i][0];
        viewports[i].zfar = st.depthRanges[i][1];
    }
    [encoder setViewports:viewports count:ContextState::kMaxViewports];
    // Cull / winding (the vertex stage flips Y, so GL's CCW is Metal's CW).
    MTLCullMode cull = MTLCullModeNone;
    if (st.cullFace) cull = st.cullFaceMode == GL_FRONT ? MTLCullModeFront : MTLCullModeBack;
    [encoder setCullMode:cull];
    // With an UPPER_LEFT clip origin (GL_ARB_clip_control) Y is not flipped.
    const bool flipped = st.clipOrigin == GL_LOWER_LEFT;
    [encoder setFrontFacingWinding:(st.frontFace == GL_CCW) == flipped ? MTLWindingClockwise : MTLWindingCounterClockwise];
    [encoder setTriangleFillMode:st.polygonMode == GL_LINE ? MTLTriangleFillModeLines : MTLTriangleFillModeFill];
    const bool offset = (st.polygonMode == GL_FILL && st.polygonOffsetFill) ||
                        (st.polygonMode == GL_LINE && st.polygonOffsetLine) ||
                        (st.polygonMode == GL_POINT && st.polygonOffsetPoint);
    [encoder setDepthBias:offset ? st.polygonOffsetUnits : 0.0f
               slopeScale:offset ? st.polygonOffsetFactor : 0.0f
                    clamp:offset ? st.polygonOffsetClamp : 0.0f];
    [encoder setDepthClipMode:st.depthClamp ? MTLDepthClipModeClamp : MTLDepthClipModeClip];
    // The reference is clamped to the stencil range (GL 3.3 section 4.1.5).
    [encoder setStencilFrontReferenceValue:static_cast<uint32_t>(std::clamp(st.stencilFront.ref, 0, 255))
                        backReferenceValue:static_cast<uint32_t>(std::clamp(st.stencilBack.ref, 0, 255))];
    [encoder setBlendColorRed:st.blendColor[0] green:st.blendColor[1] blue:st.blendColor[2] alpha:st.blendColor[3]];
    (void)target;
}

// --------------------------------------------------------------- prepare

id<MTLRenderCommandEncoder> DrawState::prepare(GLenum mode, MTLPrimitiveType& primitive) {
    queriesPrepareVisibility(ctx_);
    RenderTargetDesc target;
    TargetFormats formats;
    if (!drawTarget(target, formats)) return nil;
    const ContextState& st = ctx_.state;
    if (st.cullFace && st.cullFaceMode == GL_FRONT_AND_BACK && topologyClass(mode) == MTLPrimitiveTopologyClassTriangle)
        return nil;
    LinkedProgram* program = executable();
    if (!program) return nil;
    // Scissor rectangles of every viewport index (an empty one clips
    // everything). Without gl_ViewportIndex only index 0 is used, and an
    // empty rectangle there draws nothing.
    for (int i = 0; i < ContextState::kMaxViewports; ++i) {
        MTLScissorRect& scissor = lastScissors_[i];
        scissor = {0, 0, target.width, target.height};
        if (!((st.scissorTests >> i) & 1u)) continue;
        const GLint* box = st.scissors[i];
        const int64_t x0 = std::clamp<int64_t>(box[0], 0, target.width);
        const int64_t y0 = std::clamp<int64_t>(box[1], 0, target.height);
        const int64_t x1 = std::clamp<int64_t>(static_cast<int64_t>(box[0]) + std::max(0, box[2]), 0, target.width);
        const int64_t y1 = std::clamp<int64_t>(static_cast<int64_t>(box[1]) + std::max(0, box[3]), 0, target.height);
        if (x1 <= x0 || y1 <= y0) {
            if (i == 0 && !program->writesViewportIndex()) return nil;
            scissor = {0, 0, 0, 0};
            continue;
        }
        scissor = {static_cast<NSUInteger>(x0), static_cast<NSUInteger>(y0), static_cast<NSUInteger>(x1 - x0),
                   static_cast<NSUInteger>(y1 - y0)};
    }

    id<MTLRenderCommandEncoder> encoder = ctx_.stream().render(target);
    if (ctx_.stream().passGeneration() != lastPassGeneration_) {
        lastPassGeneration_ = ctx_.stream().passGeneration();
    }
    PipelineKey key;
    std::memset(&key, 0, sizeof(key));
    key.programId = program->uid;
    key.sampleCount = static_cast<uint8_t>(target.samples);
    key.alphaToCoverage = st.multisample && target.samples > 1 && st.sampleAlphaToCoverage;
    key.alphaToOne = st.multisample && target.samples > 1 && st.sampleAlphaToOne;
    // GL_SAMPLE_MASK and GL_SAMPLE_COVERAGE: the fragment function writes the
    // mask (a pipeline variant, so other draws keep Metal's fast paths).
    sampleMask_ = ~0u;
    if (st.multisample && target.samples > 1 && (st.sampleMask || st.sampleCoverage)) {
        uint32_t mask = st.sampleMask ? st.sampleMaskValue : ~0u;
        if (st.sampleCoverage) {
            const uint32_t all = target.samples >= 32 ? ~0u : (1u << target.samples) - 1;
            const long covered = std::lround(st.sampleCoverageValue * static_cast<float>(target.samples));
            uint32_t coverage = covered >= 32 ? ~0u : (1u << covered) - 1;
            if (st.sampleCoverageInvert) coverage = ~coverage & all;
            mask &= coverage;
        }
        if (mask != ~0u && program->fragmentSampleMask()) {
            key.sampleMask = 1;
            sampleMask_ = mask;
        }
    }
    // GL_SAMPLE_SHADING with more than one sample to shade: per-sample execution.
    key.sampleShading = st.multisample && st.sampleShading && target.samples > 1 &&
                        std::ceil(st.minSampleShading * static_cast<float>(target.samples)) > 1.0f;
    key.ignoreSampleMask = !(st.multisample && target.samples > 1) && program->writesSampleMask();
    key.rasterization = !st.rasterizerDiscard;
    // Metal requires the input topology class when the vertex function
    // writes the layer or viewport index; otherwise it stays unspecified.
    key.topologyClass = program->writesLayerOrViewport() ? static_cast<uint8_t>(topologyClass(mode)) : 0;
    for (int i = 0; i < 8; ++i) key.colorFormats[i] = static_cast<uint16_t>(formats.color[i]);
    // GL_COLOR_LOGIC_OP replaces blending on unsigned normalized and integer
    // attachments; the fragment function applies the operation (framebuffer
    // fetch). GL_COPY needs no variant.
    if (st.colorLogicOp && st.logicOp != GL_COPY && program->fragmentVariant(false, st.logicOp - GL_CLEAR + 1, key.colorFormats))
        key.logicOp = static_cast<uint8_t>(st.logicOp - GL_CLEAR + 1);
    for (int i = 0; i < 8; ++i) {
        if (!formats.color[i]) continue;
        PipelineKey::Blend& b = key.blend[i];
        const auto& mask = st.colorMask[i];
        b.writeMask = static_cast<uint8_t>((mask[0] ? MTLColorWriteMaskRed : 0) | (mask[1] ? MTLColorWriteMaskGreen : 0) |
                                           (mask[2] ? MTLColorWriteMaskBlue : 0) |
                                           (mask[3] && !formats.alphaIsOne[i] ? MTLColorWriteMaskAlpha : 0));
        b.enabled = (st.blendEnable >> i) & 1 && !formats.integer[i] && !(st.colorLogicOp && logicOpApplies(formats.color[i]));
        if (b.enabled) {
            b.srcRGB = static_cast<uint8_t>(mtlBlendFactor(st.blend[i].srcRGB));
            b.dstRGB = static_cast<uint8_t>(mtlBlendFactor(st.blend[i].dstRGB));
            b.srcAlpha = static_cast<uint8_t>(mtlBlendFactor(st.blend[i].srcAlpha));
            b.dstAlpha = static_cast<uint8_t>(mtlBlendFactor(st.blend[i].dstAlpha));
            b.opRGB = static_cast<uint8_t>(mtlBlendOp(st.blend[i].equationRGB));
            b.opAlpha = static_cast<uint8_t>(mtlBlendOp(st.blend[i].equationAlpha));
        }
    }
    key.depthFormat = static_cast<uint16_t>(formats.depth);
    key.stencilFormat = static_cast<uint16_t>(formats.stencil);
    (void)primitive;
    lastKey_ = key;
    lastFormats_ = formats;
    return encoder;
}

// ------------------------------------------------------------------ draws

namespace {

MTLPrimitiveType nativePrimitive(GLenum mode, bool& needsIndices) {
    needsIndices = false;
    switch (mode) {
    case GL_POINTS: return MTLPrimitiveTypePoint;
    case GL_LINES: return MTLPrimitiveTypeLine;
    case GL_LINE_STRIP: return MTLPrimitiveTypeLineStrip;
    case GL_TRIANGLES: return MTLPrimitiveTypeTriangle;
    case GL_TRIANGLE_STRIP: return MTLPrimitiveTypeTriangleStrip;
    case GL_LINE_LOOP: needsIndices = true; return MTLPrimitiveTypeLine;
    case GL_TRIANGLE_FAN: case GL_QUADS: case GL_QUAD_STRIP: case GL_POLYGON:
        needsIndices = true;
        return MTLPrimitiveTypeTriangle;
    case GL_LINES_ADJACENCY: case GL_LINE_STRIP_ADJACENCY:  // without a geometry shader: drop adjacency (W16)
        needsIndices = true;
        return MTLPrimitiveTypeLine;
    default:
        needsIndices = true;
        return MTLPrimitiveTypeTriangle;
    }
}

// The utility kernel that rewrites geometry-shader strips as lists in GL's
// last-vertex provoking order.
id<MTLComputePipelineState> stripsProvokingLastKernel() {
    static std::once_flag once;
    static id<MTLComputePipelineState> state = nil;
    std::call_once(once, [] {
        id<MTLFunction> function = [Device::get().utilityLibrary() newFunctionWithName:@"wmgl_strips_provoking_last"];
        NSError* error = nil;
        if (function) state = [Device::get().mtl() newComputePipelineStateWithFunction:function error:&error];
        if (!state) WMGL_ERR("strip conversion kernel: %s", error ? error.localizedDescription.UTF8String : "missing");
    });
    return state;
}

// Lists the primitives of a line or triangle stream with GL's last-vertex
// provoking vertex first: Metal flat-shades from the first vertex. Each
// triangle is rotated, so its winding is unchanged.
void expandProvokingLast(GLenum mode, const std::vector<uint32_t>& v, std::vector<uint32_t>& out) {
    const size_t n = v.size();
    out.clear();
    switch (mode) {
    case GL_LINES:
        for (size_t i = 0; i + 1 < n; i += 2) out.insert(out.end(), {v[i + 1], v[i]});
        return;
    case GL_LINE_STRIP: case GL_LINE_LOOP:
        for (size_t i = 0; i + 1 < n; ++i) out.insert(out.end(), {v[i + 1], v[i]});
        if (mode == GL_LINE_LOOP && n >= 2) out.insert(out.end(), {v[0], v[n - 1]});
        return;
    case GL_TRIANGLES:
        for (size_t i = 0; i + 2 < n; i += 3) out.insert(out.end(), {v[i + 2], v[i], v[i + 1]});
        return;
    case GL_TRIANGLE_STRIP:
        // Triangle i is (i, i+1, i+2) for even i and (i+1, i, i+2) for odd i.
        for (size_t i = 0; i + 2 < n; ++i) {
            if (i % 2 == 0) out.insert(out.end(), {v[i + 2], v[i], v[i + 1]});
            else out.insert(out.end(), {v[i + 2], v[i + 1], v[i]});
        }
        return;
    case GL_TRIANGLE_FAN:
        for (size_t i = 1; i + 1 < n; ++i) out.insert(out.end(), {v[i + 1], v[0], v[i]});
        return;
    case GL_QUADS:
        // Split along the 1-3 diagonal so both triangles end with the quad's last vertex.
        for (size_t i = 0; i + 3 < n; i += 4)
            out.insert(out.end(), {v[i + 3], v[i], v[i + 1], v[i + 3], v[i + 1], v[i + 2]});
        return;
    case GL_QUAD_STRIP:
        // Quad k is (2k, 2k+1, 2k+3, 2k+2) in order; its provoking vertex is 2k+3.
        for (size_t i = 0; i + 3 < n; i += 2)
            out.insert(out.end(), {v[i + 3], v[i], v[i + 1], v[i + 3], v[i + 2], v[i]});
        return;
    // Without a geometry shader the adjacent vertices are dropped; the
    // provoking vertices are those of GL 3.3 table 2.12.
    case GL_LINES_ADJACENCY:
        for (size_t i = 0; i + 3 < n; i += 4) out.insert(out.end(), {v[i + 2], v[i + 1]});
        return;
    case GL_LINE_STRIP_ADJACENCY:
        for (size_t i = 1; i + 2 < n; ++i) out.insert(out.end(), {v[i + 1], v[i]});
        return;
    case GL_TRIANGLES_ADJACENCY:
        for (size_t i = 0; i + 5 < n; i += 6) out.insert(out.end(), {v[i + 4], v[i], v[i + 2]});
        return;
    case GL_TRIANGLE_STRIP_ADJACENCY:
        for (size_t i = 0; i + 5 < n; i += 2) {
            if ((i / 2) % 2 == 0) out.insert(out.end(), {v[i + 4], v[i], v[i + 2]});
            else out.insert(out.end(), {v[i + 4], v[i + 2], v[i]});
        }
        return;
    default:
        return;
    }
}

// Expands a GL primitive stream (vertex numbers in `in`) into a Metal list.
void expandIndices(GLenum mode, const std::vector<uint32_t>& in, std::vector<uint32_t>& out) {
    const size_t n = in.size();
    out.clear();
    switch (mode) {
    case GL_LINE_LOOP:
        if (n < 2) return;
        for (size_t i = 0; i + 1 < n; ++i) { out.push_back(in[i]); out.push_back(in[i + 1]); }
        out.push_back(in[n - 1]);
        out.push_back(in[0]);
        return;
    case GL_TRIANGLE_FAN:
        // First-vertex convention: triangle (0, i, i+1) is provoked by vertex
        // i, listed first (Metal flat-shades from it); rotated, so the
        // winding is unchanged.
        for (size_t i = 1; i + 1 < n; ++i) out.insert(out.end(), {in[i], in[i + 1], in[0]});
        return;
    case GL_POLYGON:
        // A polygon is provoked by its first vertex in both conventions.
        for (size_t i = 1; i + 1 < n; ++i) out.insert(out.end(), {in[0], in[i], in[i + 1]});
        return;
    case GL_QUADS:
        for (size_t i = 0; i + 3 < n; i += 4) {
            out.insert(out.end(), {in[i], in[i + 1], in[i + 2], in[i], in[i + 2], in[i + 3]});
        }
        return;
    case GL_QUAD_STRIP:
        for (size_t i = 0; i + 3 < n; i += 2) {
            out.insert(out.end(), {in[i], in[i + 1], in[i + 3], in[i], in[i + 3], in[i + 2]});
        }
        return;
    case GL_LINES_ADJACENCY:
        for (size_t i = 0; i + 3 < n; i += 4) { out.push_back(in[i + 1]); out.push_back(in[i + 2]); }
        return;
    case GL_LINE_STRIP_ADJACENCY:
        for (size_t i = 1; i + 2 < n; ++i) { out.push_back(in[i]); out.push_back(in[i + 1]); }
        return;
    case GL_TRIANGLES_ADJACENCY:
        for (size_t i = 0; i + 5 < n; i += 6) { out.push_back(in[i]); out.push_back(in[i + 2]); out.push_back(in[i + 4]); }
        return;
    case GL_TRIANGLE_STRIP_ADJACENCY:
        for (size_t i = 0; i + 5 < n; i += 2) {
            if ((i / 2) % 2 == 0) { out.push_back(in[i]); out.push_back(in[i + 2]); out.push_back(in[i + 4]); }
            else { out.push_back(in[i + 2]); out.push_back(in[i]); out.push_back(in[i + 4]); }
        }
        return;
    default:
        out = in;
        return;
    }
}

// The independent-primitive mode a primitive stream is drawn as when listed.
GLenum listMode(GLenum mode) {
    switch (topologyClass(mode)) {
    case MTLPrimitiveTopologyClassPoint: return GL_POINTS;
    case MTLPrimitiveTopologyClassLine: return GL_LINES;
    default: return GL_TRIANGLES;
    }
}

// Appends one restart-free segment as a list of independent primitives, each
// listing its provoking vertex first (`rotate`: the last-vertex convention).
void appendMetalList(GLenum mode, const std::vector<uint32_t>& v, bool rotate, std::vector<uint32_t>& out) {
    std::vector<uint32_t> part;
    const size_t n = v.size();
    if (rotate) {
        expandProvokingLast(mode, v, part);
    } else {
        switch (mode) {
        case GL_POINTS: part = v; break;
        case GL_LINES: part.assign(v.begin(), v.begin() + static_cast<long>(n / 2 * 2)); break;
        case GL_LINE_STRIP:
            for (size_t i = 0; i + 1 < n; ++i) part.insert(part.end(), {v[i], v[i + 1]});
            break;
        case GL_TRIANGLES: part.assign(v.begin(), v.begin() + static_cast<long>(n / 3 * 3)); break;
        case GL_TRIANGLE_STRIP:
            // Triangle i is provoked by vertex i; odd triangles are rotated to list it first.
            for (size_t i = 0; i + 2 < n; ++i) {
                if (i % 2 == 0) part.insert(part.end(), {v[i], v[i + 1], v[i + 2]});
                else part.insert(part.end(), {v[i], v[i + 2], v[i + 1]});
            }
            break;
        default: expandIndices(mode, v, part); break;
        }
    }
    out.insert(out.end(), part.begin(), part.end());
}

}  // namespace

bool DrawState::issue(GLenum mode, GLint first, GLsizei count, GLsizei instances, const uint32_t* indexData,
                      id<MTLBuffer> indexBuffer, NSUInteger indexOffset, MTLIndexType indexType, GLint baseVertex,
                      GLint minVertex, GLsizei vertexSpan, bool listed) {
    queriesCountPrimitives(ctx_, mode, count, instances);
    stats::count(&stats::Frame::draws);
    LinkedProgram* current = executable();
    if (!current) return false;
    waitForCpuVertexReads(*current, false);
    prepareSamplerStorage(*current);
    // GL_POINT polygon mode: Metal only fills or outlines triangles, so the
    // vertices of every triangle are drawn as points. Face culling is not
    // applied to them (FRONT_AND_BACK still draws nothing).
    const bool points = pointPolygons(mode);
    if (points && ctx_.state.cullFace && ctx_.state.cullFaceMode == GL_FRONT_AND_BACK) return true;
    MTLPrimitiveType primitive = MTLPrimitiveTypeTriangle;
    id<MTLRenderCommandEncoder> encoder = prepare(points ? GL_POINTS : mode, primitive);
    if (!encoder) return false;
    queriesBeforeDraw(ctx_, encoder);
    LinkedProgram& program = *current;
    PipelineKey key = lastKey_;
    // Vertices are renumbered from the first one used, so converted vertex
    // data starts at the draw's first vertex instead of vertex 0; not when
    // the program reads gl_VertexID, which is the GL number.
    const GLint rebase = minVertex > 0 && !program.readsVertexId() ? minVertex : 0;
    vertexRebase_ = rebase;
    // Instanced attributes need data for every instance they cover.
    const bool bound = bindVertexInputs(encoder, key, program, minVertex, vertexSpan, instances);
    vertexRebase_ = 0;
    if (!bound) {
        WMGL_WARN("too many vertex buffers for one draw");
        return false;
    }
    id<MTLRenderPipelineState> pipeline = ctx_.share().pipelines().render(key, program);
    if (!pipeline) return false;
    [encoder setRenderPipelineState:pipeline];
    bindDrawState(encoder, program);
    if (program.link.xfbVertexCapture) bindCaptureDefaults(encoder, program);

    bool needsIndices = false;
    MTLPrimitiveType native = nativePrimitive(mode, needsIndices);
    const bool rotate = !listed && !points && rotateProvoking(mode, program);
    if (rotate) native = topologyClass(mode) == MTLPrimitiveTopologyClassLine ? MTLPrimitiveTypeLine : MTLPrimitiveTypeTriangle;
    if (points) native = MTLPrimitiveTypePoint;
    const uint64_t serial = ctx_.stream().serial();
    if (needsIndices || rotate || points || (indexData && !indexBuffer)) {
        std::vector<uint32_t> in, out;
        in.reserve(static_cast<size_t>(count));
        for (GLsizei i = 0; i < count; ++i) in.push_back(indexData ? indexData[i] : static_cast<uint32_t>(first + i - rebase));
        if (points) appendMetalList(mode, in, false, out);  // each triangle's vertices, shared ones once per triangle
        else if (rotate) expandProvokingLast(mode, in, out);
        else if (needsIndices) expandIndices(mode, in, out);
        else out.swap(in);
        if (out.empty()) return true;
        TransientAllocation alloc = transient_.allocate(out.size() * 4, serial, 4);
        std::memcpy(alloc.data, out.data(), out.size() * 4);
        [encoder drawIndexedPrimitives:native indexCount:out.size() indexType:MTLIndexTypeUInt32
                           indexBuffer:alloc.buffer indexBufferOffset:alloc.offset instanceCount:std::max(1, instances)
                            baseVertex:indexData ? baseVertex - rebase : 0 baseInstance:baseInstance_];
    } else if (indexBuffer) {
        [encoder drawIndexedPrimitives:native indexCount:static_cast<NSUInteger>(count) indexType:indexType
                           indexBuffer:indexBuffer indexBufferOffset:indexOffset instanceCount:std::max(1, instances)
                            baseVertex:baseVertex - rebase baseInstance:baseInstance_];
    } else {
        [encoder drawPrimitives:native vertexStart:static_cast<NSUInteger>(first - rebase)
                    vertexCount:static_cast<NSUInteger>(count) instanceCount:std::max(1, instances)
                   baseInstance:baseInstance_];
    }
    ctx_.stream().noteDraw();
    return true;
}

bool DrawState::pointPolygons(GLenum mode) const {
    const ContextState& st = ctx_.state;
    return topologyClass(mode) == MTLPrimitiveTopologyClassTriangle && st.polygonMode == GL_POINT &&
           st.polygonModeBack == GL_POINT;
}

bool DrawState::rotateProvoking(GLenum mode, LinkedProgram& program) const {
    if (ctx_.state.provokingVertex != GL_LAST_VERTEX_CONVENTION || !program.stage(Stage::Fragment).flatInputs) return false;
    switch (mode) {
    case GL_LINES: case GL_LINE_STRIP: case GL_LINE_LOOP: case GL_TRIANGLES: case GL_TRIANGLE_STRIP: case GL_TRIANGLE_FAN:
    case GL_QUADS: case GL_QUAD_STRIP:  // a polygon's provoking vertex is its first in both conventions
    case GL_LINES_ADJACENCY: case GL_LINE_STRIP_ADJACENCY: case GL_TRIANGLES_ADJACENCY:
    case GL_TRIANGLE_STRIP_ADJACENCY:  // without a geometry shader (with one, drawGeometry)
        return true;
    default:
        return false;
    }
}

void DrawState::bindDrawState(id<MTLRenderCommandEncoder> encoder, LinkedProgram& program) {
    DepthStencilKey ds;
    std::memset(&ds, 0, sizeof(ds));
    const ContextState& st = ctx_.state;
    const RenderTargetDesc& target = ctx_.stream().currentTarget();
    const bool hasDepth = lastFormats_.depthAttached, hasStencil = lastFormats_.stencilAttached;
    ds.depthCompare = static_cast<uint8_t>(st.depthTest && hasDepth ? mtlCompare(st.depthFunc) : MTLCompareFunctionAlways);
    ds.depthWrite = st.depthTest && hasDepth && st.depthMask;
    ds.stencilEnabled = st.stencilTest && hasStencil;
    if (ds.stencilEnabled) {
        auto face = [](const StencilFace& f, DepthStencilKey::Face& out) {
            out.compare = static_cast<uint8_t>(mtlCompare(f.func));
            out.fail = static_cast<uint8_t>(mtlStencilOp(f.sfail));
            out.depthFail = static_cast<uint8_t>(mtlStencilOp(f.dpfail));
            out.pass = static_cast<uint8_t>(mtlStencilOp(f.dppass));
            out.readMask = f.valueMask & 0xff;
            out.writeMask = f.writeMask & 0xff;
        };
        // Front/back are GL's; the Y flip does not change which faces are front-facing.
        face(st.stencilFront, ds.front);
        face(st.stencilBack, ds.back);
    }
    [encoder setDepthStencilState:ctx_.share().pipelines().depthStencil(ds)];
    applyDynamicState(encoder, target);
    [encoder setScissorRects:lastScissors_ count:ContextState::kMaxViewports];
    bindUniforms(encoder, program);
    bindTextures(encoder, program);
}

// ------------------------------------------------------ transform feedback

namespace {

// Assembles a primitive stream into independent primitives in capture order
// (strips and fans as GL 3.3 section 2.15.2 lists their vertices).
std::vector<uint32_t> assembleForCapture(GLenum mode, const std::vector<uint32_t>& v) {
    std::vector<uint32_t> out;
    const size_t n = v.size();
    switch (mode) {
    case GL_POINTS: out = v; break;
    case GL_LINES: out.assign(v.begin(), v.begin() + static_cast<long>(n / 2 * 2)); break;
    case GL_LINE_STRIP:
        for (size_t i = 0; i + 1 < n; ++i) { out.push_back(v[i]); out.push_back(v[i + 1]); }
        break;
    case GL_LINE_LOOP:
        for (size_t i = 0; i + 1 < n; ++i) { out.push_back(v[i]); out.push_back(v[i + 1]); }
        if (n >= 2) { out.push_back(v[n - 1]); out.push_back(v[0]); }
        break;
    case GL_TRIANGLES: out.assign(v.begin(), v.begin() + static_cast<long>(n / 3 * 3)); break;
    case GL_TRIANGLE_STRIP:
        for (size_t i = 0; i + 2 < n; ++i) {
            if (i % 2 == 0) { out.push_back(v[i]); out.push_back(v[i + 1]); }
            else { out.push_back(v[i + 1]); out.push_back(v[i]); }
            out.push_back(v[i + 2]);
        }
        break;
    case GL_TRIANGLE_FAN: case GL_POLYGON:
        for (size_t i = 1; i + 1 < n; ++i) { out.push_back(v[0]); out.push_back(v[i]); out.push_back(v[i + 1]); }
        break;
    case GL_QUADS:  // compatibility profile: each quad as two triangles
        for (size_t i = 0; i + 3 < n; i += 4) out.insert(out.end(), {v[i], v[i + 1], v[i + 2], v[i], v[i + 2], v[i + 3]});
        break;
    case GL_QUAD_STRIP:
        for (size_t i = 0; i + 3 < n; i += 2) out.insert(out.end(), {v[i], v[i + 1], v[i + 3], v[i], v[i + 3], v[i + 2]});
        break;
    default: break;
    }
    return out;
}

}  // namespace

void DrawState::bindCaptureDefaults(id<MTLRenderCommandEncoder> encoder, LinkedProgram& program) {
    // Capture code is compiled into the vertex function; outside a capture
    // pass it sees an all-zero control block (capture inactive).
    static id<MTLBuffer> zero = [Device::get().mtl() newBufferWithLength:256 options:MTLResourceStorageModeShared];
    [encoder setVertexBuffer:zero offset:0 atIndex:shader::kSlotXfbControl];
    for (int b = 0; b < program.link.xfbBufferCount; ++b)
        [encoder setVertexBuffer:zero offset:0 atIndex:shader::kSlotFirstXfbBuffer + static_cast<NSUInteger>(b)];
}

void DrawState::captureTransformFeedback(GLenum mode, const std::vector<uint32_t>& vertices, GLsizei instances) {
    TransformFeedback& xfb = *ctx_.state.transformFeedback;
    LinkedProgram* lp = executable();
    if (!xfb.active || xfb.paused || !lp || !lp->link.xfbVertexCapture) return;
    const shader::LinkResult& link = lp->link;
    const uint32_t perPrimitive = xfb.primitiveMode == GL_POINTS ? 1 : xfb.primitiveMode == GL_LINES ? 2 : 3;
    const std::vector<uint32_t> list = assembleForCapture(mode, vertices);
    if (list.empty()) return;
    // Whole primitives that fit in every bound buffer.
    uint64_t primitives = (list.size() / perPrimitive) * static_cast<uint64_t>(std::max(1, instances));
    for (int b = 0; b < link.xfbBufferCount; ++b) {
        const TransformFeedback::Binding& binding = xfb.buffers[b];
        if (!binding.buffer || !binding.buffer->mtl) return;
        // The bound range, clamped to the buffer's current size (it may have shrunk since binding).
        const int64_t available = std::max<int64_t>(0, binding.buffer->size - binding.offset);
        const int64_t size = binding.size ? std::min<int64_t>(binding.size, available) : available;
        const int64_t remaining = std::max<int64_t>(0, size - xfb.written[b]);
        const uint64_t vertexBytes = static_cast<uint64_t>(link.xfbStride[b]) * 4;
        if (vertexBytes) primitives = std::min<uint64_t>(primitives, static_cast<uint64_t>(remaining) / vertexBytes / perPrimitive);
    }
    if (Query* q = ctx_.state.activeQueries[kQueryXfbPrimitivesWritten].get()) q->result += primitives;
    xfb.primitivesWritten[0] += primitives;
    if (!primitives) return;

    id<MTLBuffer> buffers[4] = {};
    uint32_t baseWords[4] = {};
    for (int b = 0; b < link.xfbBufferCount; ++b) {
        buffers[b] = xfb.buffers[b].buffer->mtl;
        baseWords[b] = static_cast<uint32_t>((xfb.buffers[b].offset + xfb.written[b]) / 4);
    }
    if (!runVertexCapture(*lp, list, instances, static_cast<uint32_t>(primitives * perPrimitive), link.xfbBufferCount, buffers,
                          baseWords, link.xfbStride))
        return;
    const uint64_t serial = ctx_.stream().serial();
    for (int b = 0; b < link.xfbBufferCount; ++b) {
        Buffer& buffer = *xfb.buffers[b].buffer;
        xfb.written[b] += static_cast<GLsizeiptr>(primitives * perPrimitive * link.xfbStride[b] * 4);
        buffer.lastUseSerial = buffer.lastWriteSerial = serial;
        ++buffer.version;
    }
}

bool DrawState::runVertexCapture(LinkedProgram& lp, const std::vector<uint32_t>& list, GLsizei instances, uint32_t limit,
                                 int bufferCount, const __strong id<MTLBuffer>* buffers, const uint32_t* baseWords,
                                 const uint32_t* strides) {
    waitForCpuVertexReads(lp, true);
    prepareSamplerStorage(lp);
    RenderTargetDesc target;
    TargetFormats formats;
    if (!drawTarget(target, formats, false)) return false;
    id<MTLRenderCommandEncoder> encoder = ctx_.stream().render(target);
    PipelineKey key;
    std::memset(&key, 0, sizeof(key));
    key.programId = lp.uid;
    key.sampleCount = static_cast<uint8_t>(target.samples);
    key.rasterization = 0;
    key.topologyClass = static_cast<uint8_t>(MTLPrimitiveTopologyClassPoint);
    for (int i = 0; i < 8; ++i) key.colorFormats[i] = static_cast<uint16_t>(formats.color[i]);
    key.depthFormat = static_cast<uint16_t>(formats.depth);
    key.stencilFormat = static_cast<uint16_t>(formats.stencil);
    if (!bindVertexInputs(encoder, key, lp, 0, static_cast<GLsizei>(list.size()), instances, &list)) return false;
    id<MTLRenderPipelineState> pipeline = ctx_.share().pipelines().render(key, lp);
    if (!pipeline) return false;
    [encoder setRenderPipelineState:pipeline];
    bindUniforms(encoder, lp);
    bindTextures(encoder, lp);
    const uint64_t serial = ctx_.stream().serial();
    // Control block: active, vertex count, slot limit, pad, base[4], stride[4], vertex ids.
    const size_t controlBytes = 12 * 4 + list.size() * 4;
    TransientAllocation control = transient_.allocate(controlBytes, serial, 16);
    auto* words = static_cast<uint32_t*>(control.data);
    std::memset(words, 0, 12 * 4);
    words[0] = 1;
    words[1] = static_cast<uint32_t>(list.size());
    words[2] = limit;
    for (int b = 0; b < bufferCount; ++b) {
        words[4 + b] = baseWords[b];
        words[8 + b] = strides[b];
        [encoder setVertexBuffer:buffers[b] offset:0 atIndex:shader::kSlotFirstXfbBuffer + static_cast<NSUInteger>(b)];
    }
    std::memcpy(words + 12, list.data(), list.size() * 4);
    [encoder setVertexBuffer:control.buffer offset:control.offset atIndex:shader::kSlotXfbControl];
    [encoder drawPrimitives:MTLPrimitiveTypePoint vertexStart:0 vertexCount:list.size()
              instanceCount:static_cast<NSUInteger>(std::max(1, instances)) baseInstance:baseInstance_];
    // Later draws in this pass may read the captured data (as vertex,
    // uniform or texture buffers): Metal does not order work inside one
    // encoder, so order them after these writes.
    [encoder memoryBarrierWithScope:MTLBarrierScopeBuffers
                        afterStages:MTLRenderStageVertex
                       beforeStages:MTLRenderStageVertex | MTLRenderStageFragment];
    ctx_.stream().noteDraw();
    return true;
}

// -------------------------------------------------------- geometry shaders

namespace {

// Lists the vertices of each geometry shader input primitive in gl_in order
// (GL 3.3 section 2.12.1, table 2.4 for triangle strips with adjacency).
std::vector<uint32_t> assembleGeometryInput(GLenum mode, const std::vector<uint32_t>& v) {
    const size_t n = v.size();
    std::vector<uint32_t> out;
    switch (mode) {
    case GL_LINES_ADJACENCY: out.assign(v.begin(), v.begin() + static_cast<long>(n / 4 * 4)); break;
    case GL_LINE_STRIP_ADJACENCY:
        for (size_t i = 0; i + 3 < n; ++i) out.insert(out.end(), {v[i], v[i + 1], v[i + 2], v[i + 3]});
        break;
    case GL_TRIANGLES_ADJACENCY: out.assign(v.begin(), v.begin() + static_cast<long>(n / 6 * 6)); break;
    case GL_TRIANGLE_STRIP_ADJACENCY: {
        if (n < 6) break;
        const size_t count = (n - 4) / 2;
        auto at = [&](size_t oneBased) { return v[oneBased - 1]; };
        for (size_t i = 0; i < count; ++i) {
            // Triangle vertices a, b, c and the vertices adjacent to edges ab, bc, ca (1-based).
            size_t a, b, c, ab, bc, ca;
            const bool odd = i % 2;
            if (count == 1) { a = 1; b = 3; c = 5; ab = 2; bc = 6; ca = 4; }
            else if (i == 0) { a = 1; b = 3; c = 5; ab = 2; bc = 7; ca = 4; }
            else if (i == count - 1 && odd) { a = 2 * i + 3; b = 2 * i + 1; c = 2 * i + 5; ab = 2 * i - 1; bc = 2 * i + 4; ca = 2 * i + 6; }
            else if (i == count - 1) { a = 2 * i + 1; b = 2 * i + 3; c = 2 * i + 5; ab = 2 * i - 1; bc = 2 * i + 6; ca = 2 * i + 4; }
            else if (odd) { a = 2 * i + 3; b = 2 * i + 1; c = 2 * i + 5; ab = 2 * i - 1; bc = 2 * i + 4; ca = 2 * i + 7; }
            else { a = 2 * i + 1; b = 2 * i + 3; c = 2 * i + 5; ab = 2 * i - 1; bc = 2 * i + 7; ca = 2 * i + 4; }
            out.insert(out.end(), {at(a), at(ab), at(b), at(bc), at(c), at(ca)});
        }
        break;
    }
    default: out = assembleForCapture(mode, v); break;
    }
    return out;
}

// The GS input primitive type a draw mode feeds (0: none).
GLenum geometryInputFor(GLenum mode) {
    switch (mode) {
    case GL_POINTS: return GL_POINTS;
    case GL_LINES: case GL_LINE_STRIP: case GL_LINE_LOOP: return GL_LINES;
    case GL_LINES_ADJACENCY: case GL_LINE_STRIP_ADJACENCY: return GL_LINES_ADJACENCY;
    case GL_TRIANGLES: case GL_TRIANGLE_STRIP: case GL_TRIANGLE_FAN: return GL_TRIANGLES;
    case GL_TRIANGLES_ADJACENCY: case GL_TRIANGLE_STRIP_ADJACENCY: return GL_TRIANGLES_ADJACENCY;
    default: return 0;
    }
}

}  // namespace

void DrawState::bindKernelResources(id<MTLComputeCommandEncoder> encoder, LinkedProgram& program,
                                    bool usesDefaultUniforms, int stage, const shader::StageOutput* kernel) {
    const int cs = stage;
    // The compute program, or (geometry and tessellation kernels) the vertex program.
    Program* owner = stageProgram(program, cs);
    if (!owner) owner = stageProgram(program, static_cast<int>(Stage::Vertex));
    if (!owner || !owner->linked) return;
    Program& p = *owner;
    applySubroutineSelections(ctx_, p);
    applyBuiltinUniforms(p);
    const uint32_t size = (p.linked->link.defaultBlockSize + 15) & ~15u;
    if (usesDefaultUniforms && size) {
        if (p.uniformData.size() < size) p.uniformData.resize(size, 0);
        if (size <= 4096) {
            [encoder setBytes:p.uniformData.data() length:size atIndex:shader::kSlotDefaultUniforms];
        } else {
            TransientAllocation alloc = transient_.allocate(size, ctx_.stream().serial(), 256);
            std::memcpy(alloc.data, p.uniformData.data(), size);
            [encoder setBuffer:alloc.buffer offset:alloc.offset atIndex:shader::kSlotDefaultUniforms];
        }
    }
    static id<MTLBuffer> zero = [Device::get().mtl() newBufferWithLength:65536 options:MTLResourceStorageModePrivate];
    for (size_t b = 0; b < program.link.blocks.size(); ++b) {
        const int slot = program.link.blocks[b].stageSlot[cs];
        if (slot < 0) continue;
        id<MTLBuffer> buffer = zero;
        NSUInteger offset = 0;
        uniformBlockBuffer(program, b, buffer, offset);
        [encoder setBuffer:buffer offset:offset atIndex:static_cast<NSUInteger>(slot)];
    }
    for (size_t i = 0; i < program.link.samplers.size(); ++i) {
        const int index = program.link.samplers[i].stageTexture[cs];
        if (index < 0) continue;
        id<MTLTexture> view = nil;
        id<MTLSamplerState> sampler = nil;
        samplerBinding(program, i, view, sampler);
        [encoder setTexture:view atIndex:static_cast<NSUInteger>(index)];
        [encoder setSamplerState:sampler atIndex:static_cast<NSUInteger>(index)];
    }
    for (size_t b = 0; b < program.link.atomicBuffers.size(); ++b) {
        const int slot = program.link.atomicBuffers[b].stageSlot[cs];
        if (slot < 0) continue;
        id<MTLBuffer> buffer = nil;
        NSUInteger offset = 0;
        atomicCounterBuffer(program, b, buffer, offset);
        [encoder setBuffer:buffer offset:offset atIndex:static_cast<NSUInteger>(slot)];
    }
    // The kernel's storage blocks: a compute program's own, or the geometry
    // or tessellation control emulation kernel's (they index the program's).
    if (kernel) bindStorageBlocks(encoder, *kernel, owner);
    for (size_t i = 0; i < program.link.images.size(); ++i) {
        const int index = program.link.images[i].stageTexture[cs];
        if (index >= 0) [encoder setTexture:imageBinding(program, i) atIndex:static_cast<NSUInteger>(index)];
    }
}

void DrawState::dispatch(GLuint x, GLuint y, GLuint z, Buffer* indirect, GLintptr offset) {
    // Conditional rendering applies to dispatches too (KHR-GL43.compute_shader.conditional-dispatching).
    if (!conditionalRenderAllows(ctx_)) return;
    ImageShadowScope shadows(*this);
    LinkedProgram* current = executable();
    if (!current) return;
    LinkedProgram& lp = *current;
    id<MTLComputePipelineState> pipeline = lp.computePipeline();
    if (!pipeline) return;
    if (indirect && indirect->lastWriteSerial) ctx_.stream().waitFor(indirect->lastWriteSerial);
    prepareSamplerStorage(lp);
    id<MTLComputeCommandEncoder> encoder = ctx_.stream().compute();
    [encoder setComputePipelineState:pipeline];
    bindKernelResources(encoder, lp, lp.stage(Stage::Compute).usesDefaultUniforms, static_cast<int>(Stage::Compute),
                        &lp.stage(Stage::Compute));
    const uint32_t* local = lp.link.computeLocalSize;
    const MTLSize threads = MTLSizeMake(local[0], local[1], local[2]);
    if (indirect) {
        indirect->lastUseSerial = ctx_.stream().serial();
        [encoder dispatchThreadgroupsWithIndirectBuffer:indirect->mtl indirectBufferOffset:static_cast<NSUInteger>(offset)
                                  threadsPerThreadgroup:threads];
    } else {
        [encoder dispatchThreadgroups:MTLSizeMake(x, y, z) threadsPerThreadgroup:threads];
    }
}

void DrawState::drawWideLines(GLenum mode, const std::vector<uint32_t>& vertices, GLsizei instances) {
    const size_t n = vertices.size();
    std::vector<std::pair<uint32_t, uint32_t>> segments;
    if (mode == GL_LINES) {
        for (size_t i = 0; i + 1 < n; i += 2) segments.emplace_back(vertices[i], vertices[i + 1]);
    } else {
        for (size_t i = 0; i + 1 < n; ++i) segments.emplace_back(vertices[i], vertices[i + 1]);
        if (mode == GL_LINE_LOOP && n >= 2) segments.emplace_back(vertices[n - 1], vertices[0]);
    }
    queriesCountPrimitives(ctx_, mode, static_cast<GLsizei>(n), instances);
    if (segments.empty()) return;
    // Two triangles per segment, each listing the provoking endpoint first
    // (Metal flat-shades from a triangle's first vertex). The program offsets
    // every vertex by half the width to the side given (fixed_function.mm).
    const bool last = ctx_.state.provokingVertex == GL_LAST_VERTEX_CONVENTION;
    std::vector<uint32_t> list, partners;
    std::vector<float> sides;
    for (const auto& [a, b] : segments) {
        const uint32_t p = last ? b : a, q = last ? a : b;
        list.insert(list.end(), {p, p, q, p, q, q});
        partners.insert(partners.end(), {q, q, p, q, p, p});
        sides.insert(sides.end(), {-1.0f, 1.0f, -1.0f, 1.0f, 1.0f, -1.0f});
    }
    LinkedProgram* current = executable();
    if (!current) return;
    waitForCpuVertexReads(*current, true);
    prepareSamplerStorage(*current);
    // Lines are not culled and ignore the polygon mode; their quads take the line offset state.
    ContextState& st = ctx_.state;
    const bool cull = st.cullFace, offsetFill = st.polygonOffsetFill;
    const GLenum polygonMode = st.polygonMode, polygonModeBack = st.polygonModeBack;
    st.cullFace = false;
    st.polygonMode = st.polygonModeBack = GL_FILL;
    st.polygonOffsetFill = st.polygonOffsetLine;
    ctx_.markDirty(kDirtyRaster);
    MTLPrimitiveType primitive = MTLPrimitiveTypeTriangle;
    if (id<MTLRenderCommandEncoder> encoder = prepare(GL_TRIANGLES, primitive)) {
        queriesBeforeDraw(ctx_, encoder);
        PipelineKey key = lastKey_;
        linePartners_ = &partners;
        lineSides_ = &sides;
        const bool bound = bindVertexInputs(encoder, key, *current, 0, static_cast<GLsizei>(list.size()), instances, &list);
        linePartners_ = nullptr;
        lineSides_ = nullptr;
        if (id<MTLRenderPipelineState> pipeline = bound ? ctx_.share().pipelines().render(key, *current) : nil) {
            stats::count(&stats::Frame::draws);
            [encoder setRenderPipelineState:pipeline];
            bindDrawState(encoder, *current);
            [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:list.size()
                      instanceCount:static_cast<NSUInteger>(std::max(1, instances)) baseInstance:baseInstance_];
            ctx_.stream().noteDraw();
        }
    }
    st.cullFace = cull;
    st.polygonOffsetFill = offsetFill;
    st.polygonMode = polygonMode;
    st.polygonModeBack = polygonModeBack;
    ctx_.markDirty(kDirtyRaster);
}

void DrawState::drawGeometry(GLenum mode, const std::vector<uint32_t>& vertices, GLsizei instances, uint32_t& primitiveBase,
                             bool assembled) {
    LinkedProgram& lp = *executable();
    const shader::GeometryEmulation& g = lp.link.geometry;
    const std::vector<uint32_t> list = assembled ? vertices : assembleGeometryInput(mode, vertices);
    const uint32_t primitives = static_cast<uint32_t>(list.size() / static_cast<size_t>(g.verticesIn));
    if (!primitives) return;
    const uint32_t copies = static_cast<uint32_t>(std::max(1, instances));
    const uint64_t total = static_cast<uint64_t>(primitives) * copies;
    // Kernel threads: one per input primitive and invocation (GS instancing).
    const uint64_t threads = total * static_cast<uint64_t>(std::max(1, g.invocations));
    const uint32_t maxV = static_cast<uint32_t>(g.maxVertices);
    id<MTLComputePipelineState> kernel = lp.geometryKernel();
    if (!kernel || !lp.geometryPassthrough()) return;
    // Anything that flushes must happen before the transient allocations below.
    waitForCpuVertexReads(lp, true);
    prepareSamplerStorage(lp);
    queriesPrepareVisibility(ctx_);
    const uint64_t serial = ctx_.stream().serial();

    // 1. Vertex outputs of every input primitive, in primitive order.
    TransientAllocation input = transient_.allocate(total * g.verticesIn * g.inputStride * 4, serial, 16);
    const id<MTLBuffer> inputBuffers[1] = {input.buffer};
    const uint32_t inputBase[1] = {static_cast<uint32_t>(input.offset / 4)};
    const uint32_t inputStride[1] = {g.inputStride};
    if (!runVertexCapture(lp, list, instances, static_cast<uint32_t>(list.size() * copies), 1, inputBuffers, inputBase,
                          inputStride))
        return;

    // 2. The geometry shader: one thread per input primitive.
    const size_t indexCount = threads * 2 * maxV;
    TransientAllocation output = transient_.allocate(threads * maxV * g.outputStride * 4, serial, 16);
    TransientAllocation indices = transient_.allocate(indexCount * 4, serial, 16);
    id<MTLComputeCommandEncoder> compute = ctx_.stream().compute();
    [compute setComputePipelineState:kernel];
    bindKernelResources(compute, lp, g.kernel.usesDefaultUniforms, static_cast<int>(Stage::Compute), &g.kernel);
    const uint32_t control[4] = {static_cast<uint32_t>(total), primitives, primitiveBase, 0};
    [compute setBuffer:input.buffer offset:input.offset atIndex:shader::kSlotFirstXfbBuffer];
    [compute setBuffer:output.buffer offset:output.offset atIndex:shader::kSlotFirstXfbBuffer + 1];
    [compute setBuffer:indices.buffer offset:indices.offset atIndex:shader::kSlotFirstXfbBuffer + 2];
    [compute setBytes:control length:sizeof(control) atIndex:shader::kSlotFirstXfbBuffer + 3];
    const NSUInteger width = std::min<NSUInteger>(64, kernel.maxTotalThreadsPerThreadgroup);
    [compute dispatchThreads:MTLSizeMake(threads, 1, 1) threadsPerThreadgroup:MTLSizeMake(width, 1, 1)];
    primitiveBase += primitives;

    // Flat-shaded strips with GL's last-vertex convention are drawn as lists
    // whose primitives list their provoking vertex first.
    TransientAllocation lastList;
    size_t lastListCount = 0;
    if (g.outputType != GL_POINTS && !ctx_.state.rasterizerDiscard &&
        ctx_.state.provokingVertex == GL_LAST_VERTEX_CONVENTION && lp.stage(Stage::Fragment).flatInputs) {
        if (id<MTLComputePipelineState> rotate = stripsProvokingLastKernel()) {
            const uint32_t perThread = (g.outputType == GL_LINE_STRIP ? 2u : 3u) * maxV;
            lastListCount = static_cast<size_t>(threads) * perThread;
            lastList = transient_.allocate(lastListCount * 4, serial, 16);
            const uint32_t params[4] = {2 * maxV, perThread, g.outputType == GL_LINE_STRIP ? 0u : 1u,
                                        static_cast<uint32_t>(threads)};
            [compute setComputePipelineState:rotate];
            [compute setBuffer:indices.buffer offset:indices.offset atIndex:0];
            [compute setBuffer:lastList.buffer offset:lastList.offset atIndex:1];
            [compute setBytes:params length:sizeof(params) atIndex:2];
            const NSUInteger rotateWidth = std::min<NSUInteger>(64, rotate.maxTotalThreadsPerThreadgroup);
            [compute dispatchThreads:MTLSizeMake(threads, 1, 1) threadsPerThreadgroup:MTLSizeMake(rotateWidth, 1, 1)];
        }
    }

    // 3. Draw the emitted vertices.
    if (!ctx_.state.rasterizerDiscard) {
        MTLPrimitiveType primitive = MTLPrimitiveTypeTriangle;
        id<MTLRenderCommandEncoder> encoder = prepare(g.outputType, primitive);
        if (encoder) {
            queriesBeforeDraw(ctx_, encoder);
            PipelineKey key = lastKey_;
            key.geometryPass = 1;
            id<MTLRenderPipelineState> pipeline = ctx_.share().pipelines().render(key, lp);
            if (pipeline) {
                [encoder setRenderPipelineState:pipeline];
                bindDrawState(encoder, lp);
                [encoder setVertexBuffer:output.buffer offset:output.offset atIndex:shader::kSlotFirstXfbBuffer];
                if (g.outputType == GL_POINTS)
                    [encoder drawPrimitives:MTLPrimitiveTypePoint vertexStart:0 vertexCount:threads * maxV];
                else if (lastList.buffer)
                    [encoder drawIndexedPrimitives:g.outputType == GL_LINE_STRIP ? MTLPrimitiveTypeLine : MTLPrimitiveTypeTriangle
                                        indexCount:lastListCount
                                         indexType:MTLIndexTypeUInt32
                                       indexBuffer:lastList.buffer
                                 indexBufferOffset:lastList.offset];
                else
                    [encoder drawIndexedPrimitives:g.outputType == GL_LINE_STRIP ? MTLPrimitiveTypeLineStrip
                                                                                  : MTLPrimitiveTypeTriangleStrip
                                        indexCount:indexCount
                                         indexType:MTLIndexTypeUInt32
                                       indexBuffer:indices.buffer
                                 indexBufferOffset:indices.offset];
                ctx_.stream().noteDraw();
            }
        }
    }

    // 4. Primitive queries and transform feedback need the emitted
    // primitives on the CPU, by vertex stream.
    TransformFeedback& xfb = *ctx_.state.transformFeedback;
    const bool capture = xfb.active && !xfb.paused && !lp.link.transformFeedback.empty();
    Query* generated[kMaxVertexStreams] = {};
    Query* writtenQuery[kMaxVertexStreams] = {};
    bool anyQuery = false;
    for (GLuint s = 0; s < static_cast<GLuint>(kMaxVertexStreams); ++s) {
        generated[s] = ctx_.state.activeQueries[queryTargetIndex(ctx_, GL_PRIMITIVES_GENERATED, s)].get();
        writtenQuery[s] = ctx_.state.activeQueries[queryTargetIndex(ctx_, GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN, s)].get();
        anyQuery |= generated[s] || writtenQuery[s];
    }
    if (!capture && !anyQuery) return;
    ctx_.stream().waitFor(serial);
    const auto* slots = static_cast<const uint32_t*>(indices.data);
    const auto* records = static_cast<const uint32_t*>(output.data);
    // Record slots of independent primitives, in order, per stream (only
    // points output has streams other than 0).
    std::vector<uint32_t> emitted[kMaxVertexStreams];
    std::vector<uint32_t> strip;
    for (uint64_t t = 0; t < threads; ++t) {
        for (uint32_t k = 0; k <= 2 * maxV; ++k) {
            const uint32_t slot = k < 2 * maxV ? slots[t * 2 * maxV + k] : 0xffffffffu;
            if (slot != 0xffffffffu) {
                strip.push_back(slot);
                if (g.outputType != GL_POINTS) continue;
            }
            if (strip.empty()) continue;
            if (g.outputType == GL_POINTS) {
                const uint32_t stream = g.streams ? records[static_cast<uint64_t>(strip[0]) * g.outputStride + g.streamOffset] : 0;
                if (stream < static_cast<uint32_t>(kMaxVertexStreams)) emitted[stream].push_back(strip[0]);
            } else {
                const std::vector<uint32_t> assembled =
                    assembleForCapture(g.outputType == GL_LINE_STRIP ? GL_LINE_STRIP : GL_TRIANGLE_STRIP, strip);
                emitted[0].insert(emitted[0].end(), assembled.begin(), assembled.end());
            }
            strip.clear();
        }
    }
    const uint32_t perPrimitive = g.outputType == GL_POINTS ? 1 : g.outputType == GL_LINE_STRIP ? 2 : 3;
    for (int s = 0; s < kMaxVertexStreams; ++s)
        if (generated[s]) generated[s]->result += emitted[s].size() / perPrimitive;
    if (!capture) return;
    const shader::LinkResult& link = lp.link;
    // Primitives each stream writes: what fits in every buffer it feeds;
    // a stream without buffers writes none.
    uint64_t written[kMaxVertexStreams] = {};
    bool captured[kMaxVertexStreams] = {};
    for (int s = 0; s < kMaxVertexStreams; ++s) written[s] = emitted[s].size() / perPrimitive;
    for (int b = 0; b < link.xfbBufferCount; ++b) {
        const TransformFeedback::Binding& binding = xfb.buffers[b];
        if (!binding.buffer || !binding.buffer->mtl) return;
        const int s = link.xfbStream[b];
        captured[s] = true;
        // The bound range, clamped to the buffer's current size (it may have shrunk since binding).
        const int64_t available = std::max<int64_t>(0, binding.buffer->size - binding.offset);
        const int64_t size = binding.size ? std::min<int64_t>(binding.size, available) : available;
        const int64_t remaining = std::max<int64_t>(0, size - xfb.written[b]);
        const uint64_t vertexBytes = static_cast<uint64_t>(link.xfbStride[b]) * 4;
        if (vertexBytes) written[s] = std::min<uint64_t>(written[s], static_cast<uint64_t>(remaining) / vertexBytes / perPrimitive);
    }
    for (int s = 0; s < kMaxVertexStreams; ++s) {
        if (!captured[s]) written[s] = 0;
        if (writtenQuery[s]) writtenQuery[s]->result += written[s];
        xfb.primitivesWritten[static_cast<size_t>(s)] += written[s];
    }
    for (const shader::TransformFeedbackVarying& v : link.transformFeedback) {
        if (v.type == GL_NONE) continue;  // gl_SkipComponents / gl_NextBuffer write nothing
        const int s = link.xfbStream[v.buffer];
        const uint64_t vertexCount = written[s] * perPrimitive;
        Buffer& buffer = *xfb.buffers[v.buffer].buffer;
        uint8_t* base = buffer.contents() + xfb.buffers[v.buffer].offset + xfb.written[v.buffer];
        for (uint64_t i = 0; i < vertexCount; ++i) {
            uint32_t* dst = reinterpret_cast<uint32_t*>(base) + i * link.xfbStride[v.buffer] + v.offset;
            if (v.sourceOffset == shader::TransformFeedbackVarying::kNotEmitted)
                std::memset(dst, 0, static_cast<size_t>(v.components) * 4);
            else
                std::memcpy(dst, records + static_cast<uint64_t>(emitted[s][i]) * g.outputStride + v.sourceOffset,
                            static_cast<size_t>(v.components) * 4);
        }
    }
    for (int b = 0; b < link.xfbBufferCount; ++b) {
        xfb.written[b] += static_cast<GLsizeiptr>(written[link.xfbStream[b]] * perPrimitive * link.xfbStride[b] * 4);
        ++xfb.buffers[b].buffer->version;
    }
}

// ------------------------------------------------------------ tessellation

void DrawState::drawPatches(const std::vector<uint32_t>& vertices, GLsizei instances) {
    LinkedProgram& lp = *executable();
    const shader::TessellationEmulation& t = lp.link.tess;
    std::shared_ptr<LinkedProgram> evaluation = lp.tessEvaluation();
    if (!evaluation) return;
    const uint32_t patchVertices = static_cast<uint32_t>(std::max(1, ctx_.state.patchVertices));
    const size_t patches = vertices.size() / patchVertices;  // an incomplete last patch is ignored
    if (!patches) return;
    const std::vector<uint32_t> list(vertices.begin(), vertices.begin() + static_cast<long>(patches * patchVertices));
    const uint32_t copies = static_cast<uint32_t>(std::max(1, instances));
    const uint64_t total = static_cast<uint64_t>(patches) * copies;  // patch p of instance i is patch i * patches + p
    if (t.hasControl && !lp.tessControlKernel()) return;
    waitForCpuVertexReads(lp, true);
    prepareSamplerStorage(lp);
    queriesPrepareVisibility(ctx_);
    uint64_t serial = ctx_.stream().serial();

    // 1. The vertex outputs of every patch vertex, in patch order.
    TransientAllocation captured = transient_.allocate(total * patchVertices * t.inputStride * 4, serial, 16);
    {
        const id<MTLBuffer> buffers[1] = {captured.buffer};
        const uint32_t base[1] = {static_cast<uint32_t>(captured.offset / 4)};
        const uint32_t stride[1] = {t.inputStride};
        if (!runVertexCapture(lp, list, instances, static_cast<uint32_t>(list.size() * copies), 1, buffers, base, stride)) return;
    }

    // 2. The control shader, one threadgroup per patch; its levels are read
    // back for the tessellator (the patch records with them).
    std::vector<uint32_t> records;  // control: the patch records, copied out of the transient memory
    const uint32_t recordWords = t.recordWords();
    if (t.hasControl) {
        TransientAllocation output = transient_.allocate(total * recordWords * 4, serial, 16);
        id<MTLComputeCommandEncoder> compute = ctx_.stream().compute();
        [compute setComputePipelineState:lp.tessControlKernel()];
        bindKernelResources(compute, lp, t.control.usesDefaultUniforms, static_cast<int>(Stage::TessControl), &t.control);
        const uint32_t control[4] = {patchVertices, static_cast<uint32_t>(total), static_cast<uint32_t>(patches), 0};
        [compute setBuffer:captured.buffer offset:captured.offset atIndex:shader::kSlotFirstXfbBuffer];
        [compute setBuffer:output.buffer offset:output.offset atIndex:shader::kSlotFirstXfbBuffer + 1];
        [compute setBytes:control length:sizeof(control) atIndex:shader::kSlotFirstXfbBuffer + 2];
        [compute dispatchThreadgroups:MTLSizeMake(total, 1, 1)
                threadsPerThreadgroup:MTLSizeMake(static_cast<NSUInteger>(t.verticesOut), 1, 1)];
        ctx_.stream().waitFor(serial);
        const auto* words = static_cast<const uint32_t*>(output.data);
        records.assign(words, words + total * recordWords);
        serial = ctx_.stream().serial();
    }

    // 3. Tessellation: domain points (patch, u, v, w) and the primitive list.
    if (!tessellator_) tessellator_ = std::make_unique<tess::Tessellator>();
    const tess::Mode mode{t.mode, t.spacing, t.order == GL_CW, t.pointMode};
    std::vector<uint32_t> points;  // 4 words per domain point
    std::vector<uint32_t> indices;
    tess::Patch patch;
    float lastOuter[4] = {}, lastInner[2] = {};
    bool haveLast = false;
    for (uint64_t p = 0; p < total; ++p) {
        float outer[4], inner[2];
        if (t.hasControl) {
            const uint32_t* levels = records.data() + p * recordWords + static_cast<uint64_t>(t.verticesOut) * t.vertexWords;
            std::memcpy(outer, levels, sizeof(outer));
            std::memcpy(inner, levels + 4, sizeof(inner));
        } else {
            std::memcpy(outer, ctx_.state.patchDefaultOuterLevel, sizeof(outer));
            std::memcpy(inner, ctx_.state.patchDefaultInnerLevel, sizeof(inner));
        }
        // Patches with the same levels tessellate the same way.
        if (!haveLast || std::memcmp(outer, lastOuter, sizeof(outer)) != 0 || std::memcmp(inner, lastInner, sizeof(inner)) != 0) {
            tessellator_->tessellate(mode, outer, inner, patch);
            std::memcpy(lastOuter, outer, sizeof(outer));
            std::memcpy(lastInner, inner, sizeof(inner));
            haveLast = true;
        }
        if (patch.indices.empty()) continue;
        const uint32_t base = static_cast<uint32_t>(points.size() / 4);
        for (size_t i = 0; i < patch.u.size(); ++i) {
            const float u = patch.u[i], v = patch.v[i], w = t.mode == GL_TRIANGLES ? 1.0f - u - v : 0.0f;
            uint32_t word[4] = {static_cast<uint32_t>(p), 0, 0, 0};
            std::memcpy(&word[1], &u, 4);
            std::memcpy(&word[2], &v, 4);
            std::memcpy(&word[3], &w, 4);
            points.insert(points.end(), word, word + 4);
        }
        for (uint32_t index : patch.indices) indices.push_back(base + index);
    }
    const GLenum listMode = t.pointMode ? GL_POINTS : t.mode == GL_ISOLINES ? GL_LINES : GL_TRIANGLES;
    if (indices.empty()) return;

    // 4. The evaluation stage over the domain points, drawn (or captured, or
    // fed to the geometry shader) as independent primitives.
    TransientAllocation pointData = transient_.allocate(points.size() * 4, serial, 16);
    std::memcpy(pointData.data, points.data(), points.size() * 4);
    TransientAllocation patchData;
    if (t.hasControl) {
        patchData = transient_.allocate(records.size() * 4, serial, 16);
        std::memcpy(patchData.data, records.data(), records.size() * 4);
    } else {
        patchData = captured;  // read in this command buffer
    }
    struct {
        uint32_t patchVertices, patchesPerInstance, pad[2];
        float outer[4], inner[2], pad2[2];
    } control = {patchVertices, static_cast<uint32_t>(patches), {0, 0}, {}, {}, {}};
    std::memcpy(control.outer, ctx_.state.patchDefaultOuterLevel, sizeof(control.outer));
    std::memcpy(control.inner, ctx_.state.patchDefaultInnerLevel, sizeof(control.inner));
    TransientAllocation controlData = transient_.allocate(sizeof(control), serial, 16);
    std::memcpy(controlData.data, &control, sizeof(control));
    tessInputs_.active = true;
    tessInputs_.buffers[0] = pointData.buffer;
    tessInputs_.offsets[0] = pointData.offset;
    tessInputs_.buffers[1] = patchData.buffer;
    tessInputs_.offsets[1] = patchData.offset;
    tessInputs_.buffers[2] = controlData.buffer;
    tessInputs_.offsets[2] = controlData.offset;
    override_ = evaluation.get();
    if (evaluation->link.geometry.active) {
        uint32_t primitiveBase = 0;
        drawGeometry(listMode, indices, 1, primitiveBase);
    } else {
        if (ctx_.state.transformFeedback->active) captureTransformFeedback(listMode, indices, 1);
        issue(listMode, 0, static_cast<GLsizei>(indices.size()), 1, indices.data(), nil, 0, MTLIndexTypeUInt32, 0, 0,
              static_cast<GLsizei>(points.size() / 4));
    }
    override_ = nullptr;
    tessInputs_ = {};
}

void DrawState::drawArrays(GLenum mode, GLint first, GLsizei count, GLsizei instances) {
    ImageShadowScope shadows(*this);
    beginDraw(mode);
    if (first < 0 || count < 0 || instances < 0) {
        ctx_.setError(GL_INVALID_VALUE);
        return;
    }
    if (!validateDraw(mode)) return;
    if (count == 0 || instances == 0 || !conditionalRenderAllows(ctx_)) return;
    if (executable() && ffWideLines_) {
        std::vector<uint32_t> vertices(static_cast<size_t>(count));
        for (GLsizei i = 0; i < count; ++i) vertices[static_cast<size_t>(i)] = static_cast<uint32_t>(first + i);
        drawWideLines(mode, vertices, instances);
        return;
    }
    if (mode == GL_PATCHES) {
        std::vector<uint32_t> vertices(static_cast<size_t>(count));
        for (GLsizei i = 0; i < count; ++i) vertices[static_cast<size_t>(i)] = static_cast<uint32_t>(first + i);
        drawPatches(vertices, instances);
        return;
    }
    if (executable()->link.geometry.active) {
        std::vector<uint32_t> vertices(static_cast<size_t>(count));
        for (GLsizei i = 0; i < count; ++i) vertices[static_cast<size_t>(i)] = static_cast<uint32_t>(first + i);
        uint32_t primitiveBase = 0;
        drawGeometry(mode, vertices, instances, primitiveBase);
        return;
    }
    if (ctx_.state.transformFeedback->active) {
        std::vector<uint32_t> vertices(static_cast<size_t>(count));
        for (GLsizei i = 0; i < count; ++i) vertices[static_cast<size_t>(i)] = static_cast<uint32_t>(first + i);
        captureTransformFeedback(mode, vertices, instances);
    }
    issue(mode, first, count, instances, nullptr, nil, 0, MTLIndexTypeUInt16, 0, first, count);
}

void DrawState::drawElements(GLenum mode, GLsizei count, GLenum type, const void* indices, GLsizei instances,
                             GLint baseVertex) {
    ImageShadowScope shadows(*this);
    beginDraw(mode);
    if (count < 0 || instances < 0) {
        ctx_.setError(GL_INVALID_VALUE);
        return;
    }
    if (type != GL_UNSIGNED_BYTE && type != GL_UNSIGNED_SHORT && type != GL_UNSIGNED_INT) {
        ctx_.setError(GL_INVALID_ENUM);
        return;
    }
    if (!validateDraw(mode)) return;
    if (count == 0 || instances == 0 || !conditionalRenderAllows(ctx_)) return;
    Buffer* elementBuffer = ctx_.state.vertexArray->elementBuffer.get();
    if (elementBuffer && elementBuffer->mappedNonPersistent()) {
        ctx_.setError(GL_INVALID_OPERATION);
        return;
    }
    const size_t indexSize = type == GL_UNSIGNED_BYTE ? 1 : type == GL_UNSIGNED_SHORT ? 2 : 4;
    const uint8_t* source;
    const uintptr_t offset = reinterpret_cast<uintptr_t>(indices);
    if (elementBuffer) {
        if (!elementBuffer->mtl || offset + static_cast<size_t>(count) * indexSize > static_cast<size_t>(elementBuffer->size)) {
            // Out-of-range index fetches are undefined; draw nothing rather than read past the buffer.
            return;
        }
        source = elementBuffer->contents() + offset;  // read on the CPU only through cpuIndices()
    } else {
        // Client-memory indices: deprecated in core profiles but accepted, as
        // other desktop drivers do (the KHR-GL33 transform feedback tests use them).
        if (!indices) {
            ctx_.setError(GL_INVALID_OPERATION);
            return;
        }
        source = static_cast<const uint8_t*>(indices);
    }
    const ContextState& st = ctx_.state;
    const uint32_t restartIndex = st.primitiveRestartFixedIndex
                                      ? (type == GL_UNSIGNED_BYTE ? 0xffu : type == GL_UNSIGNED_SHORT ? 0xffffu : 0xffffffffu)
                                      : st.primitiveRestartIndex;
    const bool restart = st.primitiveRestart || st.primitiveRestartFixedIndex;
    // CPU reads of buffer indices wait for GPU writes to the buffer first.
    bool cpuReadable = !elementBuffer;
    auto cpuSource = [&] {
        if (!cpuReadable) {
            if (elementBuffer->lastWriteSerial) ctx_.stream().waitFor(elementBuffer->lastWriteSerial);
            cpuReadable = true;
        }
        return source;
    };
    auto index = [&](const uint8_t* p, GLsizei i) -> uint32_t {
        if (indexSize == 1) return p[i];
        if (indexSize == 2) { uint16_t x; std::memcpy(&x, p + i * 2, 2); return x; }
        uint32_t x;
        std::memcpy(&x, p + i * 4, 4);
        return x;
    };
    // The used vertex range (attribute conversion, renumbering) and restart
    // use: scanned on the CPU, cached per index range of an element buffer
    // until its contents change (not for persistently mapped buffers, which
    // the application writes behind GL's back).
    uint32_t minIndex = UINT32_MAX, maxIndex = 0;
    bool hasRestart = false;
    Buffer::IndexScan* cached = nullptr;
    if (elementBuffer && !elementBuffer->mapped) {
        for (Buffer::IndexScan& scan : elementBuffer->indexScans)
            if (scan.version == elementBuffer->version && scan.offset == offset && scan.count == count &&
                scan.type == type && scan.restart == restart && (!restart || scan.restartIndex == restartIndex))
                cached = &scan;
    }
    if (cached) {
        minIndex = cached->minIndex;
        maxIndex = cached->maxIndex;
        hasRestart = cached->hasRestart;
    } else {
        const uint8_t* p = cpuSource();
        for (GLsizei i = 0; i < count; ++i) {
            const uint32_t v = index(p, i);
            if (restart && v == restartIndex) { hasRestart = true; continue; }
            minIndex = std::min(minIndex, v);
            maxIndex = std::max(maxIndex, v);
        }
        if (elementBuffer && !elementBuffer->mapped) {
            std::vector<Buffer::IndexScan>& scans = elementBuffer->indexScans;
            if (!scans.empty() && scans.front().version != elementBuffer->version) scans.clear();
            if (scans.size() >= Buffer::kMaxIndexScans) scans.erase(scans.begin());
            scans.push_back({elementBuffer->version, offset, count, type, restartIndex, restart, minIndex, maxIndex, hasRestart});
        }
    }
    if (minIndex == UINT32_MAX) return;  // only restart indices
    // The indices themselves, for the paths that rewrite them on the CPU.
    std::vector<uint32_t> values;
    auto cpuIndices = [&]() -> const std::vector<uint32_t>& {
        if (values.empty()) {
            const uint8_t* p = cpuSource();
            values.resize(static_cast<size_t>(count));
            for (GLsizei i = 0; i < count; ++i) values[static_cast<size_t>(i)] = index(p, i);
        }
        return values;
    };
    // Restart-delimited segments are combined into one list of independent
    // primitives, so every instance draws all of them before the next
    // instance starts (GL draws instance by instance).
    auto forEachSegment = [&](uint32_t bias, auto&& fn) {
        const std::vector<uint32_t>& values = cpuIndices();
        std::vector<uint32_t> segment;
        for (size_t i = 0; i <= values.size(); ++i) {
            if (i < values.size() && !(restart && values[i] == restartIndex)) {
                segment.push_back(values[i] + bias);
                continue;
            }
            if (!segment.empty()) fn(segment);
            segment.clear();
        }
    };
    const uint32_t bias = static_cast<uint32_t>(baseVertex);
    if (ffWideLines_) {
        std::vector<uint32_t> lines;
        forEachSegment(bias, [&](const std::vector<uint32_t>& segment) {
            const std::vector<uint32_t> part = assembleForCapture(mode, segment);
            lines.insert(lines.end(), part.begin(), part.end());
        });
        drawWideLines(GL_LINES, lines, instances);
        return;
    }
    if (mode == GL_PATCHES) {
        // Primitive restart does not apply to patches
        // (PRIMITIVE_RESTART_FOR_PATCHES_SUPPORTED is false).
        std::vector<uint32_t> list = cpuIndices();
        for (uint32_t& v : list) v += bias;
        drawPatches(list, instances);
        return;
    }
    if (executable()->link.geometry.active) {
        // gl_PrimitiveIDIn keeps counting across restarts.
        uint32_t primitiveBase = 0;
        std::vector<uint32_t> assembled;
        forEachSegment(bias, [&](const std::vector<uint32_t>& segment) {
            const std::vector<uint32_t> part = assembleGeometryInput(mode, segment);
            assembled.insert(assembled.end(), part.begin(), part.end());
        });
        drawGeometry(mode, assembled, instances, primitiveBase, true);
        return;
    }
    if (ctx_.state.transformFeedback->active) {
        // gl_VertexID includes basevertex.
        std::vector<uint32_t> captured;
        forEachSegment(bias, [&](const std::vector<uint32_t>& segment) {
            const std::vector<uint32_t> part = assembleForCapture(mode, segment);
            captured.insert(captured.end(), part.begin(), part.end());
        });
        if (!captured.empty()) captureTransformFeedback(listMode(mode), captured, instances);
    }
    const GLint spanFirst = static_cast<GLint>(minIndex) + baseVertex;
    const GLsizei span = static_cast<GLsizei>(std::min<uint64_t>(uint64_t{maxIndex} - minIndex + 1, INT32_MAX));
    bool needsIndices = false;
    nativePrimitive(mode, needsIndices);
    const bool rotate = rotateProvoking(mode, *executable());
    if (hasRestart) {
        std::vector<uint32_t> list;
        forEachSegment(0, [&](const std::vector<uint32_t>& segment) { appendMetalList(mode, segment, rotate, list); });
        if (!list.empty())
            issue(listMode(mode), 0, static_cast<GLsizei>(list.size()), instances, list.data(), nil, 0, MTLIndexTypeUInt32,
                  baseVertex, spanFirst, span, true);
        return;
    }
    // Metal always restarts at the type's largest index; GL only when asked.
    const uint32_t metalRestart = indexSize == 2 ? 0xffffu : 0xffffffffu;
    if (elementBuffer && !needsIndices && !rotate && !pointPolygons(mode) && indexSize != 1 && offset % indexSize == 0 &&
        maxIndex != metalRestart) {
        elementBuffer->lastUseSerial = ctx_.stream().serial();
        issue(mode, 0, count, instances, nullptr, elementBuffer->mtl, offset,
              indexSize == 2 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32, baseVertex, spanFirst, span);
        return;
    }
    issue(mode, 0, count, instances, cpuIndices().data(), nil, 0, MTLIndexTypeUInt32, baseVertex, spanFirst, span);
}

// ----------------------------------------------------------------- clears

void DrawState::clear(GLbitfield mask) {
    if (mask & ~(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT | (ctx_.isCompat() ? GL_ACCUM_BUFFER_BIT : 0u))) {
        ctx_.setError(GL_INVALID_VALUE);
        return;
    }
    const ContextState& st = ctx_.state;
    if (st.rasterizerDiscard) return;
    RenderTargetDesc target;
    TargetFormats formats;
    if (!drawTarget(target, formats)) return;
    if (!conditionalRenderAllows(ctx_)) return;
    PendingClear clear;
    bool fullMasks = true;
    if (mask & GL_COLOR_BUFFER_BIT) {
        for (int i = 0; i < 8; ++i) {
            // GL leaves Clear of integer buffers undefined; like other
            // drivers, the float clear colour is converted to the integer type.
            if (!target.color[i].texture) continue;
            clear.colorMask |= 1u << i;
            double c[4] = {st.clearColor[0], st.clearColor[1], st.clearColor[2], formats.alphaIsOne[i] ? 1.0 : st.clearColor[3]};
            if (formats.integer[i] && st.clearColorIntegerSet)  // GL_EXT_texture_integer
                for (int k = 0; k < 4; ++k) c[k] = st.clearColorInteger[k];
            clear.color[i] = MTLClearColorMake(c[0], c[1], c[2], c[3]);
            const auto& m = st.colorMask[i];
            if (!m[0] || !m[1] || !m[2] || (!m[3] && !formats.alphaIsOne[i])) fullMasks = false;
        }
    }
    if ((mask & GL_DEPTH_BUFFER_BIT) && formats.depthAttached) {
        clear.depth = true;
        clear.depthValue = std::clamp(st.clearDepth, 0.0, 1.0);
        if (!st.depthMask) fullMasks = false;
    }
    if ((mask & GL_STENCIL_BUFFER_BIT) && formats.stencilAttached) {
        clear.stencil = true;
        clear.stencilValue = static_cast<uint32_t>(st.clearStencil) & 0xff;
        if ((st.stencilFront.writeMask & 0xff) != 0xff) fullMasks = false;
    }
    if (!clear.any()) return;
    bool fullRect = true;
    if ((st.scissorTests & 1u)) {
        fullRect = st.scissors[0][0] <= 0 && st.scissors[0][1] <= 0 &&
                   static_cast<int64_t>(st.scissors[0][0]) + st.scissors[0][2] >= static_cast<int64_t>(target.width) &&
                   static_cast<int64_t>(st.scissors[0][1]) + st.scissors[0][3] >= static_cast<int64_t>(target.height);
    }
    if (fullMasks && fullRect && ctx_.stream().clearWithLoadAction(target, clear)) return;
    clearWithDraw(target, formats, clear);
}

namespace {

// Clear pipelines: the fragment shader's outputs are typed per attachment
// (float, int or uint) to match the pass; values arrive as raw 32-bit words.
// `layered`: one instance per layer of a layered framebuffer.
id<MTLRenderPipelineState> clearPipeline(const TargetFormats& formats, const uint8_t writeMasks[8], uint32_t samples,
                                         const char signature[9], bool layered) {
    static std::mutex mutex;
    static std::unordered_map<std::string, id<MTLRenderPipelineState>> cache;
    std::string key(signature, 8);
    key += layered ? "L" : "-";
    for (int i = 0; i < 8; ++i) key += std::to_string(formats.color[i]) + ":" + std::to_string(writeMasks[i]) + ",";
    key += std::to_string(formats.depth) + "," + std::to_string(formats.stencil) + "," + std::to_string(samples);
    std::lock_guard<std::mutex> lock(mutex);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    std::string source = layered ? "#include <metal_stdlib>\nusing namespace metal;\n"
                                   "struct V { float4 position [[position]]; uint layer [[render_target_array_index]]; };\n"
                                   "vertex V clear_vs(uint vid [[vertex_id]], uint iid [[instance_id]], constant float4& depth [[buffer(0)]]) {\n"
                                   "  float2 p = float2((vid << 1) & 2, vid & 2); V out; out.position = float4(p * 2.0 - 1.0, depth.x, 1.0);\n"
                                   "  out.layer = iid; return out; }\n"
                                 : "#include <metal_stdlib>\nusing namespace metal;\n"
                                   "struct V { float4 position [[position]]; };\n"
                                   "vertex V clear_vs(uint vid [[vertex_id]], constant float4& depth [[buffer(0)]]) {\n"
                                   "  float2 p = float2((vid << 1) & 2, vid & 2); V out; out.position = float4(p * 2.0 - 1.0, depth.x, 1.0); return out; }\n";
    std::string fields, body;
    for (int i = 0; i < 8; ++i) {
        if (signature[i] == '-') continue;
        const char* type = signature[i] == 'i' ? "int4" : signature[i] == 'u' ? "uint4" : "float4";
        fields += std::string(type) + " c" + std::to_string(i) + " [[color(" + std::to_string(i) + ")]]; ";
        body += "o.c" + std::to_string(i) + " = as_type<" + type + ">(v[" + std::to_string(i) + "]); ";
    }
    if (fields.empty()) {
        source += "fragment void clear_fs() {}\n";
    } else {
        source += "struct O { " + fields + "};\n"
                  "fragment O clear_fs(constant uint4* v [[buffer(0)]]) { O o; " + body + "return o; }\n";
    }
    NSError* error = nil;
    MTLCompileOptions* options = [MTLCompileOptions new];
    options.languageVersion = MTLLanguageVersion3_0;
    id<MTLLibrary> library = [Device::get().mtl() newLibraryWithSource:[NSString stringWithUTF8String:source.c_str()]
                                                               options:options
                                                                 error:&error];
    if (!library) {
        WMGL_ERR("clear shader: %s", error.localizedDescription.UTF8String);
        return cache[key] = nil;
    }
    MTLRenderPipelineDescriptor* desc = [MTLRenderPipelineDescriptor new];
    desc.vertexFunction = [library newFunctionWithName:@"clear_vs"];
    desc.fragmentFunction = [library newFunctionWithName:@"clear_fs"];
    desc.rasterSampleCount = samples;
    if (layered) desc.inputPrimitiveTopology = MTLPrimitiveTopologyClassTriangle;
    for (int i = 0; i < 8; ++i) {
        desc.colorAttachments[i].pixelFormat = formats.color[i];
        desc.colorAttachments[i].writeMask = writeMasks[i];
    }
    desc.depthAttachmentPixelFormat = formats.depth;
    desc.stencilAttachmentPixelFormat = formats.stencil;
    id<MTLRenderPipelineState> pipeline = [Device::get().mtl() newRenderPipelineStateWithDescriptor:desc error:&error];
    if (!pipeline) WMGL_ERR("clear pipeline: %s", error.localizedDescription.UTF8String);
    return cache[key] = pipeline;
}

}  // namespace

void DrawState::clearWithDraw(const RenderTargetDesc& target, const TargetFormats& formats, const PendingClear& clear) {
    const ContextState& st = ctx_.state;
    MTLScissorRect scissor = {0, 0, target.width, target.height};
    if ((st.scissorTests & 1u)) {
        const int64_t x0 = std::clamp<int64_t>(st.scissors[0][0], 0, target.width);
        const int64_t y0 = std::clamp<int64_t>(st.scissors[0][1], 0, target.height);
        const int64_t x1 = std::clamp<int64_t>(static_cast<int64_t>(st.scissors[0][0]) + std::max(0, st.scissors[0][2]), 0, target.width);
        const int64_t y1 = std::clamp<int64_t>(static_cast<int64_t>(st.scissors[0][1]) + std::max(0, st.scissors[0][3]), 0, target.height);
        if (x1 <= x0 || y1 <= y0) return;
        scissor = {static_cast<NSUInteger>(x0), static_cast<NSUInteger>(y0), static_cast<NSUInteger>(x1 - x0),
                   static_cast<NSUInteger>(y1 - y0)};
    }
    // Per-attachment output type, write mask and raw clear value.
    char signature[9] = "--------";
    uint8_t writeMasks[8] = {};
    uint32_t values[8][4] = {};
    for (int i = 0; i < 8; ++i) {
        if (!formats.color[i]) continue;
        const MTLPixelFormat f = formats.color[i];
        const bool sint = f == MTLPixelFormatR8Sint || f == MTLPixelFormatR16Sint || f == MTLPixelFormatR32Sint ||
                          f == MTLPixelFormatRG8Sint || f == MTLPixelFormatRG16Sint || f == MTLPixelFormatRG32Sint ||
                          f == MTLPixelFormatRGBA8Sint || f == MTLPixelFormatRGBA16Sint || f == MTLPixelFormatRGBA32Sint;
        signature[i] = formats.integer[i] ? (sint ? 'i' : 'u') : 'f';
        const MTLClearColor& c = clear.color[i];
        const double v[4] = {c.red, c.green, c.blue, c.alpha};
        for (int k = 0; k < 4; ++k) {
            if (signature[i] == 'f') {
                const float x = static_cast<float>(v[k]);
                std::memcpy(&values[i][k], &x, 4);
            } else if (signature[i] == 'i') {
                const int32_t x = static_cast<int32_t>(v[k]);
                std::memcpy(&values[i][k], &x, 4);
            } else {
                values[i][k] = static_cast<uint32_t>(v[k]);
            }
        }
        const auto& m = st.colorMask[i];
        const bool cleared = clear.colorMask & (1u << i);
        writeMasks[i] = !cleared ? MTLColorWriteMaskNone
                                 : static_cast<uint8_t>((m[0] ? MTLColorWriteMaskRed : 0) | (m[1] ? MTLColorWriteMaskGreen : 0) |
                                                        (m[2] ? MTLColorWriteMaskBlue : 0) |
                                                        (m[3] && !formats.alphaIsOne[i] ? MTLColorWriteMaskAlpha : 0));
    }
    id<MTLRenderPipelineState> pipeline = clearPipeline(formats, writeMasks, target.samples, signature, target.layers > 0);
    if (!pipeline) return;
    id<MTLRenderCommandEncoder> encoder = ctx_.stream().render(target);
    queriesSuspendVisibility(ctx_, encoder);  // clears do not count towards occlusion queries
    DepthStencilKey ds;
    std::memset(&ds, 0, sizeof(ds));
    ds.depthCompare = static_cast<uint8_t>(MTLCompareFunctionAlways);
    ds.depthWrite = clear.depth && st.depthMask;
    ds.stencilEnabled = clear.stencil;
    if (clear.stencil) {
        for (DepthStencilKey::Face* f : {&ds.front, &ds.back}) {
            f->compare = static_cast<uint8_t>(MTLCompareFunctionAlways);
            f->fail = f->depthFail = f->pass = static_cast<uint8_t>(MTLStencilOperationReplace);
            f->readMask = 0xff;
            f->writeMask = st.stencilFront.writeMask & 0xff;
        }
    }
    [encoder setRenderPipelineState:pipeline];
    [encoder setDepthStencilState:ctx_.share().pipelines().depthStencil(ds)];
    MTLViewport viewport = {0, 0, static_cast<double>(target.width), static_cast<double>(target.height), 0, 1};
    [encoder setViewport:viewport];
    [encoder setScissorRect:scissor];
    [encoder setCullMode:MTLCullModeNone];
    [encoder setTriangleFillMode:MTLTriangleFillModeFill];
    [encoder setDepthClipMode:MTLDepthClipModeClamp];
    [encoder setDepthBias:0.0f slopeScale:0.0f clamp:0.0f];  // the encoder may carry a draw's polygon offset
    [encoder setStencilReferenceValue:clear.stencilValue];
    const float depth[4] = {static_cast<float>(clear.depthValue), 0, 0, 0};
    [encoder setVertexBytes:depth length:sizeof(depth) atIndex:0];
    [encoder setFragmentBytes:values length:sizeof(values) atIndex:0];
    // A layered framebuffer clears every layer (GL 4.x section 9.8).
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3 instanceCount:std::max(1u, target.layers)];
    ctx_.stream().noteDraw();
}

void DrawState::clearBuffer(GLenum buffer, GLint drawbuffer, const double color[4], double depthValue,
                            GLint stencilValue) {
    const ContextState& st = ctx_.state;
    RenderTargetDesc target;
    TargetFormats formats;
    if (!drawTarget(target, formats)) return;
    if (st.rasterizerDiscard || !conditionalRenderAllows(ctx_)) return;
    PendingClear clear;
    bool fullMasks = true;
    if (buffer == GL_COLOR) {
        if (!target.color[drawbuffer].texture) return;
        clear.colorMask = static_cast<uint8_t>(1u << drawbuffer);
        clear.color[drawbuffer] = MTLClearColorMake(color[0], color[1], color[2], formats.alphaIsOne[drawbuffer] ? 1.0 : color[3]);
        const auto& m = st.colorMask[drawbuffer];
        if (!m[0] || !m[1] || !m[2] || (!m[3] && !formats.alphaIsOne[drawbuffer])) fullMasks = false;
    }
    if ((buffer == GL_DEPTH || buffer == GL_DEPTH_STENCIL) && formats.depthAttached) {
        clear.depth = true;
        clear.depthValue = std::clamp(depthValue, 0.0, 1.0);
        if (!st.depthMask) fullMasks = false;
    }
    if ((buffer == GL_STENCIL || buffer == GL_DEPTH_STENCIL) && formats.stencilAttached) {
        clear.stencil = true;
        clear.stencilValue = static_cast<uint32_t>(stencilValue) & 0xff;
        if ((st.stencilFront.writeMask & 0xff) != 0xff) fullMasks = false;
    }
    if (!clear.any()) return;
    bool fullRect = true;
    if ((st.scissorTests & 1u))
        fullRect = st.scissors[0][0] <= 0 && st.scissors[0][1] <= 0 &&
                   static_cast<int64_t>(st.scissors[0][0]) + st.scissors[0][2] >= static_cast<int64_t>(target.width) &&
                   static_cast<int64_t>(st.scissors[0][1]) + st.scissors[0][3] >= static_cast<int64_t>(target.height);
    if (fullMasks && fullRect && ctx_.stream().clearWithLoadAction(target, clear)) return;
    clearWithDraw(target, formats, clear);
}

}  // namespace wmgl
