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
    // Variables an earlier process took from the file (a launcher, Steam)
    // reach its children as environment: they are the file's, not the
    // user's, so the file is read again rather than its old values winning.
    static const char* const kApplied = "WINEMETALGL_SETTINGS_APPLIED";
    if (const char* applied = std::getenv(kApplied)) {
        std::string names = applied;
        for (size_t start = 0; start < names.size();) {
            const size_t end = names.find(',', start) == std::string::npos ? names.size() : names.find(',', start);
            const std::string name = names.substr(start, end - start);
            if (name.rfind("WINEMETALGL_", 0) == 0) unsetenv(name.c_str());
            start = end + 1;
        }
        unsetenv(kApplied);
    }
    const std::string path = settingsPath();
    if (path.empty()) return;
    std::ifstream file(path);
    std::string line, applied;
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
        if (name.rfind("WINEMETALGL_", 0) != 0 || name == "WINEMETALGL_SETTINGS" || name == kApplied) continue;
        if (std::getenv(name.c_str())) continue;  // the environment wins
        setenv(name.c_str(), value.c_str(), 1);
        applied += (applied.empty() ? "" : ",") + name;
    }
    if (!applied.empty()) setenv(kApplied, applied.c_str(), 1);
}

namespace {
__attribute__((constructor)) void applyAtLoad() { applySettingsFile(); }
}  // namespace

}  // namespace wmgl
