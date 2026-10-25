#include <stdio.h>
#include <stdlib.h>

#include <array>
#include <string>
#include <vector>
#include <fstream>
#include <algorithm>

#include "SKL/WCAS/DRM.h"

#include <fmt/format.h>
#include <opencv2/imgcodecs.hpp>

#include "drmtap.h"

namespace skl::wcas {
namespace {

/// Run a command and capture its stdout. Returns nullopt when the tool is
/// missing or exits non-zero, so callers can fall through to the next method.
optional<std::string> run_capture(const std::string &command) {
    std::array<char, 4'096> buffer{};
    std::string output;
    FILE *pipe = ::popen(command.c_str(), "r");
    if (pipe == nullptr) return std::nullopt;
    while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        output += buffer.data();
    }
    const int status = ::pclose(pipe);
    if (status != 0) return std::nullopt;
    return output;
}

/// Directory for temporary screenshots, preferring XDG_RUNTIME_DIR.
std::string capture_temp_path() {
    if (const char *runtime = ::getenv("XDG_RUNTIME_DIR"); runtime != nullptr)
        return std::string(runtime) + "/wcas-capture.png";
    return "/tmp/wcas-capture.png";
}

cv::Mat decode_png_file(const std::string &path) {
    cv::Mat image = cv::imread(path, cv::IMREAD_COLOR);
    ::remove(path.c_str());
    return image;
}

}   // namespace

optional<cv::Rect::value_type> extract(std::string_view str, std::string_view key) {
    const std::string pattern = "\"" + std::string(key) + "\": ";
    const auto start = str.find(pattern);
    if (start == std::string_view::npos) return std::nullopt;
    auto rest = str.substr(start + pattern.size());
    const auto end = rest.find_first_of(",}");
    if (end != std::string_view::npos) rest = rest.substr(0, end);
    try {
        return static_cast<cv::Rect::value_type>(std::stoi(std::string(rest)));
    } catch (...) {
        return std::nullopt;
    }
}

optional<cv::Rect> parse(std::string_view str) {
    const auto x = extract(str, "x");
    const auto y = extract(str, "y");
    const auto width = extract(str, "width");
    const auto height = extract(str, "height");
    if (!x || !y || !width || !height) return std::nullopt;
    return cv::Rect(*x, *y, *width, *height);
}

optional<cv::Rect> parse_xdotool(std::string_view &str) {
    auto read = [&str](std::string_view key) -> optional<int> {
        const auto at = str.find(key);
        if (at == std::string_view::npos) return std::nullopt;
        auto rest = str.substr(at + key.size());
        const auto end = rest.find_first_of("\n\r");
        if (end != std::string_view::npos) rest = rest.substr(0, end);
        try {
            return std::stoi(std::string(rest));
        } catch (...) {
            return std::nullopt;
        }
    };
    const auto x = read("X=");
    const auto y = read("Y=");
    const auto width = read("WIDTH=");
    const auto height = read("HEIGHT=");
    if (!x || !y || !width || !height) return std::nullopt;
    return cv::Rect(*x, *y, *width, *height);
}

