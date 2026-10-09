#include "reactor.h"

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>

namespace skl::wcas::proxy {

Reactor::Reactor() {
    epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
    notify_fd_ = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (epoll_fd_ >= 0 && notify_fd_ >= 0) {
        epoll_event event{};
        event.events = EPOLLIN;
        event.data.u64 = key_notify;
        if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, notify_fd_, &event) != 0) {
            ::close(notify_fd_);
            notify_fd_ = -1;
        }
    }
}

Reactor::~Reactor() {
    if (timer_fd_ >= 0) ::close(timer_fd_);
    if (notify_fd_ >= 0) ::close(notify_fd_);
    if (epoll_fd_ >= 0) ::close(epoll_fd_);
}

bool Reactor::add_fd(std::uint64_t key, int fd) {
    if (epoll_fd_ < 0 || fd < 0) return false;
    epoll_event event{};
    event.events = EPOLLIN;
    event.data.u64 = key;
    return ::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &event) == 0;
}

bool Reactor::add_fd_oneshot(std::uint64_t key, int fd) {
    if (epoll_fd_ < 0 || fd < 0) return false;
    epoll_event event{};
    event.events = EPOLLIN | EPOLLONESHOT;
    event.data.u64 = key;
    return ::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &event) == 0;
}

bool Reactor::remove_fd(int fd) {
    if (epoll_fd_ < 0 || fd < 0) return false;
    return ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr) == 0;
}

bool Reactor::start_timer(int interval_ms) {
    if (epoll_fd_ < 0 || interval_ms <= 0) return false;
    if (timer_fd_ < 0) {
        timer_fd_ = ::timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
        if (timer_fd_ < 0) return false;
        epoll_event event{};
        event.events = EPOLLIN;
        event.data.u64 = key_capture_timer;
        if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, timer_fd_, &event) != 0) {
            ::close(timer_fd_);
            timer_fd_ = -1;
            return false;
        }
    }
    itimerspec spec{};
    spec.it_interval.tv_nsec = static_cast<long>(interval_ms) * 1'000'000L;
    spec.it_value.tv_nsec = static_cast<long>(interval_ms) * 1'000'000L;
    return ::timerfd_settime(timer_fd_, 0, &spec, nullptr) == 0;
}

int Reactor::wait(std::vector<ReactorEvent> &events, int timeout_ms) {
    if (epoll_fd_ < 0) return -1;
    events.clear();
    epoll_event raw[64];
    const int ready = ::epoll_wait(epoll_fd_, raw, 64, timeout_ms);
    if (ready < 0) return errno == EINTR ? 0 : -1;
    for (int i = 0; i < ready; ++i) {
        // Drain the notify/timer fds so they do not stay permanently readable.
        if (raw[i].data.u64 == key_notify) {
            std::uint64_t value = 0;
            while (::read(notify_fd_, &value, sizeof(value)) > 0) {}
        } else if (raw[i].data.u64 == key_capture_timer) {
            std::uint64_t expirations = 0;
            while (::read(timer_fd_, &expirations, sizeof(expirations)) > 0) {}
        }
        events.push_back(ReactorEvent{raw[i].data.u64});
    }
    return ready;
}

void Reactor::notify() {
    if (notify_fd_ < 0) return;
    const std::uint64_t value = 1;
    const auto ignored = ::write(notify_fd_, &value, sizeof(value));
    (void)ignored;
}

}   // namespace skl::wcas::proxy
