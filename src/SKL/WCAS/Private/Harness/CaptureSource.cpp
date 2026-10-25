#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include "SKL/WCAS/Error.h"
#include "SKL/WCAS/DRM.h"

#include "SKL/WCAS/Private/X11Capture.h"
#include "SKL/WCAS/Private/Proxy/Conn.h"
#include "SKL/WCAS/Private/Proxy/Input.h"
#include "SKL/WCAS/Private/Harness/CaptureSource.h"
#include "SKL/WCAS/Private/Harness/ControlledAppImpl.h"

#include <fmt/base.h>

namespace skl::wcas {
namespace {

using skl::abix::poly::InterfaceMethod;
using skl::abix::poly::InterfaceRegistry;
using skl::abix::poly::InterfaceStatus;

std::error_code code(errc value) { return make_error_code(value); }

optional<cv::Size> session_frame_size(const std::shared_ptr<proxy::ConnState> &state) {
    if (!state) return std::nullopt;
    std::scoped_lock lock(state->data_mutex);
    const auto size_of = [&state](uint32_t surface_id) -> optional<cv::Size> {
        const auto it = state->surfaces.find(surface_id);
        if (it == state->surfaces.end()) return std::nullopt;
        if (it->second.committed_dmabuf_frame.has_value()) {
            const auto &frame = *it->second.committed_dmabuf_frame;
            if (frame.width > 0 && frame.height > 0) return cv::Size{frame.width, frame.height};
        }
        if (it->second.committed_frame.has_value()) {
            const auto &frame = *it->second.committed_frame;
            if (frame.width > 0 && frame.height > 0) return cv::Size{frame.width, frame.height};
        }
        return std::nullopt;
    };
    // Prefer the surface input is routed to, then any other committed surface.
    if (const auto size = size_of(proxy::target_surface(state))) return size;
    for (const auto &[id, _] : state->surfaces)
        if (const auto size = size_of(id)) return size;
    return std::nullopt;
}

// The only provider registered for CaptureSource in this phase. The individual
// strategies remain small, testable helpers inside it; the interface boundary
// lets a future provider replace the whole selection policy.
class AutoCaptureSource {
public:
    explicit AutoCaptureSource(SessionContext *state) noexcept
        : state_(state) {}

    // CaptureInfo
    // Keep these non-noexcept: Clang sees the const and const-noexcept
    // member_adapter partial specializations as ambiguous (the same limitation
    // documented in docs/CONTROLLER.md).
    [[nodiscard]] static uint64_t id() { return 0; }
    [[nodiscard]] static std::string name() { return "auto-capture"; }
    [[nodiscard]] bool connected() const;
    [[nodiscard]] CaptureKind kind() const;
    [[nodiscard]] static CaptureCapability capabilities() {
        return CaptureCapability::CLIENTBUF
             | CaptureCapability::SHM
             | CaptureCapability::DMABUF
             | CaptureCapability::KMS
             | CaptureCapability::X11
             | CaptureCapability::BACKGROUND
             | CaptureCapability::WINDOWGEOMETRY;
    }

    // CaptureSource
    result<void> set_target(std::string app_id, std::string title);
    result<cv::Mat> capture() const;
    [[nodiscard]] optional<cv::Size> frame_size() const;

private:
    SessionContext *state_ = nullptr;

