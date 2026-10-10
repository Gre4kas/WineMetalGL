// Framebuffer and renderbuffer objects, draw/read buffers, ReadPixels
// (GL 3.3 sections 4.2-4.4).

#include <algorithm>
#include <cstring>
#include <vector>

#include "core/context.h"
#include "core/draw.h"
#include "core/format/pixel_transfer.h"
#include "core/metal/device.h"
#include "core/metal/pixel_format.h"
#include "core/texture_storage.h"
#include "gl_api.gen.h"

namespace wmgl::api {
namespace {

RefPtr<Framebuffer>* framebufferBinding(Context& ctx, GLenum target) {
    switch (target) {
    case GL_DRAW_FRAMEBUFFER: case GL_FRAMEBUFFER: return &ctx.state.drawFramebuffer;
    case GL_READ_FRAMEBUFFER: return &ctx.state.readFramebuffer;
    default: return nullptr;
    }
}

Attachment* attachmentPoint(Framebuffer& fb, GLenum attachment) {
    if (attachment >= GL_COLOR_ATTACHMENT0 && attachment < GL_COLOR_ATTACHMENT0 + Framebuffer::kMaxColor)
        return &fb.color[attachment - GL_COLOR_ATTACHMENT0];
    if (attachment == GL_DEPTH_ATTACHMENT) return &fb.depth;
    if (attachment == GL_STENCIL_ATTACHMENT) return &fb.stencil;
    return nullptr;
}

const FormatInfo* attachmentFormat(const Attachment& a, GLsizei& width, GLsizei& height, GLsizei& samples, bool& fixedLocations) {
    fixedLocations = true;
    if (a.type == GL_RENDERBUFFER && a.renderbuffer) {
        width = a.renderbuffer->width;
        height = a.renderbuffer->height;
        samples = a.renderbuffer->samples;
        return a.renderbuffer->format;
    }
    if (a.type == GL_TEXTURE && a.texture) {
        const int face = a.cubeFace ? static_cast<int>(a.cubeFace - GL_TEXTURE_CUBE_MAP_POSITIVE_X) : 0;
        if (a.level < 0 || a.level >= Texture::kMaxLevels) return nullptr;
        const TextureImage& image = a.texture->image(face, a.level);
        width = image.width;
        height = image.height;
        samples = image.samples;
        fixedLocations = image.fixedSampleLocations;
        return image.format;
    }
    return nullptr;
}

GLenum completeness(Context& ctx, Framebuffer* fb) {
    if (!fb) return ctx.drawSurface() ? GL_FRAMEBUFFER_COMPLETE : GL_FRAMEBUFFER_UNDEFINED;
    bool any = false;
    GLsizei samples = -1;
    int fixed = -1;
    int layered = -1;
    GLenum layerTarget = 0;
    auto check = [&](const Attachment& a, int kind) -> GLenum {
        if (!a.attached()) return GL_FRAMEBUFFER_COMPLETE;
        GLsizei w = 0, h = 0, s = 0;
        bool fixedLocations = true;
        const FormatInfo* f = attachmentFormat(a, w, h, s, fixedLocations);
        if (!f || w <= 0 || h <= 0) return GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT;
        if (kind == 0 && !f->colorRenderable) return GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT;
        if (kind == 1 && !f->depthRenderable) return GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT;
        if (kind == 2 && !f->stencilRenderable) return GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT;
        if (a.type == GL_TEXTURE && a.texture) {
            const GLenum t = a.texture->target;
            if ((t == GL_TEXTURE_3D || t == GL_TEXTURE_2D_ARRAY || t == GL_TEXTURE_1D_ARRAY) && !a.layered) {
                const TextureImage& image = a.texture->image(0, a.level);
                const GLsizei layers = t == GL_TEXTURE_1D_ARRAY ? image.height : image.depth;
                if (a.layer >= layers) return GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT;
            }
        }
        any = true;
        const GLsizei normalizedSamples = std::max(0, s);
        if (samples < 0) samples = normalizedSamples;
        else if (samples != normalizedSamples) return GL_FRAMEBUFFER_INCOMPLETE_MULTISAMPLE;
        if (fixed < 0) fixed = fixedLocations;
        else if (fixed != static_cast<int>(fixedLocations) && a.type == GL_TEXTURE) return GL_FRAMEBUFFER_INCOMPLETE_MULTISAMPLE;
        const int isLayered = a.layered ? 1 : 0;
        if (layered < 0) {
            layered = isLayered;
            layerTarget = a.texture ? a.texture->target : 0;
        } else if (layered != isLayered) {
            return GL_FRAMEBUFFER_INCOMPLETE_LAYER_TARGETS;
        } else if (isLayered && a.texture && a.texture->target != layerTarget) {
            return GL_FRAMEBUFFER_INCOMPLETE_LAYER_TARGETS;
        }
        return GL_FRAMEBUFFER_COMPLETE;
    };
    for (const Attachment& a : fb->color)
        if (GLenum status = check(a, 0); status != GL_FRAMEBUFFER_COMPLETE) return status;
    if (GLenum status = check(fb->depth, 1); status != GL_FRAMEBUFFER_COMPLETE) return status;
    if (GLenum status = check(fb->stencil, 2); status != GL_FRAMEBUFFER_COMPLETE) return status;
    if (!any) return GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT;
    // GL_ARB_ES2_compatibility (advertised in every context, core in 4.1)
    // removes the INCOMPLETE_DRAW_BUFFER / INCOMPLETE_READ_BUFFER rules:
    // draw and read buffers naming empty attachments are simply ignored.
    // Metal: a combined depth/stencil image must be bound to both points as the same image.
    if (fb->depth.attached() && fb->stencil.attached()) {
        GLsizei w, h, s;
        bool fl;
        const FormatInfo* d = attachmentFormat(fb->depth, w, h, s, fl);
        const FormatInfo* st = attachmentFormat(fb->stencil, w, h, s, fl);
        const bool sameImage = fb->depth.type == fb->stencil.type && fb->depth.texture == fb->stencil.texture &&
                               fb->depth.renderbuffer == fb->stencil.renderbuffer && fb->depth.level == fb->stencil.level &&
                               fb->depth.layer == fb->stencil.layer && fb->depth.cubeFace == fb->stencil.cubeFace;
        if (d && st && (d->isStencil() || st->isDepth()) && !sameImage) return GL_FRAMEBUFFER_UNSUPPORTED;
    }
    return GL_FRAMEBUFFER_COMPLETE;
}

void framebufferChanged(Context& ctx, Framebuffer& fb) {
    ++fb.version;
    ctx.markDirty(kDirtyFramebuffer);
}

RefPtr<Texture> textureForAttach(Context& ctx, GLuint name, bool& ok) {
    ok = true;
    if (!name) return nullptr;
    ShareLock lock(ctx);
    RefPtr<Texture> tex = ctx.share().textures.getRef(name);
    if (!tex) {
        ctx.setError(GL_INVALID_OPERATION);
        ok = false;
    }
    return tex;
}

void attachTexture(Context& ctx, GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level,
                   GLint layer, bool layered, bool layerCall) {
    RefPtr<Framebuffer>* binding = framebufferBinding(ctx, target);
    if (!binding) return ctx.setError(GL_INVALID_ENUM);
    Framebuffer* fb = binding->get();
    if (!fb) return ctx.setError(GL_INVALID_OPERATION);
    const bool depthStencil = attachment == GL_DEPTH_STENCIL_ATTACHMENT;
    Attachment* point = depthStencil ? &fb->depth : attachmentPoint(*fb, attachment);
    if (!point) return ctx.setError(attachment >= GL_COLOR_ATTACHMENT0 && attachment <= GL_COLOR_ATTACHMENT0 + 31
                                        ? GL_INVALID_OPERATION
                                        : GL_INVALID_ENUM);
    bool ok;
    RefPtr<Texture> tex = textureForAttach(ctx, texture, ok);
    if (!ok) return;
    Attachment a;
    if (tex) {
        if (level < 0 || level >= Texture::kMaxLevels) return ctx.setError(GL_INVALID_VALUE);
        if (textarget) {
            const bool cubeFace = textarget >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && textarget <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z;
            const GLenum expect = cubeFace ? GL_TEXTURE_CUBE_MAP : textarget;
            if (tex->target != expect) return ctx.setError(GL_INVALID_OPERATION);
            if (cubeFace) a.cubeFace = textarget;
            if ((textarget == GL_TEXTURE_RECTANGLE || textarget == GL_TEXTURE_2D_MULTISAMPLE) && level != 0)
                return ctx.setError(GL_INVALID_VALUE);
        } else if (layerCall) {
            const GLenum t = tex->target;
            if (t != GL_TEXTURE_3D && t != GL_TEXTURE_2D_ARRAY && t != GL_TEXTURE_1D_ARRAY && t != GL_TEXTURE_2D_MULTISAMPLE_ARRAY &&
                t != GL_TEXTURE_CUBE_MAP_ARRAY)
                return ctx.setError(GL_INVALID_OPERATION);
            if (layer < 0) return ctx.setError(GL_INVALID_VALUE);
        }
        a.type = GL_TEXTURE;
        a.texture = tex;
        a.level = level;
        a.layer = layer;
        a.layered = layered && (tex->target == GL_TEXTURE_3D || tex->target == GL_TEXTURE_2D_ARRAY ||
                                tex->target == GL_TEXTURE_CUBE_MAP || tex->target == GL_TEXTURE_1D_ARRAY ||
                                tex->target == GL_TEXTURE_2D_MULTISAMPLE_ARRAY || tex->target == GL_TEXTURE_CUBE_MAP_ARRAY);
    }
    if (depthStencil) {
        fb->depth = a;
        fb->stencil = a;
    } else {
        *point = a;
    }
    framebufferChanged(ctx, *fb);
}

}  // namespace

// ---------------------------------------------------- framebuffer objects

void GenFramebuffers(Context& ctx, GLsizei n, GLuint* framebuffers) {
    if (n < 0) return ctx.setError(GL_INVALID_VALUE);
    if (framebuffers) ctx.framebuffers.generate(n, framebuffers);
}

void DeleteFramebuffers(Context& ctx, GLsizei n, const GLuint* framebuffers) {
    if (n < 0) return ctx.setError(GL_INVALID_VALUE);
    if (!framebuffers) return;
    for (GLsizei i = 0; i < n; ++i) {
        if (!framebuffers[i]) continue;
        RefPtr<Framebuffer> fb = ctx.framebuffers.remove(framebuffers[i]);
        if (!fb) continue;
        if (ctx.state.drawFramebuffer == fb) ctx.state.drawFramebuffer.reset();
        if (ctx.state.readFramebuffer == fb) ctx.state.readFramebuffer.reset();
        ctx.markDirty(kDirtyFramebuffer);
    }
}

GLboolean IsFramebuffer(Context& ctx, GLuint framebuffer) { return ctx.framebuffers.exists(framebuffer) ? GL_TRUE : GL_FALSE; }

void BindFramebuffer(Context& ctx, GLenum target, GLuint name) {
    if (target != GL_FRAMEBUFFER && target != GL_DRAW_FRAMEBUFFER && target != GL_READ_FRAMEBUFFER)
        return ctx.setError(GL_INVALID_ENUM);
    RefPtr<Framebuffer> fb;
    if (name) {
        if (!ctx.framebuffers.isGenerated(name)) {
            if (ctx.isCore()) return ctx.setError(GL_INVALID_OPERATION);
            ctx.framebuffers.reserve(name);
        }
        fb = ctx.framebuffers.getRef(name);
        if (!fb) {
            fb = makeRef<Framebuffer>(name);
            ctx.framebuffers.set(name, fb);
        }
    }
    if (target != GL_READ_FRAMEBUFFER) ctx.state.drawFramebuffer = fb;
    if (target != GL_DRAW_FRAMEBUFFER) ctx.state.readFramebuffer = fb;
    ctx.markDirty(kDirtyFramebuffer);
}

GLenum CheckFramebufferStatus(Context& ctx, GLenum target) {
    RefPtr<Framebuffer>* binding = framebufferBinding(ctx, target);
    if (!binding) {
        ctx.setError(GL_INVALID_ENUM);
        return 0;
    }
    return completeness(ctx, binding->get());
}

void FramebufferTexture(Context& ctx, GLenum target, GLenum attachment, GLuint texture, GLint level) {
    attachTexture(ctx, target, attachment, 0, texture, level, 0, true, false);
}

void FramebufferTexture1D(Context& ctx, GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level) {
    if (texture && textarget != GL_TEXTURE_1D) return ctx.setError(GL_INVALID_OPERATION);
    attachTexture(ctx, target, attachment, texture ? textarget : 0, texture, level, 0, false, false);
}

void FramebufferTexture2D(Context& ctx, GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level) {
    if (texture && textarget != GL_TEXTURE_2D && textarget != GL_TEXTURE_RECTANGLE && textarget != GL_TEXTURE_2D_MULTISAMPLE &&
        !(textarget >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && textarget <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z))
        return ctx.setError(GL_INVALID_ENUM);
    attachTexture(ctx, target, attachment, texture ? textarget : 0, texture, level, 0, false, false);
}

void FramebufferTexture3D(Context& ctx, GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level,
                          GLint zoffset) {
    if (texture && textarget != GL_TEXTURE_3D) return ctx.setError(GL_INVALID_OPERATION);
    attachTexture(ctx, target, attachment, texture ? textarget : 0, texture, level, zoffset, false, false);
}

void FramebufferTextureLayer(Context& ctx, GLenum target, GLenum attachment, GLuint texture, GLint level, GLint layer) {
    attachTexture(ctx, target, attachment, 0, texture, level, layer, false, true);
}

void FramebufferRenderbuffer(Context& ctx, GLenum target, GLenum attachment, GLenum renderbuffertarget, GLuint renderbuffer) {
    RefPtr<Framebuffer>* binding = framebufferBinding(ctx, target);
    if (!binding) return ctx.setError(GL_INVALID_ENUM);
    if (renderbuffertarget != GL_RENDERBUFFER) return ctx.setError(GL_INVALID_ENUM);
    Framebuffer* fb = binding->get();
    if (!fb) return ctx.setError(GL_INVALID_OPERATION);
    const bool depthStencil = attachment == GL_DEPTH_STENCIL_ATTACHMENT;
    Attachment* point = depthStencil ? &fb->depth : attachmentPoint(*fb, attachment);
    if (!point) return ctx.setError(GL_INVALID_ENUM);
    Attachment a;
    if (renderbuffer) {
        ShareLock lock(ctx);
        RefPtr<Renderbuffer> rb = ctx.share().renderbuffers.getRef(renderbuffer);
        if (!rb) return ctx.setError(GL_INVALID_OPERATION);
        a.type = GL_RENDERBUFFER;
        a.renderbuffer = rb;
    }
    if (depthStencil) {
        fb->depth = a;
        fb->stencil = a;
    } else {
        *point = a;
    }
    framebufferChanged(ctx, *fb);
}

void GetFramebufferAttachmentParameteriv(Context& ctx, GLenum target, GLenum attachment, GLenum pname, GLint* params) {
    RefPtr<Framebuffer>* binding = framebufferBinding(ctx, target);
    if (!binding) return ctx.setError(GL_INVALID_ENUM);
    if (!params) return;
    Framebuffer* fb = binding->get();
    if (!fb) {
        // Default framebuffer: GL_FRONT_LEFT, GL_BACK_LEFT, GL_DEPTH, GL_STENCIL.
        Surface* surface = target == GL_READ_FRAMEBUFFER ? ctx.readSurface() : ctx.drawSurface();
        const bool color = attachment == GL_FRONT_LEFT || attachment == GL_BACK_LEFT || attachment == GL_FRONT ||
                           attachment == GL_BACK || attachment == GL_LEFT;
        const bool depth = attachment == GL_DEPTH, stencil = attachment == GL_STENCIL;
        if (!color && !depth && !stencil) return ctx.setError(GL_INVALID_ENUM);
        const bool present = surface && (color || (depth && surface->hasDepth()) || (stencil && surface->hasStencil()));
        switch (pname) {
        case GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE: *params = present ? GL_FRAMEBUFFER_DEFAULT : GL_NONE; return;
        case GL_FRAMEBUFFER_ATTACHMENT_RED_SIZE: case GL_FRAMEBUFFER_ATTACHMENT_GREEN_SIZE:
        case GL_FRAMEBUFFER_ATTACHMENT_BLUE_SIZE: case GL_FRAMEBUFFER_ATTACHMENT_ALPHA_SIZE:
            if (!present) return ctx.setError(GL_INVALID_OPERATION);
            *params = color ? 8 : 0;
            return;
        case GL_FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE:
            if (!present) return ctx.setError(GL_INVALID_OPERATION);
            *params = depth ? surface->pixelFormat().depth_bits : 0;
            return;
        case GL_FRAMEBUFFER_ATTACHMENT_STENCIL_SIZE:
            if (!present) return ctx.setError(GL_INVALID_OPERATION);
            *params = stencil ? surface->pixelFormat().stencil_bits : 0;
            return;
        case GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE:
            if (!present) return ctx.setError(GL_INVALID_OPERATION);
            *params = color || depth ? GL_UNSIGNED_NORMALIZED : GL_INDEX;
            return;
        case GL_FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING:
            if (!present) return ctx.setError(GL_INVALID_OPERATION);
            *params = color && surface->srgbCapable() ? GL_SRGB : GL_LINEAR;
            return;
        default: return ctx.setError(present ? GL_INVALID_ENUM : GL_INVALID_OPERATION);
        }
    }
    Attachment* point = attachment == GL_DEPTH_STENCIL_ATTACHMENT ? &fb->depth : attachmentPoint(*fb, attachment);
    if (!point) return ctx.setError(GL_INVALID_ENUM);
    if (attachment == GL_DEPTH_STENCIL_ATTACHMENT) {
        const bool same = fb->depth.texture == fb->stencil.texture && fb->depth.renderbuffer == fb->stencil.renderbuffer;
        if (!same) return ctx.setError(GL_INVALID_OPERATION);
    }
    const Attachment& a = *point;
    if (pname == GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE) {
        *params = static_cast<GLint>(a.type);
        return;
    }
    if (!a.attached()) {
        if (pname == GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME) { *params = 0; return; }
        return ctx.setError(GL_INVALID_OPERATION);
    }
    GLsizei w, h, s;
    bool fl;
    const FormatInfo* f = attachmentFormat(a, w, h, s, fl);
    switch (pname) {
    case GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME:
        *params = static_cast<GLint>(a.type == GL_TEXTURE ? a.texture->name : a.renderbuffer->name);
        return;
    case GL_FRAMEBUFFER_ATTACHMENT_RED_SIZE: *params = f ? f->redBits : 0; return;
    case GL_FRAMEBUFFER_ATTACHMENT_GREEN_SIZE: *params = f ? f->greenBits : 0; return;
    case GL_FRAMEBUFFER_ATTACHMENT_BLUE_SIZE: *params = f ? f->blueBits : 0; return;
    case GL_FRAMEBUFFER_ATTACHMENT_ALPHA_SIZE: *params = f ? f->alphaBits : 0; return;
    case GL_FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE: *params = f ? f->depthBits : 0; return;
    case GL_FRAMEBUFFER_ATTACHMENT_STENCIL_SIZE: *params = f ? f->stencilBits : 0; return;
    case GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE:
        if (attachment == GL_DEPTH_STENCIL_ATTACHMENT) return ctx.setError(GL_INVALID_OPERATION);
        if (!f) { *params = GL_NONE; return; }
        if (attachment == GL_STENCIL_ATTACHMENT) { *params = GL_UNSIGNED_INT; return; }
        switch (f->type) {
        case ComponentType::UnsignedNormalized: *params = GL_UNSIGNED_NORMALIZED; return;
        case ComponentType::SignedNormalized: *params = GL_SIGNED_NORMALIZED; return;
        case ComponentType::Float: *params = GL_FLOAT; return;
        case ComponentType::Int: *params = GL_INT; return;
        case ComponentType::UnsignedInt: *params = GL_UNSIGNED_INT; return;
        default: *params = GL_NONE; return;
        }
    case GL_FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING: *params = f && f->srgb ? GL_SRGB : GL_LINEAR; return;
    case GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL:
        if (a.type != GL_TEXTURE) break;
        *params = a.level;
        return;
    case GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_CUBE_MAP_FACE:
        if (a.type != GL_TEXTURE) break;
        *params = static_cast<GLint>(a.cubeFace);
        return;
    case GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LAYER:
        if (a.type != GL_TEXTURE) break;
        *params = a.layer;
        return;
    case GL_FRAMEBUFFER_ATTACHMENT_LAYERED:
        if (a.type != GL_TEXTURE) break;
        *params = a.layered;
        return;
    default: break;
    }
    ctx.setError(a.type == GL_RENDERBUFFER ? GL_INVALID_ENUM : GL_INVALID_ENUM);
}

// --------------------------------------------------------- renderbuffers

void GenRenderbuffers(Context& ctx, GLsizei n, GLuint* renderbuffers) {
    if (n < 0) return ctx.setError(GL_INVALID_VALUE);
    if (!renderbuffers) return;
    ShareLock lock(ctx);
    ctx.share().renderbuffers.generate(n, renderbuffers);
}

void DeleteRenderbuffers(Context& ctx, GLsizei n, const GLuint* renderbuffers) {
    if (n < 0) return ctx.setError(GL_INVALID_VALUE);
    if (!renderbuffers) return;
    for (GLsizei i = 0; i < n; ++i) {
        if (!renderbuffers[i]) continue;
        RefPtr<Renderbuffer> rb;
        {
            ShareLock lock(ctx);
            rb = ctx.share().renderbuffers.remove(renderbuffers[i]);
        }
        if (!rb) continue;
        if (ctx.state.renderbuffer == rb) ctx.state.renderbuffer.reset();
        // Detach from the bound framebuffers of the current context.
        for (Framebuffer* fb : {ctx.state.drawFramebuffer.get(), ctx.state.readFramebuffer.get()}) {
            if (!fb) continue;
            for (Attachment* a : {&fb->depth, &fb->stencil})
                if (a->renderbuffer == rb) *a = Attachment();
            for (Attachment& a : fb->color)
                if (a.renderbuffer == rb) a = Attachment();
            framebufferChanged(ctx, *fb);
        }
    }
}

GLboolean IsRenderbuffer(Context& ctx, GLuint renderbuffer) {
    ShareLock lock(ctx);
    return ctx.share().renderbuffers.exists(renderbuffer) ? GL_TRUE : GL_FALSE;
}

void BindRenderbuffer(Context& ctx, GLenum target, GLuint name) {
    if (target != GL_RENDERBUFFER) return ctx.setError(GL_INVALID_ENUM);
    if (!name) {
        ctx.state.renderbuffer.reset();
        return;
    }
    ShareLock lock(ctx);
    RefPtr<Renderbuffer> rb = ctx.share().renderbuffers.getRef(name);
    if (!rb) {
        if (!ctx.share().renderbuffers.isGenerated(name)) {
            if (ctx.isCore()) return ctx.setError(GL_INVALID_OPERATION);
            ctx.share().renderbuffers.reserve(name);
        }
        rb = makeRef<Renderbuffer>(name);
        ctx.share().renderbuffers.set(name, rb);
    }
    ctx.state.renderbuffer = rb;
}

void RenderbufferStorageMultisample(Context& ctx, GLenum target, GLsizei samples, GLenum internalformat, GLsizei width,
                                    GLsizei height) {
    if (target != GL_RENDERBUFFER) return ctx.setError(GL_INVALID_ENUM);
    Renderbuffer* rb = ctx.state.renderbuffer.get();
    if (!rb) return ctx.setError(GL_INVALID_OPERATION);
    const GLenum resolved = resolveInternalFormat(internalformat, 0, 0);
    const FormatInfo* f = formatInfo(resolved);
    if (!f || f->compressed || (!f->colorRenderable && !f->isDepthOrStencil())) return ctx.setError(GL_INVALID_ENUM);
    if (width < 0 || height < 0 || samples < 0) return ctx.setError(GL_INVALID_VALUE);
    if (width > 16384 || height > 16384) return ctx.setError(GL_INVALID_VALUE);
    const auto& caps = Device::get().caps();
    if (samples > static_cast<GLsizei>(caps.maxSamples)) return ctx.setError(GL_INVALID_OPERATION);
    if (f->isInteger() && samples > 0) {
        // GL 3.3: MAX_INTEGER_SAMPLES; Metal supports multisampled integer targets.
    }
    // Round the sample count up to one Metal supports.
    GLsizei actual = samples;
    if (actual > 1) {
        actual = 2;
        while (actual < samples) actual *= 2;
        while (actual <= 8 && !caps.sampleCountSupported[actual]) actual *= 2;
        actual = std::min<GLsizei>(actual, static_cast<GLsizei>(caps.maxSamples));
    }
    rb->internalFormat = internalformat;
    rb->format = f;
    rb->width = width;
    rb->height = height;
    rb->samples = actual > 1 ? actual : (samples == 1 ? 1 : 0);
    rb->mtl = nil;
    if (width > 0 && height > 0) {
        MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:mtlPixelFormat(f->storage)
                                                                                         width:static_cast<NSUInteger>(width)
                                                                                        height:static_cast<NSUInteger>(height)
                                                                                     mipmapped:NO];
        desc.storageMode = MTLStorageModePrivate;
        desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
        if (actual > 1) {
            desc.textureType = MTLTextureType2DMultisample;
            desc.sampleCount = static_cast<NSUInteger>(actual);
        }
        rb->mtl = [Device::get().mtl() newTextureWithDescriptor:desc];
        if (!rb->mtl) return ctx.setError(GL_OUT_OF_MEMORY);
        // RGB formats keep alpha at one in storage.
        if (f->storedAlphaIsOne) {
            PendingClear clear;
            RenderTargetDesc target;
            target.color[0].texture = rb->mtl;
            target.width = static_cast<uint32_t>(width);
            target.height = static_cast<uint32_t>(height);
            target.samples = static_cast<uint32_t>(std::max<GLsizei>(1, actual));
            clear.colorMask = 1;
            clear.color[0] = MTLClearColorMake(0, 0, 0, 1);
            ctx.stream().clearWithLoadAction(target, clear);
        }
    }
    for (Framebuffer* fb : {ctx.state.drawFramebuffer.get(), ctx.state.readFramebuffer.get()})
        if (fb) framebufferChanged(ctx, *fb);
}

