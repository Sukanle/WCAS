#include "harness/session.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <fstream>
#include <mutex>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>

#include <sys/wait.h>
#include <unistd.h>

namespace skl::wcas::detail {
namespace {

std::optional<pid_t> parent_pid(pid_t pid) {
    std::ifstream file("/proc/" + std::to_string(pid) + "/stat");
    std::string line;
    if (!file || !std::getline(file, line)) return std::nullopt;
    const auto close = line.rfind(')');
    if (close == std::string::npos) return std::nullopt;
    std::istringstream fields(line.substr(close + 2));
    char state = 0;
    pid_t parent = -1;
    return (fields >> state >> parent) ? std::optional<pid_t>(parent) : std::nullopt;
}

/// True when `pid` has `ancestor` somewhere up its parent chain.
bool descendant(pid_t pid, pid_t ancestor) {
    for (unsigned depth = 0; pid > 1 && depth < 128; ++depth) {
        if (pid == ancestor) return true;
        const auto parent = parent_pid(pid);
        if (!parent || *parent == pid) return false;
        pid = *parent;
    }
    return pid == ancestor;
}

/// Command line + executable path of `pid`, NULs/backslashes normalised so a
/// regex can match both Unix and Wine (`Z:\...`) forms.
std::string process_text(pid_t pid) {
    std::string text;
    std::ifstream cmdline("/proc/" + std::to_string(pid) + "/cmdline", std::ios::binary);
    if (cmdline) {
        std::string content((std::istreambuf_iterator<char>(cmdline)), std::istreambuf_iterator<char>());
        for (char &c : content) {
            if (c == '\0') c = ' ';
        }
        text += content;
    }
    char exe[4'096] = {};
    const auto length = ::readlink(("/proc/" + std::to_string(pid) + "/exe").c_str(), exe, sizeof(exe) - 1);
    if (length > 0) {
        text += ' ';
        text.append(exe, static_cast<std::size_t>(length));
    }
    for (char &c : text) {
        if (c == '\\') c = '/';
    }
    return text;
}

std::mutex sessions_mutex;
std::unordered_map<pid_t, std::weak_ptr<proxy::ConnState>> sessions;

}   // namespace

std::shared_ptr<proxy::ConnState> session_for(pid_t pid) {
    std::lock_guard lock(sessions_mutex);
    std::shared_ptr<proxy::ConnState> fallback;
    auto consider = [&fallback](const std::shared_ptr<proxy::ConnState> &state) {
        if (!state || !state->connected.load()) return false;
        if (state->capturable_surface.load()) return true;
        if (!fallback) fallback = state;
        return false;
    };
    // `descendant(p, a)` is true when p has a as an ancestor. The session owner
    // is the process that connected to the proxy (for umu/Wine a grandchild of
    // the launched process), so ask whether the owner is a descendant of `pid`.
    auto belongs = [pid](const std::shared_ptr<proxy::ConnState> &state) {
        return state->owner_pid > 0
            && (state->owner_pid == pid || descendant(static_cast<pid_t>(state->owner_pid), pid));
    };
    if (auto exact = sessions.find(pid); exact != sessions.end()) {
        if (auto state = exact->second.lock(); state && consider(state)) return state;
    }
    for (auto it = sessions.begin(); it != sessions.end();) {
        auto state = it->second.lock();
        if (!state) {
            it = sessions.erase(it);
            continue;
        }
        if (belongs(state) && consider(state)) return state;
        ++it;
    }
    return fallback;
}

std::vector<std::shared_ptr<proxy::ConnState>> sessions_for(pid_t pid, const std::string &match_regex) {
    std::vector<std::shared_ptr<proxy::ConnState>> out;
    std::optional<std::regex> pattern;
    if (!match_regex.empty()) {
        try {
            pattern.emplace(match_regex);
        } catch (const std::regex_error &) {
            pattern.reset();
        }
    }
    std::lock_guard lock(sessions_mutex);
    for (auto it = sessions.begin(); it != sessions.end();) {
        auto state = it->second.lock();
        if (!state) {
            it = sessions.erase(it);
            continue;
        }
        // Only live connections: a closed child window's session stays alive
        // while a caller still holds it, and listing it would let `list`/
        // `target` pin control to a dead window.
        if (!state->connected.load()) {
            ++it;
            continue;
        }
        bool include = false;
        if (state->owner_pid > 0) {
            const auto owner = static_cast<pid_t>(state->owner_pid);
            include = owner == pid || descendant(owner, pid);
            if (!include && pattern) include = std::regex_search(process_text(owner), *pattern);
        }
        if (include) out.push_back(state);
        ++it;
    }
    std::sort(out.begin(), out.end(), [](const auto &a, const auto &b) { return a->owner_pid < b->owner_pid; });
    return out;
}

std::vector<SessionStatus> session_status(pid_t root, const std::string &match_regex) {
    std::vector<SessionStatus> out;
    std::optional<std::regex> pattern;
    if (!match_regex.empty()) {
        try {
            pattern.emplace(match_regex);
        } catch (const std::regex_error &) {
            pattern.reset();
        }
    }
    std::lock_guard lock(sessions_mutex);
    for (auto it = sessions.begin(); it != sessions.end();) {
        auto state = it->second.lock();
        if (!state) {
            it = sessions.erase(it);
            continue;
        }
        SessionStatus status;
        status.owner_pid = static_cast<long>(state->owner_pid);
        status.connected = state->connected.load();
        if (state->owner_pid > 0) {
            const auto owner = static_cast<pid_t>(state->owner_pid);
            status.matched = owner == root || descendant(owner, root);
            if (!status.matched && pattern) status.matched = std::regex_search(process_text(owner), *pattern);
        }
        status.windows = proxy::list_windows(state).size();
        const auto counters = proxy::session_counters(state);
        status.surfaces = counters.surfaces;
        status.toplevels = counters.toplevels;
        if (state->owner_pid > 0) status.command = process_text(static_cast<pid_t>(state->owner_pid));
        out.push_back(status);
        ++it;
    }
    std::sort(out.begin(), out.end(),
        [](const SessionStatus &a, const SessionStatus &b) { return a.owner_pid < b.owner_pid; });
    return out;
}

void register_session(pid_t pid, const std::shared_ptr<proxy::ConnState> &state) {
    std::lock_guard lock(sessions_mutex);
    sessions[pid] = state;
}

void erase_session(pid_t pid) {
    std::lock_guard lock(sessions_mutex);
    sessions.erase(pid);
}

void terminate_group(pid_t pid) noexcept {
    if (pid <= 0) return;
    const auto group = -pid;
    ::kill(group, SIGINT);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    int status = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        if (::waitpid(pid, &status, WNOHANG) == pid) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ::kill(group, SIGTERM);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    ::kill(group, SIGKILL);
    (void)::waitpid(pid, &status, 0);
}

}   // namespace skl::wcas::detail
