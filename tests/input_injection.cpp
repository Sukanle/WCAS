// Integration coverage for synthetic input injection through the Wayland
// proxy. Everything runs against a fake compositor over socketpairs, so the
// test never depends on a desktop session.
//
// The app's keymap is generated at runtime through xkbcommon (the same library
// the proxy uses) and handed over as a real SCM_RIGHTS fd, so the keycode
// assertions exercise the production resolution path rather than a fixture.

#include "proxy/conn.h"
#include "proxy/input.h"
#include "proxy/keymap.h"
#include "proxy/keysym.h"

#include <xkbcommon/xkbcommon.h>

#include <sys/mman.h>
#include <sys/socket.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using skl::wcas::proxy::ConnState;

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
/// Receive a message if one is queued; an empty result means the proxy sent
/// nothing for this direction.
std::vector<char> try_receive(int fd, int *received_fd = nullptr) {
    if (received_fd) *received_fd = -1;
    pollfd ready{fd, POLLIN, 0};
    if (::poll(&ready, 1, 0) <= 0 || !(ready.revents & POLLIN)) return {};

    std::vector<char> bytes(8);
    char control[CMSG_SPACE(sizeof(int) * 4)]{};
    iovec iov{bytes.data(), bytes.size()};
    msghdr input{};
    input.msg_iov = &iov;
    input.msg_iovlen = 1;
    input.msg_control = control;
    input.msg_controllen = sizeof(control);
    const auto header_size = ::recvmsg(fd, &input, MSG_WAITALL);
    if (header_size != 8) return {};
    std::uint32_t wire_header = 0;
    std::memcpy(&wire_header, bytes.data() + 4, sizeof(wire_header));
    const auto message_size = static_cast<std::size_t>(wire_header >> 16);
    if (message_size < 8 || (message_size & 3u) != 0) return {};
    bytes.resize(message_size);
    if (message_size > 8
        && ::recv(fd, bytes.data() + 8, message_size - 8, MSG_WAITALL) != static_cast<ssize_t>(message_size - 8))
        return {};
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

/// Receive one message that must be there (fails the test otherwise).
std::vector<char> receive(int fd, int *received_fd = nullptr) {
    if (received_fd) *received_fd = -1;
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
    if (received_fd) {
        for (auto *header = CMSG_FIRSTHDR(&input); header; header = CMSG_NXTHDR(&input, header)) {
            if (header->cmsg_level == SOL_SOCKET && header->cmsg_type == SCM_RIGHTS) {
                std::memcpy(received_fd, CMSG_DATA(header), sizeof(int));
                break;
            }
        }
    }
    return bytes;
}

/// Assert the proxy queued nothing more for this direction.
void expect_empty(int fd) {
    auto bytes = try_receive(fd);
    assert(bytes.empty());
}

/// The US layout compiled by the system's own xkbcommon, exported as keymap
/// text. This is exactly the shape of a `wl_keyboard.keymap` payload.
std::string system_keymap_text() {
    xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    assert(context != nullptr);
    xkb_rule_names names{};
    names.layout = "us";
    xkb_keymap *keymap = xkb_keymap_new_from_names(context, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    assert(keymap != nullptr);
    char *text = xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
    assert(text != nullptr);
    std::string result(text);
    std::free(text);
    xkb_keymap_unref(keymap);
    xkb_context_unref(context);
    return result;
}

int keymap_fd_for(const std::string &text, std::uint32_t *size) {
    const int fd = ::memfd_create("wcas-keymap-test", MFD_CLOEXEC);
    assert(fd >= 0);
    const auto bytes = text.size() + 1;
    assert(::ftruncate(fd, static_cast<off_t>(bytes)) == 0);
    assert(::pwrite(fd, text.c_str(), bytes, 0) == static_cast<ssize_t>(bytes));
    *size = static_cast<std::uint32_t>(bytes);
    return fd;
}

/// Global announcement from the fake compositor.
void announce_global(
    int compositor_fd, std::uint32_t registry, std::uint32_t global, const char *name, std::uint32_t version) {
    auto event = message(registry, 0);
    put(event, global);
    string_arg(event, name);
    put(event, version);
    finish(event);
    send_message(compositor_fd, event);
}

/// Registry bind from the app; returns the upstream object id the proxy chose.
std::uint32_t bind_global(int app_fd, int compositor_fd, std::uint32_t registry, std::uint32_t global, const char *name,
    std::uint32_t app_object, std::uint32_t version = 1) {
    // wl_registry.bind is `usun`: name(uint), interface(string), version(uint),
    // id(new_id). The version argument is mandatory on the wire.
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

}   // namespace

int main() {
    // Diagnostics are opt-in and may be enabled by the test runner while
    // diagnosing a protocol failure.

    const std::string keymap_text = system_keymap_text();

    Fake fake;
    int app_fd = fake.app[1];
    int compositor_fd = fake.compositor[1];
    // The harness normally fills this in from SO_PEERCRED; here the test acts
    // as the app, so it points at its own end of the socketpair.
    fake.state->client_fd = app_fd;
    const auto &state = fake.state;

    // --- registry wiring: wl_compositor, wl_shm, wl_seat ---
    auto get_registry = message(1, 1);
    put(get_registry, 5);
    finish(get_registry);
    send_message(app_fd, get_registry);
    const std::uint32_t registry_id = 5;
    const auto forwarded_registry = receive(compositor_fd);
    const auto upstream_registry = get(forwarded_registry, 8);
    assert(upstream_registry == registry_id);

    const std::uint32_t compositor_id = 6;
    const std::uint32_t shm_id = 8;
    const std::uint32_t seat_id = 10;
    const auto upstream_compositor =
        bind_global(app_fd, compositor_fd, registry_id, 11, "wl_compositor", compositor_id);
    const auto upstream_shm = bind_global(app_fd, compositor_fd, registry_id, 12, "wl_shm", shm_id);
    const auto upstream_seat = bind_global(app_fd, compositor_fd, registry_id, 13, "wl_seat", seat_id);
    assert(upstream_seat == seat_id);

    // wl_seat.get_pointer / get_keyboard create the objects the proxy injects
    // events on. The proxy records them from the request, so no server event
    // is needed.
    const std::uint32_t pointer_id = 21;
    const std::uint32_t keyboard_id = 22;
    auto get_pointer = message(seat_id, 0);
    put(get_pointer, pointer_id);
    finish(get_pointer);
    send_message(app_fd, get_pointer);
    const auto forwarded_pointer = receive(compositor_fd);
    const auto upstream_pointer = get(forwarded_pointer, 8);
    assert(upstream_pointer == pointer_id);

    auto get_keyboard = message(seat_id, 1);
    put(get_keyboard, keyboard_id);
    finish(get_keyboard);
    send_message(app_fd, get_keyboard);
    const auto forwarded_keyboard = receive(compositor_fd);
    const auto upstream_keyboard = get(forwarded_keyboard, 8);
    assert(upstream_keyboard == keyboard_id);

    // wl_keyboard.keymap(format, fd, size) from the compositor.
    std::uint32_t keymap_size = 0;
    const int keymap_fd = keymap_fd_for(keymap_text, &keymap_size);
    auto keymap_event = message(upstream_keyboard, 0);
    put(keymap_event, 1);             // WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1
    put(keymap_event, keymap_size);   // fd rides in SCM_RIGHTS, not the body
    finish(keymap_event);
    send_message(compositor_fd, keymap_event, keymap_fd);
    ::close(keymap_fd);

    // The proxy must forward the keymap fd to the app and keep its own copy.
    int forwarded_keymap_fd = -1;
    const auto forwarded_keymap = receive(app_fd, &forwarded_keymap_fd);
    assert(get(forwarded_keymap, 0) == keyboard_id);
    assert(forwarded_keymap_fd >= 0);
    ::close(forwarded_keymap_fd);
    assert(state->keymap_size == keymap_size);
    assert(state->keymap_fd >= 0);

    // --- a committed surface for input to be routed to ---
    const std::uint32_t surface_id = 7;
    (void)upstream_compositor;
    auto get_surface = message(compositor_id, 0);
    put(get_surface, surface_id);
    finish(get_surface);
    send_message(app_fd, get_surface);
    const auto forwarded_surface = receive(compositor_fd);
    const auto upstream_surface = get(forwarded_surface, 8);
    assert(upstream_surface == surface_id);
    auto commit = message(surface_id, 6);
    finish(commit);
    send_message(app_fd, commit);
    (void)receive(compositor_fd);
    // Input routing only requires a surface that has committed something.
    state->surfaces[surface_id].committed_buffer = 0xffff;
    state->surfaces[surface_id].commit_count = 1;
    assert(skl::wcas::proxy::target_surface(state) == surface_id);

    // --- keymap resolution against the app's own layout ---
    const auto keymap = skl::wcas::proxy::Keymap::parse(keymap_text);
    assert(keymap.valid());
    const auto return_keycode = keymap.find_keysym(*skl::wcas::proxy::keysym_for_key("Enter"));
    assert(return_keycode.has_value());

    // --- press_key("Enter") ---
    assert(skl::wcas::proxy::inject_key_raw(state, *return_keycode, true));
    auto key_event = receive(compositor_fd);
    // Injected events are app->upstream, so they carry the upstream object id.
    assert(get(key_event, 0) == upstream_keyboard);
    assert((get(key_event, 4) & 0xffffu) == 3);   // wl_keyboard.key
    assert(get(key_event, 16) == 28);             // xkb keycode 36 -> evdev 28 (Return)
    assert(get(key_event, 20) == 1);              // pressed

    assert(skl::wcas::proxy::inject_key_raw(state, *return_keycode, false));
    key_event = receive(compositor_fd);
    assert(get(key_event, 20) == 0);   // released

    // --- keyboard focus is established once, not per key ---
    assert(skl::wcas::proxy::ensure_keyboard_focus(state, surface_id));
    auto enter_event = receive(compositor_fd);
    assert(get(enter_event, 0) == upstream_keyboard);
    assert((get(enter_event, 4) & 0xffffu) == 1);   // wl_keyboard.enter
    assert(get(enter_event, 12) == upstream_surface);
    assert(get(enter_event, 16) == 0);   // empty keys array
    // `enter` is followed by a `modifiers` event, as a real compositor sends.
    auto modifiers_event = receive(compositor_fd);
    assert(get(modifiers_event, 0) == upstream_keyboard);
    assert((get(modifiers_event, 4) & 0xffffu) == 4);   // wl_keyboard.modifiers
    // Re-focusing the same surface emits nothing at all.
    assert(skl::wcas::proxy::ensure_keyboard_focus(state, surface_id));
    assert(state->keyboard_focus == surface_id);

    // --- pointer navigation ---
    assert(skl::wcas::proxy::ensure_pointer_focus(state, surface_id, 5.0, 6.0));
    const auto pointer_enter = receive(compositor_fd);
    assert(get(pointer_enter, 0) == upstream_pointer);
    assert((get(pointer_enter, 4) & 0xffffu) == 0);   // wl_pointer.enter
    assert(get(pointer_enter, 8) != 0);               // serial
    assert(get(pointer_enter, 12) == upstream_surface);
    assert(get(pointer_enter, 16) == 5 * 256);
    assert(get(pointer_enter, 20) == 6 * 256);
    const auto pointer_frame = receive(compositor_fd);
    assert((get(pointer_frame, 4) & 0xffffu) == 5);   // wl_pointer.frame
    assert(state->pointer_focus == surface_id);

    assert(skl::wcas::proxy::inject_pointer_motion(state, 7.0, 8.0));
    const auto motion = receive(compositor_fd);
    assert((get(motion, 4) & 0xffffu) == 2);   // wl_pointer.motion
    assert(get(motion, 12) == 7 * 256);
    assert(get(motion, 16) == 8 * 256);

    // --- relative pointer: pointer-locked games (3D camera) need this ---
    // The app creates a zwp_relative_pointer_v1 through the manager; the proxy
    // records its role and can then inject relative_motion deltas, which is what
    // a press-and-drag on a locked pointer actually drives the camera with.
    const std::uint32_t relative_manager_id = 13;
    bind_global(app_fd, compositor_fd, registry_id, 15, "zwp_relative_pointer_manager_v1", relative_manager_id);
    const std::uint32_t relative_pointer_id = 40;
    auto get_relative = message(relative_manager_id, 1);   // get_relative_pointer(new_id, pointer); destroy is opcode 0
    put(get_relative, relative_pointer_id);
    put(get_relative, pointer_id);
    finish(get_relative);
    send_message(app_fd, get_relative);
    const auto forwarded_relative = receive(compositor_fd);
    assert(get(forwarded_relative, 8) == relative_pointer_id);

    assert(skl::wcas::proxy::inject_relative_motion(state, 3.0, -4.0));
    const auto relative_motion = receive(compositor_fd);
    assert(get(relative_motion, 0) == relative_pointer_id);
    assert((get(relative_motion, 4) & 0xffffu) == 0);   // zwp_relative_pointer_v1.relative_motion
    assert(get(relative_motion, 16) == 3 * 256);        // dx
    assert(static_cast<std::int32_t>(get(relative_motion, 20)) == -4 * 256);   // dy

    assert(skl::wcas::proxy::inject_pointer_axis_source(state, 0));
    const auto axis_source = receive(compositor_fd);
    assert((get(axis_source, 4) & 0xffffu) == 6);   // wl_pointer.axis_source
    assert(get(axis_source, 8) == 0);               // wheel

    assert(skl::wcas::proxy::inject_pointer_axis(state, 0, 15.0));
    const auto axis = receive(compositor_fd);
    assert((get(axis, 4) & 0xffffu) == 4);   // wl_pointer.axis
    assert(get(axis, 12) == 0);              // vertical
    assert(get(axis, 16) == 15 * 256);

    // --- type_text without text-input-v3: keymap-resolved ASCII ---
    assert(state->text_input_enabled == false);
    // 'a' is unshifted; 'A' and '!' need Shift.
    assert(skl::wcas::proxy::inject_text(state, "aA!"));
    const auto a_code = keymap.resolve_char('a');
    const auto shift_code = keymap.shift_keycode();
    const auto upper_code = keymap.resolve_char('A');
    assert(a_code && !a_code->needs_shift);
    assert(upper_code && upper_code->needs_shift);
    assert(shift_code.has_value());
    assert(keymap.shift_mask() == 1u);

    auto first = receive(compositor_fd);   // 'a' press
    assert((get(first, 4) & 0xffffu) == 3);
    assert(get(first, 16) == a_code->keycode - 8);
    assert(get(first, 20) == 1);
    (void)receive(compositor_fd);   // 'a' release

    auto shift_down = receive(compositor_fd);   // Shift press
    assert(get(shift_down, 16) == *shift_code - 8);
    const auto shift_mods = receive(compositor_fd);   // modifiers with Shift mask
    assert((get(shift_mods, 4) & 0xffffu) == 4);
    assert(get(shift_mods, 12) == 1u);

    auto upper_down = receive(compositor_fd);
    assert(get(upper_down, 16) == upper_code->keycode - 8);
    assert(get(upper_down, 20) == 1);
    (void)receive(compositor_fd);   // upper release
    (void)receive(compositor_fd);   // '!' press
    (void)receive(compositor_fd);   // '!' release
    auto shift_up = receive(compositor_fd);
    assert(get(shift_up, 16) == *shift_code - 8);
    assert(get(shift_up, 20) == 0);
    const auto shift_clear = receive(compositor_fd);
    assert((get(shift_clear, 4) & 0xffffu) == 4);
    assert(get(shift_clear, 12) == 0u);

    // Non-ASCII must fail rather than emit garbage bytes.
    assert(!skl::wcas::proxy::inject_text(state, "\xc3\xa9"));

    // --- type_text with text-input-v3 enabled: commit_string + done ---
    // Create the text-input object through the normal protocol path so the
    // proxy has an app/upstream object mapping for injected messages.
    const std::uint32_t text_input_manager_id = 12;
    const auto upstream_text_manager =
        bind_global(app_fd, compositor_fd, registry_id, 14, "zwp_text_input_manager_v3", text_input_manager_id);
    assert(upstream_text_manager == text_input_manager_id);
    const std::uint32_t text_input_id = 30;
    auto get_text_input = message(text_input_manager_id, 1);
    put(get_text_input, text_input_id);
    put(get_text_input, surface_id);
    finish(get_text_input);
    send_message(app_fd, get_text_input);
    const auto forwarded_text_input = receive(compositor_fd);
    const auto upstream_text_input = get(forwarded_text_input, 8);
    assert(upstream_text_input == text_input_id);
    state->text_input_obj = text_input_id;
    state->text_input_enabled = true;
    state->text_input_commit_serial = 4;
    assert(skl::wcas::proxy::inject_text(state, "café"));
    const auto commit_event = receive(compositor_fd);
    assert(get(commit_event, 0) == upstream_text_input);
    assert((get(commit_event, 4) & 0xffffu) == 3);   // commit_string
    assert(get(commit_event, 8) == 6);               // "cafe" + UTF-8 e-acute + NUL
    assert(std::memcmp(commit_event.data() + 12, "caf\xc3\xa9", 5) == 0);
    const auto done_event = receive(compositor_fd);
    assert(get(done_event, 0) == upstream_text_input);
    assert((get(done_event, 4) & 0xffffu) == 5);   // done
    assert(get(done_event, 8) == 4);               // echoed serial
    // The keymap path must not have produced any keyboard events.
    state->text_input_enabled = false;

    // --- key combination parsing ---
    const auto combo = skl::wcas::proxy::parse_key_combination("ctrl+shift+p");
    assert(combo.key == "p");
    assert(combo.modifiers.size() == 2);
    assert(combo.modifiers[0] == "ctrl" && combo.modifiers[1] == "shift");
    assert(skl::wcas::proxy::xkb_modifier_name("ctrl").has_value());
    assert(!skl::wcas::proxy::xkb_modifier_name("hyper").has_value());

    // --- unknown key names must fail instead of guessing ---
    assert(!skl::wcas::proxy::keysym_for_key("not-a-key").has_value());

    // --- backend contract: an unsupported capture backend must not silently
    // fall back to wl_shm ---
    state->backend_unsupported = true;
    cv::Mat image;
    assert(!skl::wcas::proxy::capture_shm(state, image));

    std::printf("input injection integration: ok\n");
    return 0;
}
