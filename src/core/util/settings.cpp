#include "core/util/settings.h"

#include <cstdlib>
#include <cstring>
#include <fstream>

namespace wmgl {

std::string settingsPath() {
    if (const char* path = std::getenv("WINEMETALGL_SETTINGS")) return std::strcmp(path, "0") ? path : "";
    const char* home = std::getenv("HOME");
    return home && *home ? std::string(home) + "/Library/Application Support/WineMetalGL/settings.conf" : "";
}

void applySettingsFile() {
    const std::string path = settingsPath();
    if (path.empty()) return;
    std::ifstream file(path);
    std::string line;
    while (std::getline(file, line)) {
        const auto trim = [](std::string s) {
            const size_t first = s.find_first_not_of(" \t\r");
            const size_t last = s.find_last_not_of(" \t\r");
            return first == std::string::npos ? std::string() : s.substr(first, last - first + 1);
        };
        line = trim(line);
        const size_t equals = line.find('=');
        if (line.empty() || line[0] == '#' || equals == std::string::npos) continue;
        const std::string name = trim(line.substr(0, equals)), value = trim(line.substr(equals + 1));
        // Only WineMetalGL's own settings, and never the file's own path.
        if (name.rfind("WINEMETALGL_", 0) != 0 || name == "WINEMETALGL_SETTINGS") continue;
        setenv(name.c_str(), value.c_str(), 0);
    }
}

namespace {
__attribute__((constructor)) void applyAtLoad() { applySettingsFile(); }
}  // namespace

}  // namespace wmgl
