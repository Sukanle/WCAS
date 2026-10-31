#include <stdint.h>
#include <signal.h>
#include <stdlib.h>

#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <algorithm>
#include <unordered_map>

#include "SKL/WCAS/DRM.h"
#include "SKL/WCAS/WCAS.h"
#include "SKL/WCAS/Error.h"
#include "SKL/WCAS/Controller.h"

#include "SKL/WCAS/Private/Proxy/Conn.h"
#include "SKL/WCAS/Private/Proxy/Input.h"
#include "SKL/WCAS/Private/Harness/Session.h"
#include "SKL/WCAS/Private/Harness/ControlledAppImpl.h"

#include <opencv2/core.hpp>
#include <fmt/base.h>

namespace skl::wcas {
namespace {
skl::epcd::ErrorCode code(errc value) { return make_error_code(value); }

// Route synthetic input to the app's own surface, not whichever surface
// happened to receive a compositor enter earlier. Prefers the surface already
// focused for keyboard input, then the most recently committed one.
uint32_t input_target(const std::shared_ptr<proxy::ConnState> &session) {
    return session ? proxy::target_surface(session) : 0;
}

// Callers express pointer positions in frame (buffer) pixels, but wl_pointer
// expects surface-local (logical) pixels. Divide by the surface scale, which
// is 1.0 when the app does not use buffer scale / wp_viewport.
double to_logical(const std::shared_ptr<proxy::ConnState> &session, uint32_t surface, double value) {
    const double scale = proxy::input_scale(session, surface);
    return scale > 0.0 ? value / scale : value;
}

bool ensure_active_session_impl(ControlledApp::Impl *impl) {
    if (!impl) return false;
    // An explicitly selected session wins as long as it is still connected:
    // switching to another session of the same launch tree would silently send
    // input/capture to a different window than the caller chose.
    if (impl->session && impl->session->connected.load()) return true;
    if (impl->child > 0) {
        for (const auto &session : detail::sessions_for(impl->child, impl->match_regex)) {
            if (session->capturable_surface.load()) {
                impl->session = session;
                if (impl->focus) (void)impl->focus->apply();
                return true;
            }
        }
        if (impl->session && impl->session->connected.load()) return true;
        if (auto session = detail::session_for(impl->child)) {
            impl->session = std::move(session);
            if (impl->focus) (void)impl->focus->apply();
            return true;
        }
    }
    return impl->session && impl->session->connected.load();
}

result<cv::Mat> capture_once(ControlledApp::Impl *impl) {
    if (!impl || !impl->capture) return code(errc::frame_capture_failed);
    // XWayland has no proxy session; the capture source talks to the bridge.
    if (impl->transport == Transport::native_wayland && !ensure_active_session_impl(impl))
        return code(errc::frame_capture_failed);
    auto frame = impl->capture->capture();
    if (frame && impl->frames) impl->frames->publish(frame.value());
    return frame;
}

void pump_loop(ControlledApp::Impl *impl) {
    bool first = true;
    int consecutive_errors = 0;
    int last_width = -1;
    int last_height = -1;
    while (true) {
        {
            std::unique_lock lock(impl->pump_mutex);
            if (impl->pump_stop) break;
            if (!first && impl->pump_interval.count() > 0) {
                if (impl->pump_cv.wait_for(lock, impl->pump_interval, [impl] { return impl->pump_stop; })) break;
            }
        }
        first = false;

        {
            std::lock_guard capture_lock(impl->capture_mutex);
            auto frame = capture_once(impl);
            if (!frame) {
                if (consecutive_errors++ == 0 && impl->frames)
                    impl->frames->publish_error(
                        static_cast<int32_t>(frame.error().value), skl::wcas::error_message(frame.error()));
            } else {
                consecutive_errors = 0;
                const auto &image = frame.value();
                if (image.cols > 0 && image.rows > 0 && (image.cols != last_width || image.rows != last_height)) {
                    last_width = image.cols;
                    last_height = image.rows;
                    if (impl->frames) impl->frames->publish_state(last_width, last_height);
                }
            }
        }
    }
}

}   // namespace

ControlledApp::Impl::~Impl() {
    std::thread thread;
    {
        std::lock_guard lock(pump_mutex);
        pump_stop = true;
        pump_cv.notify_all();
        thread = std::move(pump_thread);
        pump_running = false;
    }
    if (thread.joinable()) thread.join();
}

void ControlledApp::apply_activation_selection() const {
    if (!_impl) return;
    std::lock_guard lock(_impl->capture_mutex);
    if (_impl->focus) (void)_impl->focus->apply();
}

void ControlledApp::set_allow_activation(bool value) {
    if (!_impl) return;
    std::lock_guard lock(_impl->capture_mutex);
    _impl->allow_activation = value;
    if (_impl->focus) (void)_impl->focus->set_allowed(value);
}

bool ControlledApp::ensure_active_session() const {
    if (!_impl) return false;
    std::lock_guard lock(_impl->capture_mutex);
    return ensure_active_session_impl(_impl.get());
}

ControlledApp::ControlledApp(std::unique_ptr<Impl> impl)
    : _impl(std::move(impl)) {
    if (_impl) {
        _impl->process_handle = _impl->process.get();
        _impl->capture = std::make_unique<CaptureRuntime>(_impl.get());
        _impl->windows = std::make_unique<WindowRuntime>(_impl.get());
        _impl->window_runtime = _impl->windows.get();
        _impl->focus = std::make_unique<FocusRuntime>(_impl.get());
        _impl->focus_runtime = _impl->focus.get();
        _impl->health = std::make_unique<HealthRuntime>(_impl.get());
        _impl->target = std::make_unique<TargetRuntime>(_impl.get());
        _impl->frames = std::make_unique<FrameBus>();
    }
}
result<Controller> ControlledApp::controller() {
    if (!_impl) return code(errc::unspecified);
    auto controller = Controller::create(*this);
    if (!controller) return code(errc::unspecified);
    return std::move(*controller);
}
ControlledApp::ControlledApp(ControlledApp &&) noexcept = default;
ControlledApp &ControlledApp::operator=(ControlledApp &&other) noexcept {
    if (this != &other) {
        stop();
        _impl = std::move(other._impl);
    }
    return *this;
}
ControlledApp::~ControlledApp() { stop(); }
int32_t ControlledApp::pid() const noexcept {
    if (!_impl) return -1;
    if (_impl->process) return static_cast<int32_t>(_impl->process->pid());
    return _impl->child;
}
bool ControlledApp::is_alive() const noexcept {
    if (!_impl) return false;
    if (_impl->health) return _impl->health->alive();
    if (_impl->process) return _impl->process->alive();
    return _impl->child > 0 && ::kill(_impl->child, 0) == 0;
}
Transport ControlledApp::transport() const noexcept { return _impl ? _impl->transport : Transport::native_wayland; }
uint64_t ControlledApp::total_commit_count() const {
    if (!_impl) return 0;
    std::lock_guard lock(_impl->capture_mutex);
    if (_impl->health) return _impl->health->commit_count();
    return _impl->session ? _impl->session->commit_count.load() : 0;
}
uint64_t ControlledApp::presented_frames() const {
    if (!_impl) return 0;
    std::lock_guard lock(_impl->capture_mutex);
    if (_impl->health) return _impl->health->presented_frames();
    return _impl->session ? _impl->session->presented_frames.load() : 0;
}
void ControlledApp::set_capture_backend(CaptureKind backend) {
    if (!_impl) return;
    std::lock_guard lock(_impl->capture_mutex);
    _impl->backend = backend;
    if (!_impl->session) return;
    // `shm`/`client_buffer` read wl_shm buffers; `dmabuf` reads linux-dmabuf
    // buffers; `kms` reads the DRM framebuffer synchronously. Anything else
    // must fail loudly at capture() rather than silently use a different path.
    const bool supported = backend == CaptureKind::SHM
                        || backend == CaptureKind::CLIENTBUF
                        || backend == CaptureKind::DMABUF
                        || backend == CaptureKind::KMS;
    _impl->session->backend_unsupported = !supported;
    _impl->session->capture_backend = static_cast<int32_t>(backend);
    _impl->session->background_capture = supported && backend == CaptureKind::DMABUF;
}
void ControlledApp::set_capture_target(CaptureTarget target) {
    if (!_impl) return;
    std::lock_guard lock(_impl->capture_mutex);
    _impl->capture_target = target;
    if (_impl->capture) {
        _impl->capture->set_target(target);
        return;
    }
    if (_impl->session) proxy::set_capture_target(_impl->session, std::move(target.app_id), std::move(target.title));
}
result<cv::Mat> ControlledApp::capture() const {
    if (!_impl) return code(errc::frame_capture_failed);
    std::lock_guard lock(_impl->capture_mutex);
    return capture_once(_impl.get());
}

result<uint64_t> ControlledApp::attach_frame_sink(FrameSink sink, SinkPolicy policy) {
    if (!_impl || !_impl->frames) return code(errc::unspecified);
    return _impl->frames->attach(std::move(sink), policy);
}

void ControlledApp::detach_frame_sink(uint64_t id) {
    if (_impl && _impl->frames) _impl->frames->detach(id);
}

optional<SinkStats> ControlledApp::frame_sink_stats(uint64_t id) const {
    if (!_impl || !_impl->frames) return std::nullopt;
    return _impl->frames->stats(id);
}

result<void> ControlledApp::start_frame_pump(std::chrono::milliseconds interval) {
    if (!_impl || _impl->child <= 0) return code(errc::unspecified);
    std::lock_guard lock(_impl->pump_mutex);
    if (_impl->pump_running) return code(errc::unspecified);
    _impl->pump_stop = false;
    _impl->pump_interval = interval;
    _impl->pump_running = true;
    _impl->pump_thread = std::thread([impl = _impl.get()] { pump_loop(impl); });
    return boost::outcome_v2::success();
}

void ControlledApp::stop_frame_pump() noexcept {
    if (!_impl) return;
    std::thread thread;
    {
        std::lock_guard lock(_impl->pump_mutex);
        if (!_impl->pump_running) return;
        _impl->pump_stop = true;
        _impl->pump_cv.notify_all();
        thread = std::move(_impl->pump_thread);
        _impl->pump_running = false;
    }
    if (thread.joinable()) thread.join();
}

bool ControlledApp::frame_pump_running() const noexcept {
    if (!_impl) return false;
    std::lock_guard lock(_impl->pump_mutex);
    return _impl->pump_running;
}
optional<cv::Size> ControlledApp::frame_size() const {
    if (!_impl) return std::nullopt;
    std::lock_guard capture_lock(_impl->capture_mutex);
    if (!ensure_active_session_impl(_impl.get())) return std::nullopt;
    auto state = _impl->session;
    std::scoped_lock lock(state->data_mutex);
    const auto size_of = [&state](uint32_t surface_id) -> optional<cv::Size> {
        const auto it = state->surfaces.find(surface_id);
        if (it == state->surfaces.end()) return std::nullopt;
        if (it->second.committed_dmabuf_frame.has_value())
            if (const auto &frame = *it->second.committed_dmabuf_frame; frame.width > 0 && frame.height > 0)
                return cv::Size{frame.width, frame.height};
        if (it->second.committed_frame.has_value())
            if (const auto &frame = *it->second.committed_frame; frame.width > 0 && frame.height > 0)
                return cv::Size{frame.width, frame.height};

        return std::nullopt;
    };
    // Prefer the surface input is routed to, then any other committed surface.
    if (const auto size = size_of(proxy::target_surface(state))) return size;
    for (const auto &[id, _] : state->surfaces)
        if (const auto size = size_of(id)) return size;

    return std::nullopt;
}
optional<CursorPosition> ControlledApp::cursor_position() const {
    if (!_impl) return std::nullopt;
    std::lock_guard capture_lock(_impl->capture_mutex);
    if (!ensure_active_session_impl(_impl.get())) return std::nullopt;
    auto state = _impl->session;
    if (!state->pointer_position_known.load()) return std::nullopt;
    const auto to_logical = [](uint32_t raw) {
        // wl_fixed_t is signed 24.8 fixed point.
        return static_cast<double>(static_cast<int32_t>(raw)) / 256.0;
    };
    // The compositor reports surface-local (logical) coordinates; express them
    // in frame (buffer) pixels so they match capture()/move/click coordinates.
    const uint32_t surface = state->focused_surface ? state->focused_surface : proxy::target_surface(state);
    const double scale = proxy::input_scale(state, surface);
    return CursorPosition{to_logical(state->pointer_sx.load()) * scale, to_logical(state->pointer_sy.load()) * scale};
}
std::vector<WindowInfo> ControlledApp::windows() const {
    if (!_impl || !_impl->windows) return {};
    std::lock_guard lock(_impl->capture_mutex);
    auto result = _impl->windows->list();
    if (!result) return {};
    return std::move(result.value());
}
std::vector<SessionInfo> ControlledApp::sessions() const {
    std::vector<SessionInfo> out;
    if (!_impl) return out;
    for (const auto &status : detail::session_status(_impl->child, _impl->match_regex)) {
        out.push_back(SessionInfo{static_cast<int32_t>(status.owner_pid), status.connected, status.matched,
            static_cast<int32_t>(status.windows), static_cast<int32_t>(status.surfaces),
            static_cast<int32_t>(status.toplevels), status.has_keyboard, status.has_pointer, status.has_keymap,
            status.command});
    }
    return out;
}
std::vector<SurfaceInfo> ControlledApp::surfaces() const {
    std::vector<SurfaceInfo> out;
    if (!_impl || _impl->child <= 0) return out;
    for (const auto &session : detail::sessions_for(_impl->child, _impl->match_regex)) {
        for (const auto &surface : proxy::dump_surfaces(session)) {
            SurfaceInfo info;
            info.pid = static_cast<int32_t>(session->owner_pid);
            info.surface_id = surface.surface_id;
            info.parent_surface_id = surface.parent_surface_id;
            info.toplevel_id = surface.toplevel_id;
            info.role = surface.role;
            info.app_id = surface.app_id.value_or("");
            info.title = surface.title.value_or("");
            info.width = surface.width;
            info.height = surface.height;
            info.has_frame = surface.has_frame;
            info.last_commit_seq = surface.last_commit_seq;
            out.push_back(std::move(info));
        }
    }
    return out;
}
result<void> ControlledApp::select_window(int32_t index) {
    if (!_impl || !_impl->target) return code(errc::unspecified);
    std::lock_guard capture_lock(_impl->capture_mutex);
    return _impl->target->select(index);
}

result<void> ControlledApp::select_target(TargetId target) {
    if (!_impl || !_impl->target) return code(errc::unspecified);
    std::lock_guard capture_lock(_impl->capture_mutex);
    return _impl->target->select_target(target);
}

TargetId ControlledApp::selected_target() const {
    if (!_impl || !_impl->target) return {};
    std::lock_guard capture_lock(_impl->capture_mutex);
    return _impl->target->selected_target();
}
void ControlledApp::stop() noexcept {
    if (!_impl) return;
    stop_frame_pump();
    if (_impl->frames) _impl->frames->close();
    const auto child = _impl->child;
    if (_impl->process) {
        _impl->process->terminate();
    } else if (child > 0) {
        detail::terminate_group(_impl->process_group > 0 ? _impl->process_group : child);
    }
    if (child > 0) detail::erase_sessions_for(child);
    if (_impl->session) _impl->session->client_fd = -1;
    _impl->child = -1;
}
bool frame_has_visible_content(const cv::Mat &frame) noexcept {
    if (frame.empty() || frame.type() != CV_8UC3) return false;
    constexpr int32_t step = 64;
    std::unordered_map<uint32_t, int32_t> counts;
    int32_t samples = 0;
    for (int32_t y = 0; y < frame.rows; y += step) {
        for (int32_t x = 0; x < frame.cols; x += step) {
            const auto &pixel = frame.at<cv::Vec3b>(y, x);
            const auto value = static_cast<uint32_t>(pixel[0])
                             | (static_cast<uint32_t>(pixel[1]) << 8)
                             | (static_cast<uint32_t>(pixel[2]) << 16);
            ++counts[value];
            ++samples;
        }
    }
    int32_t largest = 0;
    for (const auto &entry : counts)
        largest = std::max(largest, entry.second);
    return samples > 0 && 1.0 - static_cast<double>(largest) / samples > 0.02;
}
}   // namespace skl::wcas
