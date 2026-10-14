// Sampler border colours Metal cannot give (it has three: transparent black,
// opaque black and opaque white), and integer textures' border colours:
// clamp-to-border emulated in the shader.
//
// Every stage is translated with BorderMsl, which wraps each sample and
// gather of a non-arrayed 1D, 2D, 3D or rectangle texture in a macro named
// after the texture's Metal index (wmgl_bcs_N for samples, wmgl_bcg_N for
// gathers). The default definitions (addDefaultBorderMacros) keep only the
// sampled value, so the common path costs nothing. A draw that binds such a
// border uses a variant of the function (mslWithBorders) whose macros for
// those textures substitute the border colour where the filter footprint
// leaves the image along a CLAMP_TO_BORDER axis. The same path filters
// every lookup of a texture whose sampler has a TEXTURE_LOD_BIAS on Metal
// without sampler LOD bias (macOS 15), applying the bias itself (without
// anisotropy; cube maps keep the unbiased lookup).
#pragma once

#include <spirv_msl.hpp>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace wmgl::shader {

class BorderMsl : public spirv_cross::CompilerMSL {
public:
    using CompilerMSL::CompilerMSL;
    // Metal texture index of each non-array sampled image variable.
    std::unordered_map<uint32_t, uint32_t> textureSlots;

protected:
    std::string to_texture_op(const spirv_cross::Instruction& i, bool sparse, bool* forward,
                              spirv_cross::SmallVector<uint32_t>& inherited_expressions) override;
};

// Defines the wrapping macros the MSL uses (after "using namespace metal;").
void addDefaultBorderMacros(std::string& msl);

// One texture a draw samples with an emulated border.
struct BorderSlot {
    uint32_t texture = 0;     // Metal texture index in the stage
    char kind = 'f';          // texel type: 'f' (float, depth), 'i', 'u'
    uint32_t axes = 0;        // bits 0-2: S, T, R wrap CLAMP_TO_BORDER
    uint32_t wrap = 0;        // 4 bits per axis: 0 border, 1 repeat, 2 mirrored repeat, 3 clamp to edge, 4 mirror clamp to edge
    bool magLinear = false, minLinear = false;
    int mip = 0;              // 0: no mipmaps, 1: nearest, 2: linear
    bool rect = false;        // unnormalized coordinates
    float minLod = -1000.0f, maxLod = 1000.0f, lodBias = 0.0f;
    uint32_t compare = 0;     // GL compare function of a shadow sampler, 0: none
    bool always = false;      // filter every lookup here, not only at the border: the LOD bias
                              // where Metal's samplers have none (DeviceCaps::samplerLodBias)
    uint32_t border[4] = {};  // the GL border colour after the texture's swizzle (bits)
};

// The macro definitions a draw's border textures need in one stage
// (deterministic text: also the variant's cache key).
std::string borderDefinitions(const std::vector<BorderSlot>& slots, bool fragment);

// `msl` with the definitions of `definitions` in place of the defaults, and
// the emulation's helper functions.
std::string mslWithBorders(const std::string& msl, const std::string& definitions);

// Definitions are numbered from 1 as they first appear (pipeline keys hold
// the number); 0 is none.
uint16_t borderVariantId(const std::string& definitions);
std::string borderVariantDefinitions(uint16_t id);

}  // namespace wmgl::shader
