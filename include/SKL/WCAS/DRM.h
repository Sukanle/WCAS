#pragma once

#include "SKL/WCAS/Types.h"

#include <boost/outcome.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/lexical_cast.hpp>
#include <opencv2/opencv.hpp>
#include <boost/filesystem.hpp>

namespace skl::wcas {

template<typename T>
using result = boost::outcome_v2::std_result<T>;

auto from_swaymsg(pid_t pid) -> optional<cv::Rect>;
auto from_swaytree(pid_t pid) -> optional<cv::Rect>;
auto from_hyprctl(pid_t pid) -> optional<cv::Rect>;
auto from_xdotool(pid_t pid) -> optional<cv::Rect>;
auto parse_xdotool(std::string_view &str) -> optional<cv::Rect>;

auto parse(std::string_view str) -> optional<cv::Rect>;
auto extract(std::string_view str, std::string_view key) -> optional<cv::Rect::value_type>;

auto find_window_position_by_size_and_title(cv::Size size, optional<std::string_view> title) -> optional<cv::Rect>;
auto from_swaymsg_by_size_and_title(std::string_view json, cv::Size size, optional<std::string_view> title)
    -> optional<cv::Rect>;
auto from_swaytree_by_size_and_title(std::string_view json, cv::Size size, optional<std::string_view> title)
    -> optional<cv::Rect>;
auto from_hyprctl_by_size_and_title(cv::Size size, optional<std::string_view> title) -> optional<cv::Rect>;
auto from_xdotool_by_size_and_title(cv::Size size, optional<std::string_view> title) -> optional<cv::Rect>;

auto find_window(pid_t pid) -> optional<cv::Rect>;
auto collect_descendant(pid_t pid) -> std::vector<decltype(pid)>;
auto read_pid_children(pid_t pid) -> optional<std::vector<decltype(pid)>>;

auto capture_compositor_screen(uint32_t pid) -> result<cv::Mat>;
auto capture_via_grim(optional<cv::Rect> rect) -> result<cv::Mat>;
auto capture_via_spectacle() -> result<cv::Mat>;
auto capture_gnome_screenshot() -> result<cv::Mat>;
// Kernel-level capture via libdrmtap, optionally cropped to a window region.
auto capture_kms_screen(optional<cv::Rect> crop) -> result<cv::Mat>;
}   // namespace skl::wcas
