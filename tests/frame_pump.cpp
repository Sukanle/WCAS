// End-to-end check for the background frame pump: launch an app, attach a
// FrameSink, start the pump, and verify frames reach the sink.
#include "SKL/WCAS/WCAS.h"

#include <assert.h>
#include <chrono>
#include <condition_variable>
#include <stdio.h>
#include <stdlib.h>
#include <iterator>
#include <mutex>
#include <string>

namespace {

using namespace skl::wcas;
using skl::abix::model::TypeId;
using skl::abix::poly::InterfaceMethod;
using skl::abix::poly::InterfaceRegistry;
using skl::abix::poly::InterfaceStatus;

class CountingSink {
public:
    SinkStatus on_frame(const FrameView *view) {
        {
            std::lock_guard lock(mutex_);
            ++frames_;
            last_sequence_ = view->sequence;
            last_width_ = view->width;
            last_height_ = view->height;
        }
        cv_.notify_all();
        return SinkStatus::Accepted;
    }
    SinkStatus on_state(const FrameState *) { return SinkStatus::Accepted; }
    SinkStatus on_error(int32_t, std::string) { return SinkStatus::Accepted; }

    bool wait_for_frame(std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, timeout, [this] { return frames_ > 0; });
    }

    int frames() const {
        std::lock_guard lock(mutex_);
        return frames_;
    }
    int width() const {
        std::lock_guard lock(mutex_);
        return last_width_;
    }
    int height() const {
        std::lock_guard lock(mutex_);
        return last_height_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    int frames_ = 0;
    uint64_t last_sequence_ = 0;
    int last_width_ = 0;
    int last_height_ = 0;
};

const InterfaceMethod kSinkMethods[] = {
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(FrameSinkTraits, on_frame, &CountingSink::on_frame),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(FrameSinkTraits, on_state, &CountingSink::on_state),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(FrameSinkTraits, on_error, &CountingSink::on_error),
};

constexpr TypeId kSinkType = skl::abix::model::canonical_type_id("test::CountingFrameSink");

}   // namespace

int main() {
    if (std::getenv("WAYLAND_DISPLAY") == nullptr && std::getenv("WAYLAND_SOCKET") == nullptr) {
        std::printf("frame pump: no Wayland display, skipped\n");
        return 0;
    }

    auto use = ComputerUse::builder()
                   .native_timeout(std::chrono::seconds(1))
                   .fallback_timeout(std::chrono::seconds(20))
                   .content_timeout(std::chrono::seconds(15))
                   .build();
    if (!use) {
        std::printf("frame pump: harness unavailable, skipped\n");
        return 0;
    }

    LaunchConfig config("xmessage");
    config.arg("-timeout").arg("20").arg("wcas frame pump");
    auto app = use.value().launch(config);
    if (!app) {
        std::printf("frame pump: launch failed (environment limitation), skipped\n");
        return 0;
    }

    InterfaceRegistry<4> registry;
    CountingSink provider;
    const auto status = registry.add({FrameSinkTraits::id, kSinkType, kSinkMethods,
        static_cast<uint32_t>(std::size(kSinkMethods)), FrameSinkTraits::version, 0});
    assert(status == InterfaceStatus::ok);
    FrameSink sink(registry, &provider, kSinkType);
    assert(sink.valid());

    auto sink_id = app.value().attach_frame_sink(std::move(sink), SinkPolicy{});
    assert(sink_id.has_value());
    auto started = app.value().start_frame_pump(std::chrono::milliseconds{16});
    assert(started.has_value());
    assert(provider.wait_for_frame(std::chrono::seconds(5)));
    assert(provider.frames() > 0);
    assert(provider.width() > 0);
    assert(provider.height() > 0);

    const auto stats = app.value().frame_sink_stats(sink_id.value());
    assert(stats.has_value());
    assert(stats->received > 0);

    app.value().stop_frame_pump();
    app.value().detach_frame_sink(sink_id.value());
    app.value().stop();

    std::puts("frame pump: ok");
    return 0;
}
