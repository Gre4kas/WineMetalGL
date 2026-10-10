#include "core/metal/device.h"

#include <cstring>
#include <mutex>

#include "core/metal/utility_shaders.h"
#include "core/util/log.h"

namespace wmgl {

Device& Device::get() {
    static Device* device = new Device();
    return *device;
}

Device::Device() {
    device_ = MTLCreateSystemDefaultDevice();
    if (!device_) {
        WMGL_ERR("no Metal device");
        return;
    }
    std::strncpy(name_, device_.name.UTF8String, sizeof(name_) - 1);
    caps_.bcTextures = device_.supportsBCTextureCompression;
    caps_.depth24Stencil8 = device_.depth24Stencil8PixelFormatSupported;
    caps_.meshShaders = [device_ supportsFamily:MTLGPUFamilyMetal3];
    caps_.framebufferFetch = [device_ supportsFamily:MTLGPUFamilyApple1];
    caps_.samplerBorderColor = true;
    caps_.primitiveId = [device_ supportsFamily:MTLGPUFamilyApple7] || [device_ supportsFamily:MTLGPUFamilyMac2];
    caps_.workingSetBytes = device_.recommendedMaxWorkingSetSize;
    for (uint32_t samples : {1u, 2u, 4u, 8u}) {
        caps_.sampleCountSupported[samples] = [device_ supportsTextureSampleCount:samples];
        if (caps_.sampleCountSupported[samples]) caps_.maxSamples = samples;
    }
    WMGL_INFO("device %s: bc=%d mesh=%d fbfetch=%d maxSamples=%u", name_, caps_.bcTextures, caps_.meshShaders,
              caps_.framebufferFetch, caps_.maxSamples);
}

id<MTLLibrary> Device::utilityLibrary() {
    static std::mutex mutex;
    std::lock_guard<std::mutex> lock(mutex);
    if (!utility_) {
        NSError* error = nil;
        MTLCompileOptions* options = [MTLCompileOptions new];
        options.languageVersion = MTLLanguageVersion3_0;
        utility_ = [device_ newLibraryWithSource:[NSString stringWithUTF8String:kUtilityShaderSource]
                                         options:options
                                           error:&error];
        if (!utility_) WMGL_ERR("utility shaders failed to compile: %s", error.localizedDescription.UTF8String);
    }
    return utility_;
}

}  // namespace wmgl
