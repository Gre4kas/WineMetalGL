// Double precision (GL_ARB_gpu_shader_fp64, GL_ARB_vertex_attrib_64bit) on
// Metal, which has no double type.
//
// lowerFp64 rewrites a SPIR-V module before SPIRV-Cross translates it to MSL:
//
//   - 64-bit floats become 64-bit unsigned integers holding their bit
//     patterns (same ids, so constants keep their values), and double
//     matrices become arrays of their columns. Row-major double matrices in
//     blocks become arrays of rows, read through a transposing load.
//   - Every floating-point operation on doubles becomes a call to a generated
//     stub function ("wmgl_f64_<n>"), keeping its result id. The stubs return
//     null; finishFp64Msl replaces their bodies with calls into the MSL
//     soft-float library (fp64_soft.h plus vector and matrix forms).
//   - Stage inputs and outputs holding doubles (vertex attributes, varyings)
//     become 32-bit unsigned vectors, two words per double, so Metal can pass
//     them; the shader works on private copies converted at entry and exit.
//     A double, dvec2, dvec3 and dvec4 take 1, 1, 2 and 2 locations, as in GL.
//
// Modules without doubles are returned unchanged (and finishFp64Msl leaves
// their MSL alone), so only shaders that use doubles pay.
//
// Pure C++: no Metal types, so it can be unit-tested on the host.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace wmgl::shader {

// Returns false (with a message in `log`) if the module uses doubles in a
// way the lowering does not support.
bool lowerFp64(std::vector<uint32_t>& spirv, std::string& log);

// Whether `spirv` declares a 64-bit float type.
bool usesFp64(const std::vector<uint32_t>& spirv);

// Completes MSL that SPIRV-Cross emitted for a module lowered by lowerFp64:
// inserts the soft-float library and points the stubs at it.
std::string finishFp64Msl(const std::string& msl);

// The MSL soft-float library (for tests).
const std::string& fp64MslLibrary();

}  // namespace wmgl::shader