void RenderbufferStorage(Context& ctx, GLenum target, GLenum internalformat, GLsizei width, GLsizei height) {
    RenderbufferStorageMultisample(ctx, target, 0, internalformat, width, height);
}

void GetRenderbufferParameteriv(Context& ctx, GLenum target, GLenum pname, GLint* params) {
    if (target != GL_RENDERBUFFER) return ctx.setError(GL_INVALID_ENUM);
    Renderbuffer* rb = ctx.state.renderbuffer.get();
    if (!rb) return ctx.setError(GL_INVALID_OPERATION);
    if (!params) return;
    const FormatInfo* f = rb->format;
    switch (pname) {
    case GL_RENDERBUFFER_WIDTH: *params = rb->width; return;
    case GL_RENDERBUFFER_HEIGHT: *params = rb->height; return;
    case GL_RENDERBUFFER_INTERNAL_FORMAT: *params = static_cast<GLint>(rb->internalFormat); return;
    case GL_RENDERBUFFER_SAMPLES: *params = rb->samples > 1 ? rb->samples : 0; return;
    case GL_RENDERBUFFER_RED_SIZE: *params = f ? f->redBits : 0; return;
    case GL_RENDERBUFFER_GREEN_SIZE: *params = f ? f->greenBits : 0; return;
    case GL_RENDERBUFFER_BLUE_SIZE: *params = f ? f->blueBits : 0; return;
    case GL_RENDERBUFFER_ALPHA_SIZE: *params = f ? f->alphaBits : 0; return;
    case GL_RENDERBUFFER_DEPTH_SIZE: *params = f ? f->depthBits : 0; return;
    case GL_RENDERBUFFER_STENCIL_SIZE: *params = f ? f->stencilBits : 0; return;
    default: return ctx.setError(GL_INVALID_ENUM);
    }
}