auto read_pid_children(pid_t pid) -> optional<std::vector<pid_t>> {
    namespace fs = boost::filesystem;
    std::vector<pid_t> children;

    // -------------------- 快速路径 --------------------
    const auto children_path = fmt::format("/proc/{0}/task/{0}/children", pid);
    std::ifstream children_file(children_path);
    std::string data;
    if (children_file.is_open() && std::getline(children_file, data)) {
        std::vector<std::string> parts;
        boost::algorithm::split(parts, data, boost::algorithm::is_space(), boost::algorithm::token_compress_on);
        for (const auto &part : parts) {
            if (part.empty()) continue;
            try {
                children.push_back(boost::lexical_cast<uint32_t>(part));
            } catch (const boost::bad_lexical_cast &) {}
        }
        if (!children.empty()) return children;
    }

    // -------------------- 回退路径 --------------------
    fs::path proc_dir("/proc");
    if (!fs::exists(proc_dir) || !fs::is_directory(proc_dir)) return std::nullopt;

    children.clear();
    for (fs::directory_iterator it(proc_dir); it != fs::directory_iterator(); ++it) {
        if (!fs::is_directory(it->status())) continue;
        std::string name = it->path().filename().string();
        uint32_t candidate_pid;
        try {
            candidate_pid = boost::lexical_cast<uint32_t>(name);
        } catch (const boost::bad_lexical_cast &) {
            continue;
        }
        std::ifstream stat_file("/proc/" + name + "/stat");
        if (!stat_file.is_open()) continue;
        std::string stat_data;
        if (!std::getline(stat_file, stat_data)) continue;
        size_t close_paren = stat_data.rfind(')');
        if (close_paren == std::string::npos) continue;
        std::string after_comm = stat_data.substr(close_paren + 1);
        std::vector<std::string> fields;
        boost::algorithm::split(fields, after_comm, boost::algorithm::is_space(), boost::algorithm::token_compress_on);
        if (fields.size() >= 2) {
            uint32_t ppid;
            try {
                ppid = boost::lexical_cast<uint32_t>(fields[1]);
            } catch (const boost::bad_lexical_cast &) {
                continue;
            }
            if (ppid == pid) children.push_back(candidate_pid);
        }
    }
    return children;
}

auto collect_descendant(pid_t pid) -> std::vector<decltype(pid)> {
    std::vector<pid_t> result{pid};
    std::vector<pid_t> queue{pid};
    for (size_t index = 0; index < queue.size(); ++index) {
        const auto parent = queue[index];
        if (auto children = read_pid_children(parent)) {
            for (const auto child : *children) {
                if (std::ranges::find(result, child) == result.end()) {
                    result.push_back(child);
                    queue.push_back(child);
                }
            }
        }
    }
    return result;
}

auto from_swaymsg(pid_t pid) -> optional<cv::Rect> {
    const auto output = run_capture("swaymsg -t get_tree 2>/dev/null");
    if (!output) return std::nullopt;
    const std::string marker = "\"pid\": " + std::to_string(pid);
    auto search_from = output->find(marker);
    while (search_from != std::string::npos) {
        const auto rect_at = output->rfind("\"rect\"", search_from);
        if (rect_at == std::string::npos) break;
        if (auto rect = parse(std::string_view(*output).substr(rect_at, 256))) {
            if (rect->width > 50 && rect->height > 50) return rect;
        }
        search_from = output->find(marker, search_from + marker.size());
    }
    return std::nullopt;
}

auto from_swaytree(pid_t pid) -> optional<cv::Rect> { return from_swaymsg(pid); }

auto from_hyprctl(pid_t pid) -> optional<cv::Rect> {
    const auto output = run_capture("hyprctl clients -j 2>/dev/null");
    if (!output) return std::nullopt;
    const std::string marker = "\"pid\": " + std::to_string(pid);
    auto search_from = output->find(marker);
    while (search_from != std::string::npos) {
        const auto at = output->find("\"at\": [", search_from);
        const auto size = output->find("\"size\": [", search_from);
        if (at != std::string::npos && size != std::string::npos) {
            const auto values = [&](size_t pos) {
                std::vector<int> parsed;
                auto rest = std::string_view(*output).substr(pos);
                const auto close = rest.find(']');
                if (close == std::string_view::npos) return parsed;
                std::string inner(rest.substr(0, close));
                std::string current;
                for (const char c : inner) {
                    if (c == ',' || c == '[' || c == ' ') {
                        if (!current.empty()) {
                            try {
                                parsed.push_back(std::stoi(current));
                            } catch (...) {}
                            current.clear();
                        }
                    } else {
                        current += c;
                    }
                }
                if (!current.empty()) {
                    try {
                        parsed.push_back(std::stoi(current));
                    } catch (...) {}
                }
                return parsed;
            };
            const auto at_values = values(at + 7);
            const auto size_values = values(size + 9);
            if (at_values.size() >= 2 && size_values.size() >= 2 && size_values[0] > 50 && size_values[1] > 50) {
                return cv::Rect(at_values[0], at_values[1], size_values[0], size_values[1]);
            }
        }
        search_from = output->find(marker, search_from + marker.size());
    }
    return std::nullopt;
}

