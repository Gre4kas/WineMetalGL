#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

#include "core/util/settings.h"
#include "test.h"

using namespace wmgl;

// The settings file fills what the environment leaves unset, and only
// WineMetalGL's variables.
TEST(settings_file_under_environment) {
    char path[] = "/tmp/wmgl-settings-XXXXXX";
    const int fd = mkstemp(path);
    CHECK(fd >= 0);
    const std::string text = "# written by WineMetalGL Settings\n"
                             "WINEMETALGL_UPSCALE = performance\n"
                             "  WINEMETALGL_TEST_SETTING_A=from-file\r\n"
                             "WINEMETALGL_TEST_SETTING_B=from-file\n"
                             "HOME=/nowhere\n"
                             "WINEMETALGL_SETTINGS=/elsewhere\n"
                             "not a setting\n";
    CHECK(write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size()));
    close(fd);

    const char* home = std::getenv("HOME");
    const std::string savedHome = home ? home : "";
    setenv("WINEMETALGL_SETTINGS", path, 1);
    unsetenv("WINEMETALGL_UPSCALE");
    unsetenv("WINEMETALGL_TEST_SETTING_A");
    setenv("WINEMETALGL_TEST_SETTING_B", "from-environment", 1);
    CHECK(settingsPath() == path);
    applySettingsFile();
    CHECK(std::getenv("WINEMETALGL_UPSCALE") && !std::strcmp(std::getenv("WINEMETALGL_UPSCALE"), "performance"));
    CHECK(std::getenv("WINEMETALGL_TEST_SETTING_A") && !std::strcmp(std::getenv("WINEMETALGL_TEST_SETTING_A"), "from-file"));
    CHECK(!std::strcmp(std::getenv("WINEMETALGL_TEST_SETTING_B"), "from-environment"));
    CHECK(savedHome == (std::getenv("HOME") ? std::getenv("HOME") : ""));
    CHECK(!std::strcmp(std::getenv("WINEMETALGL_SETTINGS"), path));

    // A child of a process that applied the file inherits its values, marked:
    // the file as it is now wins over them, the environment still over it.
    CHECK(std::getenv("WINEMETALGL_SETTINGS_APPLIED") != nullptr);
    setenv("WINEMETALGL_TEST_SETTING_A", "stale", 1);
    setenv("WINEMETALGL_UPSCALE", "stale", 1);
    setenv("WINEMETALGL_SETTINGS_APPLIED", "WINEMETALGL_TEST_SETTING_A,WINEMETALGL_UPSCALE", 1);
    applySettingsFile();
    CHECK(!std::strcmp(std::getenv("WINEMETALGL_TEST_SETTING_A"), "from-file"));
    CHECK(!std::strcmp(std::getenv("WINEMETALGL_UPSCALE"), "performance"));
    CHECK(!std::strcmp(std::getenv("WINEMETALGL_TEST_SETTING_B"), "from-environment"));

    setenv("WINEMETALGL_SETTINGS", "0", 1);
    CHECK(settingsPath().empty());
    unsetenv("WINEMETALGL_SETTINGS");
    unsetenv("WINEMETALGL_UPSCALE");
    unsetenv("WINEMETALGL_TEST_SETTING_A");
    unsetenv("WINEMETALGL_TEST_SETTING_B");
    unsetenv("WINEMETALGL_SETTINGS_APPLIED");
    std::remove(path);
}
