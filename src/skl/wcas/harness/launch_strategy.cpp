#include "harness/launch_strategy.h"

#include "skl/wcas/error.h"

#include <algorithm>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

namespace skl::wcas {
namespace {

using skl::abix::poly::InterfaceMethod;
using skl::abix::poly::InterfaceRegistry;
using skl::abix::poly::InterfaceStatus;

class NativeStrategy {
public:
    [[nodiscard]] std::string name() const { return "native-wayland"; }
    [[nodiscard]] LaunchKind kind() const { return LaunchKind::NativeWayland; }
    [[nodiscard]] bool available() const { return true; }
    [[nodiscard]] int32_t priority() const { return 100; }
    [[nodiscard]] bool requires_x11() const { return false; }
};

class XWaylandStrategy {
public:
    [[nodiscard]] std::string name() const { return "xwayland"; }
    [[nodiscard]] LaunchKind kind() const { return LaunchKind::XWayland; }
    [[nodiscard]] bool available() const { return true; }
    [[nodiscard]] int32_t priority() const { return 0; }
    [[nodiscard]] bool requires_x11() const { return true; }
};

const InterfaceMethod kNativeMethods[] = {
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(LaunchStrategyTraits, name, &NativeStrategy::name),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(LaunchStrategyTraits, kind, &NativeStrategy::kind),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(LaunchStrategyTraits, available, &NativeStrategy::available),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(LaunchStrategyTraits, priority, &NativeStrategy::priority),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(LaunchStrategyTraits, requires_x11, &NativeStrategy::requires_x11),
};
const InterfaceMethod kXWaylandMethods[] = {
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(LaunchStrategyTraits, name, &XWaylandStrategy::name),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(LaunchStrategyTraits, kind, &XWaylandStrategy::kind),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(LaunchStrategyTraits, available, &XWaylandStrategy::available),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(LaunchStrategyTraits, priority, &XWaylandStrategy::priority),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(LaunchStrategyTraits, requires_x11, &XWaylandStrategy::requires_x11),
};

static_assert(std::size(kNativeMethods) == LaunchStrategyTraits::signatures.size());
static_assert(std::size(kXWaylandMethods) == LaunchStrategyTraits::signatures.size());

constexpr auto kNativeType = skl::abix::model::canonical_type_id("skl::wcas::NativeLaunchStrategy");
constexpr auto kXWaylandType = skl::abix::model::canonical_type_id("skl::wcas::XWaylandLaunchStrategy");

InterfaceStatus register_launch_strategies(InterfaceRegistry<4> &registry) {
    InterfaceStatus status = registry.add({LaunchStrategyTraits::id, kNativeType, kNativeMethods,
        static_cast<std::uint32_t>(std::size(kNativeMethods)), LaunchStrategyTraits::version, 0});
    if (status != InterfaceStatus::ok) return status;
    return registry.add({LaunchStrategyTraits::id, kXWaylandType, kXWaylandMethods,
        static_cast<std::uint32_t>(std::size(kXWaylandMethods)), LaunchStrategyTraits::version, 0});
}

struct StrategyRegistry {
    StrategyRegistry() {
        if (register_launch_strategies(registry) != InterfaceStatus::ok) return;
    }
    InterfaceRegistry<4> registry;
};

const InterfaceRegistry<4> &strategy_registry() {
    static const StrategyRegistry holder;
    return holder.registry;
}

}   // namespace

std::vector<LaunchStrategy> launch_strategies() {
    // Providers must outlive the proxies returned to the caller.
    static NativeStrategy native;
    static XWaylandStrategy xwayland;
    const auto &registry = strategy_registry();
    LaunchStrategy native_proxy(registry, &native, kNativeType);
    LaunchStrategy xwayland_proxy(registry, &xwayland, kXWaylandType);
    std::vector<LaunchStrategy> strategies;
    if (native_proxy.valid()) strategies.push_back(std::move(native_proxy));
    if (xwayland_proxy.valid()) strategies.push_back(std::move(xwayland_proxy));
    std::sort(strategies.begin(), strategies.end(),
        [](const LaunchStrategy &lhs, const LaunchStrategy &rhs) { return lhs.priority() > rhs.priority(); });
    return strategies;
}

}   // namespace skl::wcas
