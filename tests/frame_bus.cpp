// FrameBus fan-out/backpressure test. Uses the internal bus header so the
// threading and queue policy are exercised, not just the ABIX contract.
#include "SKL/WCAS/Private/Harness/FrameBus.h"

#include <assert.h>
#include <chrono>
#include <condition_variable>
#include <stdio.h>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>

#include <opencv2/core.hpp>

namespace {

using namespace skl::wcas;
using skl::abix::model::TypeId;
using skl::abix::poly::InterfaceMethod;
using skl::abix::poly::InterfaceRegistry;
using skl::abix::poly::InterfaceStatus;

class FakeSink {
public:
    explicit FakeSink(int sleep_ms = 0, bool retain = false)
        : sleep_ms_(sleep_ms)
        , retain_(retain) {}

    SinkStatus on_frame(const FrameView *view) {
        {
            std::lock_guard lock(mutex_);
            ++frames_;
            last_sequence_ = view->sequence;
            last_width_ = view->width;
            last_height_ = view->height;
            last_stride_ = view->stride;
            last_format_ = view->format;
        }
        if (retain_ && view->owner != nullptr && view->retain != nullptr) {
            view->retain(view->owner);
            retained_ = *view;
        }
        cv_.notify_all();
        if (sleep_ms_ > 0) std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms_));
        return SinkStatus::Accepted;
    }

    SinkStatus on_state(const FrameState *state) {
        {
            std::lock_guard lock(mutex_);
            ++states_;
            last_state_ = *state;
        }
        cv_.notify_all();
        return SinkStatus::Accepted;
    }

    SinkStatus on_error(int32_t code, std::string) {
        {
            std::lock_guard lock(mutex_);
            ++errors_;
            last_error_ = code;
        }
        cv_.notify_all();
        return SinkStatus::Accepted;
    }

    bool wait_for_frames(int count, std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, timeout, [this, count] { return frames_ >= count; });
    }

    int frames() const {
        std::lock_guard lock(mutex_);
        return frames_;
    }
    uint64_t last_sequence() const {
        std::lock_guard lock(mutex_);
        return last_sequence_;
    }

    FrameView retained() const {
        std::lock_guard lock(mutex_);
        return retained_;
    }

    int states() const {
        std::lock_guard lock(mutex_);
        return states_;
    }
    int errors() const {
        std::lock_guard lock(mutex_);
        return errors_;
    }
    int last_error() const {
        std::lock_guard lock(mutex_);
        return last_error_;
    }

private:
    int sleep_ms_ = 0;
    bool retain_ = false;
    FrameView retained_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    int frames_ = 0;
    int states_ = 0;
    int errors_ = 0;
    int last_error_ = 0;
    FrameState last_state_;
    uint64_t last_sequence_ = 0;
    int32_t last_width_ = 0;
    int32_t last_height_ = 0;
    int32_t last_stride_ = 0;
    uint32_t last_format_ = 0;
};

const InterfaceMethod kSinkMethods[] = {
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(FrameSinkTraits, on_frame, &FakeSink::on_frame),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(FrameSinkTraits, on_state, &FakeSink::on_state),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(FrameSinkTraits, on_error, &FakeSink::on_error),
};

constexpr TypeId kFakeSinkType = skl::abix::model::canonical_type_id("test::FakeFrameSink");

FrameSink make_sink(InterfaceRegistry<2> &registry, FakeSink &provider) {
    const auto status = registry.add({FrameSinkTraits::id, kFakeSinkType, kSinkMethods,
        static_cast<uint32_t>(std::size(kSinkMethods)), FrameSinkTraits::version, 0});
    assert(status == InterfaceStatus::ok);
    FrameSink sink(registry, &provider, kFakeSinkType);
    assert(sink.valid());
    return sink;
}

cv::Mat make_frame(int value) { return cv::Mat(4, 5, CV_8UC3, cv::Scalar(value, value, value)); }

}   // namespace