// ---------------------------------------------------- draw/read buffers

void DrawBuffers(Context& ctx, GLsizei n, const GLenum* bufs) {
    if (n < 0 || n > ContextState::kMaxDrawBuffers) return ctx.setError(GL_INVALID_VALUE);
    Framebuffer* fb = ctx.state.drawFramebuffer.get();
    if (!fb) {
        if (n != 1) return ctx.setError(GL_INVALID_OPERATION);
        const GLenum b = bufs[0];
        if (b != GL_NONE && b != GL_BACK && b != GL_FRONT && b != GL_FRONT_AND_BACK && b != GL_LEFT &&
            b != GL_BACK_LEFT && b != GL_FRONT_LEFT)
            return ctx.setError(b >= GL_COLOR_ATTACHMENT0 && b < GL_COLOR_ATTACHMENT0 + 32 ? GL_INVALID_OPERATION : GL_INVALID_ENUM);
        ctx.state.defaultDrawBuffer = b;
        ctx.markDirty(kDirtyFramebuffer);
        return;
    }
    for (GLsizei i = 0; i < n; ++i) {
        const GLenum b = bufs[i];
        if (b == GL_NONE) continue;
        if (b < GL_COLOR_ATTACHMENT0 || b >= GL_COLOR_ATTACHMENT0 + 32)
            return ctx.setError(b == GL_BACK || b == GL_FRONT || b == GL_FRONT_AND_BACK || b == GL_LEFT ? GL_INVALID_OPERATION : GL_INVALID_ENUM);
        if (b >= GL_COLOR_ATTACHMENT0 + Framebuffer::kMaxColor) return ctx.setError(GL_INVALID_OPERATION);
        if (b != GL_COLOR_ATTACHMENT0 + static_cast<GLenum>(i)) return ctx.setError(GL_INVALID_OPERATION);
    }
    for (int i = 0; i < Framebuffer::kMaxColor; ++i) fb->drawBuffers[i] = i < n ? bufs[i] : GL_NONE;
    framebufferChanged(ctx, *fb);
}

