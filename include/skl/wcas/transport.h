#pragma once

#include "skl/wcas/api.h"
#include "skl/wcas/types.h"

#include "ABIX/Interface/Declaration.h"

#include <stdint.h>

#include <string>
#include <utility>
#include <vector>

namespace skl::wcas {

/// Informational transport identity. `Custom` values are allowed for future
/// platforms; consumers must query capabilities, not switch on this enum.
enum class TransportKind : uint32_t {
    NativeWayland = 0,
    XWayland = 1,
    Custom = 0X100,
    Unknown = 0XFFFFFFFF,
};

/// Capabilities a transport may provide. This is the discovery/negotiation
/// contract; it is deliberately not a list of methods.
enum class TransportCapability : uint64_t {
    Process = 1ull << 0,
    Capture = 1ull << 1,
    WindowEnumeration = 1ull << 2,
    WindowResolution = 1ull << 3,
    Pointer = 1ull << 4,
    Keyboard = 1ull << 5,
    Text = 1ull << 6,
    Gamepad = 1ull << 7,
    Focus = 1ull << 8,
    Health = 1ull << 9,
    Target = 1ull << 10,
    BackgroundCapture = 1ull << 11,
};

SKL_WCAS_ENABLE_BITMASK_OPERATORS(TransportCapability);

/// Opaque target identity. Providers interpret `value`; consumers never assume
/// it is a Wayland surface id or an X11 window id.
enum class TargetKind : uint32_t {
    WaylandSurface = 0,
    X11Window = 1,
    Custom = 0X100,
    Unknown = 0XFFFFFFFF,
};

struct TargetId {
    uint64_t value = 0;
    TargetKind kind = TargetKind::Unknown;

    friend constexpr bool operator==(const TargetId &lhs, const TargetId &rhs) noexcept {
        return lhs.value == rhs.value && lhs.kind == rhs.kind;
    }
    friend constexpr bool operator!=(const TargetId &lhs, const TargetId &rhs) noexcept { return !(lhs == rhs); }
};

/// Opaque session handle for future factory/provider plumbing.
struct SessionHandle {
    uint64_t value = 0;
    TransportKind kind = TransportKind::Unknown;
};

/// Platform-neutral launch description. Platform-specific environment
/// extensions stay in the provider's ProcessSpec, not in this contract.
struct LaunchSpec {
    std::string program;
    std::vector<std::string> args;
    std::vector<std::pair<std::string, optional<std::string>>> env;
    optional<std::string> current_dir;
};

enum class TransportStatus : uint32_t {
    Ok = 0,
    Unavailable = 1,
    NotConnected = 2,
    Busy = 3,
    Error = 4,
};

// RESERVED interface. No provider is registered in this build yet.
//
// TransportBackend is a descriptor/factory-facing contract, not a data-plane
// interface: it says which transport is present and which capabilities it can
// provide. Capture/Window/Input/Focus/Health remain separate contracts.
#define SKL_WCAS_TRANSPORT_BACKEND_METHODS(X)       \
    X(name, std::string, str, {})                   \
    X(kind, TransportKind, reg_transport::kind, {}) \
    X(available, bool, bool_t, {})                  \
    X(capabilities, TransportCapability, reg_transport::capability, {})

SKL_ABIX_INTERFACE_UNIQUE_DECLARE(
    TransportBackend, SKL_WCAS_TRANSPORT_BACKEND_METHODS, SKL_ABIX_VERSION_NUMBER(0, 1, 0), SKL_WCAS_API);

#undef SKL_WCAS_TRANSPORT_BACKEND_METHODS

}   // namespace skl::wcas
