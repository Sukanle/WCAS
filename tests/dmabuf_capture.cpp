// Integration coverage for DMA-BUF capture through the Wayland proxy.
//
// Everything runs against a fake compositor over socketpairs, so the test
// never depends on a desktop session or a GPU: the "DMA-BUF" is a memfd-backed
// linear buffer with a non-zero plane offset, which is exactly the shape the
// proxy's linear readback path supports.

#include "proxy/conn.h"
#include "proxy/vulkan_dmabuf.h"

#ifdef WCAS_HAVE_GBM
#  include <gbm.h>
#endif

#include <sys/mman.h>
#include <sys/socket.h>
#include <poll.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

#include <cassert>
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

constexpr std::uint32_t drm_argb8888 = 0x34325241u;   // memory: B,G,R,A
constexpr std::uint32_t drm_nv12 = 0x3231564eu;       // 'NV12', two planes

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
/// Receive one message that must be there (fails the test otherwise).
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

/// A memfd-backed linear "DMA-BUF" with a non-zero plane offset, filled with a
/// per-pixel BGRA gradient so the decoded result can be checked exactly.
int make_dmabuf(int width, int height, std::uint32_t *stride, std::uint32_t *offset, std::size_t *map_size) {
    *offset = 64;
    *stride = static_cast<std::uint32_t>(width) * 4u;
    *map_size = *offset + static_cast<std::size_t>(*stride) * static_cast<std::size_t>(height);
    const int fd = ::memfd_create("wcas-dmabuf-test", MFD_CLOEXEC);
    assert(fd >= 0);
    assert(::ftruncate(fd, static_cast<off_t>(*map_size)) == 0);
    void *base = ::mmap(nullptr, *map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    assert(base != MAP_FAILED);
    auto *bytes = static_cast<unsigned char *>(base) + *offset;
    for (int y = 0; y < height; ++y) {
        auto *row = bytes + static_cast<std::size_t>(y) * *stride;
        for (int x = 0; x < width; ++x) {
            const int i = y * width + x;
            row[x * 4 + 0] = static_cast<unsigned char>(i);        // B
            row[x * 4 + 1] = static_cast<unsigned char>(i + 10);   // G
            row[x * 4 + 2] = static_cast<unsigned char>(i + 20);   // R
            row[x * 4 + 3] = 255;
        }
    }
    ::munmap(base, *map_size);
    return fd;
}

int open_fd_count() {
    DIR *dir = ::opendir("/proc/self/fd");
    assert(dir != nullptr);
    int count = 0;
    while (auto *entry = ::readdir(dir)) {
        if (entry->d_name[0] != '.') ++count;
    }
    ::closedir(dir);
    return count;
}

/// Global announcement is unnecessary here: the proxy forwards the bind
/// request itself, and the test only cares about the object it creates.
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
    const auto upstream = get(forwarded, forwarded.size() - 4);
    // Ids pass through unchanged (single client-owned upstream id space).
    assert(upstream == app_object);
    return upstream;
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

/// Add one plane to a params object and create an immediate buffer from it.
void submit_dmabuf(int app_fd, int compositor_fd, std::uint32_t params_id, std::uint32_t buffer_id, int width,
    int height, std::uint32_t format, int fd, std::uint32_t offset, std::uint32_t stride) {
    auto add = message(params_id, 1);   // zwp_linux_buffer_params_v1.add = "huuuuu"
    put(add, 0);                        // plane index
    put(add, offset);
    put(add, stride);
    put(add, 0);   // modifier high (DRM_FORMAT_MOD_LINEAR)
    put(add, 0);   // modifier low
    finish(add);
    send_message(app_fd, add, fd);
    receive(compositor_fd);

    auto create = message(params_id, 3);   // create_immed = "niiuu"
    put(create, buffer_id);
    put(create, static_cast<std::uint32_t>(width));
    put(create, static_cast<std::uint32_t>(height));
    put(create, format);
    put(create, 0);
    finish(create);
    send_message(app_fd, create);
    receive(compositor_fd);
}

std::uint32_t create_params(int app_fd, int compositor_fd, std::uint32_t dmabuf_id, std::uint32_t params_id) {
    auto create = message(dmabuf_id, 1);   // zwp_linux_dmabuf_v1.create_params = "n"
    put(create, params_id);
    finish(create);
    send_message(app_fd, create);
    const auto forwarded = receive(compositor_fd);
    const auto upstream = get(forwarded, 8);
    assert(upstream == params_id);
    return upstream;
}

void attach_and_commit(int app_fd, int compositor_fd, std::uint32_t surface_id, std::uint32_t buffer_id) {
    auto attach = message(surface_id, 1);   // wl_surface.attach = "?oii"
    put(attach, buffer_id);
    put(attach, 0);
    put(attach, 0);
    finish(attach);
    send_message(app_fd, attach);
    receive(compositor_fd);

    auto commit = message(surface_id, 6);   // wl_surface.commit = ""
    finish(commit);
    send_message(app_fd, commit);
    receive(compositor_fd);
}

}   // namespace