    [[nodiscard]] result<cv::Mat> capture_native() const;
    [[nodiscard]] result<cv::Mat> capture_x11() const;
};

bool AutoCaptureSource::connected() const {
    if (!state_) return false;
    if (state_->transport == Transport::xwayland) return state_->bridge && state_->bridge->running();
    return state_->session && state_->session->connected.load();
}

CaptureKind AutoCaptureSource::kind() const {
    if (!state_) return CaptureKind::UNKNOWN;
    if (state_->transport == Transport::xwayland) return CaptureKind::X11;
    return state_->backend;
}

result<void> AutoCaptureSource::set_target(std::string app_id, std::string title) {
    if (!state_) return code(errc::feature_not_implemented);
    state_->capture_target.app_id = app_id.empty() ? std::nullopt : optional<std::string>(std::move(app_id));
    state_->capture_target.title = title.empty() ? std::nullopt : optional<std::string>(std::move(title));
    if (state_->session)
        proxy::set_capture_target(state_->session, state_->capture_target.app_id, state_->capture_target.title);
    return boost::outcome_v2::success();
}

result<cv::Mat> AutoCaptureSource::capture_x11() const {
    if (!state_ || !state_->bridge || !state_->bridge->running()) return code(errc::frame_capture_failed);
    auto frame = capture_x11_window(state_->bridge->display(), state_->bridge->xauthority());
    if (!frame.has_value()) return code(errc::frame_capture_failed);
    return cv::Mat{std::move(frame.value())};
}

result<cv::Mat> AutoCaptureSource::capture_native() const {
    if (!state_ || !state_->session) return code(errc::frame_capture_failed);
    const auto session = state_->session;
    if (session->backend_unsupported.load()) return code(errc::feature_not_implemented);

    cv::Mat image;
    {
        // The background scheduler keeps the latest completed frame ready, so
        // prefer it over a synchronous capture.
        std::lock_guard lock(session->data_mutex);
        if (session->pre_captured_frame.has_value()) image = session->pre_captured_frame->clone();
    }
    if (!image.empty()) return cv::Mat{std::move(image)};

    const auto backend = state_->backend;
    if (backend == CaptureKind::KMS) {
        auto frame = capture_kms_screen(std::nullopt);
        if (!frame.has_value()) return code(errc::frame_capture_failed);
        return cv::Mat{std::move(frame.value())};
    }

    // Prefer the requested backend, but a selected window may use the other
    // buffer type (the game renders through DMA-BUF while a child overlay uses
    // wl_shm). Try both so switching windows keeps working.
    bool ok =
        backend == CaptureKind::DMABUF ? proxy::capture_dmabuf(session, image) : proxy::capture_shm(session, image);
    if (!ok)
        ok =
            backend == CaptureKind::DMABUF ? proxy::capture_shm(session, image) : proxy::capture_dmabuf(session, image);
    if (ok) return cv::Mat{std::move(image)};

    // Last resort: the window's own buffer could not be decoded (e.g. a
    // framebuffer-compressed modifier that no import path accepts here).
    // Fall back to the composited output, scoped to the window so a
    // multi-monitor scanout is never returned whole.
    fmt::print(stderr, "[wcas] capture: window buffer undecodable, falling back to compositor capture\n");
    optional<cv::Rect> geometry;
    if (const auto size = frame_size()) {
        optional<std::string> title = state_->capture_target.title;
        if (!title.has_value()) {
            std::scoped_lock lock(session->data_mutex);
            title = session->capture_title;
        }
        if (state_->window_runtime) {
            if (auto rect =
                    state_->window_runtime->find_by_size_and_title(size->width, size->height, title.value_or(""));
                rect)
                geometry = rect.value();
        } else {
            const optional<std::string_view> title_view =
                title.has_value() ? optional<std::string_view>(*title) : std::nullopt;
            geometry = find_window_position_by_size_and_title(cv::Size(size->width, size->height), title_view);
        }
    }
    // grim takes compositor layout coordinates, so it stays aligned under
    // output scaling; KMS is the in-process fallback (its crop is in raw
    // framebuffer pixels). Both are window-scoped when `geometry` is known.
    if (auto frame = capture_via_grim(geometry); frame.has_value()) return cv::Mat{std::move(frame.value())};
    if (auto frame = capture_kms_screen(geometry); frame.has_value()) return cv::Mat{std::move(frame.value())};
    return code(errc::frame_capture_failed);
}

result<cv::Mat> AutoCaptureSource::capture() const {
    if (!state_) return code(errc::frame_capture_failed);
    if (state_->transport == Transport::xwayland) return capture_x11();
    return capture_native();
}

optional<cv::Size> AutoCaptureSource::frame_size() const {
    if (!state_ || state_->transport == Transport::xwayland) return std::nullopt;
    return session_frame_size(state_->session);
}

#define SKL_WCAS_ACS_BIND_CAPTURE_INFO_LIST(X)                     \
    X(CaptureInfoTraits, id, &AutoCaptureSource::id)               \
    X(CaptureInfoTraits, name, &AutoCaptureSource::name)           \
    X(CaptureInfoTraits, kind, &AutoCaptureSource::kind)           \
    X(CaptureInfoTraits, connected, &AutoCaptureSource::connected) \
    X(CaptureInfoTraits, capabilities, &AutoCaptureSource::capabilities)

SKL_ABIX_INTERFACE_MEMBER_TABLE(CaptureInfo, SKL_WCAS_ACS_BIND_CAPTURE_INFO_LIST)

#define SKL_WCAS_ACS_BIND_CAPTURE_SOURCE_LIST(X)                       \
    X(CaptureSourceTraits, capture, &AutoCaptureSource::capture)       \
    X(CaptureSourceTraits, set_target, &AutoCaptureSource::set_target) \
    X(CaptureSourceTraits, frame_size, &AutoCaptureSource::frame_size)

SKL_ABIX_INTERFACE_MEMBER_TABLE(CaptureSource, SKL_WCAS_ACS_BIND_CAPTURE_SOURCE_LIST)

static_assert(std::size(CaptureInfoMethods) == CaptureInfoTraits::signatures.size());
static_assert(std::size(CaptureSourceMethods) == CaptureSourceTraits::signatures.size());

constexpr auto kAutoCaptureType = skl::abix::model::canonical_type_id("skl::wcas::AutoCaptureSource");

InterfaceStatus register_capture_source(InterfaceRegistry<4> &registry) {
    InterfaceStatus status = registry.add(SKL_ABIX_INTERFACE_IMPLEMENTATION(CaptureInfo, kAutoCaptureType, 0));
    if (status != InterfaceStatus::ok) return status;
    return registry.add(SKL_ABIX_INTERFACE_IMPLEMENTATION(CaptureSource, kAutoCaptureType, 0));
}

struct CaptureRegistry {
    CaptureRegistry() {
        if (register_capture_source(registry) != InterfaceStatus::ok) return;
    }
    InterfaceRegistry<4> registry;
};

const InterfaceRegistry<4> &capture_registry() {
    static const CaptureRegistry holder;
    return holder.registry;
}

}   // namespace

struct CaptureRuntime::Impl {
    explicit Impl(SessionContext *state)
        : device(state) {
        const auto &registry = capture_registry();
        info.bind(registry, &device, kAutoCaptureType);
        source.bind(registry, &device, kAutoCaptureType);
    }

    AutoCaptureSource device;
    CaptureInfo info;
    CaptureSource source;
};

CaptureRuntime::CaptureRuntime(SessionContext *state)
    : _impl(std::make_unique<Impl>(state)) {}
CaptureRuntime::~CaptureRuntime() = default;

result<cv::Mat> CaptureRuntime::capture() const {
    if (!_impl || !_impl->source.valid()) return code(errc::feature_not_implemented);
    return _impl->source.capture();
}

optional<cv::Size> CaptureRuntime::frame_size() const {
    if (!_impl || !_impl->source.valid()) return std::nullopt;
    return _impl->source.frame_size();
}

void CaptureRuntime::set_target(const CaptureTarget &target) {
    if (!_impl || !_impl->source.valid()) return;
    (void)_impl->source.set_target(target.app_id.value_or(""), target.title.value_or(""));
}

}   // namespace skl::wcas
