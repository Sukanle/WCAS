#include "skl/wcas/dmabuf_probe.h"

#include <liburing.h>

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <fmt/format.h>

namespace skl::wcas {
namespace {

// From <linux/dma-buf.h>.
constexpr std::uint64_t dma_buf_sync_read = 1u << 0;
constexpr std::uint64_t dma_buf_sync_write = 2u << 0;
constexpr std::uint64_t dma_buf_sync_start = 0u << 2;
constexpr std::uint64_t dma_buf_sync_end = 1u << 2;

struct DmaBufSync {
    std::uint64_t flags;
};
struct DmaBufExportSyncFile {
    std::uint32_t flags;
    std::int32_t fd;
};

constexpr unsigned long dma_buf_ioctl_export_sync_file =
    (3ul << 30) | (sizeof(DmaBufExportSyncFile) << 16) | (static_cast<unsigned long>('b') << 8) | 2ul;
constexpr unsigned long dma_buf_ioctl_sync =
    (1ul << 30) | (sizeof(DmaBufSync) << 16) | (static_cast<unsigned long>('b') << 8) | 0ul;

using Clock = std::chrono::steady_clock;

ProbeResult make_result(std::string name, bool passed, std::string detail, Clock::time_point start) {
    ProbeResult result;
    result.name = std::move(name);
    result.passed = passed;
    result.detail = std::move(detail);
    result.elapsed = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start);
    return result;
}

bool starts_with(const std::string &value, const char *prefix) { return value.rfind(prefix, 0) == 0; }

std::mutex g_tracked_mutex;
std::vector<int> g_tracked_fds;

}   // namespace

std::string ProbeResults::report() const {
    fmt::memory_buffer out;

    fmt::format_to(fmt::appender(out), "═══ DMA-BUF 同步能力测试报告 ═══\n");

    // 逐行缩进 basic_info
    std::istringstream info(basic_info);
    std::string line;
    while (std::getline(info, line)) {
        fmt::format_to(fmt::appender(out), "  {}\n", line);
    }
    fmt::format_to(fmt::appender(out), "\n");

    std::size_t passed = 0;
    for (const auto &result : results) {
        fmt::format_to(fmt::appender(out),
            "  {} {}: {}\n"
            "     耗时: {}µs\n",
            result.passed ? "✔" : "✘", result.name, result.detail, result.elapsed.count());
        if (result.passed) ++passed;
    }

    fmt::format_to(fmt::appender(out), "\n  合计: {}/{} 通过\n", passed, results.size());

    return fmt::to_string(out);
}

void track_dmabuf_fd(int fd) {
    std::scoped_lock lock(g_tracked_mutex);
    for (const int tracked : g_tracked_fds)
        if (tracked == fd) return;
    g_tracked_fds.push_back(fd);
}

std::vector<int> tracked_dmabuf_fds() {
    std::scoped_lock lock(g_tracked_mutex);
    return g_tracked_fds;
}

void clear_tracked_dmabuf_fds() {
    std::scoped_lock lock(g_tracked_mutex);
    g_tracked_fds.clear();
}

std::string DmaBufSyncProbe::basic_info() const {
    fmt::memory_buffer info;
    std::ifstream fdinfo("/proc/self/fdinfo/" + std::to_string(fd_));
    if (fdinfo) {
        std::string line;
        bool found = false;
        while (std::getline(fdinfo, line)) {
            if (starts_with(line, "exp_name:") || starts_with(line, "size:")) {
                fmt::format_to(fmt::appender(info), "  {}\n", line);
                found = true;
            }
        }
        if (!found) fmt::format_to(fmt::appender(info), "  fdinfo: 无 DMA-BUF 字段\n");
    } else {
        fmt::format_to(fmt::appender(info), "  fdinfo: 不可用\n");
    }

    char target[512]{};
    const auto link = std::string("/proc/self/fd/") + std::to_string(fd_);
    const auto length = ::readlink(link.c_str(), target, sizeof(target) - 1);
    if (length > 0) {
        fmt::format_to(fmt::appender(info), "  target: {}\n", target);
    }
    return fmt::to_string(info);
}

