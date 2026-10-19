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

// GL_RENDERER: WINEMETALGL_RENDERER, else rendererName(the Metal device's
// name), plus the development suffix.
const std::string& rendererString();

// "<GPU> (WineMetalGL)" without the "Apple " of Metal's device names: "M4
// (WineMetalGL)". Engines take "Apple" in GL_RENDERER for a tile-based mobile
// or native macOS GPU and change how they render: Godot 3 turns its depth
// pre-pass off (rendering/quality/depth_prepass/disable_for_vendors), and its
// depth_draw_alpha_prepass materials then write no depth at all (Cassette
// Beasts' trees, outlines and fence chains, drawn over by what came after
// them). Under Wine they are Windows programs on a desktop GL.
inline std::string rendererName(const std::string& device) {
    static const std::string kVendor = "Apple ";
    const bool apple = device.compare(0, kVendor.size(), kVendor) == 0 && device.size() > kVendor.size();
    return (apple ? device.substr(kVendor.size()) : device) + " (WineMetalGL)";
}

}  // namespace wmgl
