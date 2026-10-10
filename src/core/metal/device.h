// The Metal device shared by every WineMetalGL context, plus the device
// capabilities that decide GL limits and format support.
#pragma once

#import <Metal/Metal.h>

#include <cstdint>

namespace wmgl {

struct DeviceCaps {
    bool bcTextures = false;            // BC1-7 (S3TC, RGTC, BPTC)
    bool depth24Stencil8 = false;       // never on Apple GPUs; emulated with D32F_S8
    bool meshShaders = false;           // Metal 3 mesh pipelines (geometry-shader emulation)
    bool framebufferFetch = false;      // programmable blending (logic op emulation)
    bool samplerBorderColor = false;
    bool primitiveId = false;
    uint32_t maxTextureSize2D = 16384;
    uint32_t maxTextureSize3D = 2048;
    uint32_t maxTextureLayers = 2048;
    uint32_t maxColorAttachments = 8;
    uint32_t maxSamples = 4;            // highest power of two supported for colour and depth
    uint64_t workingSetBytes = 0;       // recommendedMaxWorkingSetSize
    bool sampleCountSupported[9] = {};  // index = sample count (1, 2, 4, 8)
};

class Device {
public:
    static Device& get();

    id<MTLDevice> mtl() const { return device_; }
    const DeviceCaps& caps() const { return caps_; }
    const char* name() const { return name_; }

    // Compute pipeline / render pipeline helpers for internal utility shaders.
    id<MTLLibrary> utilityLibrary();

private:
    Device();
    id<MTLDevice> device_;
    id<MTLLibrary> utility_ = nil;
    DeviceCaps caps_;
    char name_[128] = {};
};

}  // namespace wmgl
