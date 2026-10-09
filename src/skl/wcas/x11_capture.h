#pragma once

#include <string>

#include "skl/wcas/drm.h"

namespace skl {
namespace wcas {

/// Capture the largest mapped X11 window on `display` (an XWayland or plain X
/// server) via XComposite + XGetImage, trying each top-level window until one
/// yields non-uniform content. Falls back to the root window when no suitable
/// child exists. `xauthority` may be empty to use the ambient one.
auto capture_x11_window(const std::string &display, const std::string &xauthority) -> result<cv::Mat>;

/// True when `image` contains non-uniform content (not a flat colour).
bool x11_frame_has_visible_content(const cv::Mat &image);

}   // namespace wcas
}   // namespace skl
