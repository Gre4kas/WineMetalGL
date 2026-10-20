// Caches for Metal pipeline, depth-stencil and sampler state objects. Keys
// are plain structs hashed byte-wise, so they must be fully initialised
// (memset) before filling.
#pragma once

#import <Metal/Metal.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/objects.h"

namespace wmgl {

struct LinkedProgram;

struct VertexAttributeKey {
    uint8_t format;      // MTLVertexFormat (0 = unused)
    uint8_t layout;      // index into layouts
    uint16_t offset;
};
struct VertexLayoutKey {
    uint32_t stride;
    uint8_t stepFunction;  // MTLVertexStepFunction
    uint8_t pad[3];
    uint32_t stepRate;
};

struct PipelineKey {
    uint64_t programId;
    VertexAttributeKey attributes[31];  // VertexArray::kAttribSlots
    VertexLayoutKey layouts[13];
    uint8_t layoutCount;
    uint8_t sampleCount;
    uint8_t alphaToCoverage;
    uint8_t alphaToOne;
    uint8_t sampleMask;     // fragment function writes the GL sample mask (LinkedProgram::fragmentVariant)
    uint8_t sampleShading;  // fragment function runs per sample (GL_SAMPLE_SHADING)
    uint8_t ignoreSampleMask;  // single-sample rasterization: gl_SampleMask writes are ignored
    uint8_t logicOp;        // GL logical operation + 1 the fragment function applies (0: none; above 16: advanced blend + 17)
    uint8_t rasterization;
    uint8_t topologyClass;  // MTLPrimitiveTopologyClass
    uint8_t geometryPass;   // vertex function: the geometry emulation's pass-through
    uint8_t cull;           // cull distances the vertex function writes (LinkedProgram::fragmentVariant)
    uint8_t countFragments;  // fragment function counts its invocations (FRAGMENT_SHADER_INVOCATIONS)
    // Metal tessellation (the vertex function is the post-tessellation
    // variant): 0 = none, else bit 0 set, bit 1 quads, bits 2-3 the
    // MTLTessellationPartitionMode, bit 4 MTLWindingClockwise.
    uint8_t tessellation;
    // A depth-only pass whose depth texture was never drawn with colour
    // (shadow maps): the vertex function may write only the position
    // (LinkedProgram::positionOnlyVertex), with no fragment function.
    uint8_t positionOnly;
    uint16_t borders[2];    // clamp-to-border emulation variants: vertex, fragment (shader::borderVariantId)
    uint16_t colorFormats[8];  // MTLPixelFormat
    uint16_t depthFormat, stencilFormat;
    struct Blend {
        uint8_t enabled, writeMask;
        uint8_t srcRGB, dstRGB, srcAlpha, dstAlpha;  // MTLBlendFactor
        uint8_t opRGB, opAlpha;                      // MTLBlendOperation
    } blend[8];
};

struct DepthStencilKey {
    uint8_t depthCompare, depthWrite, stencilEnabled, pad;
    struct Face {
        uint8_t compare, fail, depthFail, pass;
        uint32_t readMask, writeMask;
    } front, back;
};

struct SamplerKey {
    uint8_t minFilter, magFilter, mipFilter, sAddress, tAddress, rAddress, compare, border;
    uint8_t maxAnisotropy, normalized, pad[2];
    float lodMin, lodMax;
    float lodBias;  // TEXTURE_LOD_BIAS (macOS 26+)
};

// Render pipelines are created against a binary archive on disk, one per
// program (WINEMETALGL_PIPELINE_CACHE, pipeline_cache.mm), so a pipeline
// compiles once ever rather than once per run. And they are predicted: the
// render state of each new pipeline (formats, blending, vertex layout) is
// built in the background for the other programs with the same vertex inputs
// and fragment outputs, and a program that links gets the states seen so far,
// so an effect's first draw usually finds its pipeline made
// (WINEMETALGL_PREDICT_PIPELINES=0 turns that off).
// Starts reading the on-disk pipeline cache in the background (a context
// was made: its first pipelines then rarely wait for it).
void startPipelineArchive();

class PipelineCache {
public:
    PipelineCache();
    ~PipelineCache();  // waits for the background predictions under way
    PipelineCache(const PipelineCache&) = delete;
    PipelineCache& operator=(const PipelineCache&) = delete;

    id<MTLRenderPipelineState> render(const PipelineKey& key, LinkedProgram& program);
    id<MTLDepthStencilState> depthStencil(const DepthStencilKey& key);
    id<MTLSamplerState> sampler(const SamplerKey& key);
    // A program that linked: the render states seen so far are predicted for it.
    void linked(const std::shared_ptr<LinkedProgram>& program);

private:
    using Future = std::shared_future<id<MTLRenderPipelineState>>;
    // Creates and caches the pipeline (one creation per key: a creation in
    // progress is waited for). `predicted`: a background guess, which logs
    // no failure and caches none.
    id<MTLRenderPipelineState> build(const PipelineKey& key, LinkedProgram& program, bool predicted);
    // A pipeline a draw created: its render state, if new, is predicted for
    // the programs it suits.
    void learn(const PipelineKey& key, LinkedProgram& program);
    // The descriptor of a key's pipeline (nil when the program has no vertex function).
    MTLRenderPipelineDescriptor* describe(const PipelineKey& key, LinkedProgram& program);
    void predict(const PipelineKey& key, const std::shared_ptr<LinkedProgram>& program);

    template <class K>
    struct Hash {
        size_t operator()(const K& k) const;
    };
    template <class K>
    struct Equal {
        bool operator()(const K& a, const K& b) const { return std::memcmp(&a, &b, sizeof(K)) == 0; }
    };
    std::mutex mutex_;
    std::unordered_map<PipelineKey, id<MTLRenderPipelineState>, Hash<PipelineKey>, Equal<PipelineKey>> render_;
    std::unordered_map<PipelineKey, Future, Hash<PipelineKey>, Equal<PipelineKey>> building_;
    // Prediction (under predictMutex_): render states by the interface
    // signature of the programs they suit, the linked programs by theirs,
    // and the keys already predicted.
    std::mutex predictMutex_;
    std::unordered_set<PipelineKey, Hash<PipelineKey>, Equal<PipelineKey>> states_;
    std::unordered_map<std::string, std::vector<PipelineKey>> statesBySignature_;
    std::unordered_map<std::string, std::vector<std::weak_ptr<LinkedProgram>>> programsBySignature_;
    std::unordered_set<PipelineKey, Hash<PipelineKey>, Equal<PipelineKey>> predicted_;
    size_t nextQueue_ = 0;
    dispatch_group_t predictions_;
    std::atomic<bool> closing_{false};
    std::unordered_map<DepthStencilKey, id<MTLDepthStencilState>, Hash<DepthStencilKey>, Equal<DepthStencilKey>> depth_;
    std::unordered_map<SamplerKey, id<MTLSamplerState>, Hash<SamplerKey>, Equal<SamplerKey>> samplers_;
};

}  // namespace wmgl
