// Context versions above what the core implements, for development only.
#pragma once

#include <string>

namespace wmgl {

// WINEMETALGL_DEV_MAX_VERSION=4.N (development and CTS coverage runs only,
// never a user setting): contexts get version 4.N although the core does not
// implement all of it, and GL_RENDERER ends in " (dev 4.N)" so results
// recorded this way cannot pass as conformance evidence. Returns 10 * major +
// minor, or 0 when unset or invalid.
int devMaxVersion();

// GL_RENDERER: WINEMETALGL_RENDERER, else "<GPU> (WineMetalGL)", plus the
// development suffix.
const std::string& rendererString();

}  // namespace wmgl