auto from_xdotool(pid_t pid) -> optional<cv::Rect> {
    const auto windows = run_capture("xdotool search --pid " + std::to_string(pid) + " 2>/dev/null");
    if (!windows) return std::nullopt;
    std::string current;
    for (const char c : *windows) {
        if (c == '\n' || c == '\r') {
            if (!current.empty()) {
                if (auto geometry = run_capture("xdotool getwindowgeometry --shell " + current + " 2>/dev/null")) {
                    std::string_view view(*geometry);
                    if (auto rect = parse_xdotool(view)) {
                        if (rect->width > 50 && rect->height > 50) return rect;
                    }
                }
                current.clear();
            }
        } else {
            current += c;
        }
    }
    return std::nullopt;
}

auto find_window(pid_t pid) -> optional<cv::Rect> {
    for (const auto candidate : collect_descendant(pid)) {
        if (auto rect = from_swaymsg(candidate)) return rect;
        if (auto rect = from_hyprctl(candidate)) return rect;
        if (auto rect = from_xdotool(candidate)) return rect;
    }
    return std::nullopt;
}

auto from_swaytree_by_size_and_title(std::string_view json, cv::Size size, optional<std::string_view> title)
    -> optional<cv::Rect> {
    size_t search_from = 0;
    while (true) {
        const auto rect_at = json.find("\"rect\"", search_from);
        if (rect_at == std::string_view::npos) return std::nullopt;
        const auto open = json.find('{', rect_at);
        if (open == std::string_view::npos) return std::nullopt;
        int depth = 1;
        size_t close = open + 1;
        for (size_t i = open + 1; i < json.size(); ++i) {
            if (json[i] == '{')
                ++depth;
            else if (json[i] == '}') {
                if (--depth == 0) {
                    close = i;
                    break;
                }
            }
        }
        const auto rect = parse(json.substr(open, close - open + 1));
        if (rect && rect->width == size.width && rect->height == size.height) {
            if (!title.has_value()) return rect;
            const auto name_at = json.rfind("\"name\": \"", rect_at);
            if (name_at != std::string_view::npos) {
                auto rest = json.substr(name_at + 9);
                const auto end = rest.find('"');
                if (end != std::string_view::npos && rest.substr(0, end).find(*title) != std::string_view::npos) {
                    return rect;
                }
            }
        }
        search_from = rect_at + 1;
    }
}

auto from_swaymsg_by_size_and_title(std::string_view json, cv::Size size, optional<std::string_view> title)
    -> optional<cv::Rect> {
    return from_swaytree_by_size_and_title(json, size, title);
}

