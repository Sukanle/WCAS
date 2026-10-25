#include <signal.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <mutex>
#include <optional>
#include <regex>   //TODO: use pcre2, stl regex has performance problems
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>

#ifndef _WIN32
#  include <unistd.h>
#  include <sys/wait.h>
#endif

#include "SKL/WCAS/Private/Debug.h"
#include "SKL/WCAS/Private/Harness/Session.h"

namespace skl::wcas::detail {
namespace {

optional<pid_t> parent_pid(pid_t pid) {
    std::ifstream file("/proc/" + std::to_string(pid) + "/stat");
    std::string line;
    if (!file || !std::getline(file, line)) return std::nullopt;
    const auto close = line.rfind(')');
    if (close == std::string::npos) return std::nullopt;
    std::istringstream fields(line.substr(close + 2));
    char state = 0;
    pid_t parent = -1;
    return (fields >> state >> parent) ? optional<pid_t>(parent) : std::nullopt;
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
        text.append(exe, static_cast<size_t>(length));
    }
    for (char &c : text) {
        if (c == '\\') c = '/';
    }
    return text;
}

/// Compile `match_regex`, or nothing when it is empty or invalid.
optional<std::regex> compile_pattern(const std::string &match_regex) {
    if (match_regex.empty()) return std::nullopt;
    try {
        return std::regex(match_regex);
    } catch (const std::regex_error &) {
        return std::nullopt;
    }
}

std::mutex sessions_mutex;
/// Keyed by a unique connection id (never by pid: one process may open several
/// Wayland connections).
std::unordered_map<uint64_t, std::weak_ptr<proxy::ConnState>> sessions;
uint64_t next_session_id = 1;

/// True when `state` belongs to the launch rooted at `pid`: either the owner is
/// `pid`/a descendant of it, or — when `pattern` is set — the owner's command
/// line matches. The second test is what keeps a detached `wineserver` matched
/// after its ancestors have exited and it has been re-parented to init.
bool session_belongs_to(
    const std::shared_ptr<proxy::ConnState> &state, pid_t pid, const optional<std::regex> &pattern) {
    if (state->owner_pid <= 0) return false;
    const auto owner = static_cast<pid_t>(state->owner_pid);
    if (owner == pid || descendant(owner, pid)) return true;
    return pattern && std::regex_search(process_text(owner), *pattern);
}

}   // namespace

void register_session(pid_t pid, const std::shared_ptr<proxy::ConnState> &state) {
    (void)pid;   // owner_pid already lives on the state
    std::lock_guard lock(sessions_mutex);
    sessions[next_session_id++] = state;
    debug_trace(
        DebugCategory::Harness, fmt::format("session registered owner_pid={}", static_cast<long>(state->owner_pid)));
}

void erase_session(const std::shared_ptr<proxy::ConnState> &state) {
    std::lock_guard lock(sessions_mutex);
    for (auto it = sessions.begin(); it != sessions.end();) {
        auto locked = it->second.lock();
        if (!locked || locked == state)
            it = sessions.erase(it);
        else
            ++it;
    }
}

void erase_sessions_for(pid_t pid) {
    std::lock_guard lock(sessions_mutex);
    for (auto it = sessions.begin(); it != sessions.end();) {
        auto locked = it->second.lock();
        if (!locked || (locked->owner_pid > 0 && static_cast<pid_t>(locked->owner_pid) == pid))
            it = sessions.erase(it);
        else
            ++it;
    }
}

std::shared_ptr<proxy::ConnState> session_for(pid_t pid, const std::string &match_regex) {
    const auto pattern = compile_pattern(match_regex);
    std::lock_guard lock(sessions_mutex);
    std::shared_ptr<proxy::ConnState> fallback;
    auto consider = [&fallback](const std::shared_ptr<proxy::ConnState> &state) {
        if (!state || !state->connected.load()) return false;
        if (state->capturable_surface.load()) return true;
        if (!fallback) fallback = state;
        return false;
    };
    for (auto it = sessions.begin(); it != sessions.end();) {
        auto state = it->second.lock();
        if (!state) {
            it = sessions.erase(it);
            continue;
        }
        // Prefer an exact pid match over a descendant or a name match.
        if (state->owner_pid == pid && state->connected.load() && consider(state)) return state;
        if (session_belongs_to(state, pid, pattern) && consider(state)) return state;
        ++it;
    }
    return fallback;
}

std::vector<std::shared_ptr<proxy::ConnState>> sessions_for(pid_t pid, const std::string &match_regex) {
    std::vector<std::shared_ptr<proxy::ConnState>> out;
    const auto pattern = compile_pattern(match_regex);
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
        if (session_belongs_to(state, pid, pattern)) out.push_back(state);
        ++it;
    }
    std::sort(out.begin(), out.end(), [](const auto &a, const auto &b) { return a->owner_pid < b->owner_pid; });
    return out;
}

std::vector<SessionStatus> session_status(pid_t root, const std::string &match_regex) {
    std::vector<SessionStatus> out;
    const auto pattern = compile_pattern(match_regex);
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
        status.matched = session_belongs_to(state, root, pattern);
        status.windows = proxy::list_windows(state).size();
        const auto counters = proxy::session_counters(state);
        status.surfaces = counters.surfaces;
        status.toplevels = counters.toplevels;
        status.has_keyboard = counters.has_keyboard;
        status.has_pointer = counters.has_pointer;
        status.has_keymap = counters.has_keymap;
        if (state->owner_pid > 0) status.command = process_text(static_cast<pid_t>(state->owner_pid));
        out.push_back(status);
        ++it;
    }
    std::sort(out.begin(), out.end(),
        [](const SessionStatus &a, const SessionStatus &b) { return a.owner_pid < b.owner_pid; });
    return out;
}

void terminate_group(pid_t pid) noexcept {
    if (pid <= 0) return;
    debug_trace(DebugCategory::Harness, fmt::format("terminating process group pid={}", pid));
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
