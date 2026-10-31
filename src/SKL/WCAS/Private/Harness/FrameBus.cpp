#include <stdint.h>

#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <utility>
#include <optional>
#include <functional>   //TODO: boost function
#include <unordered_map>
#include <condition_variable>

#include "SKL/WCAS/Error.h"
#include "SKL/WCAS/Private/Harness/FrameBus.h"

namespace skl::wcas {
namespace {

skl::epcd::ErrorCode code(errc value) { return make_error_code(value); }

struct Frame {
    cv::Mat image;
    uint64_t sequence = 0;
    int64_t timestamp_ns = 0;
};

struct FrameHandle {
    std::shared_ptr<const Frame> frame;
    std::atomic<uint32_t> refs{1};
};

class FramePool : public std::enable_shared_from_this<FramePool> {
public:
    explicit FramePool(size_t capacity)
        : capacity_(capacity) {
        free_.reserve(capacity);
        for (size_t i = 0; i < capacity; ++i)
            free_.push_back(std::make_unique<Frame>());
    }

    std::shared_ptr<Frame> acquire() {
        std::unique_ptr<Frame> frame;
        {
            std::lock_guard lock(mutex_);
            if (free_.empty()) {
                ++misses_;
                return nullptr;
            }
            frame = std::move(free_.back());
            free_.pop_back();
        }
        auto self = shared_from_this();
        return std::shared_ptr<Frame>(
            frame.release(), [self = std::move(self)](Frame *value) { self->release(value); });
    }

    size_t misses() const {
        std::lock_guard lock(mutex_);
        return misses_;
    }

private:
    void release(Frame *frame) {
        if (frame == nullptr) return;
        frame->image.release();
        frame->sequence = 0;
        frame->timestamp_ns = 0;
        std::lock_guard lock(mutex_);
        if (free_.size() < capacity_)
            free_.emplace_back(frame);
        else
            delete frame;
    }

    size_t capacity_ = 0;
    mutable std::mutex mutex_;
    std::vector<std::unique_ptr<Frame>> free_;
    size_t misses_ = 0;
};

class SharedDispatcher {
public:
    SharedDispatcher() {
        worker_ = std::thread([this] { run(); });
    }
    ~SharedDispatcher() { stop(); }

    SharedDispatcher(const SharedDispatcher &) = delete;
    SharedDispatcher &operator=(const SharedDispatcher &) = delete;

    void post(std::function<void()> task) {
        std::lock_guard lock(mutex_);
        if (stopping_) return;
        tasks_.push_back(std::move(task));
        cv_.notify_one();
    }

    void stop() {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
            tasks_.clear();
        }
        cv_.notify_all();
        if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id()) worker_.join();
    }

private:
    void run() {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock lock(mutex_);
                cv_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
                if (stopping_) break;
                task = std::move(tasks_.front());
                tasks_.pop_front();
            }
            try {
                task();
            } catch (...) {
                // A sink callback is a noexcept function pointer in normal use.
                // Keep the shared dispatcher alive if a wrapper still throws.
            }
        }
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> tasks_;
    bool stopping_ = false;
    std::thread worker_;
};

void frame_retain(void *owner) {
    if (owner == nullptr) return;
    static_cast<FrameHandle *>(owner)->refs.fetch_add(1, std::memory_order_relaxed);
}

void frame_release(void *owner) {
    if (owner == nullptr) return;
    auto *handle = static_cast<FrameHandle *>(owner);
    if (handle->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) delete handle;
}

FrameView make_view(FrameHandle *handle) {
    FrameView view;
    if (handle == nullptr || !handle->frame) return view;
    const auto &frame = *handle->frame;
    view.data = frame.image.data;
    view.width = frame.image.cols;
    view.height = frame.image.rows;
    view.stride = static_cast<int32_t>(frame.image.step[0]);
    view.format = static_cast<uint32_t>(FrameFormat::Bgr8);
    view.sequence = frame.sequence;
    view.timestamp_ns = frame.timestamp_ns;
    view.owner = handle;
    view.retain = &frame_retain;
    view.release = &frame_release;
    return view;
}

