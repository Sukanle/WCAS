#pragma once

#include "skl/wcas/frame.h"

#include <cstddef>
#include <cstdint>
#include <memory>

#include <opencv2/core.hpp>

namespace skl::wcas {

/// Internal push fan-out bus. The capture path calls publish() with a frame;
/// each attached sink has an independent runtime (mailbox/queue + worker) and
/// never runs on the capture thread.
class FrameBus {
public:
    explicit FrameBus(size_t frame_pool_capacity = 8);
    ~FrameBus();
    FrameBus(const FrameBus &) = delete;
    FrameBus &operator=(const FrameBus &) = delete;
    FrameBus(FrameBus &&) = delete;
    FrameBus &operator=(FrameBus &&) = delete;

    [[nodiscard]] result<uint64_t> attach(FrameSink sink, SinkPolicy policy);
    void detach(uint64_t id);
    void publish(const cv::Mat &image);
    void publish_state(int32_t width, int32_t height);
    void publish_error(int32_t code, std::string message);
    void close();
    [[nodiscard]] optional<SinkStats> stats(uint64_t id) const;

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

}   // namespace skl::wcas
