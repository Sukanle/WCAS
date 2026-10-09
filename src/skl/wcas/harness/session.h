#pragma once

#include "proxy/conn.h"

#include <memory>
#include <string>
#include <sys/types.h>
#include <vector>

namespace skl::wcas::detail {

/// Locate the proxy session belonging to `pid` or one of its descendants,
/// preferring a connection that already has a capturable surface. A launched
/// umu/Wine app opens several Wayland connections (launcher CEF, helpers, the
/// game), so the capturable one is the useful match.
std::shared_ptr<proxy::ConnState> session_for(pid_t pid);

/// All sessions belonging to `pid` or its descendants. When `match_regex` is
/// non-empty, also include sessions whose owner's command line / executable
/// path (normalised `\\` -> `/`) matches it — this catches child windows that
/// are launched later and re-parented out of the tree.
std::vector<std::shared_ptr<proxy::ConnState>> sessions_for(pid_t pid, const std::string &match_regex);

/// Diagnostic snapshot of every registered session (including disconnected /
/// unmatched ones) for the `sessions` CLI command.
struct SessionStatus {
    long owner_pid = 0;
    bool connected = false;
    bool matched = false;
    std::size_t windows = 0;
    std::size_t surfaces = 0;
    std::size_t toplevels = 0;
};
std::vector<SessionStatus> session_status(pid_t root, const std::string &match_regex);

/// Register / remove a proxy session keyed by the connecting process id.
void register_session(pid_t pid, const std::shared_ptr<proxy::ConnState> &state);
void erase_session(pid_t pid);

/// SIGINT -> bounded wait -> SIGTERM -> SIGKILL a process group and reap the
/// leader process.
void terminate_group(pid_t pid) noexcept;

}   // namespace skl::wcas::detail