// One independent runtime per sink. The worker owns the sink callback; the
// capture thread only enqueues a reference-counted frame handle.
struct SinkRuntime : std::enable_shared_from_this<SinkRuntime> {
    SinkRuntime(uint64_t id, FrameSink sink, SinkPolicy policy)
        : id(id)
        , sink(std::move(sink))
        , policy(policy) {}

    ~SinkRuntime() { stop(); }

    SinkRuntime(const SinkRuntime &) = delete;
    SinkRuntime &operator=(const SinkRuntime &) = delete;

    void start(SharedDispatcher *dispatcher = nullptr) {
        if (dispatcher != nullptr && policy.execution == SinkExecution::SharedDispatcher) {
            shared_ = dispatcher;
            return;
        }
        auto self = shared_from_this();
        worker = std::thread([self = std::move(self)] { self->run(); });
    }

    struct ErrorEvent {
        int32_t code = 0;
        std::string message;
    };

    [[nodiscard]] bool has_work_locked() const {
        return latest != nullptr || !queue.empty() || !errors.empty() || pending_state.has_value();
    }

    // Called while holding mutex. Dedicated workers wake on cv; shared
    // dispatchers get one drain task per busy period.
    void wake_locked() {
        if (shared_ != nullptr) {
            if (scheduled_) return;
            scheduled_ = true;
            auto self = shared_from_this();
            shared_->post([self = std::move(self)] { self->drain_shared(); });
            return;
        }
        cv.notify_one();
    }

    void enqueue_error(int32_t error_code, const std::string &message) {
        std::lock_guard lock(mutex);
        if (stopping) return;
        if (errors.size() >= 8) errors.pop_front();
        errors.push_back(ErrorEvent{error_code, message});
        wake_locked();
    }

    void enqueue_state(const FrameState &state) {
        std::lock_guard lock(mutex);
        if (stopping) return;
        pending_state = state;
        wake_locked();
    }

    void enqueue(FrameHandle *handle) {
        std::unique_lock lock(mutex);
        if (stopping) return;
        ++stats.received;

        if (policy.overflow == SinkOverflow::LatestOnly) {
            if (latest != nullptr) {
                frame_release(latest);
                ++stats.dropped;
            }
            frame_retain(handle);
            latest = handle;
            wake_locked();
            return;
        }

        const auto capacity = policy.capacity == 0 ? 1 : policy.capacity;
        if (queue.size() >= capacity) {
            switch (policy.overflow) {
                case SinkOverflow::DropOldest:
                    frame_release(queue.front());
                    queue.pop_front();
                    ++stats.dropped;
                    break;
                case SinkOverflow::DropNewest:       ++stats.dropped; return;
                case SinkOverflow::BlockWithTimeout: {
                    const auto timeout = std::chrono::milliseconds(policy.timeout_ms);
                    const bool space =
                        cv.wait_for(lock, timeout, [this, capacity] { return stopping || queue.size() < capacity; });
                    if (!space || stopping) {
                        ++stats.dropped;
                        return;
                    }
                    break;
                }
                case SinkOverflow::LatestOnly: break;
            }
        }

        frame_retain(handle);
        queue.push_back(handle);
        wake_locked();
    }

    struct Event {
        enum class Kind {
            Error,
            State,
            Frame
        } kind = Kind::Frame;
        ErrorEvent error;
        FrameState state;
        FrameHandle *handle = nullptr;
    };

