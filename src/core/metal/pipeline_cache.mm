#include "core/metal/pipeline_cache.h"

#import <objc/message.h>

#include "core/metal/device.h"
#include "core/program.h"
#include "core/shader/compiler.h"
#include "core/util/log.h"
#include "core/stats.h"

namespace wmgl {

template <class K>
size_t PipelineCache::Hash<K>::operator()(const K& k) const {
    // Eight key bytes a step (keys are hundreds of bytes, hashed per draw).
    const auto* bytes = reinterpret_cast<const uint8_t*>(&k);
    uint64_t hash = 1469598103934665603ull ^ sizeof(K);
    size_t i = 0;
    for (; i + 8 <= sizeof(K); i += 8) {
        uint64_t word;
        std::memcpy(&word, bytes + i, 8);
        hash = (hash ^ word) * 0x9e3779b97f4a7c15ull;
        hash ^= hash >> 29;
    }
    for (; i < sizeof(K); ++i) hash = (hash ^ bytes[i]) * 1099511628211ull;
    return static_cast<size_t>(hash ^ (hash >> 32));
}

id<MTLRenderPipelineState> PipelineCache::render(const PipelineKey& key, LinkedProgram& program) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = render_.find(key);
        if (it != render_.end()) return it->second;
    }
    stats::count(&stats::Frame::pipelineMisses);
    stats::Timer timer(&stats::Frame::pipelineNanos);
    const bool pointSizeAllowed = key.topologyClass == MTLPrimitiveTopologyClassUnspecified ||
                                  key.topologyClass == MTLPrimitiveTopologyClassPoint;
    id<MTLFunction> vertex = key.borders[0] && !key.geometryPass
                                 ? program.vertexWithBorders(key.rasterization, pointSizeAllowed, key.borders[0])
                             : !key.rasterization ? program.vertexNoRaster()
                             : !pointSizeAllowed  ? program.vertexWithoutPointSize(key.geometryPass)
                             : key.geometryPass   ? program.geometryPassthrough()
                                                  : program.function(shader::Stage::Vertex);
    id<MTLFunction> fragment =
        key.sampleMask || key.logicOp || key.sampleShading || key.ignoreSampleMask || key.cull || key.borders[1] ||
                key.countFragments
            ? program.fragmentVariant(key.sampleMask, key.logicOp, key.colorFormats, key.sampleShading, key.ignoreSampleMask,
                                      key.cull, key.borders[1], key.countFragments)
            : program.function(shader::Stage::Fragment);
    if (!vertex) return nil;
    MTLRenderPipelineDescriptor* desc = [MTLRenderPipelineDescriptor new];
    desc.vertexFunction = vertex;
    desc.fragmentFunction = key.rasterization ? fragment : nil;
    MTLVertexDescriptor* vd = [MTLVertexDescriptor vertexDescriptor];
    for (uint32_t i = 0; i < std::size(key.attributes); ++i) {
        if (!key.attributes[i].format) continue;
        vd.attributes[i].format = static_cast<MTLVertexFormat>(key.attributes[i].format);
        vd.attributes[i].offset = key.attributes[i].offset;
        vd.attributes[i].bufferIndex = shader::kSlotFirstVertexBuffer + key.attributes[i].layout;
    }
    for (uint32_t l = 0; l < key.layoutCount; ++l) {
        MTLVertexBufferLayoutDescriptor* layout = vd.layouts[shader::kSlotFirstVertexBuffer + l];
        layout.stride = key.layouts[l].stride;
        layout.stepFunction = static_cast<MTLVertexStepFunction>(key.layouts[l].stepFunction);
        layout.stepRate = key.layouts[l].stepFunction == MTLVertexStepFunctionConstant ? 0 : key.layouts[l].stepRate;
    }
    desc.vertexDescriptor = vd;
    desc.rasterSampleCount = key.sampleCount ? key.sampleCount : 1;
    desc.alphaToCoverageEnabled = key.alphaToCoverage;
    desc.alphaToOneEnabled = key.alphaToOne;
    desc.rasterizationEnabled = key.rasterization;
    desc.inputPrimitiveTopology = static_cast<MTLPrimitiveTopologyClass>(key.topologyClass);
    for (int i = 0; i < 8; ++i) {
        desc.colorAttachments[i].pixelFormat = static_cast<MTLPixelFormat>(key.colorFormats[i]);
        if (!key.colorFormats[i]) continue;
        const auto& b = key.blend[i];
        desc.colorAttachments[i].writeMask = b.writeMask;
        desc.colorAttachments[i].blendingEnabled = b.enabled;
        desc.colorAttachments[i].sourceRGBBlendFactor = static_cast<MTLBlendFactor>(b.srcRGB);
        desc.colorAttachments[i].destinationRGBBlendFactor = static_cast<MTLBlendFactor>(b.dstRGB);
        desc.colorAttachments[i].sourceAlphaBlendFactor = static_cast<MTLBlendFactor>(b.srcAlpha);
        desc.colorAttachments[i].destinationAlphaBlendFactor = static_cast<MTLBlendFactor>(b.dstAlpha);
        desc.colorAttachments[i].rgbBlendOperation = static_cast<MTLBlendOperation>(b.opRGB);
        desc.colorAttachments[i].alphaBlendOperation = static_cast<MTLBlendOperation>(b.opAlpha);
    }
    desc.depthAttachmentPixelFormat = static_cast<MTLPixelFormat>(key.depthFormat);
    desc.stencilAttachmentPixelFormat = static_cast<MTLPixelFormat>(key.stencilFormat);
    NSError* error = nil;
    id<MTLRenderPipelineState> state = [Device::get().mtl() newRenderPipelineStateWithDescriptor:desc error:&error];
    if (!state) WMGL_ERR("render pipeline creation failed: %s", error.localizedDescription.UTF8String);
    std::lock_guard<std::mutex> lock(mutex_);
    render_.emplace(key, state);
    return state;
}