void DrawBuffer(Context& ctx, GLenum buf) {
    Framebuffer* fb = ctx.state.drawFramebuffer.get();
    if (!fb) return DrawBuffers(ctx, 1, &buf);
    if (buf != GL_NONE && (buf < GL_COLOR_ATTACHMENT0 || buf >= GL_COLOR_ATTACHMENT0 + Framebuffer::kMaxColor))
        return ctx.setError(buf >= GL_COLOR_ATTACHMENT0 && buf < GL_COLOR_ATTACHMENT0 + 32 ? GL_INVALID_OPERATION : GL_INVALID_OPERATION);
    // DrawBuffer on an FBO routes fragment colour 0 to `buf` for every draw buffer slot.
    for (int i = 0; i < Framebuffer::kMaxColor; ++i) fb->drawBuffers[i] = i == 0 ? buf : GL_NONE;
    framebufferChanged(ctx, *fb);
}

void ReadBuffer(Context& ctx, GLenum src) {
    Framebuffer* fb = ctx.state.readFramebuffer.get();
    if (!fb) {
        if (src != GL_NONE && src != GL_BACK && src != GL_FRONT && src != GL_LEFT && src != GL_BACK_LEFT && src != GL_FRONT_LEFT)
            return ctx.setError(src >= GL_COLOR_ATTACHMENT0 && src < GL_COLOR_ATTACHMENT0 + 32 ? GL_INVALID_OPERATION : GL_INVALID_ENUM);
        ctx.state.defaultReadBuffer = src;
        return;
    }
    if (src != GL_NONE && (src < GL_COLOR_ATTACHMENT0 || src >= GL_COLOR_ATTACHMENT0 + Framebuffer::kMaxColor))
        return ctx.setError(src >= GL_COLOR_ATTACHMENT0 && src < GL_COLOR_ATTACHMENT0 + 32 ? GL_INVALID_VALUE
                            : (src == GL_BACK || src == GL_FRONT || src == GL_LEFT) ? GL_INVALID_OPERATION
                                                                                     : GL_INVALID_ENUM);
    fb->readBuffer = src;
    framebufferChanged(ctx, *fb);
}

