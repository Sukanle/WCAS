#pragma once

#include <memory>
#include <string>

#include "SKL/WCAS/Capture.h"
#include "SKL/WCAS/WCAS.h"

#include "SKL/WCAS/Private/XWayland.h"
#include "SKL/WCAS/Private/Proxy/Conn.h"

namespace skl::wcas {

class FocusRuntime;
class ProcessHost;
class WindowRuntime;

/// Transport-owned state shared by capability providers. It deliberately does
/// not include the process host, frame bus, pump or UI policy: those belong to
/// the app runtime, not to a transport/session.
struct SessionContext {
    Transport transport = Transport::native_wayland;
    std::shared_ptr<proxy::ConnState> session;
    std::unique_ptr<XwaylandBridge> bridge;
    std::string match_regex;
    CaptureKind backend = CaptureKind::CLIENTBUF;
    CaptureTarget capture_target;
    TargetId selected_target{};
    pid_t child = -1;
    pid_t process_group = -1;
    /// Whether the app may request compositor activation of its selected
    /// surface. FocusBackend owns the policy; the flag is kept here so session
    /// providers can apply it without knowing the runtime object.
    bool allow_activation = false;
    /// Non-owning pointer to the app's ProcessHost (owned by ControlledApp).
    ProcessHost *process_handle = nullptr;
    /// Set by ControlledApp after the optional runtimes are constructed.
    WindowRuntime *window_runtime = nullptr;
    FocusRuntime *focus_runtime = nullptr;
};

}   // namespace skl::wcas