id<MTLDepthStencilState> PipelineCache::depthStencil(const DepthStencilKey& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = depth_.find(key);
    if (it != depth_.end()) return it->second;
    MTLDepthStencilDescriptor* desc = [MTLDepthStencilDescriptor new];
    desc.depthCompareFunction = static_cast<MTLCompareFunction>(key.depthCompare);
    desc.depthWriteEnabled = key.depthWrite;
    if (key.stencilEnabled) {
        auto face = [](const DepthStencilKey::Face& f) {
            MTLStencilDescriptor* s = [MTLStencilDescriptor new];
            s.stencilCompareFunction = static_cast<MTLCompareFunction>(f.compare);
            s.stencilFailureOperation = static_cast<MTLStencilOperation>(f.fail);
            s.depthFailureOperation = static_cast<MTLStencilOperation>(f.depthFail);
            s.depthStencilPassOperation = static_cast<MTLStencilOperation>(f.pass);
            s.readMask = f.readMask;
            s.writeMask = f.writeMask;
            return s;
        };
        desc.frontFaceStencil = face(key.front);
        desc.backFaceStencil = face(key.back);
    }
    id<MTLDepthStencilState> state = [Device::get().mtl() newDepthStencilStateWithDescriptor:desc];
    depth_.emplace(key, state);
    return state;
}

id<MTLSamplerState> PipelineCache::sampler(const SamplerKey& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = samplers_.find(key);
    if (it != samplers_.end()) return it->second;
    MTLSamplerDescriptor* desc = [MTLSamplerDescriptor new];
    desc.minFilter = static_cast<MTLSamplerMinMagFilter>(key.minFilter);
    desc.magFilter = static_cast<MTLSamplerMinMagFilter>(key.magFilter);
    desc.mipFilter = static_cast<MTLSamplerMipFilter>(key.mipFilter);
    desc.sAddressMode = static_cast<MTLSamplerAddressMode>(key.sAddress);
    desc.tAddressMode = static_cast<MTLSamplerAddressMode>(key.tAddress);
    desc.rAddressMode = static_cast<MTLSamplerAddressMode>(key.rAddress);
    desc.compareFunction = static_cast<MTLCompareFunction>(key.compare);
    desc.borderColor = static_cast<MTLSamplerBorderColor>(key.border);
    desc.maxAnisotropy = std::max<uint8_t>(1, key.maxAnisotropy);
    desc.normalizedCoordinates = key.normalized;
    desc.lodMinClamp = key.lodMin;
    desc.lodMaxClamp = key.lodMax;
    // MTLSamplerDescriptor.lodBias is macOS 26 API; set it through the
    // runtime so builds with older SDKs (CI) keep it on systems that have it.
    static const SEL setLodBias = NSSelectorFromString(@"setLodBias:");
    if (key.lodBias != 0.0f && [desc respondsToSelector:setLodBias])
        reinterpret_cast<void (*)(id, SEL, float)>(objc_msgSend)(desc, setLodBias, key.lodBias);
    desc.supportArgumentBuffers = NO;
    id<MTLSamplerState> state = [Device::get().mtl() newSamplerStateWithDescriptor:desc];
    samplers_.emplace(key, state);
    return state;
}

}  // namespace wmgl
