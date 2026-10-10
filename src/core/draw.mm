#include "core/draw.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

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
        if (!image.defined() || a.level >= tex->mtlLevels) return false;
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
        out.slice = tex->target == GL_TEXTURE_CUBE_MAP ? static_cast<uint32_t>(face) : static_cast<uint32_t>(a.layer);
        width = static_cast<uint32_t>(image.width);
        height = static_cast<uint32_t>(image.height);
        samples = std::max(1, image.samples);
        format = out.texture.pixelFormat;
        info = image.format;
        return true;
    }
    return false;
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
        // GL_NONE / GL_FRONT_AND_BACK draw buffer selection on the default framebuffer.
        if (ctx_.state.defaultDrawBuffer == GL_NONE) {
            target.color[0] = {};
            formats.color[0] = MTLPixelFormatInvalid;
        }
        return true;
    }
    uint32_t width = UINT32_MAX, height = UINT32_MAX, samples = 0;
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

bool DrawState::validateDraw(GLenum mode) {
    switch (mode) {
    case GL_POINTS: case GL_LINES: case GL_LINE_LOOP: case GL_LINE_STRIP: case GL_TRIANGLES: case GL_TRIANGLE_STRIP:
    case GL_TRIANGLE_FAN: case GL_LINES_ADJACENCY: case GL_LINE_STRIP_ADJACENCY: case GL_TRIANGLES_ADJACENCY:
    case GL_TRIANGLE_STRIP_ADJACENCY:
        break;
    case GL_QUADS: case GL_QUAD_STRIP: case GL_POLYGON:
        if (ctx_.isCompat()) break;
        [[fallthrough]];
    default:
        ctx_.setError(GL_INVALID_ENUM);
        return false;
    }
    Program* program = ctx_.state.program.get();
    if (!program || !program->linked) {
        if (ctx_.isCore() || !program) {
            // Without a program, core profile draws are undefined but not an error;
            // the compatibility profile needs fixed-function emulation (W22).
            if (program && !program->linked) ctx_.setError(GL_INVALID_OPERATION);
            return false;
        }
    }
    if (ctx_.isCore() && ctx_.state.vertexArray == ctx_.defaultVertexArray) {
        ctx_.setError(GL_INVALID_OPERATION);
        return false;
    }
    // Mapped buffers may not be used by draws.
    for (const VertexAttrib& a : ctx_.state.vertexArray->attribs)
        if (a.enabled && a.buffer && a.buffer->mapped) {
            ctx_.setError(GL_INVALID_OPERATION);
            return false;
        }
    // Active transform feedback restricts the draw mode (GL 3.3 section 2.15.2).
    const TransformFeedback& xfb = *ctx_.state.transformFeedback;
    if (xfb.active && !xfb.paused) {
        GLenum family = 0;
        switch (mode) {
        case GL_POINTS: family = GL_POINTS; break;
        case GL_LINES: case GL_LINE_LOOP: case GL_LINE_STRIP: family = GL_LINES; break;
        case GL_TRIANGLES: case GL_TRIANGLE_STRIP: case GL_TRIANGLE_FAN: family = GL_TRIANGLES; break;
        default: break;
        }
        LinkedProgram* lp = program ? program->linked.get() : nullptr;
        const bool geometry = lp && lp->hasStage(Stage::Geometry);
        if (!geometry && family != xfb.primitiveMode) {
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

// Reads one component of a GL vertex attribute element as float.
float readComponent(const uint8_t* p, GLenum type, bool normalized, int index) {
    switch (type) {
    case GL_BYTE: {
        const int8_t v = reinterpret_cast<const int8_t*>(p)[index];
        return normalized ? std::max(-1.0f, v / 127.0f) : static_cast<float>(v);
    }
    case GL_UNSIGNED_BYTE: {
        const uint8_t v = p[index];
        return normalized ? v / 255.0f : static_cast<float>(v);
    }
    case GL_SHORT: {
        int16_t v;
        std::memcpy(&v, p + index * 2, 2);
        return normalized ? std::max(-1.0f, v / 32767.0f) : static_cast<float>(v);
    }
    case GL_UNSIGNED_SHORT: {
        uint16_t v;
        std::memcpy(&v, p + index * 2, 2);
        return normalized ? v / 65535.0f : static_cast<float>(v);
    }
    case GL_INT: {
        int32_t v;
        std::memcpy(&v, p + index * 4, 4);
        return normalized ? std::max(-1.0f, static_cast<float>(v / 2147483647.0)) : static_cast<float>(v);
    }
    case GL_UNSIGNED_INT: {
        uint32_t v;
        std::memcpy(&v, p + index * 4, 4);
        return normalized ? static_cast<float>(v / 4294967295.0) : static_cast<float>(v);
    }
    case GL_FIXED: {
        int32_t v;
        std::memcpy(&v, p + index * 4, 4);
        return v / 65536.0f;
    }
    case GL_DOUBLE: {
        double v;
        std::memcpy(&v, p + index * 8, 8);
        return static_cast<float>(v);
    }
    case GL_FLOAT: {
        float v;
        std::memcpy(&v, p + index * 4, 4);
        return v;
    }
    default: return 0.0f;
    }
}

uint32_t typeBytes(GLenum type) {
    switch (type) {
    case GL_BYTE: case GL_UNSIGNED_BYTE: return 1;
    case GL_SHORT: case GL_UNSIGNED_SHORT: case GL_HALF_FLOAT: return 2;
    case GL_DOUBLE: return 8;
    default: return 4;
    }
}

}  // namespace


void DrawState::waitForCpuVertexReads(LinkedProgram& program, bool gather) {
    // Waiting flushes the command buffer, so it must happen before a draw opens its encoder.
    Queue& queue = ctx_.stream().queue();
    const VertexArray& vao = *ctx_.state.vertexArray;
    for (const shader::StageOutput::VertexInput& input : program.stage(Stage::Vertex).vertexInputs) {
        if (input.location < 0 || input.location >= VertexArray::kMaxAttribs) continue;
        const VertexAttrib& a = vao.attribs[input.location];
        if (!a.enabled || !a.buffer || !a.buffer->lastWriteSerial || queue.isComplete(a.buffer->lastWriteSerial)) continue;
        const MTLVertexFormat direct = a.isDouble ? MTLVertexFormatInvalid : mtlVertexFormat(a.type, a.size, a.normalized, a.integer);
        const uint32_t stride = static_cast<uint32_t>(a.effectiveStride());
        const bool cpuRead = (gather && !a.divisor) || direct == MTLVertexFormatInvalid || a.offset % 4 || stride % 4 || !stride;
        if (cpuRead) ctx_.stream().waitFor(a.buffer->lastWriteSerial);
    }
}

bool DrawState::bindVertexInputs(id<MTLRenderCommandEncoder> encoder, PipelineKey& key, LinkedProgram& program,
                                 GLint firstVertex, GLsizei vertexCount, GLsizei instances,
                                 const std::vector<uint32_t>* gather) {
    VertexArray& vao = *ctx_.state.vertexArray;
    const uint64_t serial = ctx_.stream().serial();
    struct Layout {
        id<MTLBuffer> buffer;
        NSUInteger offset;
        uint32_t stride;
        uint32_t divisor;
        bool constant;
    };
    std::vector<Layout> layouts;
    auto addLayout = [&](id<MTLBuffer> buffer, NSUInteger offset, uint32_t stride, uint32_t divisor, bool constant) -> int {
        if (layouts.size() >= shader::kMaxVertexBufferSlots) return -1;
        layouts.push_back({buffer, offset, stride, divisor, constant});
        return static_cast<int>(layouts.size() - 1);
    };
    for (const shader::StageOutput::VertexInput& input : program.stage(Stage::Vertex).vertexInputs) {
        if (input.location < 0 || input.location >= VertexArray::kMaxAttribs) continue;
        const VertexAttrib& a = vao.attribs[input.location];
        VertexAttributeKey& out = key.attributes[input.location];
        if (!a.enabled) {
            // Current (generic) attribute value, one element for every vertex.
            const ContextState::GenericAttrib& generic = ctx_.state.genericAttribs[input.location];
            TransientAllocation alloc = transient_.allocate(16, serial, 16);
            std::memcpy(alloc.data, &generic.value, 16);
            const int layout = addLayout(alloc.buffer, alloc.offset, 16, 0, true);
            if (layout < 0) return false;
            out.format = static_cast<uint8_t>(input.kind == 'i' ? MTLVertexFormatInt4
                                              : input.kind == 'u' ? MTLVertexFormatUInt4
                                                                  : MTLVertexFormatFloat4);
            out.layout = static_cast<uint8_t>(layout);
            out.offset = 0;
            continue;
        }
        const uint32_t elementBytes = a.elementBytes();
        const uint32_t stride = static_cast<uint32_t>(a.effectiveStride());
        const MTLVertexFormat direct = a.isDouble ? MTLVertexFormatInvalid
                                                  : mtlVertexFormat(a.type, a.size, a.normalized, a.integer);
        const bool aligned = (a.offset % 4) == 0 && (stride % 4) == 0 && stride > 0;
        const bool gathered = gather && !a.divisor;
        if (a.buffer && a.buffer->mtl && direct != MTLVertexFormatInvalid && aligned && !gathered) {
            // Share a layout with an earlier attribute from the same buffer/stride/divisor when it fits.
            int layout = -1;
            for (size_t l = 0; l < layouts.size(); ++l) {
                const Layout& existing = layouts[l];
                if (existing.constant || existing.buffer != a.buffer->mtl || existing.stride != stride ||
                    existing.divisor != a.divisor)
                    continue;
                if (static_cast<NSUInteger>(a.offset) >= existing.offset &&
                    a.offset - existing.offset + elementBytes <= stride) {
                    layout = static_cast<int>(l);
                    break;
                }
            }
            if (layout < 0) layout = addLayout(a.buffer->mtl, static_cast<NSUInteger>(a.offset), stride, a.divisor, false);
            if (layout < 0) return false;
            out.format = static_cast<uint8_t>(direct);
            out.layout = static_cast<uint8_t>(layout);
            out.offset = static_cast<uint16_t>(a.offset - layouts[layout].offset);
            a.buffer->lastUseSerial = serial;
            continue;
        }
        // Conversion: expand the used range to 4 x 32-bit components.
        const uint8_t* source = nullptr;
        if (a.buffer) {
            if (!a.buffer->mtl) {
                ctx_.setError(GL_INVALID_OPERATION);
                return false;
            }
            source = a.buffer->contents() + a.offset;  // GPU writes were waited for in waitForCpuVertexReads
        } else {
            source = reinterpret_cast<const uint8_t*>(a.offset);
        }
        if (!source) return false;
        uint32_t first = 0, count = 0;
        if (gathered) {
            count = static_cast<uint32_t>(gather->size());
        } else if (a.divisor) {
            // Instanced attribute: one element per `divisor` instances.
            count = static_cast<uint32_t>((std::max<GLsizei>(1, instances) + a.divisor - 1) / a.divisor);
        } else {
            first = static_cast<uint32_t>(std::max(0, firstVertex));
            count = static_cast<uint32_t>(std::max(0, vertexCount));
        }
        const size_t elements = static_cast<size_t>(first) + count;
        TransientAllocation alloc = transient_.allocate(elements * 16, serial, 16);
        auto* dst = static_cast<uint8_t*>(alloc.data);
        for (uint32_t v = first; v < first + count; ++v) {
            const uint32_t fetch = gathered ? (*gather)[v] : v;
            const uint8_t* element = source + static_cast<size_t>(fetch) * stride;
            uint8_t* to = dst + static_cast<size_t>(v) * 16;
            if (a.integer) {
                int32_t values[4] = {0, 0, 0, 1};
                for (int c = 0; c < std::min(a.size, 4); ++c) {
                    switch (a.type) {
                    case GL_BYTE: values[c] = reinterpret_cast<const int8_t*>(element)[c]; break;
                    case GL_UNSIGNED_BYTE: values[c] = element[c]; break;
                    case GL_SHORT: { int16_t x; std::memcpy(&x, element + c * 2, 2); values[c] = x; break; }
                    case GL_UNSIGNED_SHORT: { uint16_t x; std::memcpy(&x, element + c * 2, 2); values[c] = x; break; }
                    default: std::memcpy(&values[c], element + c * 4, 4); break;
                    }
                }
                std::memcpy(to, values, 16);
            } else {
                float values[4] = {0, 0, 0, 1};
                const int size = a.size == GL_BGRA ? 4 : a.size;
                for (int c = 0; c < std::min(size, 4); ++c) values[c] = readComponent(element, a.type, a.normalized, c);
                if (a.size == GL_BGRA) std::swap(values[0], values[2]);
                std::memcpy(to, values, 16);
            }
        }
        const int layout = addLayout(alloc.buffer, alloc.offset, 16, a.divisor, false);
        if (layout < 0) return false;
        out.format = static_cast<uint8_t>(a.integer ? (input.kind == 'u' ? MTLVertexFormatUInt4 : MTLVertexFormatInt4)
                                                    : MTLVertexFormatFloat4);
        out.layout = static_cast<uint8_t>(layout);
        out.offset = 0;
    }
    key.layoutCount = static_cast<uint8_t>(layouts.size());
    for (size_t l = 0; l < layouts.size(); ++l) {
        const Layout& layout = layouts[l];
        key.layouts[l].stride = layout.constant ? 16 : layout.stride;
        key.layouts[l].stepFunction = static_cast<uint8_t>(layout.constant ? MTLVertexStepFunctionConstant
                                                           : layout.divisor ? MTLVertexStepFunctionPerInstance
                                                                            : MTLVertexStepFunctionPerVertex);
        key.layouts[l].stepRate = layout.constant ? 0 : std::max(1u, layout.divisor);
        [encoder setVertexBuffer:layout.buffer offset:layout.offset atIndex:shader::kSlotFirstVertexBuffer + l];
    }
    return true;
}

void DrawState::bindUniforms(id<MTLRenderCommandEncoder> encoder, LinkedProgram& program) {
    Program& p = *ctx_.state.program;
    // Driver uniforms read by the generated vertex code (see compiler.cpp addPointSize).
    const struct {
        float pointSize;
        uint32_t programPointSize;
    } driver = {std::clamp(ctx_.state.pointSize, 1.0f, 511.0f), ctx_.state.programPointSize ? 1u : 0u};
    [encoder setVertexBytes:&driver length:sizeof(driver) atIndex:shader::kSlotDriverUniforms];
    const uint32_t size = (program.link.defaultBlockSize + 15) & ~15u;
    const uint64_t serial = ctx_.stream().serial();
    static id<MTLBuffer> zero = nil;
    if (!zero) zero = [Device::get().mtl() newBufferWithLength:65536 options:MTLResourceStorageModePrivate];
    for (int s : {static_cast<int>(Stage::Vertex), static_cast<int>(Stage::Fragment)}) {
        const shader::StageOutput& stage = program.link.stages[s];
        if (!stage.present) continue;
        const bool vertex = s == static_cast<int>(Stage::Vertex);
        if (stage.usesDefaultUniforms && size) {
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
            const GLuint binding = b < p.uniformBlockBindings.size() ? p.uniformBlockBindings[b] : 0;
            id<MTLBuffer> buffer = zero;
            NSUInteger offset = 0;
            if (binding < ContextState::kMaxUniformBufferBindings) {
                const IndexedBufferBinding& ib = ctx_.state.uniformBufferBindings[binding];
                if (ib.buffer && ib.buffer->mtl) {
                    buffer = ib.buffer->mtl;
                    offset = static_cast<NSUInteger>(ib.offset);
                    ib.buffer->lastUseSerial = serial;
                }
            }
            if (vertex) [encoder setVertexBuffer:buffer offset:offset atIndex:static_cast<NSUInteger>(slot)];
            else [encoder setFragmentBuffer:buffer offset:offset atIndex:static_cast<NSUInteger>(slot)];
        }
    }
}

void DrawState::bindTextures(id<MTLRenderCommandEncoder> encoder, LinkedProgram& program) {
    Program& p = *ctx_.state.program;
    const uint64_t serial = ctx_.stream().serial();
    for (size_t i = 0; i < program.link.samplers.size(); ++i) {
        const shader::SamplerSlotInfo& slot = program.link.samplers[i];
        const GLint unit = i < p.samplerUnits.size() ? p.samplerUnits[i] : 0;
        id<MTLTexture> view = nil;
        id<MTLSamplerState> sampler = nil;
        Texture* texture = nullptr;
        if (unit >= 0 && unit < ContextState::kMaxTextureUnits && slot.textureTarget >= 0)
            texture = ctx_.state.textureUnits[unit].bound[slot.textureTarget].get();
        Sampler* samplerObject = unit >= 0 && unit < ContextState::kMaxTextureUnits
                                     ? ctx_.state.textureUnits[unit].sampler.get()
                                     : nullptr;
        if (texture && textureComplete(ctx_, *texture, samplerObject ? &samplerObject->params : &texture->sampler)) {
            view = samplingView(ctx_, *texture);
            sampler = samplerState(ctx_, *texture, samplerObject ? samplerObject->params : texture->sampler, slot.shadow);
            texture->lastUseSerial = serial;
        } else {
            view = dummyTexture(slot.type);
            sampler = samplerState(ctx_, SamplerParams(), slot.shadow);
        }
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
}

void DrawState::applyDynamicState(id<MTLRenderCommandEncoder> encoder, const RenderTargetDesc& target) {
    const ContextState& st = ctx_.state;
    MTLViewport viewport;
    viewport.originX = st.viewport[0];
    viewport.originY = st.viewport[1];
    viewport.width = std::max(0, st.viewport[2]);
    viewport.height = std::max(0, st.viewport[3]);
    viewport.znear = st.depthRange[0];
    viewport.zfar = st.depthRange[1];
    [encoder setViewport:viewport];
    // Cull / winding (the vertex stage flips Y, so GL's CCW is Metal's CW).
    MTLCullMode cull = MTLCullModeNone;
    if (st.cullFace) cull = st.cullFaceMode == GL_FRONT ? MTLCullModeFront : MTLCullModeBack;
    [encoder setCullMode:cull];
    [encoder setFrontFacingWinding:st.frontFace == GL_CCW ? MTLWindingClockwise : MTLWindingCounterClockwise];
    [encoder setTriangleFillMode:st.polygonMode == GL_LINE ? MTLTriangleFillModeLines : MTLTriangleFillModeFill];
    const bool offset = (st.polygonMode == GL_FILL && st.polygonOffsetFill) ||
                        (st.polygonMode == GL_LINE && st.polygonOffsetLine) ||
                        (st.polygonMode == GL_POINT && st.polygonOffsetPoint);
    [encoder setDepthBias:offset ? st.polygonOffsetUnits : 0.0f
               slopeScale:offset ? st.polygonOffsetFactor : 0.0f
                    clamp:offset ? st.polygonOffsetClamp : 0.0f];
    [encoder setDepthClipMode:st.depthClamp ? MTLDepthClipModeClamp : MTLDepthClipModeClip];
    [encoder setStencilFrontReferenceValue:static_cast<uint32_t>(st.stencilFront.ref) & 0xff
                        backReferenceValue:static_cast<uint32_t>(st.stencilBack.ref) & 0xff];
    [encoder setBlendColorRed:st.blendColor[0] green:st.blendColor[1] blue:st.blendColor[2] alpha:st.blendColor[3]];
    (void)target;
}

// --------------------------------------------------------------- prepare

id<MTLRenderCommandEncoder> DrawState::prepare(GLenum mode, MTLPrimitiveType& primitive) {
    RenderTargetDesc target;
    TargetFormats formats;
    if (!drawTarget(target, formats)) return nil;
    const ContextState& st = ctx_.state;
    // Scissor: an empty rectangle draws nothing.
    MTLScissorRect scissor = {0, 0, target.width, target.height};
    if (st.scissorTest) {
        const int64_t x0 = std::clamp<int64_t>(st.scissor[0], 0, target.width);
        const int64_t y0 = std::clamp<int64_t>(st.scissor[1], 0, target.height);
        const int64_t x1 = std::clamp<int64_t>(static_cast<int64_t>(st.scissor[0]) + std::max(0, st.scissor[2]), 0, target.width);
        const int64_t y1 = std::clamp<int64_t>(static_cast<int64_t>(st.scissor[1]) + std::max(0, st.scissor[3]), 0, target.height);
        if (x1 <= x0 || y1 <= y0) return nil;
        scissor = {static_cast<NSUInteger>(x0), static_cast<NSUInteger>(y0), static_cast<NSUInteger>(x1 - x0),
                   static_cast<NSUInteger>(y1 - y0)};
    }
    if (st.cullFace && st.cullFaceMode == GL_FRONT_AND_BACK && topologyClass(mode) == MTLPrimitiveTopologyClassTriangle)
        return nil;
    LinkedProgram* program = st.program ? st.program->linked.get() : nullptr;
    if (!program) return nil;

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
    key.rasterization = !st.rasterizerDiscard;
    key.topologyClass = 0;
    for (int i = 0; i < 8; ++i) {
        key.colorFormats[i] = static_cast<uint16_t>(formats.color[i]);
        if (!formats.color[i]) continue;
        PipelineKey::Blend& b = key.blend[i];
        const auto& mask = st.colorMask[i];
        b.writeMask = static_cast<uint8_t>((mask[0] ? MTLColorWriteMaskRed : 0) | (mask[1] ? MTLColorWriteMaskGreen : 0) |
                                           (mask[2] ? MTLColorWriteMaskBlue : 0) |
                                           (mask[3] && !formats.alphaIsOne[i] ? MTLColorWriteMaskAlpha : 0));
        b.enabled = (st.blendEnable >> i) & 1 && !formats.integer[i];
        if (b.enabled) {
            b.srcRGB = static_cast<uint8_t>(mtlBlendFactor(st.blendSrcRGB));
            b.dstRGB = static_cast<uint8_t>(mtlBlendFactor(st.blendDstRGB));
            b.srcAlpha = static_cast<uint8_t>(mtlBlendFactor(st.blendSrcAlpha));
            b.dstAlpha = static_cast<uint8_t>(mtlBlendFactor(st.blendDstAlpha));
            b.opRGB = static_cast<uint8_t>(mtlBlendOp(st.blendEquationRGB));
            b.opAlpha = static_cast<uint8_t>(mtlBlendOp(st.blendEquationAlpha));
        }
    }
    key.depthFormat = static_cast<uint16_t>(formats.depth);
    key.stencilFormat = static_cast<uint16_t>(formats.stencil);
    (void)primitive;
    lastKey_ = key;
    lastScissor_ = scissor;
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
    case GL_TRIANGLE_FAN: case GL_POLYGON:
        // GL's provoking vertex for fans is the last vertex of each triangle (i + 1, i + 2).
        for (size_t i = 1; i + 1 < n; ++i) { out.push_back(in[0]); out.push_back(in[i]); out.push_back(in[i + 1]); }
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

}  // namespace

bool DrawState::issue(GLenum mode, GLint first, GLsizei count, GLsizei instances, const uint32_t* indexData,
                      id<MTLBuffer> indexBuffer, NSUInteger indexOffset, MTLIndexType indexType, GLint baseVertex,
                      GLint minVertex, GLsizei vertexSpan) {
    queriesCountPrimitives(ctx_, mode, count, instances);
    if (ctx_.state.program && ctx_.state.program->linked) waitForCpuVertexReads(*ctx_.state.program->linked, false);
    MTLPrimitiveType primitive = MTLPrimitiveTypeTriangle;
    id<MTLRenderCommandEncoder> encoder = prepare(mode, primitive);
    if (!encoder) return false;
    queriesBeforeDraw(ctx_, encoder);
    LinkedProgram& program = *ctx_.state.program->linked;
    PipelineKey key = lastKey_;
    // Instanced attributes need data for every instance they cover.
    if (!bindVertexInputs(encoder, key, program, minVertex, vertexSpan, instances)) {
        WMGL_WARN("too many vertex buffers for one draw");
        return false;
    }
    id<MTLRenderPipelineState> pipeline = ctx_.share().pipelines().render(key, program);
    if (!pipeline) return false;
    [encoder setRenderPipelineState:pipeline];
    DepthStencilKey ds;
    std::memset(&ds, 0, sizeof(ds));
    const ContextState& st = ctx_.state;
    const RenderTargetDesc& target = ctx_.stream().currentTarget();
    const bool hasDepth = target.depth.texture != nil, hasStencil = target.stencil.texture != nil;
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
    [encoder setScissorRect:lastScissor_];
    bindUniforms(encoder, program);
    bindTextures(encoder, program);
    if (program.link.xfbVertexCapture) bindCaptureDefaults(encoder, program);

    bool needsIndices = false;
    const MTLPrimitiveType native = nativePrimitive(mode, needsIndices);
    const uint64_t serial = ctx_.stream().serial();
    if (needsIndices || (indexData && !indexBuffer)) {
        std::vector<uint32_t> in, out;
        in.reserve(static_cast<size_t>(count));
        for (GLsizei i = 0; i < count; ++i) in.push_back(indexData ? indexData[i] : static_cast<uint32_t>(first + i));
        if (needsIndices) expandIndices(mode, in, out);
        else out.swap(in);
        if (out.empty()) return true;
        TransientAllocation alloc = transient_.allocate(out.size() * 4, serial, 4);
        std::memcpy(alloc.data, out.data(), out.size() * 4);
        [encoder drawIndexedPrimitives:native indexCount:out.size() indexType:MTLIndexTypeUInt32
                           indexBuffer:alloc.buffer indexBufferOffset:alloc.offset instanceCount:std::max(1, instances)
                            baseVertex:indexData ? baseVertex : 0 baseInstance:0];
    } else if (indexBuffer) {
        [encoder drawIndexedPrimitives:native indexCount:static_cast<NSUInteger>(count) indexType:indexType
                           indexBuffer:indexBuffer indexBufferOffset:indexOffset instanceCount:std::max(1, instances)
                            baseVertex:baseVertex baseInstance:0];
    } else {
        [encoder drawPrimitives:native vertexStart:static_cast<NSUInteger>(first) vertexCount:static_cast<NSUInteger>(count)
                  instanceCount:std::max(1, instances) baseInstance:0];
    }
    ctx_.stream().noteDraw();
    return true;
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
    case GL_TRIANGLE_FAN:
        for (size_t i = 1; i + 1 < n; ++i) { out.push_back(v[0]); out.push_back(v[i]); out.push_back(v[i + 1]); }
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
    Program* p = ctx_.state.program.get();
    LinkedProgram* lp = p ? p->linked.get() : nullptr;
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
        const int64_t size = binding.size ? binding.size : binding.buffer->size - binding.offset;
        const int64_t remaining = std::max<int64_t>(0, size - xfb.written[b]);
        const uint64_t vertexBytes = static_cast<uint64_t>(link.xfbStride[b]) * 4;
        if (vertexBytes) primitives = std::min<uint64_t>(primitives, static_cast<uint64_t>(remaining) / vertexBytes / perPrimitive);
    }
    if (Query* q = ctx_.state.activeQueries[kQueryXfbPrimitivesWritten].get()) q->result += primitives;
    xfb.primitivesWritten += primitives;
    if (!primitives) return;

    waitForCpuVertexReads(*lp, true);
    RenderTargetDesc target;
    TargetFormats formats;
    if (!drawTarget(target, formats, false)) return;
    id<MTLRenderCommandEncoder> encoder = ctx_.stream().render(target);
    PipelineKey key;
    std::memset(&key, 0, sizeof(key));
    key.programId = lp->uid;
    key.sampleCount = static_cast<uint8_t>(target.samples);
    key.rasterization = 0;
    key.topologyClass = static_cast<uint8_t>(MTLPrimitiveTopologyClassPoint);
    for (int i = 0; i < 8; ++i) key.colorFormats[i] = static_cast<uint16_t>(formats.color[i]);
    key.depthFormat = static_cast<uint16_t>(formats.depth);
    key.stencilFormat = static_cast<uint16_t>(formats.stencil);
    if (!bindVertexInputs(encoder, key, *lp, 0, static_cast<GLsizei>(list.size()), instances, &list)) return;
    id<MTLRenderPipelineState> pipeline = ctx_.share().pipelines().render(key, *lp);
    if (!pipeline) return;
    [encoder setRenderPipelineState:pipeline];
    bindUniforms(encoder, *lp);
    bindTextures(encoder, *lp);
    const uint64_t serial = ctx_.stream().serial();
    // Control block: active, vertex count, slot limit, pad, base[4], stride[4], vertex ids.
    const size_t controlBytes = 12 * 4 + list.size() * 4;
    TransientAllocation control = transient_.allocate(controlBytes, serial, 16);
    auto* words = static_cast<uint32_t*>(control.data);
    std::memset(words, 0, 12 * 4);
    words[0] = 1;
    words[1] = static_cast<uint32_t>(list.size());
    words[2] = static_cast<uint32_t>(primitives * perPrimitive);
    for (int b = 0; b < link.xfbBufferCount; ++b) {
        const TransformFeedback::Binding& binding = xfb.buffers[b];
        words[4 + b] = static_cast<uint32_t>((binding.offset + xfb.written[b]) / 4);
        words[8 + b] = link.xfbStride[b];
        [encoder setVertexBuffer:binding.buffer->mtl offset:0 atIndex:shader::kSlotFirstXfbBuffer + static_cast<NSUInteger>(b)];
    }
    std::memcpy(words + 12, list.data(), list.size() * 4);
    [encoder setVertexBuffer:control.buffer offset:control.offset atIndex:shader::kSlotXfbControl];
    [encoder drawPrimitives:MTLPrimitiveTypePoint vertexStart:0 vertexCount:list.size()
              instanceCount:static_cast<NSUInteger>(std::max(1, instances)) baseInstance:0];
    ctx_.stream().noteDraw();
    for (int b = 0; b < link.xfbBufferCount; ++b) {
        Buffer& buffer = *xfb.buffers[b].buffer;
        xfb.written[b] += static_cast<GLsizeiptr>(primitives * perPrimitive * link.xfbStride[b] * 4);
        buffer.lastUseSerial = buffer.lastWriteSerial = serial;
        ++buffer.version;
    }
}

void DrawState::drawArrays(GLenum mode, GLint first, GLsizei count, GLsizei instances) {
    if (first < 0 || count < 0 || instances < 0) {
        ctx_.setError(GL_INVALID_VALUE);
        return;
    }
    if (!validateDraw(mode)) return;
    if (count == 0 || instances == 0 || !conditionalRenderAllows(ctx_)) return;
    if (ctx_.state.transformFeedback->active) {
        std::vector<uint32_t> vertices(static_cast<size_t>(count));
        for (GLsizei i = 0; i < count; ++i) vertices[static_cast<size_t>(i)] = static_cast<uint32_t>(first + i);
        captureTransformFeedback(mode, vertices, instances);
    }
    issue(mode, first, count, instances, nullptr, nil, 0, MTLIndexTypeUInt16, 0, first, count);
}

void DrawState::drawElements(GLenum mode, GLsizei count, GLenum type, const void* indices, GLsizei instances,
                             GLint baseVertex) {
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
    if (elementBuffer && elementBuffer->mapped) {
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
        if (elementBuffer->lastWriteSerial) ctx_.stream().waitFor(elementBuffer->lastWriteSerial);
        source = elementBuffer->contents() + offset;
    } else {
        // Client-memory indices: deprecated in core profiles but accepted, as
        // other desktop drivers do (the KHR-GL33 transform feedback tests use them).
        if (!indices) {
            ctx_.setError(GL_INVALID_OPERATION);
            return;
        }
        source = static_cast<const uint8_t*>(indices);
    }
    // Scan the indices: the used vertex range drives attribute conversion, and
    // primitive restart / 8-bit indices / non-native modes need rewriting.
    std::vector<uint32_t> values(static_cast<size_t>(count));
    uint32_t minIndex = UINT32_MAX, maxIndex = 0;
    const ContextState& st = ctx_.state;
    const uint32_t restartIndex = st.primitiveRestartFixedIndex
                                      ? (type == GL_UNSIGNED_BYTE ? 0xffu : type == GL_UNSIGNED_SHORT ? 0xffffu : 0xffffffffu)
                                      : st.primitiveRestartIndex;
    const bool restart = st.primitiveRestart || st.primitiveRestartFixedIndex;
    bool hasRestart = false;
    for (GLsizei i = 0; i < count; ++i) {
        uint32_t v;
        if (indexSize == 1) v = source[i];
        else if (indexSize == 2) { uint16_t x; std::memcpy(&x, source + i * 2, 2); v = x; }
        else std::memcpy(&v, source + i * 4, 4);
        values[static_cast<size_t>(i)] = v;
        if (restart && v == restartIndex) { hasRestart = true; continue; }
        minIndex = std::min(minIndex, v);
        maxIndex = std::max(maxIndex, v);
    }
    if (minIndex == UINT32_MAX) return;  // only restart indices
    if (ctx_.state.transformFeedback->active) {
        // Capture each restart-delimited segment in order; gl_VertexID includes basevertex.
        std::vector<uint32_t> segment;
        for (size_t i = 0; i <= values.size(); ++i) {
            if (i < values.size() && !(restart && values[i] == restartIndex)) {
                segment.push_back(values[i] + static_cast<uint32_t>(baseVertex));
                continue;
            }
            if (!segment.empty()) captureTransformFeedback(mode, segment, instances);
            segment.clear();
        }
    }
    const GLint spanFirst = static_cast<GLint>(minIndex) + baseVertex;
    const GLsizei span = static_cast<GLsizei>(maxIndex - minIndex + 1);
    bool needsIndices = false;
    nativePrimitive(mode, needsIndices);
    if (hasRestart) {
        // Draw each restart-delimited segment as its own primitive stream.
        size_t start = 0;
        for (size_t i = 0; i <= values.size(); ++i) {
            if (i < values.size() && values[i] != restartIndex) continue;
            if (i > start) {
                std::vector<uint32_t> segment(values.begin() + static_cast<long>(start), values.begin() + static_cast<long>(i));
                issue(mode, 0, static_cast<GLsizei>(segment.size()), instances, segment.data(), nil, 0,
                      MTLIndexTypeUInt32, baseVertex, spanFirst, span);
            }
            start = i + 1;
        }
        return;
    }
    if (elementBuffer && !needsIndices && indexSize != 1 && offset % indexSize == 0) {
        elementBuffer->lastUseSerial = ctx_.stream().serial();
        issue(mode, 0, count, instances, nullptr, elementBuffer->mtl, offset,
              indexSize == 2 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32, baseVertex, spanFirst, span);
        return;
    }
    issue(mode, 0, count, instances, values.data(), nil, 0, MTLIndexTypeUInt32, baseVertex, spanFirst, span);
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
            if (!target.color[i].texture || formats.integer[i]) continue;
            clear.colorMask |= 1u << i;
            float c[4] = {st.clearColor[0], st.clearColor[1], st.clearColor[2], formats.alphaIsOne[i] ? 1.0f : st.clearColor[3]};
            clear.color[i] = MTLClearColorMake(c[0], c[1], c[2], c[3]);
            const auto& m = st.colorMask[i];
            if (!m[0] || !m[1] || !m[2] || (!m[3] && !formats.alphaIsOne[i])) fullMasks = false;
        }
    }
    if ((mask & GL_DEPTH_BUFFER_BIT) && target.depth.texture) {
        clear.depth = true;
        clear.depthValue = std::clamp(st.clearDepth, 0.0, 1.0);
        if (!st.depthMask) fullMasks = false;
    }
    if ((mask & GL_STENCIL_BUFFER_BIT) && target.stencil.texture) {
        clear.stencil = true;
        clear.stencilValue = static_cast<uint32_t>(st.clearStencil) & 0xff;
        if ((st.stencilFront.writeMask & 0xff) != 0xff) fullMasks = false;
    }
    if (!clear.any()) return;
    bool fullRect = true;
    if (st.scissorTest) {
        fullRect = st.scissor[0] <= 0 && st.scissor[1] <= 0 &&
                   static_cast<int64_t>(st.scissor[0]) + st.scissor[2] >= static_cast<int64_t>(target.width) &&
                   static_cast<int64_t>(st.scissor[1]) + st.scissor[3] >= static_cast<int64_t>(target.height);
    }
    if (fullMasks && fullRect && ctx_.stream().clearWithLoadAction(target, clear)) return;
    clearWithDraw(target, formats, clear);
}

