#include "harness/health_monitor.h"

#include "skl/wcas/error.h"

#include "harness/controlled_app_impl.h"
#include "harness/process_host.h"

#include <iterator>
#include <memory>
#include <string>

namespace skl::wcas {
namespace {

using skl::abix::poly::InterfaceMethod;
using skl::abix::poly::InterfaceRegistry;
using skl::abix::poly::InterfaceStatus;

class HealthProvider {
public:
    explicit HealthProvider(SessionContext *state) noexcept
        : state_(state) {}

    [[nodiscard]] std::string name() const { return "auto-health"; }
    [[nodiscard]] bool available() const { return state_ != nullptr; }

    [[nodiscard]] bool alive() const {
        if (!state_) return false;
        if (state_->process_handle) return state_->process_handle->alive();
        return state_->child > 0;
    }

    [[nodiscard]] bool connected() const {
        if (!state_) return false;
        if (state_->transport == Transport::xwayland) return state_->bridge && state_->bridge->running();
        return state_->session && state_->session->connected.load();
    }

    [[nodiscard]] uint64_t commit_count() const {
        return state_ && state_->session ? state_->session->commit_count.load() : 0;
    }

    [[nodiscard]] uint64_t presented_frames() const {
        return state_ && state_->session ? state_->session->presented_frames.load() : 0;
    }

private:
    SessionContext *state_ = nullptr;
};

const InterfaceMethod kMethods[] = {
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(HealthMonitorTraits, name, &HealthProvider::name),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(HealthMonitorTraits, available, &HealthProvider::available),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(HealthMonitorTraits, alive, &HealthProvider::alive),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(HealthMonitorTraits, connected, &HealthProvider::connected),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(HealthMonitorTraits, commit_count, &HealthProvider::commit_count),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(HealthMonitorTraits, presented_frames, &HealthProvider::presented_frames),
};

static_assert(std::size(kMethods) == HealthMonitorTraits::signatures.size());

constexpr auto kProviderType = skl::abix::model::canonical_type_id("skl::wcas::AutoHealthProvider");

InterfaceStatus register_health_provider(InterfaceRegistry<4> &registry) {
    return registry.add({HealthMonitorTraits::id, kProviderType, kMethods,
        static_cast<std::uint32_t>(std::size(kMethods)), HealthMonitorTraits::version, 0});
}

struct HealthRegistry {
    HealthRegistry() {
        if (register_health_provider(registry) != InterfaceStatus::ok) return;
    }
    InterfaceRegistry<4> registry;
};

const InterfaceRegistry<4> &health_registry() {
    static const HealthRegistry holder;
    return holder.registry;
}

}   // namespace

struct HealthRuntime::Impl {
    explicit Impl(SessionContext *state)
        : device(state) {
        const auto &registry = health_registry();
        (void)monitor.bind(registry, &device, kProviderType);
    }

    HealthProvider device;
    HealthMonitor monitor;
};

HealthRuntime::HealthRuntime(SessionContext *state)
    : _impl(std::make_unique<Impl>(state)) {}
HealthRuntime::~HealthRuntime() = default;

bool HealthRuntime::available() const { return _impl && _impl->monitor.valid() && _impl->monitor.available(); }
bool HealthRuntime::alive() const { return _impl && _impl->monitor.valid() && _impl->monitor.alive(); }
bool HealthRuntime::connected() const { return _impl && _impl->monitor.valid() && _impl->monitor.connected(); }
uint64_t HealthRuntime::commit_count() const {
    return _impl && _impl->monitor.valid() ? _impl->monitor.commit_count() : 0;
}
uint64_t HealthRuntime::presented_frames() const {
    return _impl && _impl->monitor.valid() ? _impl->monitor.presented_frames() : 0;
}

}   // namespace skl::wcas
