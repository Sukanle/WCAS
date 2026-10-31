#pragma once

#include <optional>
#include <string>
#include <vector>

namespace skl::wcas {

struct ProfileEnv {
    std::string key;
    std::optional<std::string> value;
};

/// One user-authored launch profile from a TOML file.
struct LaunchProfile {
    std::string id;
    std::string target;
    std::string type = "game";
    std::string mode;
    std::string description;
    std::string transport = "auto";
    std::string graphics = "auto";
    std::string capture_backend;
    std::string command;
    std::string match;
    std::string launch = "direct";
    std::string parent;
    std::string helper;
    std::string games_exe;
    std::vector<std::string> args;
    std::vector<std::string> dependencies;
    std::vector<ProfileEnv> env;
    bool allow_activation = false;
};

struct ProfileConfig {
    int version = 1;
    std::string default_profile;
    std::vector<ProfileEnv> global_env;
    std::vector<LaunchProfile> profiles;
};

/// Loads and validates a TOML profile file. Unknown/duplicate dependencies and
/// dependency cycles are rejected before anything is launched.
[[nodiscard]] std::optional<ProfileConfig> load_profile_config(const std::string &path, std::string &error);

[[nodiscard]] const LaunchProfile *find_profile(const ProfileConfig &config, const std::string &id) noexcept;

}   // namespace skl::wcas
