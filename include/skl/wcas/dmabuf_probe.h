#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "skl/wcas/api.h"

namespace skl {
namespace wcas {

/// Result of a single DMA-BUF sync probe.
struct ProbeResult {
    std::string name;
    bool passed = false;
    std::string detail;
    std::chrono::microseconds elapsed{};
};

/// A full probe run: basic fd info plus each individual result.
struct ProbeResults {
    std::string basic_info;
    std::vector<ProbeResult> results;

    /// Multi-line human-readable report (mirrors the Rust probe's `report()`).
    [[nodiscard]] std::string report() const;
};

/// Non-destructive DMA-BUF synchronisation capability probe.
///
/// Exercises `DMA_BUF_IOCTL_SYNC`, `DMA_BUF_IOCTL_EXPORT_SYNC_FILE` (READ /
/// WRITE / READ|WRITE), `poll()` on both directions and the io_uring
/// `POLL_ADD` single-shot and multishot paths. The caller owns `fd`.
class SKL_WCAS_API DmaBufSyncProbe {
public:
    explicit DmaBufSyncProbe(int fd) noexcept
        : fd_(fd) {}

    [[nodiscard]] std::string basic_info() const;
    [[nodiscard]] ProbeResult validate_fd() const;
    [[nodiscard]] ProbeResult test_sync_ioctl() const;
    [[nodiscard]] ProbeResult test_export_sync_file(std::uint32_t flags, const char *label) const;
    [[nodiscard]] ProbeResult test_poll(std::int16_t events, const char *label, int timeout_ms) const;
    [[nodiscard]] ProbeResult test_io_uring_poll(
        std::uint32_t events, const char *label, std::uint64_t timeout_ms) const;
    [[nodiscard]] ProbeResult test_io_uring_multishot(std::uint32_t events, const char *label) const;
    [[nodiscard]] ProbeResults run_all() const;

private:
    int fd_ = -1;
};

/// Record a discovered DMA-BUF fd for the `testdmabuf` CLI command (deduped).
SKL_WCAS_API void track_dmabuf_fd(int fd);
[[nodiscard]] SKL_WCAS_API std::vector<int> tracked_dmabuf_fds();
SKL_WCAS_API void clear_tracked_dmabuf_fds();

}   // namespace wcas
}   // namespace skl
