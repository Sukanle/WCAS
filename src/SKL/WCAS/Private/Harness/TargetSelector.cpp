#include <stdint.h>

#include <iterator>
#include <memory>
#include <string>
#include <utility>

#include "SKL/WCAS/Error.h"

#include "SKL/WCAS/Private/Proxy/Conn.h"
#include "SKL/WCAS/Private/Harness/Session.h"
#include "SKL/WCAS/Private/Harness/FocusBackend.h"
#include "SKL/WCAS/Private/Harness/ControlledAppImpl.h"
#include "SKL/WCAS/Private/Harness/TargetSelector.h"


namespace skl::wcas {
namespace {

using skl::abix::poly::InterfaceMethod;
using skl::abix::poly::InterfaceRegistry;
using skl::abix::poly::InterfaceStatus;

skl::epcd::ErrorCode code(errc value) { return make_error_code(value); }

class TargetProvider {
public:
    explicit TargetProvider(SessionContext *state) noexcept
        : state_(state) {}

    [[nodiscard]] std::string name() const { return "auto-target"; }
    [[nodiscard]] bool available() const { return state_ && state_->child > 0; }

    result<void> select(int32_t index) {
        if (!state_ || state_->child <= 0) return code(errc::unspecified);
        int32_t current = 0;
        for (const auto &session : detail::sessions_for(state_->child, state_->match_regex)) {
            const auto windows = proxy::list_windows(session);
            for (size_t i = 0; i < windows.size(); ++i) {
                if (current++ != index) continue;
                uint32_t surface = windows[i].surface_id;
                // Shell toplevels often carry no committed frame of their own;
                // resolve to the largest frameful surface in the same session.
                if (windows[i].width <= 0 || windows[i].height <= 0) {
                    const std::string want_app_id = windows[i].app_id.value_or("");
                    const std::string want_title = windows[i].title.value_or("");
                    int64_t best_area = -1;
                    int64_t fallback_area = -1;
                    uint32_t best = 0;
                    uint32_t fallback = 0;
                    for (const auto &candidate : windows) {
                        const int64_t area = static_cast<int64_t>(candidate.width) * candidate.height;
                        if (area <= 0) continue;
                        if (area > fallback_area) {
                            fallback_area = area;
                            fallback = candidate.surface_id;
                        }
                        const bool same = (want_app_id.empty() || candidate.app_id.value_or("") == want_app_id)
                                       && (want_title.empty() || candidate.title.value_or("") == want_title);
                        if (same && area > best_area) {
                            best_area = area;
                            best = candidate.surface_id;
                        }
                    }
                    if (best)
                        surface = best;
                    else if (fallback)
                        surface = fallback;
                }
                state_->session = session;
                state_->selected_target = TargetId{surface, TargetKind::WaylandSurface};
                {
                    std::lock_guard lock(session->data_mutex);
                    session->preferred_surface = surface;
                }
                if (state_->focus_runtime) (void)state_->focus_runtime->apply();
                // Re-apply the requested backend to the newly selected session.
                const bool supported = state_->backend == CaptureKind::SHM
                                    || state_->backend == CaptureKind::CLIENTBUF
                                    || state_->backend == CaptureKind::DMABUF
                                    || state_->backend == CaptureKind::KMS;
                session->backend_unsupported = !supported;
                session->capture_backend = static_cast<int32_t>(state_->backend);
                session->background_capture = supported && state_->backend == CaptureKind::DMABUF;
                return boost::outcome_v2::success();
            }
        }
        return code(errc::unspecified);
    }

    [[nodiscard]] uint32_t selected() const {
        if (!state_ || !state_->session) return 0;
        return state_->session->preferred_surface;
    }

    result<void> clear() {
        if (!state_) return code(errc::unspecified);
        state_->selected_target = TargetId{};
        if (!state_->session) return boost::outcome_v2::success();
        std::lock_guard lock(state_->session->data_mutex);
        state_->session->preferred_surface = 0;
        return boost::outcome_v2::success();
    }

