#include "SKL/WCAS/Private/Harness/ProfileConfig.h"

#include <assert.h>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

std::string write_config(const std::string &name, const std::string &contents) {
    const auto path = std::filesystem::temp_directory_path() / ("wcas-profile-" + name + ".toml");
    std::ofstream stream(path, std::ios::trunc);
    stream << contents;
    return path.string();
}

}   // namespace

int main() {
    using namespace skl::wcas;

    const auto valid = write_config("valid", R"(
version = 1
default = "launcher"

[env]
WINEPREFIX = "~/Games/hypergryph"

[[profiles]]
id = "launcher"
target = "Hypergryph Launcher"
type = "launcher"
transport = "xwayland"
command = "/tmp/Launcher.exe"
games_exe = "auto"

[[profiles]]
id = "game"
type = "game"
transport = "wayland"
graphics = "vulkan"
command = "/tmp/Endfield.exe"
dependencies = ["launcher"]
launch = "parent-spoof"
helper = "/tmp/helper.exe"
)");
    std::string error;
    const auto config = load_profile_config(valid, error);
    assert(config.has_value());
    assert(config->default_profile == "launcher");
    assert(config->global_env.size() == 1);
    assert(config->profiles.size() == 2);
    assert(config->profiles[1].dependencies.at(0) == "launcher");
    assert(config->profiles[1].launch == "parent-spoof");

    const auto unknown = write_config("unknown", R"(
[[profiles]]
id = "game"
command = "/tmp/game.exe"
dependencies = ["missing"]
)");
    assert(!load_profile_config(unknown, error));

    const auto cycle = write_config("cycle", R"(
[[profiles]]
id = "a"
command = "/tmp/a.exe"
dependencies = ["b"]

[[profiles]]
id = "b"
command = "/tmp/b.exe"
dependencies = ["a"]
)");
    assert(!load_profile_config(cycle, error));

    return 0;
}
