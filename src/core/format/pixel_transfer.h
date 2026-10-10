// Pixel transfer: client memory <-> texture storage (GL 3.3 sections 3.7, 4.3).
//
// Everything goes through a canonical texel: four channels as double (exact
// for float, normalized and 32-bit integer values), plus depth and stencil.
// Pure C++ with no Metal dependency; tests/host/pixel_tests.cpp covers it.
#pragma once

#include <cstddef>
#include <cstdint>

#include "core/format/formats.h"

namespace wmgl {

struct Texel {
    double c[4] = {0, 0, 0, 1};
    double depth = 0;
    uint32_t stencil = 0;
};

struct PixelStoreParams {
    int alignment = 4, rowLength = 0, imageHeight = 0, skipPixels = 0, skipRows = 0, skipImages = 0;
    bool swapBytes = false;
};

// True if (format, type) is a valid client pixel combination in GL 3.3.
bool validTransferFormatType(GLenum format, GLenum type, bool compatibility);
// True if the combination may be used with a texture/framebuffer of `info`.
bool transferCompatible(GLenum format, GLenum type, const FormatInfo& info);

// Bytes per pixel group of (format, type), 0 if invalid.
size_t groupBytes(GLenum format, GLenum type);

struct TransferLayout {
    size_t groupBytes = 0;
    size_t rowStride = 0;      // bytes between rows
    size_t imageStride = 0;    // bytes between images (3D)
    size_t skipBytes = 0;      // offset of the first group
    size_t bytesNeeded = 0;    // last byte touched + 1 (for PBO bounds checks)
};
TransferLayout transferLayout(GLenum format, GLenum type, int width, int height, int depth, const PixelStoreParams& ps);

// Converts client pixels to tightly packed storage texels for `info`.
// out must hold width*height*depth*storageBytes(info.storage) bytes.
void unpackPixels(const void* src, GLenum format, GLenum type, int width, int height, int depth,
                  const PixelStoreParams& ps, const FormatInfo& info, void* out);

// Converts tightly packed storage texels of `info` to client pixels.
// `clampColor` applies GL's read colour clamping.
void packPixels(const void* storage, const FormatInfo& info, int width, int height, int depth, GLenum format,
                GLenum type, const PixelStoreParams& ps, bool clampColor, void* dst);

// Single-texel conversions (exposed for tests and the clear/compressed paths).
void decodeStorage(Storage storage, const uint8_t* texel, Texel& out);
void encodeStorage(Storage storage, const Texel& in, uint8_t* texel);

// RGTC (BC4/BC5) block compression between canonical RGBA float texels
// (tightly packed, 4 floats per texel) and blocks. Images are compressed
// one 4x4 block at a time; partial edge blocks repeat the edge texels.
bool isRgtcStorage(Storage storage);
void encodeRgtc(Storage storage, const float* rgba, int width, int height, int depth, uint8_t* blocks);
void decodeRgtc(Storage storage, const uint8_t* blocks, int width, int height, int depth, float* rgba);

float halfToFloat(uint16_t h);
uint16_t floatToHalf(float f);

}  // namespace wmgl
