// Small SPIR-V rewrites applied before SPIRV-Cross translates a stage to MSL.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace wmgl::shader {

// textureGatherOffsets (GL 4.0, GL_ARB_gpu_shader5): Metal's gather takes a
// single offset, so an OpImage(Dref)Gather with ConstOffsets becomes four
// gathers with one ConstOffset each, keeping the i0j0 texel (component w)
// of each, as GL defines the four-offset form.
std::vector<uint32_t> lowerGatherOffsets(std::vector<uint32_t> spirv);

// Storage (image load/store) cube and cube array images become 2D array
// images: Metal has no texture atomics on cubes, and GL addresses cube
// images by (x, y, layer-face) anyway. The draw path binds a 2D array view.
// imageSize results are adjusted back: (w, h) for cubes, (w, h, layers / 6)
// for cube arrays.
std::vector<uint32_t> cubeStorageImagesAs2DArrays(std::vector<uint32_t> spirv);

// Arrays of arrays of opaque types and of stage inputs and outputs, which
// MSL cannot declare, become one-dimensional arrays: access chains through
// every dimension index them at ((i0 * d1 + i1) * d2 + ...). A variable used
// any other way (a partial access, a whole copy) is left as it is.
std::vector<uint32_t> flattenArraysOfArrays(std::vector<uint32_t> spirv);

// Storage images a stage only reads get NonWritable, and those it only
// writes NonReadable, so they become access::read or access::write Metal
// textures: a Metal function takes only 8 read_write textures. Atomics,
// images passed to functions and any other use keep both accesses.
std::vector<uint32_t> inferStorageImageAccess(std::vector<uint32_t> spirv);

// What the flat inputs of `fragment` are computed from in `vertex`, the
// stage that feeds them. When nothing that differs between the vertices of
// a primitive reaches them other than vertex attributes, `perVertex` is
// false and `attribs` lists those attributes (bit = GL location). With each
// of them per instance or disabled, the vertices of a primitive then give
// the same flat values, so its provoking vertex makes no difference.
// Uniforms, constants, buffer and texture reads, gl_InstanceID,
// gl_BaseVertex, gl_BaseInstance and gl_DrawID are the same for every
// vertex of a primitive; gl_VertexID or a memory write makes `perVertex`
// true. A branch condition counts as a source of the stores and return
// values whose running it decides (control dependence). `attribute`
// gives the locations (bit = GL location) of the vertex input named
// `name`, false for an unknown name.
struct FlatSources {
    bool perVertex = true;
    uint32_t attribs = 0;
};
FlatSources flatVaryingSources(const std::vector<uint32_t>& vertex, const std::vector<uint32_t>& fragment,
                               const std::function<bool(const std::string& name, uint32_t& locations)>& attribute);

}  // namespace wmgl::shader
