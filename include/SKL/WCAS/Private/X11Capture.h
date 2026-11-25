#pragma once

#include <string>

#include "SKL/WCAS/DRM.h"
#include "SKL/WCAS/Window.h"

namespace skl {
namespace wcas {

/// Capture the largest mapped X11 window on `display` (an XWayland or plain X
/// server) via XComposite + XGetImage, trying each top-level window until one
/// yields non-uniform content. Falls back to the root window when no suitable
/// child exists. `xauthority` may be empty to use the ambient one.
auto capture_x11_window(const std::string &display, const std::string &xauthority) -> result<cv::Mat>;

/// Capture one specific X11 window.
auto capture_x11_window(const std::string &display, const std::string &xauthority, uint64_t window_id)
    -> result<cv::Mat>;

/// List mapped top-level X11 windows. `surface_id` is the X window id, so a
/// caller can pass it back to X11-specific capture/selection code.
auto list_x11_windows(const std::string &display, const std::string &xauthority) -> result<std::vector<WindowInfo>>;

/// Absolute root coordinates and client size of a mapped X11 window.
auto x11_window_rect(const std::string &display, const std::string &xauthority, uint64_t window_id) -> result<cv::Rect>;

/// Give the private X11 bridge display keyboard focus to `window_id` and raise it.
/// This does not affect the user's real Wayland seat.
auto x11_focus_window(const std::string &display, const std::string &xauthority, uint64_t window_id) -> result<void>;

/// Pick the largest mapped X11 window as the default target.
auto x11_default_target(const std::string &display, const std::string &xauthority) -> result<TargetId>;

/// True when `image` contains non-uniform content (not a flat colour).
bool x11_frame_has_visible_content(const cv::Mat &image);

}   // namespace wcas
}   // namespace skl