auto from_hyprctl_by_size_and_title(cv::Size size, optional<std::string_view> title) -> optional<cv::Rect> {
    const auto output = run_capture("hyprctl clients -j 2>/dev/null");
    if (!output) return std::nullopt;
    std::string_view json(*output);
    size_t search_from = 0;
    while (true) {
        const auto size_at = json.find("\"size\": [", search_from);
        if (size_at == std::string_view::npos) return std::nullopt;
        auto rest = json.substr(size_at + 9);
        const auto close = rest.find(']');
        if (close == std::string_view::npos) return std::nullopt;
        std::vector<int> values;
        std::string current;
        for (const char c : rest.substr(0, close)) {
            if (c == ',' || c == ' ') {
                if (!current.empty()) {
                    try {
                        values.push_back(std::stoi(current));
                    } catch (...) {}
                    current.clear();
                }
            } else {
                current += c;
            }
        }
        if (!current.empty()) {
            try {
                values.push_back(std::stoi(current));
            } catch (...) {}
        }
        if (values.size() >= 2 && values[0] == size.width && values[1] == size.height) {
            const auto at = json.rfind("\"at\": [", size_at);
            if (at != std::string_view::npos) {
                auto at_rest = json.substr(at + 7);
                const auto at_close = at_rest.find(']');
                if (at_close != std::string_view::npos) {
                    std::vector<int> at_values;
                    std::string token;
                    for (const char c : at_rest.substr(0, at_close)) {
                        if (c == ',' || c == ' ') {
                            if (!token.empty()) {
                                try {
                                    at_values.push_back(std::stoi(token));
                                } catch (...) {}
                                token.clear();
                            }
                        } else {
                            token += c;
                        }
                    }
                    if (!token.empty()) {
                        try {
                            at_values.push_back(std::stoi(token));
                        } catch (...) {}
                    }
                    if (at_values.size() >= 2) return cv::Rect(at_values[0], at_values[1], size.width, size.height);
                }
            }
        }
        search_from = size_at + 1;
    }
}

auto from_xdotool_by_size_and_title(cv::Size size, optional<std::string_view> title) -> optional<cv::Rect> {
    const auto output = run_capture("xdotool getactivewindow getwindowgeometry --shell 2>/dev/null");
    if (!output) return std::nullopt;
    std::string_view view(*output);
    auto rect = parse_xdotool(view);
    if (!rect || rect->width != size.width || rect->height != size.height) return std::nullopt;
    if (title.has_value()) {
        const auto window_at = view.find("WINDOW=");
        if (window_at == std::string_view::npos) return std::nullopt;
        auto window_id = view.substr(window_at + 7);
        const auto end = window_id.find_first_of("\n\r");
        if (end != std::string_view::npos) window_id = window_id.substr(0, end);
        const auto name = run_capture("xdotool getwindowname " + std::string(window_id) + " 2>/dev/null");
        if (!name || name->find(*title) == std::string::npos) return std::nullopt;
    }
    return rect;
}

