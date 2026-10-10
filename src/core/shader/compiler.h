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
#include <string>
#include <vector>

#include "core/gl/types.h"

namespace wmgl::shader {

enum class Stage : uint8_t { Vertex = 0, TessControl, TessEval, Geometry, Fragment, Compute, Count };
constexpr int kStageCount = static_cast<int>(Stage::Count);

// Metal argument-table layout used by every generated stage function.
constexpr uint32_t kSlotDefaultUniforms = 0;   // buffer: default uniform block
constexpr uint32_t kSlotDriverUniforms = 1;    // buffer: WineMetalGL driver constants
constexpr uint32_t kSlotFirstUniformBlock = 2; // buffers 2..13: uniform blocks used by the stage
constexpr uint32_t kMaxStageUniformBlocks = 12;
constexpr uint32_t kSlotFirstXfbBuffer = 14;   // buffers 14..17: transform feedback outputs
constexpr uint32_t kSlotFirstVertexBuffer = 18;  // buffers 18..30: vertex buffers
constexpr uint32_t kMaxVertexBufferSlots = 13;
constexpr uint32_t kMaxStageSamplers = 16;     // textures/samplers 0..15

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
    uint32_t stageMask = 0;    // stages referencing it
};

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
};

struct FragOutputInfo {
    std::string name;
    GLenum type = 0;
    GLint location = 0;
    GLint index = 0;
    GLint size = 1;
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
    std::string entryPoint;
    std::vector<uint32_t> spirv;
    bool usesDefaultUniforms = false;
    bool usesDriverUniforms = false;
    bool writesPointSize = false;
    bool discards = false;
    // Vertex stage: Metal [[attribute(n)]] -> GL location and input component type.
    struct VertexInput { GLint location; int components; char kind; /* 'f', 'i', 'u' */ };
    std::vector<VertexInput> vertexInputs;
    // Fragment stage: outputs written (location, index, component kind).
    struct FragOutput { GLint location; GLint index; char kind; };
    std::vector<FragOutput> fragOutputs;
};

struct TransformFeedbackVarying {
    std::string name;
    GLenum type = 0;
    GLint size = 1;
    int components = 0;        // total float/int components
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
    std::vector<TransformFeedbackVarying> transformFeedback;
    uint32_t defaultBlockSize = 0;
    StageOutput stages[kStageCount];
    // Geometry shader layout (for emulation).
    GLenum geometryInputType = 0, geometryOutputType = 0;
    GLint geometryVerticesOut = 0, geometryInvocations = 1;
};

LinkResult link(const LinkInput& input);

// GL type helpers.
int glTypeComponents(GLenum type);       // e.g. GL_FLOAT_MAT3 -> 9
int glTypeLocations(GLenum type);        // attribute locations used (matrix columns)
bool glTypeIsSampler(GLenum type);
const char* glTypeName(GLenum type);

}  // namespace wmgl::shader
