// Caches for Metal pipeline, depth-stencil and sampler state objects. Keys
// are plain structs hashed byte-wise, so they must be fully initialised
// (memset) before filling.
#pragma once

#import <Metal/Metal.h>

#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>

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
    uint8_t sampleMask;     // fragment function writes the GL sample mask (LinkedProgram::fragmentSampleMask)
    uint8_t rasterization;
    uint8_t topologyClass;  // MTLPrimitiveTopologyClass
    uint8_t geometryPass;   // vertex function: the geometry emulation's pass-through
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

class PipelineCache {
public:
    id<MTLRenderPipelineState> render(const PipelineKey& key, LinkedProgram& program);
    id<MTLDepthStencilState> depthStencil(const DepthStencilKey& key);
    id<MTLSamplerState> sampler(const SamplerKey& key);

private:
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
    std::unordered_map<DepthStencilKey, id<MTLDepthStencilState>, Hash<DepthStencilKey>, Equal<DepthStencilKey>> depth_;
    std::unordered_map<SamplerKey, id<MTLSamplerState>, Hash<SamplerKey>, Equal<SamplerKey>> samplers_;
};

}  // namespace wmgl
