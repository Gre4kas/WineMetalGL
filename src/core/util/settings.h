// Settings from a file, for what launches programs without a way to set
// their environment (the WineMetalGL Settings app, src/settings-app).
#pragma once

#include <string>

namespace wmgl {

// The settings file: WINEMETALGL_SETTINGS when set ("0": none), else
// ~/Library/Application Support/WineMetalGL/settings.conf.
std::string settingsPath();

// Sets the WINEMETALGL_* variables of the settings file ("NAME=value" lines,
// # comments) that the environment does not set: the environment wins. Runs
// when the core loads, before anything reads its settings.
void applySettingsFile();

}  // namespace wmgl
