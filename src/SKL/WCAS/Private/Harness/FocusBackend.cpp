#include <iterator>
#include <memory>
#include <string>
#include <utility>

#include "SKL/WCAS/Error.h"

#include "SKL/WCAS/Private/Harness/Session.h"
#include "SKL/WCAS/Private/Harness/FocusBackend.h"
#include "SKL/WCAS/Private/Harness/ControlledAppImpl.h"
#include "SKL/WCAS/Private/X11Capture.h"

namespace skl::wcas {
namespace {

using skl::abix::poly::InterfaceMethod;
using skl::abix::poly::InterfaceRegistry;
using skl::abix::poly::InterfaceStatus;

skl::epcd::ErrorCode code(errc value) { return make_error_code(value); }

// Focus policy provider. Activation itself is forwarded by the Wayland proxy;
// this provider decides which session may request it.
class FocusProvider {
public:
    explicit FocusProvider(SessionContext *state) noexcept
        : state_(state) {}

    [[nodiscard]] std::string name() const { return "auto-focus"; }
    [[nodiscard]] bool available() const {
        if (!state_) return false;
        if (state_->transport == Transport::xwayland) return state_->bridge && state_->bridge->running();
        return true;
    }

    result<void> set_allowed(bool value) {
        if (!state_) return code(errc::unspecified);
        state_->allow_activation = value;
        return apply();
    }

    [[nodiscard]] bool allowed() const { return state_ && state_->allow_activation; }

    result<void> apply() {
        if (!state_) return code(errc::unspecified);
        if (state_->transport == Transport::xwayland) return apply_x11();
        if (state_->child > 0) {
            for (const auto &session : detail::sessions_for(state_->child, state_->match_regex))
                session->allow_activation = state_->allow_activation && session == state_->session;
        }
        return boost::outcome_v2::success();
    }

private:
    result<void> apply_x11() {
        if (!state_->bridge || !state_->bridge->running()) return code(errc::xwayland_bridge_failed);
        uint64_t target = state_->selected_target.kind == TargetKind::X11Window ? state_->selected_target.value : 0;
        if (target == 0) {
            auto selected = x11_default_target(state_->bridge->display(), state_->bridge->xauthority());
            if (!selected) return code(errc::unspecified);
            state_->selected_target = selected.value();
            target = selected.value().value;
        }
        return x11_focus_window(state_->bridge->display(), state_->bridge->xauthority(), target);
    }

private:
    SessionContext *state_ = nullptr;
};

const InterfaceMethod kMethods[] = {
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(FocusBackendTraits, name, &FocusProvider::name),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(FocusBackendTraits, available, &FocusProvider::available),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(FocusBackendTraits, set_allowed, &FocusProvider::set_allowed),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(FocusBackendTraits, allowed, &FocusProvider::allowed),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(FocusBackendTraits, apply, &FocusProvider::apply),
};

static_assert(std::size(kMethods) == FocusBackendTraits::signatures.size());

constexpr auto kProviderType = skl::abix::model::canonical_type_id("skl::wcas::AutoFocusProvider");

InterfaceStatus register_focus_provider(InterfaceRegistry<4> &registry) {
    return registry.add({FocusBackendTraits::id, kProviderType, kMethods, static_cast<uint32_t>(std::size(kMethods)),
        FocusBackendTraits::version, 0});
}

struct FocusRegistry {
    FocusRegistry() {
        if (register_focus_provider(registry) != InterfaceStatus::ok) return;
    }
    InterfaceRegistry<4> registry;
};

const InterfaceRegistry<4> &focus_registry() {
    static const FocusRegistry holder;
    return holder.registry;
}

}   // namespace

struct FocusRuntime::Impl {
    explicit Impl(SessionContext *state)
        : device(state) {
        const auto &registry = focus_registry();
        (void)backend.bind(registry, &device, kProviderType);
    }

    FocusProvider device;
    FocusBackend backend;
};

FocusRuntime::FocusRuntime(SessionContext *state)
    : _impl(std::make_unique<Impl>(state)) {}
FocusRuntime::~FocusRuntime() = default;

bool FocusRuntime::available() const { return _impl && _impl->backend.valid() && _impl->backend.available(); }
result<void> FocusRuntime::set_allowed(bool value) {
    if (!_impl || !_impl->backend.valid()) return code(errc::feature_not_implemented);
    return _impl->backend.set_allowed(value);
}
bool FocusRuntime::allowed() const { return _impl && _impl->backend.valid() && _impl->backend.allowed(); }
result<void> FocusRuntime::apply() {
    if (!_impl || !_impl->backend.valid()) return code(errc::feature_not_implemented);
    return _impl->backend.apply();
}

}   // namespace skl::wcas
