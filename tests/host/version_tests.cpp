#include <string>

#include "core/util/version.h"
#include "test.h"

using namespace wmgl;

// GL_RENDERER names the GPU without "Apple" (Godot 3 turns its depth
// pre-pass off for renderers that contain it).
TEST(renderer_name_without_vendor) {
    CHECK(rendererName("Apple M4") == "M4 (WineMetalGL)");
    CHECK(rendererName("Apple M2 Max") == "M2 Max (WineMetalGL)");
    CHECK(rendererName("AMD Radeon Pro 5500M") == "AMD Radeon Pro 5500M (WineMetalGL)");
    CHECK(rendererName("Apple ") == "Apple  (WineMetalGL)");
    CHECK(rendererName("Apple M4").find("Apple") == std::string::npos);
}
