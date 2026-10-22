#include "capture_scheduler.h"

#include "debug.h"
#include "reactor.h"

#include <utility>
#include <algorithm>
#include <fmt/std.h>

namespace skl::wcas::proxy {

CaptureScheduler::CaptureScheduler() {
    slots_.reserve(kSlotCount);
    for (int i = 0; i < kSlotCount; ++i)
        slots_.emplace_back(i);
}

CaptureScheduler::~CaptureScheduler() = default;

std::vector<CaptureState> CaptureScheduler::slot_states() const {
    std::vector<CaptureState> states;
    states.reserve(slots_.size());
    for (const auto &slot : slots_)
        states.push_back(slot.state());
    return states;
}

bool CaptureScheduler::has_free_slot() const {
    // for (const auto &slot : slots_)
    //     if (slot.is_free()) return true;
    // return false;
    return std::ranges::any_of(slots_, [](decltype(slots_)::const_reference e) { return e.is_free(); });
}

bool CaptureScheduler::has_submitted() const {
    // for (const auto &slot : slots_) {
    //     if (slot.state() == CaptureState::Submitted) return true;
    // }
    // return false;
    return std::ranges::any_of(
        slots_, [](decltype(slots_)::const_reference e) { return e.state() == CaptureState::Submitted; });
}

bool CaptureScheduler::on_timer_tick(CaptureJob job, Reactor &reactor) {
    for (auto &slot : slots_) {
        if (!slot.is_free()) continue;
        const int index = slot.index();
        const bool pending = slot.start(std::move(job));
        if (pending) {
            if (!reactor.add_fd_oneshot(slot.sync_key(), slot.sync_fd())) {
                slot.reset();
                return false;
            }
        } else {
            // Fence already signaled: submit and complete immediately.
            if (slot.submit()) {
                latest_completed_ = index;
                ++total_completed_;
            }
        }
        ++total_captures_;
        return true;
    }
    // All slots busy: drop this tick (latest-frame policy).
    debug_trace(DebugCategory::Capture, "capture: all slots busy, tick dropped");
    return false;
}

bool CaptureScheduler::on_sync_ready(int slot_index) {
    if (slot_index < 0 || slot_index >= static_cast<int>(slots_.size())) return false;
    auto &slot = slots_[slot_index];
    if (slot.state() != CaptureState::WaitingProducer) return true;
    if (!slot.submit()) return false;
    latest_completed_ = slot_index;
    ++total_completed_;
    return true;
}

int CaptureScheduler::check_timeouts() {
    int reset_count = 0;
    for (int i = 0; i < static_cast<int>(slots_.size()); ++i) {
        if (slots_[i].check_timeout()) {
            if (latest_completed_ == i) latest_completed_.reset();
            ++reset_count;
            debug_trace(DebugCategory::Capture, fmt::format("capture: slot {} timed out and was reset", i));
        }
    }
    return reset_count;
}

optional<cv::Mat> CaptureScheduler::take_latest_snapshot() {
    if (!latest_completed_.has_value()) return std::nullopt;
    auto result = slots_[*latest_completed_].take_result();
    if (result.has_value()) latest_completed_.reset();
    return result;
}

}   // namespace skl::wcas::proxy
