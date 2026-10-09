#pragma once

#include "skl/wcas/wcas.h"
#include "xwayland.h"

#include "proxy/conn.h"

#include <memory>
#include <string>
#include <sys/types.h>

namespace skl::wcas {

/// Shared out-of-class definition of `ControlledApp::Impl`. It lives in a
/// header because `ComputerUse::launch` (a friend) constructs the app while
/// `ControlledApp`'s own methods live in a separate translation unit.
struct ControlledApp::Impl {
    pid_t child = -1;
    pid_t process_group = -1;
    Transport transport = Transport::native_wayland;
    std::shared_ptr<proxy::ConnState> session;
    std::unique_ptr<XwaylandBridge> bridge;
    /// Extra process matcher for non-descendant child windows (see
    /// LaunchConfig::match_regex).
    std::string match_regex;
    /// Requested capture backend, re-applied when the target window changes so
    /// a newly selected session captures through the same backend.
    CaptureBackend backend = CaptureBackend::client_buffer;
};

}   // namespace skl::wcas
