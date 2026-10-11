// Block compression of uncompressed uploads (glTexImage/glTexSubImage with
// a specific compressed internal format): S3TC, BPTC, ETC2 and EAC. Each
// encoder writes a valid block close to the texels; RGTC has its own
// (pixel_transfer.h encodeRgtc).
#pragma once

#include <cstdint>

#include "core/format/formats.h"

namespace wmgl {

// Whether encodeBlocks can produce `storage`.
bool blockEncodable(Storage storage);

// Encodes RGBA float texels (width x height x depth, row-major) into the
// storage's 4x4 blocks, slice by slice; edge blocks repeat the last texels.
void encodeBlocks(Storage storage, const float* rgba, int width, int height, int depth, uint8_t* blocks);

}  // namespace wmgl
