#pragma once

#include <chrono>

#include <fmt/format.h>

namespace skl::wcas {

/// Category-gated tracing used to diagnose launches, forwarding and capture.
///
/// `WCAS_DEBUG` (set by `--debug-all`) turns on every category at once; each
/// `WCAS_DEBUG_<CATEGORY>` variable (set by the matching `--debug-<category>`
/// flag) turns on a single one. The result is cached in a function-local static,
/// so the check is cheap enough to run on a per-message hot path.
enum class DebugCategory {
    Harness,    ///< launch, native-wait and XWayland fallback
    Proxy,      ///< connection lifecycle and message forwarding
    Input,      ///< injected pointer/keyboard input
    Capture,    ///< capture pipeline (slots, scheduler)
    Dmabuf,     ///< DMA-BUF import and readback
    Xwayland,   ///< XWayland bridge
    Wire,       ///< per-message Wayland tracing
};

inline const char *debug_category_name(DebugCategory category) {
    switch (category) {
        case DebugCategory::Harness:  return "harness";
        case DebugCategory::Proxy:    return "proxy";
        case DebugCategory::Input:    return "input";
        case DebugCategory::Capture:  return "capture";
        case DebugCategory::Dmabuf:   return "dmabuf";
        case DebugCategory::Xwayland: return "xwayland";
        case DebugCategory::Wire:     return "wire";
    }
    return "unknown";
}

inline bool debug_enabled(DebugCategory category) {
    static const bool umbrella = getenv("WCAS_DEBUG") != nullptr;
    if (umbrella) return true;
    switch (category) {
        case DebugCategory::Harness: {
            static const bool enabled = getenv("WCAS_DEBUG_HARNESS") != nullptr;
            return enabled;
        }
        case DebugCategory::Proxy: {
            static const bool enabled = getenv("WCAS_DEBUG_PROXY") != nullptr;
            return enabled;
        }
        case DebugCategory::Input: {
            static const bool enabled = getenv("WCAS_DEBUG_INPUT") != nullptr;
            return enabled;
        }
        case DebugCategory::Capture: {
            static const bool enabled = getenv("WCAS_DEBUG_CAPTURE") != nullptr;
            return enabled;
        }
        case DebugCategory::Dmabuf: {
            static const bool enabled = getenv("WCAS_DEBUG_DMABUF") != nullptr;
            return enabled;
        }
        case DebugCategory::Xwayland: {
            static const bool enabled = getenv("WCAS_DEBUG_XWAYLAND") != nullptr;
            return enabled;
        }
        case DebugCategory::Wire: {
            static const bool enabled = getenv("WCAS_DEBUG_WIRE") != nullptr;
            return enabled;
        }
    }
    return false;
}

/// Prints `[wcas:<category> t=<monotonic ms>] <message>` to stderr when enabled.
/// The monotonic timestamp lets these lines be correlated with `WINEDEBUG` and
/// the DMA-BUF capture trace, which use the same clock.
inline void debug_trace(DebugCategory category, std::string_view message) {
    if (!debug_enabled(category)) return;
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count();
    fmt::print(stderr, "[wcas:{} t={}] {}\n", debug_category_name(category), ms, message);
}

}   // namespace skl::wcas
