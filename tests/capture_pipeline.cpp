// End-to-end coverage for the background capture pipeline: the proxy event
// loop's periodic timer must drive the slot scheduler and publish the latest
// frame into `ConnState::pre_captured_frame`, which `capture()` then returns.

#include "proxy/conn.h"

#include <sys/mman.h>
#include <sys/socket.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>

#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <opencv2/core.hpp>

namespace {

using skl::wcas::proxy::ConnState;
constexpr std::uint32_t drm_argb8888 = 0x34325241u;

void put(std::vector<char> &message, std::uint32_t value) {
    const auto at = message.size();
    message.resize(at + 4);
    std::memcpy(message.data() + at, &value, 4);
}
std::uint32_t get(const std::vector<char> &message, std::size_t at) {
    std::uint32_t value = 0;
    if (at + 4 > message.size()) return 0;
    std::memcpy(&value, message.data() + at, 4);
    return value;
}
void string_arg(std::vector<char> &message, const char *value) {
    const auto length = static_cast<std::uint32_t>(std::strlen(value) + 1);
    put(message, length);
    const auto at = message.size();
    message.resize(at + ((length + 3u) & ~3u), 0);
    std::memcpy(message.data() + at, value, length);
}
std::vector<char> message(std::uint32_t object, std::uint16_t opcode) {
    std::vector<char> result(8);
    std::memcpy(result.data(), &object, 4);
    std::uint32_t header = opcode;
    std::memcpy(result.data() + 4, &header, 4);
    return result;
}
void finish(std::vector<char> &message) {
    const auto size = static_cast<std::uint32_t>(message.size());
    std::uint32_t header = 0;
    std::memcpy(&header, message.data() + 4, 4);
    header = (size << 16) | (header & 0xffffu);
    std::memcpy(message.data() + 4, &header, 4);
}
void send_message(int fd, const std::vector<char> &message, int passed_fd = -1) {
    iovec iov{const_cast<char *>(message.data()), message.size()};
    msghdr output{};
    output.msg_iov = &iov;
    output.msg_iovlen = 1;
    char control[CMSG_SPACE(sizeof(int))]{};
    if (passed_fd >= 0) {
        output.msg_control = control;
        output.msg_controllen = sizeof(control);
        auto *header = CMSG_FIRSTHDR(&output);
        header->cmsg_level = SOL_SOCKET;
        header->cmsg_type = SCM_RIGHTS;
        header->cmsg_len = CMSG_LEN(sizeof(int));
        std::memcpy(CMSG_DATA(header), &passed_fd, sizeof(passed_fd));
    }
    assert(::sendmsg(fd, &output, 0) == static_cast<ssize_t>(message.size()));
}
std::vector<char> receive(int fd) {
    std::vector<char> bytes(8);
    char control[CMSG_SPACE(sizeof(int) * 4)]{};
    iovec iov{bytes.data(), bytes.size()};
    msghdr input{};
    input.msg_iov = &iov;
    input.msg_iovlen = 1;
    input.msg_control = control;
    input.msg_controllen = sizeof(control);
    assert(::recvmsg(fd, &input, MSG_WAITALL) == 8);
    std::uint32_t wire_header = 0;
    std::memcpy(&wire_header, bytes.data() + 4, sizeof(wire_header));
    const auto message_size = static_cast<std::size_t>(wire_header >> 16);
    assert(message_size >= 8 && (message_size & 3u) == 0);
    bytes.resize(message_size);
    if (message_size > 8)
        assert(::recv(fd, bytes.data() + 8, message_size - 8, MSG_WAITALL) == static_cast<ssize_t>(message_size - 8));
    return bytes;
}

int make_linear_dmabuf(int width, int height, std::uint32_t *stride) {
    *stride = static_cast<std::uint32_t>(width) * 4u;
    const std::size_t size = static_cast<std::size_t>(*stride) * static_cast<std::size_t>(height);
    const int fd = ::memfd_create("wcas-pipeline-test", MFD_CLOEXEC);
    assert(fd >= 0);
    assert(::ftruncate(fd, static_cast<off_t>(size)) == 0);
    void *base = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    assert(base != MAP_FAILED);
    for (int y = 0; y < height; ++y) {
        auto *row = static_cast<unsigned char *>(base) + static_cast<std::size_t>(y) * *stride;
        for (int x = 0; x < width; ++x) {
            const int i = y * width + x;
            row[x * 4 + 0] = static_cast<unsigned char>(i);
            row[x * 4 + 1] = static_cast<unsigned char>(i + 10);
            row[x * 4 + 2] = static_cast<unsigned char>(i + 20);
            row[x * 4 + 3] = 255;
        }
    }
    ::munmap(base, size);
    return fd;
}

struct Fake {
    int app[2]{};
    int compositor[2]{};
    std::shared_ptr<ConnState> state = std::make_shared<ConnState>();
    std::thread proxy;

