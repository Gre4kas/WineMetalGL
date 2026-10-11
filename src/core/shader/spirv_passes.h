// Small SPIR-V rewrites applied before SPIRV-Cross translates a stage to MSL.
#pragma once

#include <cstdint>
#include <vector>

namespace wmgl::shader {

// textureGatherOffsets (GL 4.0, GL_ARB_gpu_shader5): Metal's gather takes a
// single offset, so an OpImage(Dref)Gather with ConstOffsets becomes four
// gathers with one ConstOffset each, keeping the i0j0 texel (component w)
// of each, as GL defines the four-offset form.
std::vector<uint32_t> lowerGatherOffsets(std::vector<uint32_t> spirv);

}  // namespace wmgl::shader