ProbeResult DmaBufSyncProbe::validate_fd() const {
    const auto start = Clock::now();
    const std::string name = "FD 有效性验证";

    const int fcntl_result = ::fcntl(fd_, F_GETFD);
    if (fcntl_result < 0)
        return make_result(
            name, false, fmt::format("fcntl(F_GETFD) 失败: {} — FD={} 在当前进程不可用", strerror(errno), fd_), start);

    const char *cloexec = (fcntl_result & FD_CLOEXEC) != 0 ? "FD_CLOEXEC" : "0";

    struct stat info{};
    if (::fstat(fd_, &info) < 0) return make_result(name, false, fmt::format("fstat 失败: {}", strerror(errno)), start);

    const auto detail = fmt::format("fcntl(F_GETFD)={} ({}), fstat: mode={:o} size={} dev={} ino={}", fcntl_result,
        cloexec, info.st_mode, info.st_size, info.st_dev, info.st_ino);
    return make_result(name, true, detail, start);
}

ProbeResult DmaBufSyncProbe::test_sync_ioctl() const {
    const auto start = Clock::now();
    const std::string name = "DMA_BUF_IOCTL_SYNC (CPU sync)";

    DmaBufSync sync{dma_buf_sync_read | dma_buf_sync_start};
    if (::ioctl(fd_, dma_buf_ioctl_sync, &sync) != 0)
        return make_result(name, false, fmt::format("不支持: {}", strerror(errno)), start);

    sync.flags = dma_buf_sync_read | dma_buf_sync_end;

    if (::ioctl(fd_, dma_buf_ioctl_sync, &sync) != 0)
        return make_result(name, false, fmt::format("START 成功但 END 失败: ", strerror(errno)), start);
    return make_result(name, true, "支持 (START + END)", start);
}

ProbeResult DmaBufSyncProbe::test_export_sync_file(std::uint32_t flags, const char *label) const {
    const auto start = Clock::now();
    const std::string name = fmt::format("EXPORT_SYNC_FILE ({})", label);

    DmaBufExportSyncFile exported{flags, -1};
    if (::ioctl(fd_, dma_buf_ioctl_export_sync_file, &exported) != 0) {
        const int error = errno;
        const char *hint = error == ENOTTY ? "ENOTTY: exporter 不支持此 ioctl"
                         : error == EINVAL ? "EINVAL: 参数无效"
                                           : strerror(error);
        return make_result(name, false, fmt::format("失败: {} ({})", strerror(error), hint), start);
    }

    const int sync_fd = exported.fd;
    pollfd ready{sync_fd, POLLIN, 0};
    const int polled = ::poll(&ready, 1, 0);
    const char *status = polled > 0 ? "AlreadySignaled" : polled == 0 ? "Active" : "PollError";
    const auto detail = fmt::format("成功 (sync_fd={}, status={})", sync_fd, status);
    ::close(sync_fd);
    return make_result(name, true, fmt::to_string(detail), start);
}

ProbeResult DmaBufSyncProbe::test_poll(std::int16_t events, const char *label, int timeout_ms) const {
    const auto start = Clock::now();
    const auto name = fmt::format("poll({}) timeout={}ms", label, timeout_ms);

    pollfd descriptor{fd_, events, 0};
    const int polled = ::poll(&descriptor, 1, timeout_ms);
    if (polled > 0) {
        std::string flags;
        const auto append = [&flags](const char *flag) {
            if (!flags.empty()) flags += "|";
            flags += flag;
        };
        if (descriptor.revents & POLLIN) append("POLLIN");
        if (descriptor.revents & POLLOUT) append("POLLOUT");
        if (descriptor.revents & POLLERR) append("POLLERR");
        if (descriptor.revents & POLLHUP) append("POLLHUP");
        return make_result(name, true, fmt::format("就绪 (revents={}", flags), start);
    }
    if (polled == 0) return make_result(name, true, fmt::format("超时 (timeout={}ms", timeout_ms), start);
    return make_result(name, false, fmt::format("失败: {}", strerror(errno)), start);
}