    // Process exactly one queued event. For a dedicated worker `wait` blocks;
    // for a shared drain task it returns false as soon as the mailbox is empty.
    bool process_one(bool wait) {
        Event event;
        {
            std::unique_lock lock(mutex);
            if (wait) cv.wait(lock, [this] { return stopping || has_work_locked(); });
            if (stopping) return false;
            if (!has_work_locked()) {
                if (shared_ != nullptr) scheduled_ = false;
                return false;
            }
            if (!errors.empty()) {
                event.kind = Event::Kind::Error;
                event.error = std::move(errors.front());
                errors.pop_front();
            } else if (pending_state.has_value()) {
                event.kind = Event::Kind::State;
                event.state = *pending_state;
                pending_state.reset();
            } else if (latest != nullptr) {
                event.kind = Event::Kind::Frame;
                event.handle = latest;
                latest = nullptr;
            } else {
                event.kind = Event::Kind::Frame;
                event.handle = queue.front();
                queue.pop_front();
            }
        }

        SinkStatus status = SinkStatus::Accepted;
        if (event.kind == Event::Kind::Error) {
            status = sink.on_error(event.error.code, std::move(event.error.message));
        } else if (event.kind == Event::Kind::State) {
            status = sink.on_state(&event.state);
        } else {
            const FrameView view = make_view(event.handle);
            status = sink.on_frame(&view);
            frame_release(event.handle);
        }

        {
            std::lock_guard lock(mutex);
            if (event.kind == Event::Kind::Frame) ++stats.delivered;
            if (status == SinkStatus::Dropped) ++stats.dropped;
            if (status == SinkStatus::Busy) ++stats.busy;
            if (status == SinkStatus::Stop) {
                stopping = true;
                cv.notify_all();
                return false;
            }
        }
        return true;
    }

    void run() {
        while (process_one(true)) {}
    }

    void drain_shared() {
        while (process_one(false)) {}
    }

    void stop() {
        {
            std::lock_guard lock(mutex);
            if (stopping) {
                // The worker may have set stopping itself (SinkStatus::Stop).
                // Still join below if this is not the worker thread.
            } else {
                stopping = true;
            }
            if (latest != nullptr) {
                frame_release(latest);
                latest = nullptr;
            }
            for (auto *handle : queue)
                frame_release(handle);
            queue.clear();
            errors.clear();
            pending_state.reset();
        }
        cv.notify_all();
        if (worker.joinable()) {
            if (worker.get_id() == std::this_thread::get_id())
                worker.detach();
            else
                worker.join();
        }
    }

    SinkStats snapshot() const {
        std::lock_guard lock(mutex);
        SinkStats out = stats;
        out.queue_depth = static_cast<uint32_t>(queue.size() + (latest != nullptr ? 1 : 0));
        return out;
    }

    uint64_t id;
    FrameSink sink;
    SinkPolicy policy;
    mutable std::mutex mutex;
    std::condition_variable cv;
    std::deque<FrameHandle *> queue;
    std::deque<ErrorEvent> errors;
    std::optional<FrameState> pending_state;
    FrameHandle *latest = nullptr;
    bool stopping = false;
    SharedDispatcher *shared_ = nullptr;
    bool scheduled_ = false;
    std::thread worker;
    SinkStats stats;
};

}   // namespace

struct FrameBus::Impl {
    explicit Impl(size_t frame_pool_capacity)
        : pool(std::make_shared<FramePool>(frame_pool_capacity)) {}

    std::shared_ptr<FramePool> pool;
    std::unique_ptr<SharedDispatcher> shared;
    mutable std::mutex mutex;
    std::unordered_map<uint64_t, std::shared_ptr<SinkRuntime>> sinks;
    uint64_t next_id = 1;
    uint64_t sequence = 0;
    bool closed = false;
};

FrameBus::FrameBus(size_t frame_pool_capacity)
    : _impl(std::make_unique<Impl>(frame_pool_capacity)) {}
FrameBus::~FrameBus() { close(); }