namespace {

// Clear pipelines: the fragment shader's outputs are typed per attachment
// (float, int or uint) to match the pass; values arrive as raw 32-bit words.
id<MTLRenderPipelineState> clearPipeline(const TargetFormats& formats, const uint8_t writeMasks[8], uint32_t samples,
                                         const char signature[9]) {
    static std::mutex mutex;
    static std::unordered_map<std::string, id<MTLRenderPipelineState>> cache;
    std::string key(signature, 8);
    for (int i = 0; i < 8; ++i) key += std::to_string(formats.color[i]) + ":" + std::to_string(writeMasks[i]) + ",";
    key += std::to_string(formats.depth) + "," + std::to_string(formats.stencil) + "," + std::to_string(samples);
    std::lock_guard<std::mutex> lock(mutex);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    std::string source = "#include <metal_stdlib>\nusing namespace metal;\n"
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
    if (st.scissorTest) {
        const int64_t x0 = std::clamp<int64_t>(st.scissor[0], 0, target.width);
        const int64_t y0 = std::clamp<int64_t>(st.scissor[1], 0, target.height);
        const int64_t x1 = std::clamp<int64_t>(static_cast<int64_t>(st.scissor[0]) + std::max(0, st.scissor[2]), 0, target.width);
        const int64_t y1 = std::clamp<int64_t>(static_cast<int64_t>(st.scissor[1]) + std::max(0, st.scissor[3]), 0, target.height);
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
    id<MTLRenderPipelineState> pipeline = clearPipeline(formats, writeMasks, target.samples, signature);
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
    [encoder setStencilReferenceValue:clear.stencilValue];
    const float depth[4] = {static_cast<float>(clear.depthValue), 0, 0, 0};
    [encoder setVertexBytes:depth length:sizeof(depth) atIndex:0];
    [encoder setFragmentBytes:values length:sizeof(values) atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
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
    if ((buffer == GL_DEPTH || buffer == GL_DEPTH_STENCIL) && target.depth.texture) {
        clear.depth = true;
        clear.depthValue = std::clamp(depthValue, 0.0, 1.0);
        if (!st.depthMask) fullMasks = false;
    }
    if ((buffer == GL_STENCIL || buffer == GL_DEPTH_STENCIL) && target.stencil.texture) {
        clear.stencil = true;
        clear.stencilValue = static_cast<uint32_t>(stencilValue) & 0xff;
        if ((st.stencilFront.writeMask & 0xff) != 0xff) fullMasks = false;
    }
    if (!clear.any()) return;
    bool fullRect = true;
    if (st.scissorTest)
        fullRect = st.scissor[0] <= 0 && st.scissor[1] <= 0 &&
                   static_cast<int64_t>(st.scissor[0]) + st.scissor[2] >= static_cast<int64_t>(target.width) &&
                   static_cast<int64_t>(st.scissor[1]) + st.scissor[3] >= static_cast<int64_t>(target.height);
    if (fullMasks && fullRect && ctx_.stream().clearWithLoadAction(target, clear)) return;
    clearWithDraw(target, formats, clear);
}

}  // namespace wmgl
