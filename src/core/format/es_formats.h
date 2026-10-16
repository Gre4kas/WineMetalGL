// OpenGL ES texture and renderbuffer format rules (OpenGL ES 3.2 section 8.4
// tables 8.2 and 8.3, section 9.4 table 9.3 / 8.10, and the extensions ES
// contexts advertise: OES_texture_float / half_float, OES_depth_texture,
// OES_packed_depth_stencil, EXT_texture_rg, EXT_sRGB, EXT_texture_format_BGRA8888,
// EXT_texture_type_2_10_10_10_REV, EXT_texture_norm16, EXT_color_buffer_float /
// half_float, EXT_render_snorm, OES_rgb8_rgba8, OES_depth24/32, OES_texture_stencil8).
//
// Pure C++ (host-testable), like formats.h.
#pragma once

#include "core/format/formats.h"
#include "core/gl/types.h"

namespace wmgl::es {

// OES_texture_half_float's type token; OpenGL ES 3's HALF_FLOAT otherwise.
constexpr GLenum kHalfFloatOes = 0x8D61;
// EXT_texture_format_BGRA8888 / EXT_read_format_bgra.
constexpr GLenum kBgra = 0x80E1;

// glTexImage*: the sized internal format a (internalformat, format, type)
// makes, or 0 with `error` set to the GL error (INVALID_ENUM for unknown
// format or type enums, INVALID_VALUE for an unknown internal format,
// INVALID_OPERATION for a combination the tables do not list).
// `type` comes back as GL_HALF_FLOAT for HALF_FLOAT_OES.
GLenum texImageFormat(GLenum internalformat, GLenum format, GLenum& type, GLenum& error);

// glTexSubImage*: whether (format, type) may update an image of
// `internalformat` (the sized format the image has), with the error to
// report otherwise.
bool texSubImageCompatible(GLenum internalformat, GLenum format, GLenum& type, GLenum& error);

// Sized internal formats glTexStorage* and glRenderbufferStorage take.
bool textureStorageFormat(GLenum internalformat);
bool renderbufferFormat(GLenum internalformat);

// Color-renderable in OpenGL ES (with EXT_color_buffer_float / half_float,
// EXT_texture_norm16 and EXT_render_snorm), and depth/stencil-renderable.
bool colorRenderable(GLenum internalformat);
bool depthOrStencilRenderable(GLenum internalformat);

// glReadPixels: whether (format, type) reads a buffer of `internalformat`
// (sized), besides IMPLEMENTATION_COLOR_READ_FORMAT/TYPE.
bool readPixelsCompatible(GLenum internalformat, GLenum format, GLenum type);

// glCopyTexImage*: the format of the new image when the read buffer has
// `source`, or 0 with `error` set (INVALID_VALUE for an internal format
// glCopyTexImage does not take, INVALID_OPERATION for a conversion OpenGL ES
// does not allow: section 8.6, table 8.21). Unsized formats take their sizes
// from the source; sized ones must match its components exactly.
GLenum copyTexImageFormat(const FormatInfo& source, GLenum internalformat, GLenum& error);

// glCopyTexSubImage*: whether a read buffer of `source` may update an image of `dest`.
bool copyTexSubImageCompatible(const FormatInfo& source, const FormatInfo& dest);

// A format or type enum glTexImage/glReadPixels know at all (else INVALID_ENUM).
bool isFormat(GLenum format);
bool isType(GLenum type);

}  // namespace wmgl::es