    Fake() {
        assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, app) == 0);
        assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, compositor) == 0);
        proxy = std::thread([this] {
            skl::wcas::proxy::Connection bridge(app[0], compositor[0], state);
            bridge.run();
        });
    }
    ~Fake() {
        ::shutdown(app[1], SHUT_RDWR);
        ::close(app[1]);
        ::close(compositor[1]);
        proxy.join();
    }
    Fake(const Fake &) = delete;
    Fake &operator=(const Fake &) = delete;
};

std::uint32_t bind_global(int app_fd, int compositor_fd, std::uint32_t registry, std::uint32_t global, const char *name,
    std::uint32_t app_object, std::uint32_t version = 1) {
    auto bind = message(registry, 0);
    put(bind, global);
    string_arg(bind, name);
    put(bind, version);
    put(bind, app_object);
    finish(bind);
    send_message(app_fd, bind);
    const auto forwarded = receive(compositor_fd);
    return get(forwarded, forwarded.size() - 4);
}

}   // namespace

int main() {
    Fake fake;
    int app_fd = fake.app[1];
    int compositor_fd = fake.compositor[1];
    fake.state->client_fd = app_fd;
    // Ask the event loop to run the background capture scheduler.
    fake.state->background_capture = true;
    fake.state->capture_backend = 3;   // dmabuf
    const auto &state = fake.state;

    auto get_registry = message(1, 1);
    put(get_registry, 5);
    finish(get_registry);
    send_message(app_fd, get_registry);
    const std::uint32_t registry_id = 5;
    receive(compositor_fd);

    const std::uint32_t compositor_id = 6;
    const std::uint32_t dmabuf_id = 9;
    bind_global(app_fd, compositor_fd, registry_id, 11, "wl_compositor", compositor_id);
    bind_global(app_fd, compositor_fd, registry_id, 12, "zwp_linux_dmabuf_v1", dmabuf_id, 4);

    const std::uint32_t surface_id = 7;
    auto get_surface = message(compositor_id, 0);
    put(get_surface, surface_id);
    finish(get_surface);
    send_message(app_fd, get_surface);
    receive(compositor_fd);

    const int width = 4;
    const int height = 3;
    const std::uint32_t params_id = 20;
    const std::uint32_t buffer_id = 21;
    auto create_params = message(dmabuf_id, 1);
    put(create_params, params_id);
    finish(create_params);
    send_message(app_fd, create_params);
    receive(compositor_fd);

    std::uint32_t stride = 0;
    const int buf_fd = make_linear_dmabuf(width, height, &stride);
    auto add = message(params_id, 1);
    put(add, 0);
    put(add, 0);
    put(add, stride);
    put(add, 0);
    put(add, 0);
    finish(add);
    send_message(app_fd, add, buf_fd);
    receive(compositor_fd);
    ::close(buf_fd);

    auto create = message(params_id, 3);
    put(create, buffer_id);
    put(create, static_cast<std::uint32_t>(width));
    put(create, static_cast<std::uint32_t>(height));
    put(create, drm_argb8888);
    put(create, 0);
    finish(create);
    send_message(app_fd, create);
    receive(compositor_fd);

    auto attach = message(surface_id, 1);
    put(attach, buffer_id);
    put(attach, 0);
    put(attach, 0);
    finish(attach);
    send_message(app_fd, attach);
    receive(compositor_fd);

    auto commit = message(surface_id, 6);
    finish(commit);
    send_message(app_fd, commit);
    receive(compositor_fd);

    // Wait for the periodic timer to drive the scheduler.
    bool published = false;
    for (int i = 0; i < 300; ++i) {
        {
            std::lock_guard lock(state->data_mutex);
            if (state->pre_captured_frame.has_value()) {
                published = true;
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    assert(published);

    std::lock_guard lock(state->data_mutex);
    const auto &image = *state->pre_captured_frame;
    assert(image.rows == height && image.cols == width);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int i = y * width + x;
            const auto px = image.at<cv::Vec3b>(y, x);
            assert(px[0] == static_cast<unsigned char>(i));
            assert(px[1] == static_cast<unsigned char>(i + 10));
            assert(px[2] == static_cast<unsigned char>(i + 20));
        }
    }

    std::printf("capture pipeline integration: ok\n");
    return 0;
}
