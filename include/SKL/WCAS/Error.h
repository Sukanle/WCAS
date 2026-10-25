#pragma once
#include "SKL/ErrorCode.h"
#include <stdint.h>

namespace skl {
#define SKL_WCAS_ERROR_CODES(X)                                                                                       \
    X(no_wayland_session, "No Wayland session is available to proxy into.", 0X0001, SKL_STDERR_FALLBACK)              \
    X(missing_runtime_dir, "XDG_RUNTIME_DIR is not set; no runtime directory for the seat socket.", 0X0002,           \
        SKL_STDERR_FALLBACK)                                                                                          \
    X(socket_bind_failed, "Could not create or bind the agent seat socket.", 0X0003, std::errc::address_in_use)       \
    X(xwayland_bridge_failed, "XWayland bridge failed to start or serve.", 0X0004, std::errc::protocol_not_supported) \
    X(xwayland_authority_failed, "XWayland xauth credential setup failed.", 0X0005, std::errc::permission_denied)     \
    X(peer_credentials_not_valid, "Kernel peer credentials could not be read or did not validate.", 0X0006,           \
        std::errc::permission_denied)                                                                                 \
    X(frame_capture_failed, "Reading or decoding the app's rendered frame failed.", 0X0007, std::errc::io_error)      \
    X(input_injection_failed, "Synthesizing or delivering input events to the app failed.", 0X0008,                   \
        std::errc::io_error)                                                                                          \
    X(native_wayland_timeout,                                                                                         \
        "The application did not produce a stable capturable native Wayland surface before timeout.", 0X000B,         \
        std::errc::timed_out)                                                                                         \
    X(feature_not_implemented, "This capability is intentionally not implemented in the C++ phase-one migration.",    \
        0X000C, std::errc::not_supported)                                                                             \
    X(process_spawn_failed, "Spawning or supervising a helper process or worker thread failed.", 0X0009,              \
        std::errc::no_child_process)                                                                                  \
    X(io_error, "An underlying I/O operation failed.", 0X000A, std::errc::io_error)                                   \
    X(unspecified, "Any other failure that does not fit a dedicated variant.", 0X00FF, SKL_STDERR_FALLBACK)


SKL_DERIVE_ERROR_H(wcas, WCAS)
}   // namespace skl
