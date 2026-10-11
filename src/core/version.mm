#include "core/util/version.h"

#include <cstdio>
#include <cstdlib>

#include "core/metal/device.h"

namespace wmgl {

int devMaxVersion() {
    static const int version = [] {
        const char* value = std::getenv("WINEMETALGL_DEV_MAX_VERSION");
        int major = 0, minor = 0;
        if (!value || std::sscanf(value, "%d.%d", &major, &minor) != 2) return 0;
        if (major != 4 || minor < 0 || minor > 6) return 0;
        return major * 10 + minor;
    }();
    return version;
}

const std::string& rendererString() {
    static const std::string renderer = [] {
        const char* r = std::getenv("WINEMETALGL_RENDERER");
        std::string name = r && *r ? std::string(r) : std::string(Device::get().name()) + " (WineMetalGL)";
        if (const int dev = devMaxVersion())
            name += " (dev " + std::to_string(dev / 10) + "." + std::to_string(dev % 10) + ")";
        return name;
    }();
    return renderer;
}

}  // namespace wmgl