// ------------------------------------------------------------ ReadPixels

void ReadPixels(Context& ctx, GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void* pixels) {
    if (width < 0 || height < 0) return ctx.setError(GL_INVALID_VALUE);
    if (!validTransferFormatType(format, type, ctx.isCompat())) {
        if (groupBytes(format, type) == 0 && !validTransferFormatType(format, GL_UNSIGNED_BYTE, ctx.isCompat()))
            return ctx.setError(GL_INVALID_ENUM);
        return ctx.setError(GL_INVALID_OPERATION);
    }
    Framebuffer* fb = ctx.state.readFramebuffer.get();
    if (completeness(ctx, fb) != GL_FRAMEBUFFER_COMPLETE) return ctx.setError(GL_INVALID_FRAMEBUFFER_OPERATION);
    const bool depth = format == GL_DEPTH_COMPONENT, stencil = format == GL_STENCIL_INDEX, depthStencil = format == GL_DEPTH_STENCIL;
    id<MTLTexture> source = nil;
    uint32_t level = 0, slice = 0;
    GLenum internalFormat = 0;
    int levelWidth = 0, levelHeight = 0;
    if (depth || stencil || depthStencil) {
        const Attachment* a = nullptr;
        if (fb) {
            a = depth ? &fb->depth : &fb->stencil;
            if (depthStencil && !(fb->depth.attached() && fb->stencil.attached())) return ctx.setError(GL_INVALID_OPERATION);
            if (!a->attached()) return ctx.setError(GL_INVALID_OPERATION);
            GLsizei w, h, s;
            bool fl;
            const FormatInfo* f = attachmentFormat(*a, w, h, s, fl);
            if (!f || s > 1) return ctx.setError(GL_INVALID_OPERATION);
            if (a->type == GL_TEXTURE) {
                ensureTextureStorage(ctx, *a->texture);
                source = a->texture->mtl;
                level = static_cast<uint32_t>(a->level);
                slice = a->cubeFace ? a->cubeFace - GL_TEXTURE_CUBE_MAP_POSITIVE_X : static_cast<uint32_t>(a->layer);
            } else {
                source = a->renderbuffer->mtl;
            }
            internalFormat = f->internalFormat;
            levelWidth = w;
            levelHeight = h;
        } else {
            Surface* surface = ctx.readSurface();
            if (!surface || (depth && !surface->hasDepth()) || (stencil && !surface->hasStencil()) ||
                (depthStencil && !(surface->hasDepth() && surface->hasStencil())))
                return ctx.setError(GL_INVALID_OPERATION);
            if (surface->samples() > 1) return ctx.setError(GL_INVALID_OPERATION);
            source = surface->depthStencil();
            internalFormat = surface->hasStencil() ? GL_DEPTH24_STENCIL8 : surface->pixelFormat().depth_bits == 16 ? GL_DEPTH_COMPONENT16 : GL_DEPTH_COMPONENT24;
            levelWidth = surface->width();
            levelHeight = surface->height();
        }
    } else {
        if (fb) {
            if (fb->readBuffer == GL_NONE) return ctx.setError(GL_INVALID_OPERATION);
            GLsizei w, h, s;
            bool fl;
            const FormatInfo* f = attachmentFormat(*attachmentPoint(*fb, fb->readBuffer), w, h, s, fl);
            if (f && s > 1) return ctx.setError(GL_INVALID_OPERATION);
            if (f && !transferCompatible(format, type, *f)) return ctx.setError(GL_INVALID_OPERATION);
        } else if (ctx.state.defaultReadBuffer == GL_NONE) {
            return ctx.setError(GL_INVALID_OPERATION);
        } else if (format == GL_RGBA_INTEGER || format == GL_RED_INTEGER || format == GL_RG_INTEGER ||
                   format == GL_RGB_INTEGER || format == GL_BGRA_INTEGER || format == GL_BGR_INTEGER) {
            return ctx.setError(GL_INVALID_OPERATION);
        }
        source = ctx.draw().readColorTexture(level, slice, internalFormat, levelWidth, levelHeight);
    }
    if (!source) return ctx.setError(GL_INVALID_OPERATION);
    const FormatInfo* info = formatInfo(internalFormat);
    if (!info) return ctx.setError(GL_INVALID_OPERATION);

    PixelStoreParams ps;
    const PixelStore& pack = ctx.state.pack;
    ps.alignment = pack.alignment;
    ps.rowLength = pack.rowLength;
    ps.skipPixels = pack.skipPixels;
    ps.skipRows = pack.skipRows;
    ps.swapBytes = pack.swapBytes;
    Buffer* pbo = ctx.state.pixelPackBuffer.get();
    uint8_t* destination = static_cast<uint8_t*>(pixels);
    if (pbo) {
        const TransferLayout layout = transferLayout(format, type, width, height, 1, ps);
        const uintptr_t offset = reinterpret_cast<uintptr_t>(pixels);
        if (pbo->mapped || !pbo->mtl || offset + layout.bytesNeeded > static_cast<size_t>(pbo->size))
            return ctx.setError(GL_INVALID_OPERATION);
        ctx.stream().waitFor(pbo->lastUseSerial);
        destination = pbo->contents() + offset;
        ++pbo->version;
    }
    if (!destination || width == 0 || height == 0) return;
    // Clip to the source; pixels outside are left untouched.
    const int x0 = std::max(0, x), y0 = std::max(0, y);
    const int x1 = std::min(levelWidth, x + width), y1 = std::min(levelHeight, y + height);
    if (x1 <= x0 || y1 <= y0) return;
    const int w = x1 - x0, h = y1 - y0;
    // Copy the region to CPU-visible memory.
    Storage storage = info->storage;
    MTLBlitOption option = MTLBlitOptionNone;
    size_t texelBytes = storageBytes(storage);
    FormatInfo readInfo = *info;
    if (storage == Storage::Depth32Float_Stencil8) {
        if (stencil) {
            option = MTLBlitOptionStencilFromDepthStencil;
            readInfo.storage = Storage::Stencil8;
            texelBytes = 1;
        } else if (depth) {
            option = MTLBlitOptionDepthFromDepthStencil;
            readInfo.storage = Storage::Depth32Float;
            texelBytes = 4;
        }
    }
    std::vector<uint8_t> texels;
    auto readRegion = [&](MTLBlitOption opt, size_t bytes) {
        const size_t rowBytes = static_cast<size_t>(w) * bytes;
        id<MTLBuffer> staging = [Device::get().mtl() newBufferWithLength:rowBytes * static_cast<size_t>(h)
                                                                 options:MTLResourceStorageModeShared];
        id<MTLBlitCommandEncoder> blit = ctx.stream().blit();
        [blit copyFromTexture:source sourceSlice:slice sourceLevel:level sourceOrigin:MTLOriginMake(x0, y0, 0)
                   sourceSize:MTLSizeMake(w, h, 1) toBuffer:staging destinationOffset:0 destinationBytesPerRow:rowBytes
        destinationBytesPerImage:rowBytes * static_cast<size_t>(h) options:opt];
        ctx.stream().finish();
        return staging;
    };
    if (depthStencil && storage == Storage::Depth32Float_Stencil8) {
        id<MTLBuffer> d = readRegion(MTLBlitOptionDepthFromDepthStencil, 4);
        id<MTLBuffer> s = readRegion(MTLBlitOptionStencilFromDepthStencil, 1);
        texels.resize(static_cast<size_t>(w) * h * 8);
        for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
            std::memcpy(&texels[i * 8], static_cast<uint8_t*>(d.contents) + i * 4, 4);
            texels[i * 8 + 4] = static_cast<uint8_t*>(s.contents)[i];
        }
    } else {
        id<MTLBuffer> staging = readRegion(option, texelBytes);
        texels.assign(static_cast<uint8_t*>(staging.contents),
                      static_cast<uint8_t*>(staging.contents) + texelBytes * static_cast<size_t>(w) * h);
    }
    PixelStoreParams region = ps;
    region.skipPixels += x0 - x;
    region.skipRows += y0 - y;
    if (region.rowLength == 0) region.rowLength = width;
    const bool clampColor = ctx.state.clampReadColorMode == GL_TRUE ||
                            (ctx.state.clampReadColorMode == GL_FIXED_ONLY &&
                             (info->type == ComponentType::UnsignedNormalized || info->type == ComponentType::SignedNormalized));
    packPixels(texels.data(), readInfo, w, h, 1, format, type, region, clampColor, destination);
}

// ------------------------------------------------------------ sample positions

void GetMultisamplefv(Context& ctx, GLenum pname, GLuint index, GLfloat* val) {
    if (pname != GL_SAMPLE_POSITION) return ctx.setError(GL_INVALID_ENUM);
    RenderTargetDesc target;
    TargetFormats formats;
    if (!ctx.draw().drawTarget(target, formats, false)) {
        if (index >= 1) return ctx.setError(GL_INVALID_VALUE);
        val[0] = val[1] = 0.5f;
        return;
    }
    const uint32_t samples = std::max(1u, target.samples);
    if (index >= samples) return ctx.setError(GL_INVALID_VALUE);
    if (samples == 1) {
        val[0] = val[1] = 0.5f;
        return;
    }
    MTLSamplePosition positions[16];
    [Device::get().mtl() getDefaultSamplePositions:positions count:samples];
    // Metal positions are in a top-left origin pixel; GL's sample y grows upwards,
    // which is the same direction as Metal rows here (GL row 0 = Metal row 0).
    val[0] = positions[index].x;
    val[1] = positions[index].y;
}

}  // namespace wmgl::api
