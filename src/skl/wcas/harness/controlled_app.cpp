#include "skl/wcas/wcas.h"
#include "skl/wcas/error.h"

#include "harness/controlled_app_impl.h"
#include "harness/session.h"
#include "skl/wcas/drm.h"

#include "proxy/conn.h"
#include "proxy/input.h"
#include "proxy/keymap.h"
#include "proxy/keysym.h"

#include <opencv2/core.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <signal.h>

namespace skl::wcas {
namespace {
std::error_code code(errc value) { return make_error_code(value); }

// Route synthetic input to the app's own surface, not whichever surface
// happened to receive a compositor enter earlier. Prefers the surface already
// focused for keyboard input, then the most recently committed one.
std::uint32_t input_target(const std::shared_ptr<proxy::ConnState> &session) {
    return session ? proxy::target_surface(session) : 0;
}

// Callers express pointer positions in frame (buffer) pixels, but wl_pointer
// expects surface-local (logical) pixels. Divide by the surface scale, which
// is 1.0 when the app does not use buffer scale / wp_viewport.
double to_logical(const std::shared_ptr<proxy::ConnState> &session, std::uint32_t surface, double value) {
    const double scale = proxy::input_scale(session, surface);
    return scale > 0.0 ? value / scale : value;
}

}   // namespace

bool ControlledApp::ensure_active_session() const {
    if (!impl_) return false;
    // Keep the selected session while it is live and actually renders.
    if (impl_->session && impl_->session->connected.load() && impl_->session->capturable_surface.load()) return true;
    if (impl_->child > 0) {
        // Prefer another live session that has a rendered frame (e.g. the main
        // window after a dismissed overlay). Persist it so status/list agree.
        for (const auto &session : detail::sessions_for(impl_->child, impl_->match_regex)) {
            if (session->capturable_surface.load()) {
                impl_->session = session;
                return true;
            }
        }
        if (impl_->session && impl_->session->connected.load()) return true;
        if (auto session = detail::session_for(impl_->child)) {
            impl_->session = session;
            return true;
        }
    }
    return impl_->session && impl_->session->connected.load();
}

ControlledApp::ControlledApp(std::unique_ptr<Impl> p) noexcept
    : impl_(std::move(p)) {}
ControlledApp::ControlledApp(ControlledApp &&) noexcept = default;
ControlledApp &ControlledApp::operator=(ControlledApp &&) noexcept = default;
ControlledApp::~ControlledApp() { stop(); }
int ControlledApp::pid() const noexcept { return impl_ ? impl_->child : -1; }
bool ControlledApp::is_alive() const noexcept { return impl_ && impl_->child > 0 && ::kill(impl_->child, 0) == 0; }
Transport ControlledApp::transport() const noexcept { return impl_ ? impl_->transport : Transport::native_wayland; }
std::uint64_t ControlledApp::total_commit_count() const noexcept {
    return impl_ && impl_->session ? impl_->session->commit_count.load() : 0;
}
std::uint64_t ControlledApp::presented_frames() const noexcept {
    return impl_ && impl_->session ? impl_->session->presented_frames.load() : 0;
}
void ControlledApp::set_capture_backend(CaptureBackend backend) noexcept {
    if (!impl_ || !impl_->session) return;
    // `shm`/`client_buffer` read wl_shm buffers; `dmabuf` reads linux-dmabuf
    // buffers; `kms` reads the DRM framebuffer synchronously. Anything else
    // must fail loudly at capture() rather than silently use a different path.
    const bool supported = backend == CaptureBackend::shm
                        || backend == CaptureBackend::client_buffer
                        || backend == CaptureBackend::dmabuf
                        || backend == CaptureBackend::kms;
    impl_->backend = backend;
    impl_->session->backend_unsupported = !supported;
    impl_->session->capture_backend = static_cast<int>(backend);
    impl_->session->background_capture = supported && backend == CaptureBackend::dmabuf;
}
void ControlledApp::set_capture_target(CaptureTarget target) {
    if (impl_) proxy::set_capture_target(impl_->session, std::move(target.app_id), std::move(target.title));
}
result<CapturedFrame> ControlledApp::capture() const {
    if (!impl_ || !ensure_active_session()) return code(errc::frame_capture_failed);
    auto s = impl_->session;
    if (s->backend_unsupported.load()) return code(errc::feature_not_implemented);
    cv::Mat image;
    {
        // The background scheduler keeps the latest completed frame ready, so
        // prefer it over a synchronous capture.
        std::lock_guard lock(s->data_mutex);
        if (s->pre_captured_frame.has_value()) {
            image = s->pre_captured_frame->clone();
        }
    }
    if (!image.empty()) return CapturedFrame{std::move(image)};
    const auto backend = static_cast<CaptureBackend>(s->capture_backend.load());
    if (backend == CaptureBackend::kms) {
        auto frame = capture_kms_screen(std::nullopt);
        if (frame.has_value()) return CapturedFrame{std::move(frame.value())};
        return code(errc::frame_capture_failed);
    }
    // Prefer the requested backend, but a selected window may use the other
    // buffer type (the game renders through DMA-BUF while a child overlay uses
    // wl_shm). Try both so switching windows keeps working.
    bool ok = backend == CaptureBackend::dmabuf ? proxy::capture_dmabuf(s, image) : proxy::capture_shm(s, image);
    if (!ok) {
        ok = backend == CaptureBackend::dmabuf ? proxy::capture_shm(s, image) : proxy::capture_dmabuf(s, image);
    }
    if (!ok) return code(errc::frame_capture_failed);
    return CapturedFrame{std::move(image)};
}
std::optional<FrameSize> ControlledApp::frame_size() const {
    if (!impl_ || !ensure_active_session()) return std::nullopt;
    auto state = impl_->session;
    std::lock_guard lock(state->data_mutex);
    const auto size_of = [&state](std::uint32_t surface_id) -> std::optional<FrameSize> {
        const auto it = state->surfaces.find(surface_id);
        if (it == state->surfaces.end()) return std::nullopt;
        if (it->second.committed_dmabuf_frame.has_value()) {
            const auto &frame = *it->second.committed_dmabuf_frame;
            if (frame.width > 0 && frame.height > 0) return FrameSize{frame.width, frame.height};
        }
        if (it->second.committed_frame.has_value()) {
            const auto &frame = *it->second.committed_frame;
            if (frame.width > 0 && frame.height > 0) return FrameSize{frame.width, frame.height};
        }
        return std::nullopt;
    };
    // Prefer the surface input is routed to, then any other committed surface.
    if (const auto size = size_of(proxy::target_surface(state))) return size;
    for (const auto &[id, surface] : state->surfaces) {
        (void)surface;
        if (const auto size = size_of(id)) return size;
    }
    return std::nullopt;
}
std::optional<CursorPosition> ControlledApp::cursor_position() const {
    if (!impl_ || !ensure_active_session()) return std::nullopt;
    auto state = impl_->session;
    if (!state->pointer_position_known.load()) return std::nullopt;
    const auto to_logical = [](std::uint32_t raw) {
        // wl_fixed_t is signed 24.8 fixed point.
        return static_cast<double>(static_cast<std::int32_t>(raw)) / 256.0;
    };
    // The compositor reports surface-local (logical) coordinates; express them
    // in frame (buffer) pixels so they match capture()/move/click coordinates.
    const std::uint32_t surface = state->focused_surface ? state->focused_surface : proxy::target_surface(state);
    const double scale = proxy::input_scale(state, surface);
    return CursorPosition{to_logical(state->pointer_sx.load()) * scale, to_logical(state->pointer_sy.load()) * scale};
}
std::vector<WindowInfo> ControlledApp::windows() const {
    std::vector<WindowInfo> out;
    if (!impl_ || impl_->child <= 0) return out;
    for (const auto &session : detail::sessions_for(impl_->child, impl_->match_regex)) {
        const bool active = session == impl_->session;
        // When no surface was explicitly selected, the active target is the
        // heuristic one, so the marker still points at the right window.
        const std::uint32_t active_surface =
            active ? (session->preferred_surface ? session->preferred_surface : proxy::target_surface(session)) : 0;
        for (const auto &window : proxy::list_windows(session)) {
            WindowInfo info;
            info.app_id = window.app_id.value_or("");
            info.title = window.title.value_or("");
            info.width = window.width;
            info.height = window.height;
            info.pid = static_cast<int>(session->owner_pid);
            info.selected = active && active_surface == window.surface_id;
            out.push_back(std::move(info));
        }
    }
    return out;
}
std::vector<SessionInfo> ControlledApp::sessions() const {
    std::vector<SessionInfo> out;
    if (!impl_) return out;
    for (const auto &status : detail::session_status(impl_->child, impl_->match_regex)) {
        out.push_back(SessionInfo{
            static_cast<int>(status.owner_pid), status.connected, status.matched, static_cast<int>(status.windows)});
    }
    return out;
}
result<void> ControlledApp::select_window(int index) {
    if (!impl_ || impl_->child <= 0) return code(errc::unspecified);
    int current = 0;
    for (const auto &session : detail::sessions_for(impl_->child, impl_->match_regex)) {
        const auto windows = proxy::list_windows(session);
        for (std::size_t i = 0; i < windows.size(); ++i) {
            if (current++ != index) continue;
            std::uint32_t surface = windows[i].surface_id;
            // Shell toplevels often carry no committed frame of their own; the
            // app renders into a child surface. Resolve to the largest frameful
            // surface in the same session so capture and input agree with the
            // selection marker.
            if (windows[i].width <= 0 || windows[i].height <= 0) {
                std::int64_t best_area = -1;
                std::uint32_t best = 0;
                for (const auto &candidate : windows) {
                    const std::int64_t area = static_cast<std::int64_t>(candidate.width) * candidate.height;
                    if (area > best_area) {
                        best_area = area;
                        best = candidate.surface_id;
                    }
                }
                if (best) surface = best;
            }
            impl_->session = session;
            {
                std::lock_guard lock(session->data_mutex);
                session->preferred_surface = surface;
            }
            // Re-apply the requested backend to the newly selected session so
            // it does not silently fall back to the default.
            const bool supported = impl_->backend == CaptureBackend::shm
                                || impl_->backend == CaptureBackend::client_buffer
                                || impl_->backend == CaptureBackend::dmabuf
                                || impl_->backend == CaptureBackend::kms;
            session->backend_unsupported = !supported;
            session->capture_backend = static_cast<int>(impl_->backend);
            session->background_capture = supported && impl_->backend == CaptureBackend::dmabuf;
            return boost::outcome_v2::success();
        }
    }
    return code(errc::unspecified);
}
result<void> ControlledApp::move_mouse(double x, double y) const {
    if (!impl_ || !ensure_active_session()) return code(errc::input_injection_failed);
    auto s = impl_->session;

    const auto target = proxy::target_surface(s);
    if (!target) return code(errc::input_injection_failed);
    const double lx = to_logical(s, target, x);
    const double ly = to_logical(s, target, y);
    if (!proxy::ensure_pointer_focus(s, target, lx, ly)) return code(errc::input_injection_failed);

    if (!proxy::inject_pointer_motion(s, lx, ly)) return code(errc::input_injection_failed);
    if (!proxy::inject_pointer_frame(s)) return code(errc::input_injection_failed);
    return boost::outcome_v2::success();
}

result<void> ControlledApp::click(double x, double y, PointerButton button, unsigned count) const {
    if (!impl_ || !ensure_active_session()) return code(errc::input_injection_failed);
    auto s = impl_->session;

    // Map PointerButton to Linux evdev BTN_ codes
    static constexpr std::uint32_t btn_map[] = {0x110, 0x111, 0x112};   // left, right, middle
    const auto btn = btn_map[static_cast<int>(button)];

    const auto target = proxy::target_surface(s);
    if (!target) return code(errc::input_injection_failed);
    const double lx = to_logical(s, target, x);
    const double ly = to_logical(s, target, y);
    if (!proxy::ensure_pointer_focus(s, target, lx, ly)) return code(errc::input_injection_failed);

    // Send motion to position cursor
    if (!proxy::inject_pointer_motion(s, lx, ly)) return code(errc::input_injection_failed);

    for (unsigned i = 0; i < count; ++i) {
        if (!proxy::inject_pointer_button(s, btn, true)) return code(errc::input_injection_failed);
        if (!proxy::inject_pointer_frame(s)) return code(errc::input_injection_failed);
        if (!proxy::inject_pointer_button(s, btn, false)) return code(errc::input_injection_failed);
        if (!proxy::inject_pointer_frame(s)) return code(errc::input_injection_failed);
    }
    return boost::outcome_v2::success();
}

result<void> ControlledApp::click_with_modifiers(
    double x, double y, PointerButton button, unsigned count, std::string modifiers) const {
    if (!impl_ || !ensure_active_session()) return code(errc::input_injection_failed);
    auto s = impl_->session;
    const auto target = input_target(s);
    if (!target) return code(errc::input_injection_failed);
    if (!proxy::ensure_pointer_focus(s, target, to_logical(s, target, x), to_logical(s, target, y)))
        return code(errc::input_injection_failed);

    proxy::Keymap keymap;
    std::vector<std::uint32_t> held;
    std::uint32_t depressed = 0;
    if (!modifiers.empty()) {
        const auto keymap_text = proxy::read_keymap_text(*s);
        if (!keymap_text) return code(errc::input_injection_failed);
        keymap = proxy::Keymap::parse(*keymap_text);
        if (!keymap.valid()) return code(errc::input_injection_failed);
        const auto parsed = proxy::parse_key_combination(modifiers + "+");
        for (const auto &name : parsed.modifiers) {
            const auto xkb_name = proxy::xkb_modifier_name(name);
            if (!xkb_name) return code(errc::input_injection_failed);
            const auto mask = keymap.modifier_mask(*xkb_name);
            const auto keysym = proxy::keysym_for_key(name);
            if (!mask || !keysym) return code(errc::input_injection_failed);
            const auto keycode = keymap.find_keysym(*keysym);
            if (!keycode) return code(errc::input_injection_failed);
            if (!proxy::inject_key_raw(s, *keycode, true)) return code(errc::input_injection_failed);
            depressed |= *mask;
            held.push_back(*keycode);
            if (!proxy::inject_modifiers(s, depressed, 0, 0, 0)) return code(errc::input_injection_failed);
        }
    }

    auto release_modifiers = [&] {
        for (auto it = held.rbegin(); it != held.rend(); ++it) {
            if (!proxy::inject_key_raw(s, *it, false)) return false;
        }
        return held.empty() || proxy::inject_modifiers(s, 0, 0, 0, 0);
    };

    const auto clicked = click(x, y, button, count);
    if (!release_modifiers() || !clicked) return code(errc::input_injection_failed);
    return boost::outcome_v2::success();
}

result<void> ControlledApp::scroll(double x, double y, int dx, int dy) const {
    if (!impl_ || !ensure_active_session()) return code(errc::input_injection_failed);
    auto s = impl_->session;

    const auto target = input_target(s);
    if (!target) return code(errc::input_injection_failed);
    if (!proxy::ensure_pointer_focus(s, target, to_logical(s, target, x), to_logical(s, target, y)))
        return code(errc::input_injection_failed);

    // Send axis events: axis 0 = vertical, axis 1 = horizontal
    if (dy != 0) {
        if (!proxy::inject_pointer_axis_source(s, 0))   // wheel
            return code(errc::input_injection_failed);
        if (!proxy::inject_pointer_axis(s, 0, static_cast<double>(dy) * 15.0))
            return code(errc::input_injection_failed);
        if (!proxy::inject_pointer_frame(s)) return code(errc::input_injection_failed);
    }
    if (dx != 0) {
        if (!proxy::inject_pointer_axis_source(s, 3))   // wheel_tilt
            return code(errc::input_injection_failed);
        if (!proxy::inject_pointer_axis(s, 1, static_cast<double>(dx) * 15.0))
            return code(errc::input_injection_failed);
        if (!proxy::inject_pointer_frame(s)) return code(errc::input_injection_failed);
    }
    return boost::outcome_v2::success();
}

result<void> ControlledApp::press_key(std::string combination) const {
    if (!impl_ || !ensure_active_session()) return code(errc::input_injection_failed);
    auto s = impl_->session;
    if (!s->keyboard_obj) return code(errc::input_injection_failed);
    const auto surface = input_target(s);
    if (!surface) return code(errc::input_injection_failed);

    const auto parsed = proxy::parse_key_combination(combination);
    if (parsed.key.empty()) return code(errc::input_injection_failed);
    const auto keymap_text = proxy::read_keymap_text(*s);
    if (!keymap_text) return code(errc::input_injection_failed);
    const auto keymap = proxy::Keymap::parse(*keymap_text);
    if (!keymap.valid()) return code(errc::input_injection_failed);

    if (!proxy::ensure_keyboard_focus(s, surface)) return code(errc::input_injection_failed);

    // Resolve every modifier and the key itself against the app's own layout;
    // there is no hard-coded keycode table to fall back to.
    std::vector<std::uint32_t> held;
    std::uint32_t depressed = 0;
    for (const auto &name : parsed.modifiers) {
        const auto xkb_name = proxy::xkb_modifier_name(name);
        if (!xkb_name) return code(errc::input_injection_failed);
        const auto mask = keymap.modifier_mask(*xkb_name);
        const auto keysym = proxy::keysym_for_key(name);
        if (!mask || !keysym) return code(errc::input_injection_failed);
        const auto keycode = keymap.find_keysym(*keysym);
        if (!keycode) return code(errc::input_injection_failed);
        if (!proxy::inject_key_raw(s, *keycode, true)) return code(errc::input_injection_failed);
        depressed |= *mask;
        held.push_back(*keycode);
        if (!proxy::inject_modifiers(s, depressed, 0, 0, 0)) return code(errc::input_injection_failed);
    }

    const auto keysym = proxy::keysym_for_key(parsed.key);
    if (!keysym) return code(errc::input_injection_failed);
    const auto keycode = keymap.find_keysym(*keysym);
    if (!keycode) return code(errc::input_injection_failed);

    bool ok = proxy::inject_key_raw(s, *keycode, true);
    if (ok) ok = proxy::inject_key_raw(s, *keycode, false);
    for (auto it = held.rbegin(); it != held.rend(); ++it) {
        if (!proxy::inject_key_raw(s, *it, false)) ok = false;
    }
    if (!held.empty() && !proxy::inject_modifiers(s, 0, 0, 0, 0)) ok = false;
    if (!ok) return code(errc::input_injection_failed);
    return boost::outcome_v2::success();
}

result<void> ControlledApp::type_text(std::string text) const {
    if (!impl_ || !ensure_active_session()) return code(errc::input_injection_failed);
    auto s = impl_->session;
    // Layout-independent when text-input-v3 is enabled, keymap-resolved
    // otherwise; both paths live in the input layer.
    if (!proxy::inject_text(s, text)) return code(errc::input_injection_failed);
    return boost::outcome_v2::success();
}


result<void> ControlledApp::drag(double from_x, double from_y, double to_x, double to_y) const {
    if (!impl_ || !ensure_active_session()) return code(errc::input_injection_failed);
    auto s = impl_->session;

    const auto target = input_target(s);
    if (!target) return code(errc::input_injection_failed);
    const double from_lx = to_logical(s, target, from_x);
    const double from_ly = to_logical(s, target, from_y);
    const double to_lx = to_logical(s, target, to_x);
    const double to_ly = to_logical(s, target, to_y);
    if (!proxy::ensure_pointer_focus(s, target, from_lx, from_ly)) return code(errc::input_injection_failed);

    // Motion to start position
    if (!proxy::inject_pointer_motion(s, from_lx, from_ly)) return code(errc::input_injection_failed);
    if (!proxy::inject_pointer_frame(s)) return code(errc::input_injection_failed);

    // Press left button
    if (!proxy::inject_pointer_button(s, 0x110, true)) return code(errc::input_injection_failed);
    if (!proxy::inject_pointer_frame(s)) return code(errc::input_injection_failed);

    // Motion to target
    if (!proxy::inject_pointer_motion(s, to_lx, to_ly)) return code(errc::input_injection_failed);
    if (!proxy::inject_pointer_frame(s)) return code(errc::input_injection_failed);

    // Release
    if (!proxy::inject_pointer_button(s, 0x110, false)) return code(errc::input_injection_failed);
    if (!proxy::inject_pointer_frame(s)) return code(errc::input_injection_failed);

    return boost::outcome_v2::success();
}
void ControlledApp::stop() noexcept {
    if (!impl_ || impl_->child <= 0) return;
    const auto child = impl_->child;
    detail::terminate_group(impl_->process_group > 0 ? impl_->process_group : child);
    detail::erase_session(child);
    if (impl_->session) impl_->session->client_fd = -1;
    impl_->child = -1;
}
bool frame_has_visible_content(const CapturedFrame &frame) noexcept {
    if (frame.image.empty() || frame.image.type() != CV_8UC3) return false;
    constexpr int step = 64;
    std::unordered_map<std::uint32_t, int> counts;
    int samples = 0;
    for (int y = 0; y < frame.image.rows; y += step) {
        for (int x = 0; x < frame.image.cols; x += step) {
            const auto pixel = frame.image.at<cv::Vec3b>(y, x);
            const auto value = static_cast<std::uint32_t>(pixel[0])
                             | (static_cast<std::uint32_t>(pixel[1]) << 8)
                             | (static_cast<std::uint32_t>(pixel[2]) << 16);
            ++counts[value];
            ++samples;
        }
    }
    int largest = 0;
    for (const auto &entry : counts)
        largest = std::max(largest, entry.second);
    return samples > 0 && 1.0 - static_cast<double>(largest) / samples > 0.02;
}
}   // namespace skl::wcas