auto find_window_position_by_size_and_title(cv::Size size, optional<std::string_view> title) -> optional<cv::Rect> {
    if (const auto output = run_capture("swaymsg -t get_tree 2>/dev/null")) {
        if (auto rect = from_swaytree_by_size_and_title(*output, size, title)) return rect;
    }
    if (auto rect = from_hyprctl_by_size_and_title(size, title)) return rect;
    // X11/XWayland: xdotool reports the active window, which is the captured app
    // while it holds focus.
    if (auto rect = from_xdotool_by_size_and_title(size, title)) return rect;
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Compositor screenshot fallbacks
// ---------------------------------------------------------------------------

auto capture_via_grim(optional<cv::Rect> rect) -> result<cv::Mat> {
    const auto path = capture_temp_path();
    std::string command = "grim -t png";
    if (rect.has_value()) command += fmt::format("-g {},{} {}x{}", rect->x, rect->y, rect->width, rect->height);
    command += " \"" + path + "\" 2>/dev/null";
    if (!run_capture(command).has_value()) {
        return boost::outcome_v2::failure(std::make_error_code(std::errc::io_error));
    }
    cv::Mat image = decode_png_file(path);
    if (image.empty()) return boost::outcome_v2::failure(std::make_error_code(std::errc::io_error));
    return image;
}

auto capture_via_spectacle() -> result<cv::Mat> {
    const auto path = capture_temp_path();
    if (!run_capture("spectacle -b -n -o \"" + path + "\" 2>/dev/null").has_value()) {
        return boost::outcome_v2::failure(std::make_error_code(std::errc::io_error));
    }
    cv::Mat image = decode_png_file(path);
    if (image.empty()) return boost::outcome_v2::failure(std::make_error_code(std::errc::io_error));
    return image;
}

auto capture_gnome_screenshot() -> result<cv::Mat> {
    const auto path = capture_temp_path();
    if (!run_capture("gnome-screenshot -f \"" + path + "\" 2>/dev/null").has_value()) {
        return boost::outcome_v2::failure(std::make_error_code(std::errc::io_error));
    }
    cv::Mat image = decode_png_file(path);
    if (image.empty()) return boost::outcome_v2::failure(std::make_error_code(std::errc::io_error));
    return image;
}

auto capture_compositor_screen(uint32_t pid) -> result<cv::Mat> {
    optional<cv::Rect> geometry;
    if (pid != 0) geometry = find_window(static_cast<pid_t>(pid));
    if (auto frame = capture_via_grim(geometry); frame.has_value()) return frame;
    if (auto frame = capture_via_spectacle(); frame.has_value()) return frame;
    if (auto frame = capture_gnome_screenshot(); frame.has_value()) return frame;
    return boost::outcome_v2::failure(std::make_error_code(std::errc::io_error));
}

namespace {

/// Decode a libdrmtap frame (already linearised) into a BGR `cv::Mat`.
cv::Mat decode_kms_frame(const drmtap_frame_info &frame) {
    if (frame.data == nullptr || frame.width == 0 || frame.height == 0) return {};
    constexpr uint32_t argb8888 = 0X34325241U;
    constexpr uint32_t xrgb8888 = 0X34325258U;
    constexpr uint32_t abgr8888 = 0X34324241U;
    constexpr uint32_t xbgr8888 = 0X34324258U;
    const bool bgra = frame.format == argb8888 || frame.format == xrgb8888;
    const bool rgba = frame.format == abgr8888 || frame.format == xbgr8888;
    if (!bgra && !rgba) return {};

    cv::Mat result(static_cast<int>(frame.height), static_cast<int>(frame.width), CV_8UC3);
    const auto *base = static_cast<const unsigned char *>(frame.data);
    for (uint32_t y = 0; y < frame.height; ++y) {
        const auto *row = base + static_cast<size_t>(y) * frame.stride;
        for (uint32_t x = 0; x < frame.width; ++x) {
            const auto *pixel = row + static_cast<size_t>(x) * 4u;
            auto &dst = result.at<cv::Vec3b>(static_cast<int>(y), static_cast<int>(x));
            if (bgra)
                dst = cv::Vec3b(pixel[0], pixel[1], pixel[2]);
            else
                dst = cv::Vec3b(pixel[2], pixel[1], pixel[0]);
        }
    }
    return result;
}

}   // namespace

auto capture_kms_screen(optional<cv::Rect> crop) -> result<cv::Mat> {
    // libdrmtap handles GPU tiling and vendor modifiers, returning linear
    // pixels. Without CAP_SYS_ADMIN (or a configured helper) this fails, and
    // the caller falls through to another capture backend.
    drmtap_ctx *ctx = drmtap_open(nullptr);
    if (ctx == nullptr) {
        return boost::outcome_v2::failure(std::make_error_code(std::errc::permission_denied));
    }

    drmtap_frame_info frame{};
    const int grab = drmtap_grab_mapped(ctx, &frame);
    if (grab != 0) {
        drmtap_close(ctx);
        return boost::outcome_v2::failure(std::make_error_code(std::errc::io_error));
    }

    cv::Mat image = decode_kms_frame(frame);
    drmtap_frame_release(ctx, &frame);
    drmtap_close(ctx);
    if (image.empty()) {
        return boost::outcome_v2::failure(std::make_error_code(std::errc::io_error));
    }

    if (crop.has_value()) {
        cv::Rect region(crop->x, crop->y, crop->width, crop->height);
        region &= cv::Rect(0, 0, image.cols, image.rows);
        if (region.width > 0 && region.height > 0) image = image(region).clone();
    }
    return image;
}

}   // namespace skl::wcas
