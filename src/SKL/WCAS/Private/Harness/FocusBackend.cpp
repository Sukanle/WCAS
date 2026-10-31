#include <iterator>
#include <memory>
#include <string>
#include <utility>

#include "SKL/WCAS/Error.h"

#include "SKL/WCAS/Private/Harness/Session.h"
#include "SKL/WCAS/Private/Harness/FocusBackend.h"
#include "SKL/WCAS/Private/Harness/ControlledAppImpl.h"

namespace skl::wcas {
namespace {

using skl::abix::poly::InterfaceMethod;
using skl::abix::poly::InterfaceRegistry;
using skl::abix::poly::InterfaceStatus;

std::error_code code(errc value) { return ::skl::make_error_code(value); }

// Focus policy provider. Activation itself is forwarded by the Wayland proxy;
// this provider decides which session may request it.
class FocusProvider {
public:
    explicit FocusProvider(SessionContext *state) noexcept
        : state_(state) {}

    [[nodiscard]] std::string name() const { return "auto-focus"; }
    [[nodiscard]] bool available() const { return state_ && state_->transport != Transport::xwayland; }

    result<void> set_allowed(bool value) {
        if (!state_) return code(errc::unspecified);
        state_->allow_activation = value;
        return apply();
    }

    [[nodiscard]] bool allowed() const { return state_ && state_->allow_activation; }

    result<void> apply() {
        if (!state_) return code(errc::unspecified);
        if (state_->child > 0) {
            for (const auto &session : detail::sessions_for(state_->child, state_->match_regex))
                session->allow_activation = state_->allow_activation && session == state_->session;
        }
        return boost::outcome_v2::success();
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