result<uint64_t> FrameBus::attach(FrameSink sink, SinkPolicy policy) {
    if (!sink.valid()) return code(errc::feature_not_implemented);
    if (!_impl) return code(errc::unspecified);
    auto runtime = std::make_shared<SinkRuntime>(_impl->next_id++, std::move(sink), policy);
    std::lock_guard lock(_impl->mutex);
    if (_impl->closed) return code(errc::unspecified);
    SharedDispatcher *dispatcher = nullptr;
    if (policy.execution == SinkExecution::SharedDispatcher) {
        if (!_impl->shared) _impl->shared = std::make_unique<SharedDispatcher>();
        dispatcher = _impl->shared.get();
    }
    const uint64_t id = runtime->id;
    runtime->start(dispatcher);
    _impl->sinks.emplace(id, std::move(runtime));
    return id;
}

void FrameBus::detach(uint64_t id) {
    if (!_impl) return;
    std::shared_ptr<SinkRuntime> runtime;
    {
        std::lock_guard lock(_impl->mutex);
        const auto it = _impl->sinks.find(id);
        if (it == _impl->sinks.end()) return;
        runtime = std::move(it->second);
        _impl->sinks.erase(it);
    }
    if (runtime) runtime->stop();
}

void FrameBus::publish(const cv::Mat &image) {
    if (!_impl || image.empty()) return;
    std::vector<std::shared_ptr<SinkRuntime>> targets;
    uint64_t sequence = 0;
    {
        std::lock_guard lock(_impl->mutex);
        if (_impl->closed || _impl->sinks.empty()) return;
        sequence = ++_impl->sequence;
        targets.reserve(_impl->sinks.size());
        for (const auto &[id, runtime] : _impl->sinks) {
            (void)id;
            targets.push_back(runtime);
        }
    }

    auto frame = _impl->pool->acquire();
    if (!frame) frame = std::make_shared<Frame>();   // pool exhausted: bounded fallback
    frame->image = image;                            // Mat copy is a refcount bump, not a pixel copy.
    frame->sequence = sequence;
    frame->timestamp_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count();

    auto *handle = new FrameHandle{std::move(frame), 1};
    for (auto &runtime : targets)
        runtime->enqueue(handle);
    frame_release(handle);
}

void FrameBus::publish_state(int32_t width, int32_t height) {
    if (!_impl || width <= 0 || height <= 0) return;
    std::vector<std::shared_ptr<SinkRuntime>> targets;
    FrameState state;
    {
        std::lock_guard lock(_impl->mutex);
        if (_impl->closed || _impl->sinks.empty()) return;
        state.sequence = _impl->sequence;
        state.width = width;
        state.height = height;
        targets.reserve(_impl->sinks.size());
        for (const auto &[id, runtime] : _impl->sinks) {
            (void)id;
            targets.push_back(runtime);
        }
    }
    for (auto &runtime : targets)
        runtime->enqueue_state(state);
}

void FrameBus::publish_error(int32_t error_code, std::string message) {
    if (!_impl) return;
    std::vector<std::shared_ptr<SinkRuntime>> targets;
    {
        std::lock_guard lock(_impl->mutex);
        if (_impl->closed || _impl->sinks.empty()) return;
        targets.reserve(_impl->sinks.size());
        for (const auto &[id, runtime] : _impl->sinks) {
            (void)id;
            targets.push_back(runtime);
        }
    }
    for (auto &runtime : targets)
        runtime->enqueue_error(error_code, message);
}

void FrameBus::close() {
    if (!_impl) return;
    std::unordered_map<uint64_t, std::shared_ptr<SinkRuntime>> sinks;
    {
        std::lock_guard lock(_impl->mutex);
        if (_impl->closed) return;
        _impl->closed = true;
        sinks.swap(_impl->sinks);
    }
    for (auto &[id, runtime] : sinks) {
        (void)id;
        if (runtime) runtime->stop();
    }
    _impl->shared.reset();
}

optional<SinkStats> FrameBus::stats(uint64_t id) const {
    if (!_impl) return std::nullopt;
    std::lock_guard lock(_impl->mutex);
    const auto it = _impl->sinks.find(id);
    if (it == _impl->sinks.end() || !it->second) return std::nullopt;
    return it->second->snapshot();
}

}   // namespace skl::wcas
