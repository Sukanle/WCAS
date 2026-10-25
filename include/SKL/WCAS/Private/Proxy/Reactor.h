#pragma once

#include <stdint.h>

#include <vector>

namespace skl::wcas::proxy {

/// Event keys returned by the reactor. The `sync_file` keys are derived as
/// `key_sync_file_base + slot_index`.
enum : uint64_t {
    key_server = 1,
    key_upstream = 2,
    key_notify = 3,
    key_capture_timer = 4,
    key_sync_file_base = 100,
};

struct ReactorEvent {
    uint64_t key = 0;
};

/// Thin epoll wrapper used by the proxy event loop. It multiplexes the two
/// Wayland sockets, a periodic capture timer and one-shot DMA-BUF sync fds,
/// and can be woken from another thread (input injection).
class Reactor {
public:
    Reactor();
    ~Reactor();
    Reactor(const Reactor &) = delete;
    Reactor &operator=(const Reactor &) = delete;

    [[nodiscard]] bool valid() const { return epoll_fd_ >= 0; }

    bool add_fd(uint64_t key, int fd);
    /// Single-shot registration: the fd is disarmed after the first event.
    bool add_fd_oneshot(uint64_t key, int fd);
    bool remove_fd(int fd);
    /// Register a periodic timer firing `key_capture_timer` every
    /// `interval_ms`. Returns false when timerfd is unavailable.
    bool start_timer(int interval_ms);

    /// Wait for events. `timeout_ms < 0` blocks forever. Returns the number of
    /// events (0 on timeout) or -1 on error.
    int wait(std::vector<ReactorEvent> &events, int timeout_ms);
    void notify();

private:
    int epoll_fd_ = -1;
    int notify_fd_ = -1;
    int timer_fd_ = -1;
};

}   // namespace skl::wcas::proxy
