#pragma once

#include <string>
#include <sys/types.h>

namespace skl {
namespace wcas {

/// An authenticated XWayland bridge bound to a private Wayland display.
///
/// Starts a dedicated `Xwayland` process that renders into a Wayland surface on
/// the display named by `WAYLAND_DISPLAY`, giving X11-only applications a
/// compatibility path. The process and its Xauthority file are cleaned up on
/// destruction.
class XwaylandBridge {
public:
    XwaylandBridge() = default;
    ~XwaylandBridge();
    XwaylandBridge(const XwaylandBridge &) = delete;
    XwaylandBridge &operator=(const XwaylandBridge &) = delete;

    /// Start Xwayland against `wayland_display`, allocating X display
    /// `display_number` (e.g. 5 for ":5"). Returns false when Xwayland is
    /// unavailable or never becomes reachable.
    bool start(const std::string &wayland_display, int display_number);
    void stop() noexcept;

    [[nodiscard]] bool running() const noexcept { return child_ > 0; }
    [[nodiscard]] const std::string &display() const noexcept { return display_; }
    [[nodiscard]] const std::string &xauthority() const noexcept { return xauthority_; }
    [[nodiscard]] const std::string &runtime_dir() const noexcept { return runtime_dir_; }

private:
    pid_t child_ = -1;
    std::string runtime_dir_;
    std::string display_;
    std::string xauthority_;
};

}   // namespace wcas
}   // namespace skl
