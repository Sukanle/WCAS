#include <memory>
#include <string>
#include <vector>
#include <utility>

#include "SKL/WCAS/DRM.h"
#include "SKL/WCAS/Error.h"

#include "SKL/WCAS/Private/Proxy/Conn.h"
#include "SKL/WCAS/Private/Proxy/Input.h"
#include "SKL/WCAS/Private/X11Capture.h"
#include "SKL/WCAS/Private/Harness/Session.h"
#include "SKL/WCAS/Private/Harness/WindowSource.h"

namespace skl::wcas {
namespace {

using skl::abix::poly::InterfaceMethod;
using skl::abix::poly::InterfaceRegistry;
using skl::abix::poly::InterfaceStatus;

std::error_code code(errc value) { return make_error_code(value); }

result<std::vector<WindowInfo>> native_windows(SessionContext *state) {
    std::vector<WindowInfo> out;
    if (!state || state->child <= 0) return boost::outcome_v2::success(std::move(out));
    for (const auto &session : detail::sessions_for(state->child, state->match_regex)) {
        const bool active = session == state->session;
        const uint32_t active_surface =
            active ? (session->preferred_surface ? session->preferred_surface : proxy::target_surface(session)) : 0;
        for (const auto &window : proxy::list_windows(session)) {
            WindowInfo info;
            info.app_id = window.app_id.value_or("");
            info.title = window.title.value_or("");
            info.width = window.width;
            info.height = window.height;
            info.pid = static_cast<int32_t>(session->owner_pid);
            info.origin = static_cast<WindowOrigin>(window.origin);
            info.surface_id = window.surface_id;
            info.target = TargetId{window.surface_id, TargetKind::WaylandSurface};
            info.selected = active && active_surface == window.surface_id;
            out.push_back(std::move(info));
        }
    }
    return boost::outcome_v2::success(std::move(out));
}

// The only provider registered for the window contracts in this phase. It
// selects the native proxy or X11 enumeration from the app's transport; future
// providers can replace either capability independently.
class AutoWindowProvider {
public:
    explicit AutoWindowProvider(SessionContext *state) noexcept
        : state_(state) {}

    // WindowEnumerator
    [[nodiscard]] std::string name() const { return "auto-window"; }
    [[nodiscard]] bool connected() const;
    [[nodiscard]] result<std::vector<WindowInfo>> list() const;

    // WindowResolver
    [[nodiscard]] result<cv::Rect> find_by_pid(int32_t pid) const;
    [[nodiscard]] result<cv::Rect> find_by_size_and_title(int32_t width, int32_t height, std::string title) const;

private:
    SessionContext *state_ = nullptr;
};

bool AutoWindowProvider::connected() const {
    if (!state_) return false;
    if (state_->transport == Transport::xwayland) return state_->bridge && state_->bridge->running();
    return state_->child > 0;
}

result<std::vector<WindowInfo>> AutoWindowProvider::list() const {
    if (!state_) return code(errc::unspecified);
    if (state_->transport != Transport::xwayland) return native_windows(state_);
    if (!state_->bridge || !state_->bridge->running()) return code(errc::xwayland_bridge_failed);
    return list_x11_windows(state_->bridge->display(), state_->bridge->xauthority());
}

result<cv::Rect> AutoWindowProvider::find_by_pid(int32_t pid) const {
    if (pid <= 0) return code(errc::unspecified);
    const auto rect = find_window(pid);
    if (!rect.has_value()) return code(errc::unspecified);
    return *rect;
}

result<cv::Rect> AutoWindowProvider::find_by_size_and_title(int32_t width, int32_t height, std::string title) const {
    if (width <= 0 || height <= 0) return code(errc::unspecified);
    const optional<std::string_view> title_view = title.empty() ? std::nullopt : optional<std::string_view>(title);
    const auto rect = find_window_position_by_size_and_title(cv::Size{width, height}, title_view);
    if (!rect.has_value()) return code(errc::unspecified);
    return *rect;
}

#define SKL_WCAS_AWP_BIND_WINDOW_ENUMERATOR_LIST(X)            \
    X(WindowEnumeratorTraits, name, &AutoWindowProvider::name) \
    X(WindowEnumeratorTraits, list, &AutoWindowProvider::list) \
    X(WindowEnumeratorTraits, connected, &AutoWindowProvider::connected)

SKL_ABIX_INTERFACE_MEMBER_TABLE(WindowEnumerator, SKL_WCAS_AWP_BIND_WINDOW_ENUMERATOR_LIST)

#define SKL_WCAS_AWP_BIND_WINDOW_RESOLVER(X)                               \
    X(WindowResolverTraits, name, &AutoWindowProvider::name)               \
    X(WindowResolverTraits, connected, &AutoWindowProvider::connected)     \
    X(WindowResolverTraits, find_by_pid, &AutoWindowProvider::find_by_pid) \
    X(WindowResolverTraits, find_by_size_and_title, &AutoWindowProvider::find_by_size_and_title)

SKL_ABIX_INTERFACE_MEMBER_TABLE(WindowResolver, SKL_WCAS_AWP_BIND_WINDOW_RESOLVER)

static_assert(std::size(WindowEnumeratorMethods) == WindowEnumeratorTraits::signatures.size());
static_assert(std::size(WindowResolverMethods) == WindowResolverTraits::signatures.size());

constexpr auto kAutoWindowType = skl::abix::model::canonical_type_id("skl::wcas::AutoWindowProvider");

InterfaceStatus register_window_provider(InterfaceRegistry<4> &registry) {
    InterfaceStatus status = registry.add(SKL_ABIX_INTERFACE_IMPLEMENTATION(WindowEnumerator, kAutoWindowType, 0));
    if (status != InterfaceStatus::ok) return status;
    return registry.add(SKL_ABIX_INTERFACE_IMPLEMENTATION(WindowResolver, kAutoWindowType, 0));
}

struct WindowRegistry {
    WindowRegistry() {
        if (register_window_provider(registry) != InterfaceStatus::ok) return;
    }
    InterfaceRegistry<4> registry;
};

const InterfaceRegistry<4> &window_registry() {
    static const WindowRegistry holder;
    return holder.registry;
}

}   // namespace

struct WindowRuntime::Impl {
    explicit Impl(SessionContext *state)
        : device(state) {
        const auto &registry = window_registry();
        (void)enumerator.bind(registry, &device, kAutoWindowType);
        (void)resolver.bind(registry, &device, kAutoWindowType);
    }

    AutoWindowProvider device;
    WindowEnumerator enumerator;
    WindowResolver resolver;
};

WindowRuntime::WindowRuntime(SessionContext *state)
    : _impl(std::make_unique<Impl>(state)) {}
WindowRuntime::~WindowRuntime() = default;

result<std::vector<WindowInfo>> WindowRuntime::list() const {
    if (!_impl || !_impl->enumerator.valid()) return code(errc::feature_not_implemented);
    return _impl->enumerator.list();
}

result<cv::Rect> WindowRuntime::find_by_pid(int32_t pid) const {
    if (!_impl || !_impl->resolver.valid()) return code(errc::feature_not_implemented);
    return _impl->resolver.find_by_pid(pid);
}

result<cv::Rect> WindowRuntime::find_by_size_and_title(int32_t width, int32_t height, std::string title) const {
    if (!_impl || !_impl->resolver.valid()) return code(errc::feature_not_implemented);
    return _impl->resolver.find_by_size_and_title(width, height, std::move(title));
}

}   // namespace skl::wcas
