#pragma once

#include <vector>

#include "SKL/WCAS/Private/Proxy/CaptureSlot.h"

namespace skl::wcas::proxy {

class Reactor;

/// Manages the pool of capture slots, driven by the reactor's periodic capture
/// timer and one-shot sync_file events. Implements a "latest-frame" policy:
/// the most recently completed snapshot wins, and a timer tick is dropped when
/// all slots are busy.
class CaptureScheduler {
public:
    static constexpr int kSlotCount = 3;

    CaptureScheduler();
    ~CaptureScheduler();
    CaptureScheduler(const CaptureScheduler &) = delete;
    CaptureScheduler &operator=(const CaptureScheduler &) = delete;

    [[nodiscard]] bool valid() const { return !slots_.empty(); }
    [[nodiscard]] std::vector<CaptureState> slot_states() const;
    [[nodiscard]] bool has_free_slot() const;
    [[nodiscard]] bool has_submitted() const;

    /// Assign `job` to a free slot. Returns false when all slots are busy (the
    /// tick is dropped). Registers the sync fd with `reactor` when a producer
    /// fence is pending.
    bool on_timer_tick(CaptureJob job, Reactor &reactor);
    /// A slot's sync_file fired: submit its GPU copy.
    bool on_sync_ready(int slot_index);
    /// Reset slots stuck in WaitingProducer/Submitted. Returns the count.
    int check_timeouts();
    /// Take the most recently completed snapshot, freeing its slot.
    optional<cv::Mat> take_latest_snapshot();

    [[nodiscard]] uint64_t total_captures() const { return total_captures_; }
    [[nodiscard]] uint64_t total_completed() const { return total_completed_; }

private:
    std::vector<CaptureSlot> slots_;
    optional<int> latest_completed_;
    uint64_t total_captures_ = 0;
    uint64_t total_completed_ = 0;
};

}   // namespace skl::wcas::proxy
