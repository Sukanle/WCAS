#include "SKL/WCAS/Private/X11Capture.h"
#include "SKL/WCAS/Error.h"

#include <xcb/composite.h>
#include <xcb/xcb.h>

#include <algorithm>
#include <stdint.h>
#include <stdlib.h>
#include <stdlib.h>
#include <unordered_map>
#include <utility>
#include <vector>

namespace skl {
namespace wcas {
namespace {

/// Owns an XCB connection. `XAUTHORITY` is set only for the duration of the
/// connect so a bridge-specific cookie can be used without leaking it.
struct XcbHandle {
    xcb_connection_t *connection = nullptr;

    XcbHandle(const std::string &display, const std::string &xauthority) {
        const char *previous = std::getenv("XAUTHORITY");
        const std::string saved = previous != nullptr ? previous : "";
        const bool override_auth = !xauthority.empty();
        if (override_auth) ::setenv("XAUTHORITY", xauthority.c_str(), 1);
        connection = xcb_connect(display.empty() ? nullptr : display.c_str(), nullptr);
        if (override_auth) {
            if (previous != nullptr)
                ::setenv("XAUTHORITY", saved.c_str(), 1);
            else
                ::unsetenv("XAUTHORITY");
        }
        if (connection != nullptr && xcb_connection_has_error(connection)) {
            xcb_disconnect(connection);
            connection = nullptr;
        }
    }
    ~XcbHandle() {
        if (connection != nullptr) xcb_disconnect(connection);
    }
    XcbHandle(const XcbHandle &) = delete;
    XcbHandle &operator=(const XcbHandle &) = delete;
};

/// Decode a ZPixmap from XGetImage into a BGR `cv::Mat`. The byte order is
/// inferred from the payload size, so 24-bit (3-byte) and 32-bit (4-byte)
/// servers both work; 16-bit visuals are expanded from RGB565.
cv::Mat decode_ximage(const uint8_t *data, size_t size, int width, int height) {
    const auto pixels = static_cast<size_t>(width) * static_cast<size_t>(height);
    if (pixels == 0 || size < pixels) return {};
    const size_t bytes_per_pixel = size / pixels;

    cv::Mat result(height, width, CV_8UC3);
    if (bytes_per_pixel >= 3) {
        for (int y = 0; y < height; ++y) {
            const auto *row = data + static_cast<size_t>(y) * static_cast<size_t>(width) * bytes_per_pixel;
            for (int x = 0; x < width; ++x) {
                const auto *pixel = row + static_cast<size_t>(x) * bytes_per_pixel;
                // X stores 24/32-bit ZPixmaps as B, G, R (X).
                result.at<cv::Vec3b>(y, x) = cv::Vec3b(pixel[0], pixel[1], pixel[2]);
            }
        }
    } else if (bytes_per_pixel == 2) {
        for (int y = 0; y < height; ++y) {
            const auto *row = data + static_cast<size_t>(y) * static_cast<size_t>(width) * 2u;
            for (int x = 0; x < width; ++x) {
                const auto *pixel = row + static_cast<size_t>(x) * 2u;
                const auto value = static_cast<uint16_t>(pixel[0] | (pixel[1] << 8));
                const auto r = static_cast<uint8_t>((value >> 11) & 0x1f);
                const auto g = static_cast<uint8_t>((value >> 5) & 0x3f);
                const auto b = static_cast<uint8_t>(value & 0x1f);
                result.at<cv::Vec3b>(y, x) = cv::Vec3b(static_cast<uint8_t>((b << 3) | (b >> 2)),
                    static_cast<uint8_t>((g << 2) | (g >> 4)), static_cast<uint8_t>((r << 3) | (r >> 2)));
            }
        }
    } else {
        return {};
    }
    return result;
}

/// Capture one X11 window. When XComposite is available the window is
/// redirected to an offscreen pixmap first so DRI3-rendered content becomes
/// readable; otherwise the plain XGetImage result is used.
cv::Mat capture_window(xcb_connection_t *connection, xcb_window_t window) {
    auto *geometry = xcb_get_geometry_reply(connection, xcb_get_geometry(connection, window), nullptr);
    if (geometry == nullptr) return {};
    const int width = geometry->width;
    const int height = geometry->height;
    std::free(geometry);
    if (width <= 0 || height <= 0) return {};

    bool redirected = false;
    if (auto *version =
            xcb_composite_query_version_reply(connection, xcb_composite_query_version(connection, 0, 2), nullptr)) {
        const bool supported = version->major_version > 0 || version->minor_version >= 2;
        std::free(version);
        if (supported) {
            auto redirect = xcb_composite_redirect_window(connection, window, XCB_COMPOSITE_REDIRECT_MANUAL);
            if (xcb_generic_error_t *error = xcb_request_check(connection, redirect)) {
                std::free(error);
            } else {
                redirected = true;
            }
        }
    }

    cv::Mat result;
    if (auto *image = xcb_get_image_reply(connection,
            xcb_get_image(connection, XCB_IMAGE_FORMAT_Z_PIXMAP, window, 0, 0, static_cast<uint16_t>(width),
                static_cast<uint16_t>(height), ~0u),
            nullptr)) {
        const auto *data = xcb_get_image_data(image);
        const auto length = static_cast<size_t>(xcb_get_image_data_length(image));
        result = decode_ximage(data, length, width, height);
        std::free(image);
    }

    if (redirected) {
        xcb_composite_unredirect_window(connection, window, XCB_COMPOSITE_REDIRECT_MANUAL);
        xcb_flush(connection);
    }
    return result;
}


std::string interned_string_property(
    xcb_connection_t *connection, xcb_window_t window, const char *name, xcb_atom_t type) {
    auto *atom_reply = xcb_intern_atom_reply(connection, xcb_intern_atom(connection, 0, strlen(name), name), nullptr);
    if (atom_reply == nullptr) return {};
    const xcb_atom_t atom = atom_reply->atom;
    std::free(atom_reply);

    auto *property =
        xcb_get_property_reply(connection, xcb_get_property(connection, 0, window, atom, type, 0, 1'024), nullptr);
    if (property == nullptr) return {};
    std::string value;
    if (property->format == 8 && xcb_get_property_value_length(property) > 0) {
        const auto *data = static_cast<const char *>(xcb_get_property_value(property));
        value.assign(data, static_cast<size_t>(xcb_get_property_value_length(property)));
    }
    std::free(property);
    return value;
}

std::string window_title(xcb_connection_t *connection, xcb_window_t window) {
    auto title = interned_string_property(connection, window, "_NET_WM_NAME", XCB_ATOM_ANY);
    if (!title.empty()) return title;
    return interned_string_property(connection, window, "WM_NAME", XCB_ATOM_STRING);
}

std::string window_class(xcb_connection_t *connection, xcb_window_t window) {
    auto *property = xcb_get_property_reply(
        connection, xcb_get_property(connection, 0, window, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 0, 1'024), nullptr);
    if (property == nullptr) return {};
    std::string value;
    if (property->format == 8 && xcb_get_property_value_length(property) > 0) {
        const auto *data = static_cast<const char *>(xcb_get_property_value(property));
        const size_t length = static_cast<size_t>(xcb_get_property_value_length(property));
        const size_t first = std::string(data, length).find('\0');
        if (first != std::string::npos && first + 1 < length) {
            const size_t end = std::string(data + first + 1, length - first - 1).find('\0');
            value.assign(data + first + 1, end == std::string::npos ? length - first - 1 : end);
        }
    }
    std::free(property);
    return value;
}

uint32_t window_pid(xcb_connection_t *connection, xcb_window_t window) {
    auto *atom_reply = xcb_intern_atom_reply(connection, xcb_intern_atom(connection, 0, 12, "_NET_WM_PID"), nullptr);
    if (atom_reply == nullptr) return 0;
    const xcb_atom_t atom = atom_reply->atom;
    std::free(atom_reply);
    auto *property = xcb_get_property_reply(
        connection, xcb_get_property(connection, 0, window, atom, XCB_ATOM_CARDINAL, 0, 1), nullptr);
    if (property == nullptr) return 0;
    uint32_t pid = 0;
    if (property->format == 32 && xcb_get_property_value_length(property) >= 4)
        pid = *static_cast<const uint32_t *>(xcb_get_property_value(property));
    std::free(property);
    return pid;
}

}   // namespace

bool x11_frame_has_visible_content(const cv::Mat &image) {
    if (image.empty() || image.type() != CV_8UC3) return false;
    constexpr size_t sample_step = 64;
    constexpr double blank_threshold = 0.02;

    const auto total = static_cast<size_t>(image.rows) * static_cast<size_t>(image.cols);
    std::unordered_map<uint32_t, int> counts;
    int sampled = 0;
    for (size_t index = 0; index < total; index += sample_step) {
        const auto *pixel = image.data + index * 3u;
        const uint32_t key = (static_cast<uint32_t>(pixel[0]) << 16)
                           | (static_cast<uint32_t>(pixel[1]) << 8)
                           | static_cast<uint32_t>(pixel[2]);
        ++counts[key];
        ++sampled;
    }
    if (sampled == 0) return false;

    int max_count = 0;
    for (const auto &[key, count] : counts)
        max_count = std::max(max_count, count);
    const double non_dominant = 1.0 - static_cast<double>(max_count) / static_cast<double>(sampled);
    return non_dominant > blank_threshold;
}


auto list_x11_windows(const std::string &display, const std::string &xauthority) -> result<std::vector<WindowInfo>> {
    XcbHandle handle(display, xauthority);
    if (handle.connection == nullptr)
        return boost::outcome_v2::failure(skl::wcas::from_std_errc(std::errc::connection_refused));
    xcb_connection_t *connection = handle.connection;

    const xcb_setup_t *setup = xcb_get_setup(connection);
    xcb_screen_t *screen = xcb_setup_roots_iterator(setup).data;
    if (screen == nullptr) return boost::outcome_v2::failure(skl::wcas::from_std_errc(std::errc::io_error));

    std::vector<WindowInfo> windows;
    if (auto *tree = xcb_query_tree_reply(connection, xcb_query_tree(connection, screen->root), nullptr)) {
        const int count = xcb_query_tree_children_length(tree);
        xcb_window_t *children = xcb_query_tree_children(tree);
        for (int i = 0; i < count; ++i) {
            auto *attributes = xcb_get_window_attributes_reply(
                connection, xcb_get_window_attributes(connection, children[i]), nullptr);
            if (attributes == nullptr) continue;
            const bool viewable = attributes->map_state == XCB_MAP_STATE_VIEWABLE;
            std::free(attributes);
            if (!viewable) continue;

            auto *geometry = xcb_get_geometry_reply(connection, xcb_get_geometry(connection, children[i]), nullptr);
            if (geometry == nullptr) continue;
            if (geometry->width <= 0 || geometry->height <= 0) {
                std::free(geometry);
                continue;
            }
            WindowInfo info;
            info.app_id = window_class(connection, children[i]);
            info.title = window_title(connection, children[i]);
            info.width = geometry->width;
            info.height = geometry->height;
            info.pid = static_cast<int32_t>(window_pid(connection, children[i]));
            info.selected = false;
            info.origin = WindowOrigin::Toplevel;
            info.surface_id = children[i];
            info.target = TargetId{children[i], TargetKind::X11Window};
            std::free(geometry);
            windows.push_back(std::move(info));
        }
        std::free(tree);
    }
    return boost::outcome_v2::success(std::move(windows));
}

auto capture_x11_window(const std::string &display, const std::string &xauthority) -> result<cv::Mat> {
    XcbHandle handle(display, xauthority);
    if (handle.connection == nullptr) {
        return boost::outcome_v2::failure(skl::wcas::from_std_errc(std::errc::connection_refused));
    }
    xcb_connection_t *connection = handle.connection;

    const xcb_setup_t *setup = xcb_get_setup(connection);
    xcb_screen_t *screen = xcb_setup_roots_iterator(setup).data;
    if (screen == nullptr) {
        return boost::outcome_v2::failure(skl::wcas::from_std_errc(std::errc::io_error));
    }

    // Candidate windows: mapped children of the root, largest first. A
    // GPU-accelerated app may render into a sub-window while the top-level
    // stays blank, so try each until one has visible content.
    std::vector<std::pair<xcb_window_t, uint32_t>> windows;
    if (auto *tree = xcb_query_tree_reply(connection, xcb_query_tree(connection, screen->root), nullptr)) {
        const int count = xcb_query_tree_children_length(tree);
        xcb_window_t *children = xcb_query_tree_children(tree);
        for (int i = 0; i < count; ++i) {
            auto *attributes = xcb_get_window_attributes_reply(
                connection, xcb_get_window_attributes(connection, children[i]), nullptr);
            if (attributes == nullptr) continue;
            const bool viewable = attributes->map_state == XCB_MAP_STATE_VIEWABLE;
            std::free(attributes);
            if (!viewable) continue;

            auto *geometry = xcb_get_geometry_reply(connection, xcb_get_geometry(connection, children[i]), nullptr);
            if (geometry == nullptr) continue;
            const auto area = static_cast<uint32_t>(geometry->width) * geometry->height;
            std::free(geometry);
            if (area < 10'000) continue;
            windows.emplace_back(children[i], area);
        }
        std::free(tree);
    }
    std::sort(windows.begin(), windows.end(), [](const auto &a, const auto &b) { return a.second > b.second; });

    if (windows.empty()) {
        cv::Mat root = capture_window(connection, screen->root);
        if (root.empty()) {
            return boost::outcome_v2::failure(skl::wcas::from_std_errc(std::errc::io_error));
        }
        return root;
    }

    cv::Mat fallback;
    for (const auto &[window, area] : windows) {
        (void)area;
        cv::Mat image = capture_window(connection, window);
        if (image.empty()) continue;
        if (x11_frame_has_visible_content(image)) return image;
        if (fallback.empty()) fallback = image;
    }
    if (fallback.empty()) {
        return boost::outcome_v2::failure(skl::wcas::from_std_errc(std::errc::io_error));
    }
    return fallback;
}

}   // namespace wcas
}   // namespace skl
