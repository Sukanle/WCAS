#include "SKL/WCAS/Private/Harness/ProfileConfig.h"

#include <toml++/toml.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <unordered_set>

namespace skl::wcas {
namespace {

[[nodiscard]] std::optional<std::string> string_value(const toml::table &table, const char *key) {
    if (const auto *node = table.get(key); node != nullptr) {
        if (const auto value = node->value<std::string>(); value.has_value()) return *value;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<bool> bool_value(const toml::table &table, const char *key) {
    if (const auto *node = table.get(key); node != nullptr) {
        if (const auto value = node->value<bool>(); value.has_value()) return *value;
    }
    return std::nullopt;
}

[[nodiscard]] bool string_array(
    const toml::table &table, const char *key, std::vector<std::string> &out, std::string &error) {
    const auto *node = table.get(key);
    if (node == nullptr) return true;
    const auto *array = node->as_array();
    if (array == nullptr) {
        error = std::string(key) + " must be an array of strings";
        return false;
    }
    for (const auto &entry : *array) {
        const auto value = entry.value<std::string>();
        if (!value.has_value()) {
            error = std::string(key) + " must contain only strings";
            return false;
        }
        out.push_back(*value);
    }
    return true;
}

[[nodiscard]] bool env_table(const toml::table &table, std::vector<ProfileEnv> &out, std::string &error) {
    const auto *node = table.get("env");
    if (node == nullptr) return true;
    const auto *env = node->as_table();
    if (env == nullptr) {
        error = "env must be an inline table of string values";
        return false;
    }
    for (const auto &[key, value] : *env) {
        const auto text = value.value<std::string>();
        if (!text.has_value()) {
            error = "env." + std::string(key.str()) + " must be a string";
            return false;
        }
        out.push_back(ProfileEnv{std::string(key.str()), *text});
    }
    return true;
}

[[nodiscard]] bool parse_profile(const toml::table &table, LaunchProfile &profile, std::string &error) {
    const auto id = string_value(table, "id");
    if (!id.has_value() || id->empty()) {
        error = "each profile requires a non-empty id";
        return false;
    }
    profile.id = *id;
    profile.target = string_value(table, "target").value_or(profile.id);
    profile.type = string_value(table, "type").value_or("game");
    std::transform(profile.type.begin(), profile.type.end(), profile.type.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (profile.type != "launcher" && profile.type != "game") {
        error = "profile '" + profile.id + "' has invalid type (expected launcher or game)";
        return false;
    }
    profile.mode = string_value(table, "mode").value_or("");
    profile.description = string_value(table, "description").value_or("");
    profile.transport = string_value(table, "transport").value_or("auto");
    profile.graphics = string_value(table, "graphics").value_or("auto");
    profile.capture_backend = string_value(table, "capture_backend").value_or("");
    profile.command = string_value(table, "command").value_or("");
    profile.match = string_value(table, "match").value_or(string_value(table, "match_regex").value_or(""));
    profile.launch = string_value(table, "launch").value_or("direct");
    profile.parent = string_value(table, "parent").value_or("");
    profile.helper = string_value(table, "helper").value_or("");
    profile.games_exe = string_value(table, "games_exe").value_or("");
    profile.allow_activation = bool_value(table, "allow_activation").value_or(false);

    if (profile.command.empty()) {
        error = "profile '" + profile.id + "' requires command";
        return false;
    }
    if (profile.launch != "direct" && profile.launch != "parent-spoof") {
        error = "profile '" + profile.id + "' has invalid launch (expected direct or parent-spoof)";
        return false;
    }
    if (!string_array(table, "args", profile.args, error)) {
        error = "profile '" + profile.id + "': " + error;
        return false;
    }
    if (!string_array(table, "dependencies", profile.dependencies, error)) {
        error = "profile '" + profile.id + "': " + error;
        return false;
    }
    if (!env_table(table, profile.env, error)) {
        error = "profile '" + profile.id + "': " + error;
        return false;
    }
    return true;
}

[[nodiscard]] bool validate_dependencies(const ProfileConfig &config, std::string &error) {
    std::unordered_map<std::string, std::size_t> indices;
    for (std::size_t i = 0; i < config.profiles.size(); ++i) {
        if (!indices.emplace(config.profiles[i].id, i).second) {
            error = "duplicate profile id: " + config.profiles[i].id;
            return false;
        }
    }
    for (const auto &profile : config.profiles) {
        for (const auto &dependency : profile.dependencies) {
            if (indices.find(dependency) == indices.end()) {
                error = "profile '" + profile.id + "' depends on unknown profile '" + dependency + "'";
                return false;
            }
        }
    }

    std::vector<int> state(config.profiles.size(), 0);
    std::function<bool(std::size_t)> visit = [&](std::size_t index) {
        if (state[index] == 1) {
            error = "dependency cycle at profile '" + config.profiles[index].id + "'";
            return false;
        }
        if (state[index] == 2) return true;
        state[index] = 1;
        for (const auto &dependency : config.profiles[index].dependencies) {
            if (!visit(indices.at(dependency))) return false;
        }
        state[index] = 2;
        return true;
    };
    for (std::size_t i = 0; i < config.profiles.size(); ++i) {
        if (!visit(i)) return false;
    }
    if (!config.default_profile.empty() && indices.find(config.default_profile) == indices.end()) {
        error = "default profile does not exist: " + config.default_profile;
        return false;
    }
    return true;
}

}   // namespace

std::optional<ProfileConfig> load_profile_config(const std::string &path, std::string &error) {
    toml::table document;
    try {
        document = toml::parse_file(path);
    } catch (const toml::parse_error &parse_error) {
        error = "TOML parse error: " + std::string(parse_error.description());
        return std::nullopt;
    } catch (const std::exception &exception) {
        error = std::string("cannot read profile config: ") + exception.what();
        return std::nullopt;
    }

    ProfileConfig config;
    if (const auto *version = document.get("version"); version != nullptr) {
        if (const auto value = version->value<std::int64_t>(); value.has_value()) {
            config.version = static_cast<int>(*value);
        }
    }
    config.default_profile = string_value(document, "default").value_or("");
    if (!env_table(document, config.global_env, error)) return std::nullopt;

    const auto *profiles = document.get("profiles");
    if (profiles == nullptr) {
        error = "profile config requires a [[profiles]] array";
        return std::nullopt;
    }
    const auto *array = profiles->as_array();
    if (array == nullptr) {
        error = "profiles must be an array of tables";
        return std::nullopt;
    }
    for (const auto &entry : *array) {
        const auto *table = entry.as_table();
        if (table == nullptr) {
            error = "each profiles entry must be a table";
            return std::nullopt;
        }
        LaunchProfile profile;
        if (!parse_profile(*table, profile, error)) return std::nullopt;
        config.profiles.push_back(std::move(profile));
    }
    if (config.profiles.empty()) {
        error = "profile config contains no profiles";
        return std::nullopt;
    }
    if (!validate_dependencies(config, error)) return std::nullopt;
    if (config.default_profile.empty()) config.default_profile = config.profiles.front().id;
    return config;
}

const LaunchProfile *find_profile(const ProfileConfig &config, const std::string &id) noexcept {
    for (const auto &profile : config.profiles) {
        if (profile.id == id) return &profile;
    }
    return nullptr;
}

}   // namespace skl::wcas
