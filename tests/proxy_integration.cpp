#include "proxy/conn.h"

#include <sys/mman.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>

#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

namespace {
void put(std::vector<char> &message, std::uint32_t value) {
    const auto at = message.size();
    message.resize(at + 4);
    std::memcpy(message.data() + at, &value, 4);
}
std::uint32_t get(const std::vector<char> &message, std::size_t at) {
    std::uint32_t value = 0;
    std::memcpy(&value, message.data() + at, 4);
    return value;
}
void string_arg(std::vector<char> &message, const char *value) {
    const auto length = static_cast<std::uint32_t>(std::strlen(value) + 1);
    put(message, length);
    const auto at = message.size();
    message.resize(at + ((length + 3u) & ~3u));
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
std::vector<char> receive(int fd, int *received_fd = nullptr) {
    std::vector<char> bytes(8);
    char control[CMSG_SPACE(sizeof(int) * 4)]{};
    iovec iov{bytes.data(), bytes.size()};
    msghdr input{};
    input.msg_iov = &iov;
    input.msg_iovlen = 1;
    input.msg_control = control;
    input.msg_controllen = sizeof(control);
    assert(::recvmsg(fd, &input, MSG_WAITALL) == 8);
    std::uint32_t header = 0;
    std::memcpy(&header, bytes.data() + 4, sizeof(header));
    const auto size = static_cast<std::size_t>(header >> 16);
    assert(size >= 8 && (size & 3u) == 0);
    bytes.resize(size);
    if (size > 8) assert(::recv(fd, bytes.data() + 8, size - 8, MSG_WAITALL) == static_cast<ssize_t>(size - 8));
    if (received_fd) {
        *received_fd = -1;
        for (auto *header = CMSG_FIRSTHDR(&input); header; header = CMSG_NXTHDR(&input, header)) {
            if (header->cmsg_level == SOL_SOCKET && header->cmsg_type == SCM_RIGHTS) {
                std::memcpy(received_fd, CMSG_DATA(header), sizeof(int));
                break;
            }
        }
    }
    return bytes;
}
void bind_global(int app_fd, int compositor_fd, std::uint32_t global, const char *name, std::uint32_t app_registry,
    std::uint32_t app_object, std::uint32_t version = 1) {
    // Ids pass through unchanged, so the registry's upstream id equals the
    // app-side id the test chose.
    auto event = message(app_registry, 0);
    put(event, global);
    string_arg(event, name);
    put(event, version);
    finish(event);
    send_message(compositor_fd, event);
    (void)receive(app_fd);
    auto bind = message(app_registry, 0);
    put(bind, global);
    string_arg(bind, name);
    put(bind, version);
    put(bind, app_object);
    finish(bind);
    send_message(app_fd, bind);
    auto forwarded = receive(compositor_fd);
    assert(get(forwarded, 8) == global);
    assert(get(forwarded, forwarded.size() - 4) == app_object);
}
}   // namespace

int main() {
    int app_pair[2]{};
    int compositor_pair[2]{};
    assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, app_pair) == 0);
    assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, compositor_pair) == 0);
    auto state = std::make_shared<skl::wcas::proxy::ConnState>();
    std::thread proxy_thread([&] {
        skl::wcas::proxy::Connection bridge(app_pair[0], compositor_pair[0], state);
        bridge.run();
    });

    auto registry = message(1, 1);
    put(registry, 5);
    finish(registry);
    send_message(app_pair[1], registry);
    auto upstream_registry = receive(compositor_pair[1]);
    assert(get(upstream_registry, 0) == 1);
    const auto upstream_registry_id = get(upstream_registry, 8);
    assert(upstream_registry_id == 5);

    bind_global(app_pair[1], compositor_pair[1], 11, "wl_shm", 5, 8);
    bind_global(app_pair[1], compositor_pair[1], 12, "wl_compositor", 5, 6);
    {
        std::lock_guard lock(state->data_mutex);
        assert(state->object_roles.at(8) == "wl_shm");
    }

    const int shm_fd = ::memfd_create("wcas-shm-test", MFD_CLOEXEC);
    assert(shm_fd >= 0);
    assert(::ftruncate(shm_fd, 4'096) == 0);
    const std::uint32_t pixels[4]{0x000000ffu, 0x0000ff00u, 0x00ff0000u, 0x00ffffffu};
    assert(::pwrite(shm_fd, pixels, sizeof(pixels), 0) == static_cast<ssize_t>(sizeof(pixels)));
    auto pool = message(8, 0);
    put(pool, 9);
    put(pool, 4'096);
    finish(pool);
    send_message(app_pair[1], pool, shm_fd);
    int forwarded_pool_fd = -1;
    auto upstream_pool = receive(compositor_pair[1], &forwarded_pool_fd);
    assert(forwarded_pool_fd >= 0);
    const auto upstream_pool_id = get(upstream_pool, 8);
    assert(upstream_pool_id == 9);
    ::close(forwarded_pool_fd);

    assert(::ftruncate(shm_fd, 8'192) == 0);
    auto resize_pool = message(9, 2);
    put(resize_pool, 8'192);
    finish(resize_pool);
    send_message(app_pair[1], resize_pool);
    (void)receive(compositor_pair[1]);
    {
        std::lock_guard lock(state->data_mutex);
        assert(state->shm_pools.at(9).size == 8'192);
    }

    auto buffer = message(9, 0);
    put(buffer, 10);
    put(buffer, 0);
    put(buffer, 2);
    put(buffer, 2);
    put(buffer, 8);
    put(buffer, 0x34325258u);
    finish(buffer);
    send_message(app_pair[1], buffer);
    (void)receive(compositor_pair[1]);
    auto surface = message(6, 0);
    put(surface, 7);
    finish(surface);
    send_message(app_pair[1], surface);
    (void)receive(compositor_pair[1]);
    auto attach = message(7, 1);
    put(attach, 10);
    put(attach, 0);
    put(attach, 0);
    finish(attach);
    send_message(app_pair[1], attach);
    (void)receive(compositor_pair[1]);
    auto commit = message(7, 6);
    finish(commit);
    send_message(app_pair[1], commit);
    (void)receive(compositor_pair[1]);

    assert(state->shm_pools.find(9) != state->shm_pools.end());
    assert(state->shm_buffers.find(10) != state->shm_buffers.end());
    {
        std::lock_guard lock(state->data_mutex);
        auto &surf = state->surfaces.at(7);
        assert(surf.committed_buffer.has_value() && *surf.committed_buffer == 10);
    }
    {
        std::lock_guard lock(state->data_mutex);
        assert(state->shm_buffers.at(10).pool_id == 9);
        assert(state->shm_buffers.at(10).width == 2 && state->shm_buffers.at(10).height == 2);
        assert(state->shm_buffers.at(10).format == 0x34325258u);
        assert(state->shm_pools.at(9).fd >= 0 && state->shm_pools.at(9).size == 8'192);
        assert(::fcntl(state->shm_pools.at(9).fd, F_GETFD) >= 0);
    }
    cv::Mat image;
    assert(skl::wcas::proxy::capture_shm(state, image));
    assert(image.cols == 2 && image.rows == 2);
    assert(image.at<cv::Vec3b>(0, 0)[2] == 0);
    assert(state->commit_count.load() == 1);
    assert(state->capturable_surface.load());

    auto clear_attach = message(7, 1);
    put(clear_attach, 0);
    put(clear_attach, 0);
    put(clear_attach, 0);
    finish(clear_attach);
    send_message(app_pair[1], clear_attach);
    (void)receive(compositor_pair[1]);
    auto clear_commit = message(7, 6);
    finish(clear_commit);
    send_message(app_pair[1], clear_commit);
    (void)receive(compositor_pair[1]);
    cv::Mat cleared_image;
    assert(!skl::wcas::proxy::capture_shm(state, cleared_image));

    auto restore_attach = message(7, 1);
    put(restore_attach, 10);
    put(restore_attach, 0);
    put(restore_attach, 0);
    finish(restore_attach);
    send_message(app_pair[1], restore_attach);
    (void)receive(compositor_pair[1]);
    auto restore_commit = message(7, 6);
    finish(restore_commit);
    send_message(app_pair[1], restore_commit);
    (void)receive(compositor_pair[1]);
    assert(skl::wcas::proxy::capture_shm(state, cleared_image));
    state->surface_to_toplevel[7] = 20;
    state->toplevels[20] = {7, std::string("target.app"), std::string("Target window")};
    {
        // `list_windows` must surface the toplevel with its metadata and size.
        bool found = false;
        for (const auto &window : skl::wcas::proxy::list_windows(state)) {
            if (window.surface_id == 7
                && window.app_id
                && *window.app_id == "target.app"
                && window.title
                && *window.title == "Target window") {
                assert(window.width == 2 && window.height == 2);
                found = true;
            }
        }
        assert(found);
    }
    skl::wcas::proxy::set_capture_target(state, std::string("target.app"), std::string("Target window"));
    assert(skl::wcas::proxy::capture_shm(state, cleared_image));
    // A requested target that matches nothing must fail instead of silently
    // returning an unrelated surface's frame.
    skl::wcas::proxy::set_capture_target(state, std::string("missing.app"), std::nullopt);
    assert(!skl::wcas::proxy::capture_shm(state, cleared_image));
    skl::wcas::proxy::set_capture_target(state, std::nullopt, std::string("Missing title"));
    assert(!skl::wcas::proxy::capture_shm(state, cleared_image));
    skl::wcas::proxy::set_capture_target(state, std::nullopt, std::nullopt);
    assert(skl::wcas::proxy::capture_shm(state, cleared_image));

    auto destroy_buffer = message(10, 0);
    finish(destroy_buffer);
    send_message(app_pair[1], destroy_buffer);
    (void)receive(compositor_pair[1]);
    auto destroy_pool = message(9, 1);
    finish(destroy_pool);
    send_message(app_pair[1], destroy_pool);
    (void)receive(compositor_pair[1]);
    assert(state->shm_buffers.find(10) == state->shm_buffers.end());
    assert(state->shm_pools.find(9) == state->shm_pools.end());
    cv::Mat retained_image;
    assert(skl::wcas::proxy::capture_shm(state, retained_image));
    assert(retained_image.cols == 2 && retained_image.rows == 2);

    // ---- compositor-reported pointer position ----
    // The proxy must remember where the real device is so callers can query it.
    bind_global(app_pair[1], compositor_pair[1], 13, "wl_seat", 5, 30);
    auto get_pointer = message(30, 0);   // wl_seat.get_pointer = "n"
    put(get_pointer, 31);
    finish(get_pointer);
    send_message(app_pair[1], get_pointer);
    (void)receive(compositor_pair[1]);

    auto enter = message(31, 0);      // wl_pointer.enter = "uoff"
    put(enter, 1);                    // serial
    put(enter, 7);                    // surface
    put(enter, 100u * 256u + 128u);   // surface_x = 100.5
    put(enter, 200u * 256u + 64u);    // surface_y = 200.25
    finish(enter);
    send_message(compositor_pair[1], enter);
    (void)receive(app_pair[1]);
    assert(state->pointer_position_known.load());
    assert(state->pointer_sx.load() == 100u * 256u + 128u);
    assert(state->pointer_sy.load() == 200u * 256u + 64u);

    auto motion = message(31, 2);   // wl_pointer.motion = "uff"
    put(motion, 1'234);             // time
    put(motion, 300u * 256u);       // surface_x = 300.0
    put(motion, 400u * 256u);       // surface_y = 400.0
    finish(motion);
    send_message(compositor_pair[1], motion);
    (void)receive(app_pair[1]);
    assert(state->pointer_sx.load() == 300u * 256u);
    assert(state->pointer_sy.load() == 400u * 256u);

    // ---- toplevel metadata lifetime and subsurface inheritance ----
    // A rendering surface is often not the toplevel: games put the visible
    // content in a wl_subsurface. Such a surface must inherit the toplevel's
    // app_id/title so callers can still identify the window, and destroying the
    // toplevel must not leave its metadata (a 0x0 ghost) behind.
    bind_global(app_pair[1], compositor_pair[1], 40, "wl_subcompositor", 5, 51);
    bind_global(app_pair[1], compositor_pair[1], 41, "xdg_wm_base", 5, 52);

    // Create the shell surface (60) and its render child surface (61) so the
    // proxy records their wl_surface role and can clean them up on destroy.
    for (std::uint32_t sid : {60u, 61u}) {
        auto create_surface = message(6, 0);   // wl_compositor.create_surface = "n"
        put(create_surface, sid);
        finish(create_surface);
        send_message(app_pair[1], create_surface);
        (void)receive(compositor_pair[1]);
    }

    auto get_xdg_surface = message(52, 2);   // xdg_wm_base.get_xdg_surface = "no"
    put(get_xdg_surface, 62);
    put(get_xdg_surface, 60);
    finish(get_xdg_surface);
    send_message(app_pair[1], get_xdg_surface);
    (void)receive(compositor_pair[1]);

    auto get_toplevel = message(62, 1);   // xdg_surface.get_toplevel = "n"
    put(get_toplevel, 63);
    finish(get_toplevel);
    send_message(app_pair[1], get_toplevel);
    (void)receive(compositor_pair[1]);

    auto set_title = message(63, 2);   // xdg_toplevel.set_title = "s"
    string_arg(set_title, "UIPlatform");
    finish(set_title);
    send_message(app_pair[1], set_title);
    (void)receive(compositor_pair[1]);

    auto set_app_id = message(63, 3);   // xdg_toplevel.set_app_id = "s"
    string_arg(set_app_id, "platformprocess.exe");
    finish(set_app_id);
    send_message(app_pair[1], set_app_id);
    (void)receive(compositor_pair[1]);

    // Only the subsurface renders; the toplevel surface stays 0x0, which is
    // exactly the shape that made `list` show metadata-less content.
    auto get_subsurface = message(51, 1);   // wl_subcompositor.get_subsurface = "noo"
    put(get_subsurface, 64);
    put(get_subsurface, 61);
    put(get_subsurface, 60);
    finish(get_subsurface);
    send_message(app_pair[1], get_subsurface);
    (void)receive(compositor_pair[1]);

    {
        std::lock_guard lock(state->data_mutex);
        auto &surf = state->surfaces[61];
        surf.committed_frame = skl::wcas::proxy::RetainedShm{};
        surf.committed_frame->width = 800;
        surf.committed_frame->height = 600;
        surf.last_commit_seq = 999;
    }

    {
        bool saw_toplevel = false;
        bool saw_inherited = false;
        for (const auto &window : skl::wcas::proxy::list_windows(state)) {
            if (window.surface_id == 60) {
                assert(window.origin == skl::wcas::proxy::WindowOrigin::Toplevel);
                assert(window.app_id && *window.app_id == "platformprocess.exe");
                saw_toplevel = true;
            }
            if (window.surface_id == 61) {
                // Metadata must come from the parent chain, not be empty.
                assert(window.origin == skl::wcas::proxy::WindowOrigin::Subsurface);
                assert(window.app_id && *window.app_id == "platformprocess.exe");
                assert(window.title && *window.title == "UIPlatform");
                assert(window.width == 800 && window.height == 600);
                saw_inherited = true;
            }
        }
        assert(saw_toplevel && saw_inherited);
    }

    // Destroying the toplevel must drop its metadata, otherwise `list` keeps
    // reporting a window that no longer exists.
    auto destroy_toplevel = message(63, 0);
    finish(destroy_toplevel);
    send_message(app_pair[1], destroy_toplevel);
    (void)receive(compositor_pair[1]);
    {
        std::lock_guard lock(state->data_mutex);
        assert(state->toplevels.find(63) == state->toplevels.end());
        assert(state->surface_to_toplevel.find(60) == state->surface_to_toplevel.end());
    }
    // The toplevel object is gone, but this is still the actively rendered
    // surface, so it keeps its last known name and stays selectable.
    for (const auto &window : skl::wcas::proxy::list_windows(state)) {
        if (window.surface_id == 61) {
            assert(window.origin == skl::wcas::proxy::WindowOrigin::Subsurface);
            assert(window.app_id && *window.app_id == "platformprocess.exe");
            assert(window.title && *window.title == "UIPlatform");
        }
    }

    // Destroying the shell surface (60) must not rob the still-rendering child
    // (61) of its name: the proxy hands the remembered metadata down first.
    auto destroy_shell_surface = message(60, 0);   // wl_surface.destroy
    finish(destroy_shell_surface);
    send_message(app_pair[1], destroy_shell_surface);
    (void)receive(compositor_pair[1]);
    {
        std::lock_guard lock(state->data_mutex);
        assert(state->surface_metadata.find(60) == state->surface_metadata.end());
        assert(state->surface_to_parent_surface.find(61) == state->surface_to_parent_surface.end());
    }
    for (const auto &window : skl::wcas::proxy::list_windows(state)) {
        if (window.surface_id == 61) {
            assert(window.origin == skl::wcas::proxy::WindowOrigin::Subsurface);
            assert(window.app_id && *window.app_id == "platformprocess.exe");
            assert(window.title && *window.title == "UIPlatform");
        }
    }

    // ---- shell toplevel attribution ----
    // A window whose xdg_toplevel surface never renders (0x0) but whose content
    // is drawn into a separate surface: the most recently committed nameless
    // surface should inherit the toplevel's name, while an older stale surface
    // stays bare so it cannot masquerade as the live window.
    {
        std::lock_guard lock(state->data_mutex);
        state->surface_to_toplevel[70] = 80;
        state->toplevels[80] = {70, std::string("shell.app"), std::string("Shell window")};
        auto &recent = state->surfaces[71];
        recent.committed_frame = skl::wcas::proxy::RetainedShm{};
        recent.committed_frame->width = 1'280;
        recent.committed_frame->height = 720;
        recent.last_commit_seq = 1'000;
        auto &stale = state->surfaces[72];
        stale.committed_frame = skl::wcas::proxy::RetainedShm{};
        stale.committed_frame->width = 640;
        stale.committed_frame->height = 480;
        stale.last_commit_seq = 10;
    }
    {
        bool recent_named = false;
        bool stale_bare = false;
        for (const auto &window : skl::wcas::proxy::list_windows(state)) {
            if (window.surface_id == 71) {
                assert(window.origin == skl::wcas::proxy::WindowOrigin::Subsurface);
                assert(window.app_id && *window.app_id == "shell.app");
                assert(window.title && *window.title == "Shell window");
                recent_named = true;
            }
            if (window.surface_id == 72) {
                assert(window.origin == skl::wcas::proxy::WindowOrigin::Bare);
                assert(!window.app_id && !window.title);
                stale_bare = true;
            }
        }
        assert(recent_named && stale_bare);
    }
    {
        std::lock_guard lock(state->data_mutex);
        state->toplevels.erase(80);
        state->surface_to_toplevel.erase(70);
        state->surfaces.erase(71);
        state->surfaces.erase(72);
    }

    // Destroying the subsurface drops the parent relation too.
    auto destroy_subsurface = message(64, 0);
    finish(destroy_subsurface);
    send_message(app_pair[1], destroy_subsurface);
    (void)receive(compositor_pair[1]);
    {
        std::lock_guard lock(state->data_mutex);
        assert(state->subsurface_to_surface.find(64) == state->subsurface_to_surface.end());
        assert(state->surface_to_parent_surface.find(61) == state->surface_to_parent_surface.end());
    }

    ::shutdown(app_pair[1], SHUT_RDWR);
    ::close(app_pair[1]);
    ::close(compositor_pair[1]);
    ::close(shm_fd);
    proxy_thread.join();
    return 0;
}
