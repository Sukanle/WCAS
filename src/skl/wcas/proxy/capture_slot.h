#pragma once
#include "skl/wcas/types.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <utility>

#include <unistd.h>

#include <opencv2/core.hpp>

#include "reactor.h"

namespace skl::wcas::proxy {

/// State of a single capture slot.
enum class CaptureState {
    /// Ready for a new capture.
    Free,
    /// DMA-BUF acquired, waiting for the producer sync_file to signal.
    WaitingProducer,
    /// Vulkan copy submitted, waiting for the GPU.
    Submitted,
    /// GPU copy complete, ready for CPU readback.
    GpuComplete,
    /// Snapshot ready for consumption.
    CpuProcessing,
};

/// A capture job: one committed DMA-BUF plane plus its geometry. Owns its
/// duplicated plane fd for as long as it exists.
struct CaptureJob {
    int fd = -1;
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::uint32_t format = 0;
    std::uint64_t modifier = 0;
    std::uint32_t stride = 0;
    std::uint32_t offset = 0;
    std::uint64_t generation = 0;

    CaptureJob() = default;
    CaptureJob(CaptureJob &&other) noexcept
        : fd(other.fd)
        , width(other.width)
        , height(other.height)
        , format(other.format)
        , modifier(other.modifier)
        , stride(other.stride)
        , offset(other.offset)
        , generation(other.generation) {
        other.fd = -1;
    }
    CaptureJob &operator=(CaptureJob &&other) noexcept {
        if (this != &other) {
            if (fd >= 0) ::close(fd);
            fd = other.fd;
            width = other.width;
            height = other.height;
            format = other.format;
            modifier = other.modifier;
            stride = other.stride;
            offset = other.offset;
            generation = other.generation;
            other.fd = -1;
        }
        return *this;
    }
    CaptureJob(const CaptureJob &) = delete;
    CaptureJob &operator=(const CaptureJob &) = delete;
    ~CaptureJob() {
        if (fd >= 0) ::close(fd);
    }
};

/// One of the three capture pipeline slots. The state machine mirrors the Rust
/// implementation. The GPU copy runs synchronously inside `submit()` because
/// the C++ readback path shares a single Vulkan device; the slot still owns its
/// lifecycle so several captures can be in flight across the event loop.
class CaptureSlot {
public:
    explicit CaptureSlot(int index);
    ~CaptureSlot();
    CaptureSlot(const CaptureSlot &) = delete;
    CaptureSlot &operator=(const CaptureSlot &) = delete;
    CaptureSlot(CaptureSlot &&other) noexcept
        : index_(other.index_)
        , state_(other.state_)
        , job_(std::move(other.job_))
        , sync_fd_(other.sync_fd_)
        , result_(std::move(other.result_))
        , tick_start_(other.tick_start_) {
        other.sync_fd_ = -1;
        other.state_ = CaptureState::Free;
    }
    CaptureSlot &operator=(CaptureSlot &&other) noexcept {
        if (this != &other) {
            cleanup();
            index_ = other.index_;
            state_ = other.state_;
            job_ = std::move(other.job_);
            sync_fd_ = other.sync_fd_;
            result_ = std::move(other.result_);
            tick_start_ = other.tick_start_;
            other.sync_fd_ = -1;
            other.state_ = CaptureState::Free;
        }
        return *this;
    }

    [[nodiscard]] int index() const { return index_; }
    [[nodiscard]] CaptureState state() const { return state_; }
    [[nodiscard]] bool is_free() const { return state_ == CaptureState::Free; }
    [[nodiscard]] std::uint64_t sync_key() const { return key_sync_file_base + static_cast<std::uint64_t>(index_); }
    [[nodiscard]] int sync_fd() const { return sync_fd_; }

    /// Take ownership of `job` and export its producer fence. Returns true when
    /// the caller must register `sync_fd()` with the reactor; false when the
    /// fence was already signaled and `submit()` can run immediately.
    bool start(CaptureJob job);
    /// Import + GPU copy + readback, leaving the slot in CpuProcessing.
    bool submit();
    /// Reset a slot stuck in WaitingProducer/Submitted for too long.
    bool check_timeout();
    optional<cv::Mat> take_result();
    void reset();

private:
    void cleanup();

    int index_ = 0;
    CaptureState state_ = CaptureState::Free;
    optional<CaptureJob> job_;
    int sync_fd_ = -1;
    optional<cv::Mat> result_;
    std::chrono::steady_clock::time_point tick_start_{};
};

}   // namespace skl::wcas::proxy