int main() {
    Fake fake;
    int app_fd = fake.app[1];
    int compositor_fd = fake.compositor[1];
    fake.state->client_fd = app_fd;
    const auto &state = fake.state;

    // --- registry: wl_compositor and zwp_linux_dmabuf_v1 ---
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

    // --- a surface to commit into ---
    const std::uint32_t surface_id = 7;
    auto get_surface = message(compositor_id, 0);   // wl_compositor.create_surface = "n"
    put(get_surface, surface_id);
    finish(get_surface);
    send_message(app_fd, get_surface);
    receive(compositor_fd);

    // --- a single-plane linear ARGB8888 buffer ---
    const int width = 4;
    const int height = 3;
    const std::uint32_t params_id = 20;
    const std::uint32_t buffer_id = 21;
    create_params(app_fd, compositor_fd, dmabuf_id, params_id);

    std::uint32_t stride = 0;
    std::uint32_t offset = 0;
    std::size_t map_size = 0;
    const int buf_fd = make_dmabuf(width, height, &stride, &offset, &map_size);
    submit_dmabuf(app_fd, compositor_fd, params_id, buffer_id, width, height, drm_argb8888, buf_fd, offset, stride);
    ::close(buf_fd);
    attach_and_commit(app_fd, compositor_fd, surface_id, buffer_id);

    {
        std::lock_guard lock(state->data_mutex);
        assert(state->surfaces[surface_id].committed_dmabuf_frame.has_value());
        assert(state->surfaces[surface_id].committed_dmabuf_frame->width == width);
        assert(state->surfaces[surface_id].committed_dmabuf_frame->height == height);
    }

    // --- capture decodes BGRA and honours the plane offset/stride ---
    cv::Mat image;
    // One warm-up capture: the first call lazily initialises the shared Vulkan
    // device (opening driver fds), which must not count as a per-capture leak.
    assert(skl::wcas::proxy::capture_dmabuf(state, image));
    const int fds_before = open_fd_count();
    for (int i = 0; i < 20; ++i) {
        assert(skl::wcas::proxy::capture_dmabuf(state, image));
    }
    assert(open_fd_count() == fds_before);   // repeated capture must not leak FDs
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

    // --- the snapshot survives destroying the wl_buffer ---
    auto destroy_buffer = message(buffer_id, 0);   // wl_buffer.destroy = ""
    finish(destroy_buffer);
    send_message(app_fd, destroy_buffer);
    receive(compositor_fd);
    assert(skl::wcas::proxy::capture_dmabuf(state, image));

    // --- destroying the params object releases the retained plane FDs ---
    const int fds_after_captures = open_fd_count();
    auto destroy_params = message(params_id, 0);   // zwp_linux_buffer_params_v1.destroy = ""
    finish(destroy_params);
    send_message(app_fd, destroy_params);
    receive(compositor_fd);
    // The committed frame keeps its own duplicated FD, so capture still works
    // and the fd count stays flat (params fd released, snapshot fd retained).
    assert(open_fd_count() <= fds_after_captures);
    assert(skl::wcas::proxy::capture_dmabuf(state, image));

    // --- an unsupported single-plane format is rejected, not mis-decoded ---
    const std::uint32_t bad_params = 22;
    const std::uint32_t bad_buffer = 23;
    create_params(app_fd, compositor_fd, dmabuf_id, bad_params);
    std::uint32_t bad_stride = 0;
    std::uint32_t bad_offset = 0;
    std::size_t bad_size = 0;
    const int bad_fd = make_dmabuf(width, height, &bad_stride, &bad_offset, &bad_size);
    submit_dmabuf(
        app_fd, compositor_fd, bad_params, bad_buffer, width, height, 0xdeadbeefu, bad_fd, bad_offset, bad_stride);
    ::close(bad_fd);
    attach_and_commit(app_fd, compositor_fd, surface_id, bad_buffer);
    assert(!skl::wcas::proxy::capture_dmabuf(state, image));

    // --- a multi-plane buffer is rejected (needs GPU interop) ---
    const std::uint32_t multi_params = 24;
    const std::uint32_t multi_buffer = 25;
    create_params(app_fd, compositor_fd, dmabuf_id, multi_params);
    std::uint32_t m_stride = 0;
    std::uint32_t m_offset = 0;
    std::size_t m_size = 0;
    const int m_fd = make_dmabuf(width, height, &m_stride, &m_offset, &m_size);
    {
        // plane 0
        auto add0 = message(multi_params, 1);
        put(add0, 0);
        put(add0, m_offset);
        put(add0, m_stride);
        put(add0, 0);
        put(add0, 0);
        finish(add0);
        send_message(app_fd, add0, m_fd);
        receive(compositor_fd);
        // plane 1
        auto add1 = message(multi_params, 1);
        put(add1, 1);
        put(add1, m_offset);
        put(add1, m_stride);
        put(add1, 0);
        put(add1, 0);
        finish(add1);
        send_message(app_fd, add1, m_fd);
        receive(compositor_fd);
        // create_immed with a two-plane format
        auto create = message(multi_params, 3);
        put(create, multi_buffer);
        put(create, static_cast<std::uint32_t>(width));
        put(create, static_cast<std::uint32_t>(height));
        put(create, drm_nv12);
        put(create, 0);
        finish(create);
        send_message(app_fd, create);
        receive(compositor_fd);
    }
    ::close(m_fd);
    attach_and_commit(app_fd, compositor_fd, surface_id, multi_buffer);
    assert(!skl::wcas::proxy::capture_dmabuf(state, image));

#if defined(WCAS_HAVE_GBM)
    // --- real GPU import: allocate a genuine DMA-BUF through gbm ---
    // Skipped silently when gbm or the render node is unavailable.
    // A LINEAR buffer is used because a tiled buffer written through
    // gbm_bo_map needs a GPU producer fence before it becomes visible; this
    // test only exercises the import/readback path itself.
    if (const int render_fd = ::open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC); render_fd >= 0) {
        ::setenv("GBM_BACKEND", "nvidia-drm", 1);
        if (gbm_device *gbm = gbm_create_device(render_fd); gbm != nullptr) {
            if (gbm_bo *bo = gbm_bo_create(gbm, width, height, GBM_FORMAT_ARGB8888, GBM_BO_USE_LINEAR); bo != nullptr) {
                const std::uint32_t g_stride = gbm_bo_get_stride(bo);
                const std::uint32_t g_offset = gbm_bo_get_offset(bo, 0);
                const std::uint64_t g_modifier = gbm_bo_get_modifier(bo);
                void *map_data = nullptr;
                std::uint32_t map_stride = 0;
                if (void *ptr = gbm_bo_map(bo, 0, 0, width, height, GBM_BO_TRANSFER_WRITE, &map_stride, &map_data);
                    ptr != nullptr) {
                    for (int y = 0; y < height; ++y) {
                        auto *row = static_cast<unsigned char *>(ptr) + static_cast<std::size_t>(y) * map_stride;
                        for (int x = 0; x < width; ++x) {
                            const int i = y * width + x;
                            row[x * 4 + 0] = static_cast<unsigned char>(i);
                            row[x * 4 + 1] = static_cast<unsigned char>(i + 10);
                            row[x * 4 + 2] = static_cast<unsigned char>(i + 20);
                            row[x * 4 + 3] = 255;
                        }
                    }
                    gbm_bo_unmap(bo, map_data);
                }
                const int dmabuf_fd = gbm_bo_get_fd(bo);
                if (dmabuf_fd >= 0) {
                    // Verify the Vulkan import path directly. The driver may
                    // take ownership of the fd, so hand it a duplicate.
                    const int import_fd = ::dup(dmabuf_fd);
                    auto gpu_frame = skl::wcas::proxy::vulkan_import_dmabuf(
                        import_fd, width, height, g_stride, g_modifier, drm_argb8888, g_offset);
                    if (gpu_frame.has_value()) {
                        assert(gpu_frame->pixels.size() == static_cast<std::size_t>(width) * height * 4u);
                        std::printf(
                            "dmabuf capture: Vulkan import verified (stride=%u offset=%u "
                            "modifier=0x%llx)\n",
                            g_stride, g_offset, static_cast<unsigned long long>(g_modifier));
                    } else {
                        std::printf("dmabuf capture: Vulkan import unavailable, mmap path only\n");
                    }

                    // The full protocol path must also capture the GPU buffer.
                    const std::uint32_t g_params = 40;
                    const std::uint32_t g_buffer = 41;
                    create_params(app_fd, compositor_fd, dmabuf_id, g_params);
                    submit_dmabuf(app_fd, compositor_fd, g_params, g_buffer, width, height, drm_argb8888, dmabuf_fd,
                        g_offset, g_stride);
                    ::close(dmabuf_fd);
                    attach_and_commit(app_fd, compositor_fd, surface_id, g_buffer);
                    assert(skl::wcas::proxy::capture_dmabuf(state, image));
                    for (int y = 0; y < height; ++y) {
                        for (int x = 0; x < width; ++x) {
                            const int i = y * width + x;
                            const auto px = image.at<cv::Vec3b>(y, x);
                            assert(px[0] == static_cast<unsigned char>(i));
                            assert(px[1] == static_cast<unsigned char>(i + 10));
                            assert(px[2] == static_cast<unsigned char>(i + 20));
                        }
                    }
                }
                gbm_bo_destroy(bo);
            }
            gbm_device_destroy(gbm);
        }
        ::close(render_fd);
    }
#endif
    std::printf("dmabuf capture integration: ok\n");
    return 0;
}