int main() {
    // LatestOnly coalesces a burst while the sink is busy.
    {
        InterfaceRegistry<2> registry;
        FakeSink provider(30);
        auto sink = make_sink(registry, provider);
        FrameBus bus;
        SinkPolicy policy;
        policy.overflow = SinkOverflow::LatestOnly;
        auto id = bus.attach(std::move(sink), policy);
        assert(id.has_value());
        for (int i = 0; i < 4; ++i)
            bus.publish(make_frame(i));
        assert(provider.wait_for_frames(1, std::chrono::milliseconds(500)));
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        const auto stats = bus.stats(id.value());
        assert(stats.has_value());
        assert(stats->received == 4);
        assert(stats->delivered >= 1);
        assert(stats->dropped >= 1);
        assert(provider.last_sequence() == 4);
        bus.detach(id.value());
        bus.close();
    }

    // DropOldest keeps the newest frames in a bounded queue.
    {
        InterfaceRegistry<2> registry;
        FakeSink provider(20);
        auto sink = make_sink(registry, provider);
        FrameBus bus;
        SinkPolicy policy;
        policy.overflow = SinkOverflow::DropOldest;
        policy.capacity = 1;
        auto id = bus.attach(std::move(sink), policy);
        assert(id.has_value());
        for (int i = 0; i < 4; ++i)
            bus.publish(make_frame(i));
        assert(provider.wait_for_frames(1, std::chrono::milliseconds(500)));
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        const auto stats = bus.stats(id.value());
        assert(stats.has_value());
        assert(stats->received == 4);
        assert(stats->dropped >= 1);
        bus.close();
    }

    // A sink may retain a frame beyond the callback and release it later.
    {
        InterfaceRegistry<2> registry;
        FakeSink provider(0, true);
        auto sink = make_sink(registry, provider);
        FrameBus bus;
        SinkPolicy policy;
        policy.overflow = SinkOverflow::LatestOnly;
        auto id = bus.attach(std::move(sink), policy);
        assert(id.has_value());
        bus.publish(make_frame(7));
        assert(provider.wait_for_frames(1, std::chrono::milliseconds(500)));
        const auto retained = provider.retained();
        assert(retained.data != nullptr);
        assert(retained.owner != nullptr);
        bus.close();
        // The retained handle keeps the frame alive after the bus is closed.
        assert(retained.data[0] == 7);
        retained.release(retained.owner);
    }

    // Control events are delivered on the sink worker, preserving the same
    // thread model as frames.
    {
        InterfaceRegistry<2> registry;
        FakeSink provider;
        auto sink = make_sink(registry, provider);
        FrameBus bus;
        auto id = bus.attach(std::move(sink), {});
        assert(id.has_value());
        bus.publish_state(640, 480);
        bus.publish_error(9, "capture failed");
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        assert(provider.states() == 1);
        assert(provider.errors() == 1);
        assert(provider.last_error() == 9);
        bus.close();
    }

    // Two fast sinks can share one dispatcher; both still receive the frame.
    {
        InterfaceRegistry<2> registry_a;
        InterfaceRegistry<2> registry_b;
        FakeSink provider_a;
        FakeSink provider_b;
        auto sink_a = make_sink(registry_a, provider_a);
        auto sink_b = make_sink(registry_b, provider_b);
        FrameBus bus;
        SinkPolicy policy;
        policy.execution = SinkExecution::SharedDispatcher;
        auto id_a = bus.attach(std::move(sink_a), policy);
        auto id_b = bus.attach(std::move(sink_b), policy);
        assert(id_a.has_value() && id_b.has_value());
        bus.publish(make_frame(3));
        assert(provider_a.wait_for_frames(1, std::chrono::milliseconds(500)));
        assert(provider_b.wait_for_frames(1, std::chrono::milliseconds(500)));
        assert(provider_a.frames() == 1);
        assert(provider_b.frames() == 1);
        bus.detach(id_a.value());
        bus.detach(id_b.value());
        bus.close();
    }

    // A sink that did not register the contract fails attach explicitly.
    {
        InterfaceRegistry<2> registry;
        FakeSink provider;
        const auto status = registry.add({FrameSinkTraits::id, kFakeSinkType, kSinkMethods,
            static_cast<uint32_t>(std::size(kSinkMethods)), FrameSinkTraits::version, 0});
        assert(status == InterfaceStatus::ok);
        constexpr TypeId unknown_type = skl::abix::model::canonical_type_id("test::UnknownFrameSink");
        FrameSink missing(registry, &provider, unknown_type);
        assert(!missing.valid());
        FrameBus bus;
        assert(!bus.attach(std::move(missing), {}).has_value());
    }

    std::puts("frame bus: ok");
    return 0;
}
