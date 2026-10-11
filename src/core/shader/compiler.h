// GLSL -> SPIR-V -> MSL for GL programs, plus the GL program interface.
//
// glslang parses each shader with relaxed Vulkan rules (loose uniforms go
// into one default uniform block), links the program, and reflects the GL
// interface with liveness analysis (inactive uniforms are not reported, as GL
// requires). SPIRV-Cross then emits MSL with WineMetalGL's per-stage Metal
// slot layout (see kSlot* below) and the GL-assigned attribute and
// fragment-output locations.
//
// Pure C++: no Metal types, so it can be unit-tested on the host.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "core/gl/types.h"

namespace wmgl::shader {

enum class Stage : uint8_t { Vertex = 0, TessControl, TessEval, Geometry, Fragment, Compute, Count };
constexpr int kStageCount = static_cast<int>(Stage::Count);

// Metal argument-table layout used by every generated stage function.
constexpr uint32_t kSlotDefaultUniforms = 0;   // buffer: default uniform block
constexpr uint32_t kSlotDriverUniforms = 1;    // buffer: WineMetalGL driver constants
constexpr uint32_t kSlotFirstUniformBlock = 2; // buffers 2..15: uniform blocks used by the stage
constexpr uint32_t kMaxStageUniformBlocks = 14;
constexpr uint32_t kSlotFirstXfbBuffer = 16;   // buffers 16..19: transform feedback outputs
                                               // (geometry kernel: input, output, indices, control)
constexpr uint32_t kSlotFirstVertexBuffer = 20;  // buffers 20..29: vertex buffers
constexpr uint32_t kMaxVertexBufferSlots = 10;
constexpr uint32_t kSlotXfbControl = 30;       // buffer 30: transform feedback capture control
constexpr uint32_t kMaxStageSamplers = 16;     // textures/samplers 0..15
// Tessellation emulation (tessellation.h): the control kernel reads the
// captured vertices and writes patch records through buffers 16..18; the
// evaluation vertex function, which has no vertex attributes, reads its
// domain points, patch records and control block from buffers 20..22.
constexpr uint32_t kSlotTessVertices = 20, kSlotTessPatches = 21, kSlotTessControl = 22;
// Atomic counter buffers (GL 4.2): buffers 20..27 of fragment and compute
// functions (the geometry kernel included), which bind no vertex buffers.
// A vertex function's one counter buffer takes the last vertex buffer slot
// (29), leaving its program 9 vertex buffers (DrawState::vertexBufferSlots).
constexpr uint32_t kSlotFirstAtomicCounterBuffer = 20;
constexpr uint32_t kSlotVertexAtomicCounterBuffer = kSlotFirstVertexBuffer + kMaxVertexBufferSlots - 1;
constexpr uint32_t kMaxVertexAtomicCounterBuffers = 1;
// Image uniforms (GL 4.2 image load/store): textures 16..23 of every stage.
constexpr uint32_t kSlotFirstImage = 16;
constexpr uint32_t kMaxStageImages = 8;
constexpr uint32_t kMaxStageAtomicCounterBuffers = 8;

struct CompileResult {
    bool ok = false;
    std::string log;
};

// Parses a single shader for glCompileShader (status and info log only).
CompileResult compile(Stage stage, const std::string& source);

struct ShaderSource {
    Stage stage;
    std::string source;
};

struct LinkInput {
    std::vector<ShaderSource> shaders;
    std::map<std::string, GLuint> attribBindings;
    std::map<std::string, std::pair<GLuint, GLuint>> fragDataBindings;  // name -> (location, index)
    std::vector<std::string> transformFeedbackVaryings;
    GLenum transformFeedbackMode = GL_INTERLEAVED_ATTRIBS;
    bool separable = false;
    bool internal = false;  // a generated stage (geometry emulation): no separable-varying renumbering
};

struct UniformInfo {
    std::string name;          // GL name, "a[0]" for arrays
    GLenum type = 0;           // GL_FLOAT_VEC4, GL_SAMPLER_2D, ...
    GLint size = 1;            // array size (1 for non-arrays)
    GLint location = -1;       // first location; element i is location + i (-1 for block members)
    int blockIndex = -1;       // -1 = default block
    GLint offset = -1;         // byte offset in its block
    GLint arrayStride = 0, matrixStride = 0;
    bool rowMajor = false;
    int samplerSlot = -1;      // first program sampler slot (elements are consecutive)
    int atomicBufferIndex = -1;  // atomic counters: index into LinkResult::atomicBuffers
    int imageSlot = -1;          // images: first program image slot (elements are consecutive)
    GLint explicitBinding = -1;  // samplers and images: layout(binding = N) (GL 4.2), else -1
    uint32_t stageMask = 0;    // stages referencing it
};

// An active atomic counter buffer (GL 4.2): the counters a program declares
// with one binding. glslang turns them into members of a storage block named
// "gl_AtomicCounterBlock_<binding>" at their offsets.
struct AtomicCounterBufferInfo {
    GLuint binding = 0;
    GLint dataSize = 0;               // bytes the counters need
    std::vector<int> activeUniforms;  // indices into uniforms
    uint32_t stageMask = 0;
    int stageSlot[kStageCount] = {-1, -1, -1, -1, -1, -1};  // Metal buffer index per stage
};
// The GL binding of an atomic counter storage block name, or -1.
int atomicCounterBlockBinding(const std::string& blockName);

struct UniformBlockInfo {
    std::string name;          // "Block" or "Block[2]"
    GLint dataSize = 0;
    std::vector<int> activeUniforms;  // indices into uniforms
    uint32_t stageMask = 0;
    int stageSlot[kStageCount] = {-1, -1, -1, -1, -1, -1};  // Metal buffer index per stage
    GLint explicitBinding = -1;  // layout(binding = N) (GL 4.2); -1 if none
};

struct AttribInfo {
    std::string name;
    GLenum type = 0;
    GLint size = 1;
    GLint location = -1;
    int locationCount = 1;     // matrices use one location per column
    bool array = false;        // declared as an array ("name[i]" names its elements)
    // A lowered legacy attribute (wmgl_Vertex for gl_Vertex, ...) at its
    // conventional-array location; the API reports it as gl_* at location -1.
    bool conventional = false;
};

// Conventional vertex arrays (compatibility profile) follow the 16 generic
// attributes: vertex, normal, colour, secondary colour, fog coordinate and
// 8 texture coordinates (core/compat/fixed_state.h).
// Two internal inputs of the fixed-function program follow (wide lines).
constexpr int kConventionalAttribFirst = 16, kConventionalAttribCount = 15;
// The location of a lowered legacy attribute ("wmgl_Vertex", ...), or -1.
int conventionalAttribLocation(const std::string& name);

struct FragOutputInfo {
    std::string name;
    GLenum type = 0;
    GLint location = 0;
    GLint index = 0;
    GLint size = 1;
};

// One image uniform element (GL 4.2): the unit it reads comes from the
// program's image unit values.
struct ImageSlotInfo {
    GLenum type = 0;           // GL image type (GL_IMAGE_2D, ...)
    GLenum format = 0;         // the uniform's format layout qualifier (GL_R32I, ...), 0 if none
    int stageTexture[kStageCount] = {-1, -1, -1, -1, -1, -1};  // Metal texture index per stage
};

struct SamplerSlotInfo {
    GLenum type = 0;           // GL sampler type
    int textureTarget = -1;    // TextureTarget index the sampler reads (core/state.h)
    bool shadow = false;
    int stageTexture[kStageCount] = {-1, -1, -1, -1, -1, -1};  // Metal texture/sampler index per stage
};

struct StageOutput {
    bool present = false;
    std::string msl;
    std::string mslNoRaster;  // vertex stage: variant returning void (rasterization disabled)
    std::string entryPoint;
    std::vector<uint32_t> spirv;
    bool usesDefaultUniforms = false;
    bool usesDriverUniforms = false;
    bool writesPointSize = false;
    bool discards = false;
    bool usesPointCoord = false;  // fragment: reads gl_PointCoord (flip flag in kSlotDriverUniforms)
    bool flatInputs = false;      // fragment: has flat inputs (provoking vertex matters)
    // Vertex stage: Metal [[attribute(n)]] -> GL location and input component type.
    // 'd' (64-bit, GL_ARB_vertex_attrib_64bit): the Metal attribute reads 16
    // raw bytes, `part` 0 or 1 of the element of GL attribute `attrib` (a
    // dvec3/dvec4 spans two locations; see shader/fp64.h).
    struct VertexInput {
        GLint location;
        int components;
        char kind;  // 'f', 'i', 'u', 'd'
        GLint attrib = -1;  // GL attribute supplying the data (-1: `location`)
        int part = 0;
        GLint source() const { return attrib >= 0 ? attrib : location; }
    };
    std::vector<VertexInput> vertexInputs;
    // Fragment stage: outputs written (location, index, component kind).
    struct FragOutput { GLint location; GLint index; char kind; };
    std::vector<FragOutput> fragOutputs;
};

struct TransformFeedbackVarying {
    std::string name;          // as given to glTransformFeedbackVaryings
    GLenum type = 0;           // element type
    GLint size = 1;            // elements captured (array size, or 1)
    int components = 0;        // total 32-bit components
    // Capture layout (32-bit words).
    int buffer = 0;
    uint32_t offset = 0;       // within one captured vertex in `buffer`
    std::string expression;    // GLSL expression of the first element ("gl_Position", "v", "v[2]")
    bool array = false;        // expression names a whole array (elements expression[i])
    // The captured output by its qualified name ("Block.member" for a member
    // of an anonymous block captured as "member"; see StageOutputVar).
    std::string source;
    int stream = 0;            // the vertex stream it comes from (geometry shaders)
    // Geometry emulation: word offset of the first element in the emitted
    // vertex record (kNotEmitted: the shader never writes it).
    uint32_t sourceOffset = 0;
    static constexpr uint32_t kNotEmitted = 0xffffffffu;
};

// Geometry shader emulation (see geometry.h). The program's vertex stage is
// replaced by its capture variant (xfbVertexCapture rules, one buffer of
// `inputStride` words per vertex); the kernel uses the program's uniform
// block and sampler slots recorded for Stage::Compute.
struct GeometryEmulation {
    bool active = false;
    GLenum inputType = 0, outputType = 0;
    int verticesIn = 0, maxVertices = 0;
    int invocations = 1;        // kernel threads per input primitive (gl_InvocationID)
    uint32_t inputStride = 0;   // 32-bit words per captured vertex
    uint32_t outputStride = 0;  // 32-bit words per emitted vertex
    struct Field {
        std::string name;       // GL varying name ("gl_Position", "col", "Block.member")
        GLenum type = 0;
        GLint size = 1;         // array elements
        uint32_t offset = 0;    // in words
        int stream = 0;         // vertex stream (GL 4.0)
    };
    std::vector<Field> outputs;  // emitted vertex layout
    // Vertex streams: emitted vertices record their stream at word
    // `streamOffset` (only stream 0 is rasterized; each transform feedback
    // buffer captures one stream, LinkResult::xfbStream).
    bool streams = false;
    uint32_t streamOffset = 0;
    StageOutput kernel;          // compute: one thread per input primitive
    StageOutput passthrough;     // vertex: draws the emitted vertices
};

struct LinkResult;

// Tessellation emulation (see tessellation.h). The program's vertex stage is
// replaced by its capture variant (one record of `inputStride` words per
// patch vertex); the control kernel uses the program's uniform block and
// sampler slots recorded for Stage::TessControl.
struct TessellationEmulation {
    bool active = false;
    bool hasControl = false;
    GLint verticesOut = 0;          // control: layout(vertices = N)
    GLenum mode = GL_TRIANGLES;     // GL_TRIANGLES, GL_QUADS or GL_ISOLINES
    GLenum spacing = GL_EQUAL;      // GL_EQUAL, GL_FRACTIONAL_EVEN or GL_FRACTIONAL_ODD
    GLenum order = GL_CCW;
    bool pointMode = false;
    uint32_t inputStride = 0;       // 32-bit words per captured vertex
    uint32_t vertexWords = 0;       // control: words per output vertex record
    uint32_t patchWords = 0;        // control: words of the patch section (levels first)
    StageOutput control;            // compute: one threadgroup per patch
    // The evaluation stage as a vertex shader, linked with the geometry
    // shader (if any) and the transform feedback varyings.
    std::shared_ptr<LinkResult> evaluation;
    uint32_t recordWords() const { return static_cast<uint32_t>(verticesOut) * vertexWords + patchWords; }
};

// Shader subroutines of one stage (GL 4.0; shader/subroutines.h).
struct SubroutineStageInfo {
    struct Function {
        std::string name;
        std::vector<std::string> types;  // subroutine types it implements
        GLint index = -1;                // GL subroutine index (layout(index) or assigned)
    };
    struct Uniform {
        std::string name;
        std::string type;
        std::vector<int> dimensions;     // array sizes, outermost first (empty: not an array)
        GLint location = -1;             // first location (layout(location) or assigned)
        // The hidden int (array) holding the selected function index, in the
        // default uniform block (-1: not active).
        GLint selectionOffset = -1, selectionStride = 0;
        std::vector<GLint> compatible;   // indices of the functions of its type
        int elements() const {
            int n = 1;
            for (int d : dimensions) n *= d;
            return n;
        }
    };
    std::vector<std::string> types;
    std::vector<Function> functions;
    std::vector<Uniform> uniforms;
    GLint locationCount = 0;             // ACTIVE_SUBROUTINE_UNIFORM_LOCATIONS
    bool any() const { return !functions.empty() || !uniforms.empty() || !types.empty(); }
};

struct LinkResult {
    bool ok = false;
    std::string log;
    std::vector<UniformInfo> uniforms;
    // location -> (uniform index, array element)
    std::vector<std::pair<int, int>> locations;
    std::vector<UniformBlockInfo> blocks;
    std::vector<AttribInfo> attribs;
    std::vector<FragOutputInfo> fragOutputs;
    std::vector<SamplerSlotInfo> samplers;
    std::vector<ImageSlotInfo> images;
    std::vector<AtomicCounterBufferInfo> atomicBuffers;
    SubroutineStageInfo subroutines[kStageCount];
    // Default-block offsets of the hidden built-in uniforms (-1: unused):
    // gl_DepthRange's near, far and diff, gl_NumSamples (glsl_upgrade.h).
    struct BuiltinUniforms {
        int depthRange[3] = {-1, -1, -1};
        int numSamples = -1;
    } builtinUniforms;
    std::vector<TransformFeedbackVarying> transformFeedback;
    uint32_t xfbStride[4] = {};      // 32-bit words per captured vertex, per buffer
    int xfbStream[4] = {};           // the vertex stream each buffer captures (geometry streams)
    int xfbBufferCount = 0;
    bool xfbVertexCapture = false;   // the vertex stage MSL writes captured vertices
    uint32_t defaultBlockSize = 0;
    // The default block's initial contents: values of uniform initializers
    // ("uniform vec4 v = vec4(1.0);"), zero elsewhere.
    std::vector<uint8_t> defaultBlockInit;
    StageOutput stages[kStageCount];
    // Geometry shader layout (for emulation).
    GLenum geometryInputType = 0, geometryOutputType = 0;
    GLint geometryVerticesOut = 0, geometryInvocations = 1;
    GeometryEmulation geometry;
    TessellationEmulation tess;
    uint32_t computeLocalSize[3] = {1, 1, 1};  // compute programs: COMPUTE_WORK_GROUP_SIZE
    // The wmgl_FixedFunction block (lowered legacy built-in uniforms), or -1.
    // It and its members come last and are hidden from the GL interface.
    int fixedFunctionBlock = -1;
    size_t visibleBlockCount() const { return fixedFunctionBlock >= 0 ? blocks.size() - 1 : blocks.size(); }
    size_t visibleUniformCount() const {
        return fixedFunctionBlock >= 0 ? uniforms.size() - blocks[static_cast<size_t>(fixedFunctionBlock)].activeUniforms.size()
                                       : uniforms.size();
    }
};

// GL_COLOR_LOGIC_OP: how the logical operation treats one colour attachment.
struct LogicOpTarget {
    char kind = 0;         // 0: not applied (float formats, no attachment); 'n': unsigned normalized; 'i', 'u': integer
    uint8_t bits[4] = {};  // bits per channel
};
// The fragment function applying logical operation `op` (0..15: GL_CLEAR + op)
// between every colour output and the attachment's current value, read with
// framebuffer fetch (empty if the stage has no output struct).
std::string fragmentWithLogicOp(const std::string& msl, unsigned op, const LogicOpTarget targets[8]);

// GL_SAMPLE_MASK / GL_SAMPLE_COVERAGE: the fragment function with a
// [[sample_mask]] output, word 1 of buffer kSlotDriverUniforms (empty if the
// stage has no output struct or writes the sample mask itself).
std::string fragmentWithSampleMask(const std::string& msl);

// GL_SAMPLE_SHADING: the fragment function run once per sample (it takes
// [[sample_id]]) with its interpolated inputs evaluated at the sample.
std::string fragmentAtSampleRate(const std::string& msl);

// gl_SampleMask has no effect without multisample rasterization (GL 4.6
// section 14.9.3); Metal applies [[sample_mask]] to single-sample targets
// too, so this variant writes all ones (empty if the stage writes no mask).
std::string fragmentIgnoringSampleMask(const std::string& msl);

LinkResult link(const LinkInput& input);

// GL type helpers.
int glTypeComponents(GLenum type);       // e.g. GL_FLOAT_MAT3 -> 9
int glTypeLocations(GLenum type);        // attribute locations used (matrix columns)
bool glTypeIsDouble(GLenum type);        // GL_DOUBLE, GL_DOUBLE_VEC*, GL_DOUBLE_MAT*
int glTypeColumns(GLenum type);          // matrix columns (1 for scalars and vectors)
int glTypeAttribSlots(GLenum type);      // MAX_VERTEX_ATTRIBS slots of one element (dvec3/dvec4 columns count twice)
int glTypeWords(GLenum type);            // 32-bit words of one element (two per double component)
bool glTypeIsSampler(GLenum type);
bool glTypeIsImage(GLenum type);
const char* glTypeName(GLenum type);

}  // namespace wmgl::shader