    result<void> select_target(uint64_t value, uint32_t kind) {
        if (!state_ || value == 0) return code(errc::unspecified);
        if (kind != static_cast<uint32_t>(TargetKind::WaylandSurface)) return code(errc::feature_not_implemented);

        for (const auto &session : detail::sessions_for(state_->child, state_->match_regex)) {
            for (const auto &window : proxy::list_windows(session)) {
                if (window.surface_id != value) continue;
                state_->session = session;
                state_->selected_target = TargetId{value, TargetKind::WaylandSurface};
                {
                    std::lock_guard lock(session->data_mutex);
                    session->preferred_surface = static_cast<uint32_t>(value);
                }
                if (state_->focus_runtime) (void)state_->focus_runtime->apply();
                const bool supported = state_->backend == CaptureKind::SHM
                                    || state_->backend == CaptureKind::CLIENTBUF
                                    || state_->backend == CaptureKind::DMABUF
                                    || state_->backend == CaptureKind::KMS;
                session->backend_unsupported = !supported;
                session->capture_backend = static_cast<int32_t>(state_->backend);
                session->background_capture = supported && state_->backend == CaptureKind::DMABUF;
                return boost::outcome_v2::success();
            }
        }
        return code(errc::unspecified);
    }

    [[nodiscard]] TargetId selected_target() const {
        if (!state_) return {};
        if (state_->selected_target.value != 0) return state_->selected_target;
        if (state_->session && state_->session->preferred_surface != 0)
            return TargetId{state_->session->preferred_surface, TargetKind::WaylandSurface};
        return {};
    }

private:
    SessionContext *state_ = nullptr;
};

const InterfaceMethod kMethods[] = {
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(TargetSelectorTraits, name, &TargetProvider::name),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(TargetSelectorTraits, available, &TargetProvider::available),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(TargetSelectorTraits, select, &TargetProvider::select),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(TargetSelectorTraits, selected, &TargetProvider::selected),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(TargetSelectorTraits, clear, &TargetProvider::clear),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(TargetSelectorTraits, select_target, &TargetProvider::select_target),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(TargetSelectorTraits, selected_target, &TargetProvider::selected_target),
};

static_assert(std::size(kMethods) == TargetSelectorTraits::signatures.size());

constexpr auto kProviderType = skl::abix::model::canonical_type_id("skl::wcas::AutoTargetProvider");

InterfaceStatus register_target_provider(InterfaceRegistry<4> &registry) {
    return registry.add({TargetSelectorTraits::id, kProviderType, kMethods, static_cast<uint32_t>(std::size(kMethods)),
        TargetSelectorTraits::version, 0});
}

struct TargetRegistry {
    TargetRegistry() {
        if (register_target_provider(registry) != InterfaceStatus::ok) return;
    }
    InterfaceRegistry<4> registry;
};

const InterfaceRegistry<4> &target_registry() {
    static const TargetRegistry holder;
    return holder.registry;
}

}   // namespace

struct TargetRuntime::Impl {
    explicit Impl(SessionContext *state)
        : device(state) {
        const auto &registry = target_registry();
        (void)selector.bind(registry, &device, kProviderType);
    }

    TargetProvider device;
    TargetSelector selector;
};

TargetRuntime::TargetRuntime(SessionContext *state)
    : _impl(std::make_unique<Impl>(state)) {}
TargetRuntime::~TargetRuntime() = default;

result<void> TargetRuntime::select(int32_t index) {
    if (!_impl || !_impl->selector.valid()) return code(errc::feature_not_implemented);
    return _impl->selector.select(index);
}
uint32_t TargetRuntime::selected() const { return _impl && _impl->selector.valid() ? _impl->selector.selected() : 0; }
result<void> TargetRuntime::clear() {
    if (!_impl || !_impl->selector.valid()) return code(errc::feature_not_implemented);
    return _impl->selector.clear();
}
result<void> TargetRuntime::select_target(TargetId target) {
    if (!_impl || !_impl->selector.valid()) return code(errc::feature_not_implemented);
    return _impl->selector.select_target(target.value, static_cast<uint32_t>(target.kind));
}
TargetId TargetRuntime::selected_target() const {
    return _impl && _impl->selector.valid() ? _impl->selector.selected_target() : TargetId{};
}

}   // namespace skl::wcas