ProbeResult DmaBufSyncProbe::test_io_uring_poll(
    std::uint32_t events, const char *label, std::uint64_t timeout_ms) const {
    const auto start = Clock::now();
    const auto name = fmt::format("io_uring POLL_ADD ({})", label);

    io_uring ring{};
    if (::io_uring_queue_init(2, &ring, 0) < 0) {
        return make_result(name, false, "io_uring 创建失败", start);
    }

    io_uring_sqe *sqe = ::io_uring_get_sqe(&ring);
    if (sqe == nullptr) {
        ::io_uring_queue_exit(&ring);
        return make_result(name, false, "io_uring SQE 申请失败", start);
    }
    ::io_uring_prep_poll_add(sqe, fd_, static_cast<short>(events));
    ::io_uring_submit(&ring);

    int completions = 0;
    const auto deadline = start + std::chrono::milliseconds(timeout_ms);
    while (Clock::now() < deadline) {
        io_uring_cqe *cqe = nullptr;
        if (::io_uring_peek_cqe(&ring, &cqe) == 0 && cqe != nullptr) {
            ++completions;
            ::io_uring_cqe_seen(&ring, cqe);
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ::io_uring_queue_exit(&ring);

    if (completions > 0) {
        return make_result(name, true, fmt::format("成功 (收到 {} 个 CQE)", completions), start);
    }
    return make_result(name, true, fmt::format("超时 (timeout={}ms, poll 未就绪)", timeout_ms), start);
}

ProbeResult DmaBufSyncProbe::test_io_uring_multishot(std::uint32_t events, const char *label) const {
    const auto start = Clock::now();
    const auto name = fmt::format("io_uring POLL_ADD | MULTISHOT ({})", label);

    io_uring ring{};
    if (::io_uring_queue_init(4, &ring, 0) < 0) {
        return make_result(name, false, "io_uring 创建失败", start);
    }

    io_uring_sqe *sqe = ::io_uring_get_sqe(&ring);
    if (sqe == nullptr) {
        ::io_uring_queue_exit(&ring);
        return make_result(name, false, "io_uring SQE 申请失败", start);
    }
    ::io_uring_prep_poll_multishot(sqe, fd_, static_cast<short>(events));
    ::io_uring_submit(&ring);

    int completions = 0;
    bool has_more = false;
    const auto deadline = start + std::chrono::milliseconds(100);
    while (Clock::now() < deadline) {
        io_uring_cqe *cqe = nullptr;
        if (::io_uring_peek_cqe(&ring, &cqe) == 0 && cqe != nullptr) {
            ++completions;
            if (cqe->flags & IORING_CQE_F_MORE) has_more = true;
            ::io_uring_cqe_seen(&ring, cqe);
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ::io_uring_queue_exit(&ring);

    if (completions > 0) {
        const auto detail = fmt::format("MULTISHOT {} ({} CQE, {})", (has_more ? "支持" : "不支持"), completions,
            (has_more ? "F_MORE 已设置" : "无 F_MORE"));
        return make_result(name, true, detail, start);
    }
    return make_result(name, true, "超时 (timeout=100ms, poll 未就绪)", start);
}

ProbeResults DmaBufSyncProbe::run_all() const {
    ProbeResults results;
    results.basic_info = basic_info();

    results.results.push_back(validate_fd());
    results.results.push_back(test_sync_ioctl());

    results.results.push_back(test_export_sync_file(static_cast<std::uint32_t>(dma_buf_sync_read), "READ"));
    results.results.push_back(test_export_sync_file(static_cast<std::uint32_t>(dma_buf_sync_write), "WRITE"));
    results.results.push_back(
        test_export_sync_file(static_cast<std::uint32_t>(dma_buf_sync_read | dma_buf_sync_write), "READ|WRITE"));

    for (const int timeout_ms : {0, 10, 100}) {
        results.results.push_back(test_poll(POLLIN, "POLLIN", timeout_ms));
        results.results.push_back(test_poll(POLLOUT, "POLLOUT", timeout_ms));
    }

    results.results.push_back(test_io_uring_poll(POLLIN, "POLLIN", 100));
    results.results.push_back(test_io_uring_poll(POLLOUT, "POLLOUT", 100));
    results.results.push_back(test_io_uring_multishot(POLLIN, "POLLIN"));

    return results;
}

}   // namespace skl::wcas
