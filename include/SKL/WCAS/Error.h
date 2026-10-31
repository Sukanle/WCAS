#pragma once

#include "SKL/ErrorCode.h"
#include "SKL/EPCD/Core/StaticDomain.h"

#include <stdint.h>
#include <string>
#include <system_error>

namespace skl::wcas {

// WCAS owns this static error domain. The numeric values are stable: they are
// the error values inside the domain, not the DomainId itself.
//
// Domain ID allocation (SKL ecosystem):
//   owner  = SKL
//   module = 0x001 (WCAS)
//   local  = 0x0001 (WCAS error domain)
#define SKL_WCAS_ERROR_LIST(X)                                                                              \
    X(no_wayland_session, 0x0001, "No Wayland session is available to proxy into.",                         \
        ::skl::epcd::GenericCondition::unknown)                                                             \
    X(missing_runtime_dir, 0x0002, "XDG_RUNTIME_DIR is not set; no runtime directory for the seat socket.", \
        ::skl::epcd::GenericCondition::unknown)                                                             \
    X(socket_bind_failed, 0x0003, "Could not create or bind the agent seat socket.",                        \
        ::skl::epcd::GenericCondition::address_in_use)                                                      \
    X(xwayland_bridge_failed, 0x0004, "XWayland bridge failed to start or serve.",                          \
        ::skl::epcd::GenericCondition::protocol_not_supported)                                              \
    X(xwayland_authority_failed, 0x0005, "XWayland xauth credential setup failed.",                         \
        ::skl::epcd::GenericCondition::permission_denied)                                                   \
    X(peer_credentials_not_valid, 0x0006, "Kernel peer credentials could not be read or did not validate.", \
        ::skl::epcd::GenericCondition::permission_denied)                                                   \
    X(frame_capture_failed, 0x0007, "Reading or decoding the app's rendered frame failed.",                 \
        ::skl::epcd::GenericCondition::io_error)                                                            \
    X(input_injection_failed, 0x0008, "Synthesizing or delivering input events to the app failed.",         \
        ::skl::epcd::GenericCondition::io_error)                                                            \
    X(native_wayland_timeout, 0x000B,                                                                       \
        "The application did not produce a stable capturable native Wayland surface before timeout.",       \
        ::skl::epcd::GenericCondition::timed_out)                                                           \
    X(feature_not_implemented, 0x000C,                                                                      \
        "This capability is intentionally not implemented in the C++ phase-one migration.",                 \
        ::skl::epcd::GenericCondition::not_supported)                                                       \
    X(process_spawn_failed, 0x0009, "Spawning or supervising a helper process or worker thread failed.",    \
        ::skl::epcd::GenericCondition::no_child_process)                                                    \
    X(io_error, 0x000A, "An underlying I/O operation failed.", ::skl::epcd::GenericCondition::io_error)     \
    X(unspecified, 0x00FF, "Any other failure that does not fit a dedicated variant.",                      \
        ::skl::epcd::GenericCondition::unknown)

EPCD_DEFINE_STATIC_DOMAIN(WCASEC, ::skl::epcd::make_skl_domain(0x001, 0x0001), "WCAS", SKL_WCAS_ERROR_LIST);

// Compatibility spelling for existing WCAS call sites. The canonical enum is
// EPCD's generated WcasError.
using errc = WCASEC;

// std::error_code adapter. EPCD remains the source of truth for the static
// domain; this category only bridges legacy std::error_code consumers to it.
class error_category final : public ::skl::error_category {
public:
    ~error_category() override = default;

    [[nodiscard]] static const error_category &instance() noexcept;

    [[nodiscard]] const char *name() const noexcept override;
    [[nodiscard]] std::string message(int ev) const override;
    [[nodiscard]] const char *module() const noexcept override;
    [[nodiscard]] std::error_condition default_error_condition(int ev) const noexcept override;

private:
    error_category() = default;
};

/// Registers the WCAS static domain with EPCD's CategoryRegistry.
/// Idempotent; also runs automatically when the library is loaded.
[[nodiscard]] bool register_errors() noexcept;

/// True when the WCAS domain is available to EPCD consumers.
[[nodiscard]] bool errors_registered() noexcept;

}   // namespace skl::wcas

namespace skl {
[[nodiscard]] std::error_code make_error_code(wcas::errc ec) noexcept;
}   // namespace skl
