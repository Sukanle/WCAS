#pragma once

#include "skl/wcas/api.h"
#include "skl/wcas/types.h"

#include "ABIX/Interface/Declaration.h"

#include <cstdint>
#include <string>
#include <vector>

namespace skl::wcas {

/// Launch strategy identity. `Custom` is reserved for future transports or
/// platform launchers; the executor still needs to understand a custom kind.
enum class LaunchKind : uint32_t {
    NativeWayland = 0,
    XWayland = 1,
    Custom = 0x100,
    Unknown = 0xFFFFFFFF,
};

/// Descriptor contract for launch ordering, not a data-plane launcher. The
/// actual fork/exec/wait logic stays in ProcessHost/ComputerUse until a POD
/// session handle and lease model are stable.
#define SKL_WCAS_LAUNCH_STRATEGY_METHODS(X)   \
    X(name, std::string, str, {})             \
    X(kind, LaunchKind, reg_launch::kind, {}) \
    X(available, bool, bool_t, {})            \
    X(priority, int32_t, i32, {})             \
    X(requires_x11, bool, bool_t, {})

SKL_ABIX_INTERFACE_UNIQUE_DECLARE(
    LaunchStrategy, SKL_WCAS_LAUNCH_STRATEGY_METHODS, SKL_ABIX_VERSION_NUMBER(0, 1, 0), SKL_WCAS_API);

#undef SKL_WCAS_LAUNCH_STRATEGY_METHODS

/// Bound, priority-sorted launch strategy descriptors.
[[nodiscard]] std::vector<LaunchStrategy> launch_strategies();

}   // namespace skl::wcas
