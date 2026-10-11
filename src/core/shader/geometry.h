// Geometry shader emulation (GL 3.2+; Metal has no geometry stage).
//
// A draw with a geometry shader runs in three steps:
//
//   1. capture: the program's vertex shader, linked with transform-feedback
//      style capture code (transform_feedback.h), writes every output the
//      geometry shader reads for the vertices of each input primitive, in
//      primitive order, to a buffer;
//   2. geometry: the geometry shader runs as a compute kernel, one thread per
//      input primitive. EmitVertex() writes the current outputs to a fixed
//      slot (primitive * max_vertices + n) and records the slot in an index
//      list (2 * max_vertices entries per primitive, padded with restart
//      indices); EndPrimitive() of a strip records a restart index;
//   3. draw: a generated pass-through vertex shader reads the output vertex
//      for each index and feeds the program's fragment shader.
//
// The kernel is generated from GLSL that SPIRV-Cross emits for the linked
// geometry stage, so its default uniform block keeps the program's layout
// and uniform blocks and samplers keep their names.
//
// Transform feedback and PRIMITIVES_GENERATED read the emitted primitives
// back on the CPU after the draw (DrawState::drawGeometry). Not emulated:
// instanced geometry shaders (GL 4.0 invocations) and a gl_PrimitiveID
// written for the fragment stage (the fragment stage sees Metal's).
#pragma once

#include <string>
#include <vector>

#include "core/shader/compiler.h"
#include "core/shader/transform_feedback.h"

namespace wmgl::shader {

// A geometry shader input or output variable (block members are flattened).
struct GeometryVar {
    std::string glslType;     // "vec4", "int", "mat3" ...
    GLenum type = 0;
    std::string name;         // GLSL access path ("col", "gs_in" + member below)
    std::string member;       // block member name, empty for plain variables
    std::string blockName;    // interface block name for block members
    int arraySize = 0;        // array size (0 = not an array); for inputs, of a block member (not the vertex index)
    std::string declaration;  // outputs: the original declaration line(s)
};

struct GeometryShaderInfo {
    GLenum inputType = 0;     // GL_POINTS, GL_LINES, GL_LINES_ADJACENCY, GL_TRIANGLES, GL_TRIANGLES_ADJACENCY
    GLenum outputType = 0;    // GL_POINTS, GL_LINE_STRIP, GL_TRIANGLE_STRIP
    int verticesIn = 0;
    int maxVertices = 0;
    std::vector<GeometryVar> inputs;   // user inputs (each an array of verticesIn elements)
    std::vector<GeometryVar> outputs;  // user outputs
    bool readsPointSize = false, readsClipDistance = false;
    int inputClipDistances = 0;
    bool writesPointSize = false, writesLayer = false, writesPrimitiveId = false;
    int outputClipDistances = 0;
    std::string globals;      // other global declarations (uniforms, constants, structs)
    std::string functions;    // function definitions, built-ins renamed
};

// Parses SPIRV-Cross GLSL for a geometry stage. Returns false (with `log`) on
// constructs the emulation does not support.
bool parseGeometryGlsl(const std::string& glsl, GeometryShaderInfo& info, std::string& log);

// Varyings the capture pass must write: gl_Position, the user inputs and the
// built-ins the shader reads, named as transform feedback varyings.
std::vector<std::string> geometryCaptureVaryings(const GeometryShaderInfo& info);

struct GeometrySources {
    std::string compute;      // the kernel (GLSL 4.50 compute)
    std::string passthrough;  // the drawing vertex shader (GLSL 4.50)
    uint32_t outputStride = 0;  // 32-bit words per output vertex
    // The output vertex record by GL varying name ("gl_Position" first).
    std::vector<GeometryEmulation::Field> fields;
};

// `capture` is the layout of the captured vertex-shader outputs.
GeometrySources geometrySources(const GeometryShaderInfo& info, const LinkResult& capture);

// GLSL helpers shared with the tessellation emulation: the GL type of a GLSL
// type name (0 if unsupported), and an expression loading a value of `type`
// from 32-bit words buffer[base + i].
GLenum glslTypeFromName(const std::string& type);
std::string glslLoadValue(const std::string& type, GLenum glType, const std::string& buffer, const std::string& base);

}  // namespace wmgl::shader
