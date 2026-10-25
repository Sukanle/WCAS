#include <chrono>
#include <utility>

#include "SKL/WCAS/Private/Proxy/CaptureSlot.h"
#include "SKL/WCAS/Private/Proxy/DMABUF/Vulkan.h"

#include <fmt/base.h>

namespace skl::wcas::proxy {
namespace {

constexpr auto waiting_producer_timeout = std::chrono::seconds(2);
constexpr auto submitted_timeout = std::chrono::seconds(5);

}   // namespace

CaptureSlot::CaptureSlot(int index)
    : index_(index) {}

CaptureSlot::~CaptureSlot() { cleanup(); }

bool CaptureSlot::start(CaptureJob job) {
    if (state_ != CaptureState::Free) return false;

    bool active = false;
    const int sync_fd = export_dmabuf_sync_file(job.fd, &active);
    job_ = std::move(job);
    tick_start_ = std::chrono::steady_clock::now();

    if (sync_fd >= 0 && active) {
        sync_fd_ = sync_fd;
        state_ = CaptureState::WaitingProducer;
        return true;
    }
    // No producer fence to wait on: the caller submits immediately.
    if (sync_fd >= 0) ::close(sync_fd);
    return false;
}

bool CaptureSlot::submit() {
    if (!job_.has_value()) return false;
    state_ = CaptureState::Submitted;

    cv::Mat image;
    const bool ok = capture_dmabuf_plane(
        job_->fd, job_->width, job_->height, job_->stride, job_->modifier, job_->format, job_->offset, image);
    state_ = CaptureState::GpuComplete;
    if (!ok) {
        reset();
        return false;
    }
    result_ = std::move(image);
    state_ = CaptureState::CpuProcessing;
    return true;
}

bool CaptureSlot::check_timeout() {
    if (state_ != CaptureState::WaitingProducer && state_ != CaptureState::Submitted) return false;
    const auto timeout = state_ == CaptureState::WaitingProducer ? waiting_producer_timeout : submitted_timeout;
    if (std::chrono::steady_clock::now() - tick_start_ < timeout) return false;
    reset();
    return true;
}

optional<cv::Mat> CaptureSlot::take_result() {
    if (state_ != CaptureState::CpuProcessing) return std::nullopt;
    auto result = std::move(result_);
    result_.reset();
    cleanup();
    state_ = CaptureState::Free;
    return result;
}

void CaptureSlot::reset() {
    cleanup();
    state_ = CaptureState::Free;
}

void CaptureSlot::cleanup() {
    job_.reset();
    if (sync_fd_ >= 0) {
        ::close(sync_fd_);
        sync_fd_ = -1;
    }
}

}   // namespace skl::wcas::proxy
