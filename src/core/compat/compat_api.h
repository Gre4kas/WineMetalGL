// Compatibility profile hooks called by the core API (W22).
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/gl/types.h"
#include "core/state.h"

namespace wmgl {

class Context;

namespace api {

// glEnable/glDisable/glIsEnabled of fixed-function capabilities (lighting,
// texture targets, fog, ...). False if `cap` is not one.
bool setFixedCapability(Context& ctx, GLenum cap, bool on);
bool getFixedCapability(Context& ctx, GLenum cap, bool& value);
// glIsEnabled of the conventional client arrays (GL_VERTEX_ARRAY, ...).
bool getClientCapability(Context& ctx, GLenum cap, bool& value);

// glGet* of compatibility-profile state. Writes up to 16 values to `out`
// and returns how many, or 0 if `pname` is not compatibility state.
// `kind` tells the caller how integer queries convert the values: 'c' for
// colours (mapped to the integer range), 'f' otherwise. Returns -1 after
// setting an error (state of a texture unit past MAX_TEXTURE_COORDS).
int getFixedState(Context& ctx, GLenum pname, double* out, char& kind);

// glPushAttrib / glPushClientAttrib stack depths (compat/fixed_attrib.mm).
constexpr int kMaxAttribStackDepth = 16;
int attribStackDepth(const Context& ctx, bool client);

// Unpacks a GL bitmap (1 bit per pixel, unpack state `s`) to one byte per
// pixel: 255 for set bits (api/fixed_raster.mm).
std::vector<uint8_t> unpackBitmap(const PixelStore& s, const uint8_t* bits, GLsizei width, GLsizei height);
// Bytes a GL_BITMAP image of width x height spans with pixel store state `s`.
size_t bitmapBytes(const PixelStore& s, GLsizei width, GLsizei height);
// The bits glBitmap or glPolygonStipple reads: `bits` itself, or that
// offset into the bound pixel unpack buffer. False (GL_INVALID_OPERATION)
// if the buffer is mapped or the image would read past its end.
bool bitmapSource(Context& ctx, const GLubyte* bits, GLsizei width, GLsizei height, const uint8_t*& out);

// COLOR_MATERIAL: while enabled, the material parameters selected by
// glColorMaterial track the current colour (GL 3.3 compatibility section
// 2.13.3). Called whenever the current colour or the tracking state changes.
void trackColorMaterial(Context& ctx);

// glGetPointerv of the conventional arrays; false if `pname` is not one.
bool getConventionalPointer(Context& ctx, GLenum pname, void** params);

// Immediate mode: true between glBegin and glEnd, where most commands are
// invalid (GL 3.3 compatibility section 2.6.3).
bool insideBeginEnd(const Context& ctx);

}  // namespace api
}  // namespace wmgl
