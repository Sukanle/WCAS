#include "input.h"

#include "debug.h"
#include "keymap.h"
#include "keysym.h"

#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <vector>

namespace skl::wcas::proxy {
namespace {

// Wayland wire helper: pack a wl_fixed_t from a double
inline std::uint32_t wl_fixed(double v) { return static_cast<std::uint32_t>(static_cast<std::int32_t>(v * 256.0)); }

// Build the 8-byte Wayland message header at the front of a vector.
// The vector must already have object_id at [0] and will get size|opcode at [1].
inline void wl_header(std::vector<std::uint32_t> &msg, std::uint32_t object_id, std::uint32_t opcode) {
    const auto size = static_cast<std::uint32_t>(msg.size() * 4);
    msg[0] = object_id;
    msg[1] = (size << 16) | opcode;
}

/// Send a Wayland message (without FDs) on the given fd.
bool send_message(int fd, const std::vector<std::uint32_t> &msg) {
    if (fd < 0) return false;
    struct iovec iov;
    iov.iov_base = const_cast<std::uint32_t *>(msg.data());
    iov.iov_len = msg.size() * 4;
    struct msghdr hdr = {};
    hdr.msg_iov = &iov;
    hdr.msg_iovlen = 1;
    return ::sendmsg(fd, &hdr, MSG_NOSIGNAL) == static_cast<ssize_t>(iov.iov_len);
}

/// Append a Wayland string argument (length word + NUL-terminated, padded).
void put_string(std::vector<std::uint32_t> &msg, const std::string &value) {
    const auto length = static_cast<std::uint32_t>(value.size() + 1);
    msg.push_back(length);
    const auto words = (length + 3u) / 4u;
    const auto base = msg.size();
    msg.resize(base + words, 0);
    std::memcpy(msg.data() + base, value.c_str(), length);
}

/// text-input-v3 commit_string + done. Layout-independent, so this is the
/// preferred path whenever the app enabled the interface.
bool inject_text_input(const std::shared_ptr<ConnState> &state, const std::string &text) {
    std::vector<std::uint32_t> commit(2);
    wl_header(commit, state->text_input_obj, 3);   // zwp_text_input_v3.commit_string
    put_string(commit, text);
    wl_header(commit, state->text_input_obj, 3);   // size changed by put_string
    if (!send_message(state->client_fd, commit)) return false;

    std::vector<std::uint32_t> done(3);
    done[2] = state->text_input_commit_serial;
    wl_header(done, state->text_input_obj, 5);   // zwp_text_input_v3.done
    return send_message(state->client_fd, done);
}

std::string lowered(std::string value) {
    std::transform(
        value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

}   // anonymous namespace

std::uint32_t input_timestamp_ms() {
    static const auto epoch = std::chrono::steady_clock::now();
    return static_cast<std::uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - epoch).count());
}

std::uint32_t target_surface(const std::shared_ptr<ConnState> &state) {
    if (!state) return 0;
    const auto has_frame = [&state](std::uint32_t sid) {
        const auto it = state->surfaces.find(sid);
        if (it == state->surfaces.end()) return false;
        const auto &s = it->second;
        return s.committed_frame.has_value()
            || s.committed_buffer.has_value()
            || s.committed_dmabuf_frame.has_value()
            || s.committed_dmabuf.has_value();
    };
    // An explicitly selected window wins over the focus/commit heuristics so a
    // later-started child window (overlay, dialog) can be controlled.
    if (state->preferred_surface && has_frame(state->preferred_surface)) return state->preferred_surface;
    // A focused surface only qualifies when it actually rendered something:
    // with multiple outputs/windows the compositor may report focus on a
    // surface that has no committed frame, and injecting there misses the app.
    if (state->keyboard_focus && has_frame(state->keyboard_focus)) return state->keyboard_focus;
    if (state->focused_surface && has_frame(state->focused_surface)) return state->focused_surface;
    std::uint32_t best = 0;
    std::uint64_t best_commits = 0;
    for (const auto &[sid, surf] : state->surfaces) {
        if (!surf.committed_frame.has_value()
            && !surf.committed_buffer.has_value()
            && !surf.committed_dmabuf_frame.has_value()
            && !surf.committed_dmabuf.has_value())
            continue;
        if (best == 0 || surf.commit_count >= best_commits) {
            best_commits = surf.commit_count;
            best = sid;
        }
    }
    return best;
}

std::uint32_t keyboard_surface(const std::shared_ptr<ConnState> &state, std::uint32_t preferred) {
    if (!state) return preferred;
    std::lock_guard lock(state->data_mutex);
    std::uint32_t sid = preferred;
    for (int depth = 0; depth < 16 && sid; ++depth) {
        if (state->surface_to_toplevel.find(sid) != state->surface_to_toplevel.end()) return sid;
        const auto parent = state->surface_to_parent_surface.find(sid);
        if (parent == state->surface_to_parent_surface.end()) break;
        sid = parent->second;
    }
    // No ancestor owns a toplevel (or `preferred` was 0): fall back to any
    // surface that does.
    for (const auto &[toplevel_surface, toplevel] : state->surface_to_toplevel) {
        (void)toplevel;
        if (toplevel_surface) return toplevel_surface;
    }
    return preferred;
}

// ---- wl_pointer events ----
bool inject_pointer_leave(const std::shared_ptr<ConnState> &state, std::uint32_t surface_id) {
    if (!state || !state->pointer_obj) return false;
    std::lock_guard lock(state->data_mutex);
    std::vector<std::uint32_t> msg(4);
    wl_header(msg, state->pointer_obj, 1);   // opcode 1 = leave
    msg[2] = next_serial(state);
    msg[3] = surface_id;
    return send_message(state->client_fd, msg);
}

bool inject_pointer_enter(const std::shared_ptr<ConnState> &state, std::uint32_t surface_id, double sx, double sy) {
    if (!state || !state->pointer_obj) return false;
    std::lock_guard lock(state->data_mutex);
    std::vector<std::uint32_t> msg(6);
    wl_header(msg, state->pointer_obj, 0);   // opcode 0 = enter
    msg[2] = next_serial(state);
    msg[3] = surface_id;
    msg[4] = wl_fixed(sx);
    msg[5] = wl_fixed(sy);
    return send_message(state->client_fd, msg);
}

bool ensure_pointer_focus(const std::shared_ptr<ConnState> &state, std::uint32_t surface_id, double x, double y) {
    if (!state || !surface_id || !state->pointer_obj) return false;
    if (state->pointer_focus == surface_id) return true;
    if (state->pointer_focus) {
        if (!inject_pointer_leave(state, state->pointer_focus)) return false;
        if (!inject_pointer_frame(state)) return false;
    }
    if (!inject_pointer_enter(state, surface_id, x, y)) return false;
    if (!inject_pointer_frame(state)) return false;
    state->pointer_focus = surface_id;
    state->focused_surface = surface_id;
    return true;
}

bool inject_pointer_motion(const std::shared_ptr<ConnState> &state, double sx, double sy) {
    if (!state || !state->pointer_obj) return false;
    debug_trace(DebugCategory::Input, fmt::format("pointer motion ({:.1f}, {:.1f})", sx, sy));
    std::lock_guard lock(state->data_mutex);
    std::vector<std::uint32_t> msg(5);
    wl_header(msg, state->pointer_obj, 2);   // opcode 2 = motion
    msg[2] = input_timestamp_ms();
    msg[3] = wl_fixed(sx);
    msg[4] = wl_fixed(sy);
    return send_message(state->client_fd, msg);
}

bool inject_relative_motion(const std::shared_ptr<ConnState> &state, double dx, double dy) {
    if (!state) return false;
    std::lock_guard lock(state->data_mutex);
    std::uint32_t relative_pointer = 0;
    for (const auto &[object, role] : state->object_role_enum) {
        if (role == Role::RelativePointer) {
            relative_pointer = object;
            break;
        }
    }
    if (!relative_pointer) return false;
    debug_trace(DebugCategory::Input, fmt::format("relative motion ({:.1f}, {:.1f})", dx, dy));
    // zwp_relative_pointer_v1.relative_motion: utime_hi, utime_lo, dx, dy,
    // dx_unaccel, dy_unaccel (all fixed except the split 64-bit timestamp).
    const std::uint64_t utime = static_cast<std::uint64_t>(input_timestamp_ms()) * 1'000ULL;
    std::vector<std::uint32_t> msg(8);
    wl_header(msg, relative_pointer, 0);
    msg[2] = static_cast<std::uint32_t>(utime >> 32);
    msg[3] = static_cast<std::uint32_t>(utime & 0xffffffffULL);
    msg[4] = wl_fixed(dx);
    msg[5] = wl_fixed(dy);
    msg[6] = wl_fixed(dx);
    msg[7] = wl_fixed(dy);
    return send_message(state->client_fd, msg);
}

bool inject_pointer_button(const std::shared_ptr<ConnState> &state, std::uint32_t button, bool pressed) {
    if (!state || !state->pointer_obj) return false;
    debug_trace(DebugCategory::Input, fmt::format("pointer button {} pressed={}", button, pressed ? 1 : 0));
    std::lock_guard lock(state->data_mutex);
    std::vector<std::uint32_t> msg(6);
    wl_header(msg, state->pointer_obj, 3);   // opcode 3 = button
    msg[2] = next_serial(state);
    msg[3] = input_timestamp_ms();
    msg[4] = button;
    msg[5] = pressed ? 1u : 0u;
    return send_message(state->client_fd, msg);
}

bool inject_pointer_axis(const std::shared_ptr<ConnState> &state, std::uint32_t axis, double value) {
    if (!state || !state->pointer_obj) return false;
    std::lock_guard lock(state->data_mutex);
    std::vector<std::uint32_t> msg(5);
    wl_header(msg, state->pointer_obj, 4);   // opcode 4 = axis
    msg[2] = input_timestamp_ms();
    msg[3] = axis;
    msg[4] = wl_fixed(value);
    return send_message(state->client_fd, msg);
}

bool inject_pointer_frame(const std::shared_ptr<ConnState> &state) {
    if (!state || !state->pointer_obj) return false;
    std::lock_guard lock(state->data_mutex);
    std::vector<std::uint32_t> msg(2);
    wl_header(msg, state->pointer_obj, 5);   // opcode 5 = frame
    return send_message(state->client_fd, msg);
}

bool inject_pointer_axis_source(const std::shared_ptr<ConnState> &state, std::uint32_t source) {
    if (!state || !state->pointer_obj) return false;
    std::lock_guard lock(state->data_mutex);
    std::vector<std::uint32_t> msg(3);
    wl_header(msg, state->pointer_obj, 6);   // opcode 6 = axis_source
    msg[2] = source;
    return send_message(state->client_fd, msg);
}

// ---- wl_keyboard events ----

bool ensure_keyboard_focus(const std::shared_ptr<ConnState> &state, std::uint32_t surface_id) {
    if (!state || !surface_id || !state->keyboard_obj) return false;
    if (state->keyboard_focus == surface_id) return true;
    std::lock_guard lock(state->data_mutex);
    if (state->keyboard_focus) {
        std::vector<std::uint32_t> leave(4);
        wl_header(leave, state->keyboard_obj, 2);   // opcode 2 = leave
        leave[2] = next_serial(state);
        leave[3] = state->keyboard_focus;
        if (!send_message(state->client_fd, leave)) return false;
    }
    // wl_keyboard.enter: serial(u), surface(o), keys(a).
    // With an empty keys array: [length word = 0].
    std::vector<std::uint32_t> enter(5);
    wl_header(enter, state->keyboard_obj, 1);   // opcode 1 = enter
    enter[2] = next_serial(state);
    enter[3] = surface_id;
    enter[4] = 0;
    if (!send_message(state->client_fd, enter)) return false;
    // A real compositor always follows `enter` with a `modifiers` event; some
    // toolkits (notably Wine/Unity) ignore key events until they receive one.
    std::vector<std::uint32_t> modifiers(7);
    wl_header(modifiers, state->keyboard_obj, 4);   // opcode 4 = modifiers
    modifiers[2] = next_serial(state);
    modifiers[3] = 0;   // depressed
    modifiers[4] = 0;   // latched
    modifiers[5] = 0;   // locked
    modifiers[6] = 0;   // group
    if (!send_message(state->client_fd, modifiers)) return false;
    state->keyboard_focus = surface_id;
    return true;
}

bool inject_key_raw(const std::shared_ptr<ConnState> &state, std::uint32_t xkb_keycode, bool pressed) {
    if (!state || !state->keyboard_obj || !xkb_keycode) return false;
    debug_trace(DebugCategory::Input, fmt::format("key raw {} pressed={}", xkb_keycode, pressed ? 1 : 0));
    std::lock_guard lock(state->data_mutex);
    std::vector<std::uint32_t> msg(6);
    wl_header(msg, state->keyboard_obj, 3);   // opcode 3 = key
    msg[2] = next_serial(state);
    msg[3] = input_timestamp_ms();
    // wl_keyboard.key carries evdev keycodes; xkbcommon numbers keysyms from
    // evdev + 8, so translate back before putting the value on the wire.
    msg[4] = xkb_keycode > 8 ? xkb_keycode - 8 : 0;
    msg[5] = pressed ? 1u : 0u;
    return send_message(state->client_fd, msg);
}

bool inject_modifiers(const std::shared_ptr<ConnState> &state, std::uint32_t depressed, std::uint32_t latched,
    std::uint32_t locked, std::uint32_t group) {
    if (!state || !state->keyboard_obj) return false;
    std::lock_guard lock(state->data_mutex);
    std::vector<std::uint32_t> msg(7);
    wl_header(msg, state->keyboard_obj, 4);   // opcode 4 = modifiers
    msg[2] = next_serial(state);
    msg[3] = depressed;
    msg[4] = latched;
    msg[5] = locked;
    msg[6] = group;
    return send_message(state->client_fd, msg);
}

KeyCombination parse_key_combination(const std::string &combination) {
    KeyCombination parsed;
    std::vector<std::string> tokens;
    std::string current;
    for (const char c : combination) {
        if (c == '+' || c == '-') {
            tokens.push_back(current);
            current.clear();
        } else {
            current += c;
        }
    }
    tokens.push_back(current);
    for (auto &token : tokens) {
        const auto begin = token.find_first_not_of(" \t");
        if (begin == std::string::npos) continue;
        const auto end = token.find_last_not_of(" \t");
        parsed.modifiers.push_back(lowered(token.substr(begin, end - begin + 1)));
    }
    if (parsed.modifiers.empty()) return parsed;
    // The last token is the key; everything before it is a modifier. A
    // trailing modifier (`ctrl+`) is rejected by the caller because its name
    // never resolves to a keysym.
    parsed.key = parsed.modifiers.back();
    parsed.modifiers.pop_back();
    return parsed;
}

optional<const char *> xkb_modifier_name(const std::string &name) {
    const auto key = lowered(name);
    if (key == "shift") return "Shift";
    if (key == "ctrl" || key == "control") return "Control";
    if (key == "alt" || key == "option") return "Mod1";
    if (key == "super" || key == "cmd" || key == "command" || key == "meta") return "Mod4";
    return std::nullopt;
}

bool inject_text(const std::shared_ptr<ConnState> &state, const std::string &text) {
    if (!state || text.empty()) return false;

    // text-input-v3 is layout-independent and handles arbitrary Unicode, so
    // prefer it whenever the app has enabled it.
    if (state->text_input_enabled && state->text_input_obj) {
        std::lock_guard lock(state->data_mutex);
        return inject_text_input(state, text);
    }

    const auto surface = target_surface(state);
    if (!surface || !state->keyboard_obj) return false;
    const auto focus_surface = keyboard_surface(state, surface);

    const auto keymap_text = read_keymap_text(*state);
    if (!keymap_text) return false;
    const auto keymap = Keymap::parse(*keymap_text);
    if (!keymap.valid()) return false;

    if (!ensure_keyboard_focus(state, focus_surface)) return false;

    // Split into runs of plain characters and special keys so the keymap is
    // searched once per run instead of once per character.
    enum class Kind {
        Plain,
        Special
    };
    struct Item {
        Kind kind = Kind::Plain;
        std::string text;
        std::uint32_t keysym = 0;
    };
    std::vector<Item> items;
    std::string plain;
    for (const char ch : text) {
        const auto codepoint = static_cast<std::uint32_t>(static_cast<unsigned char>(ch));
        if (codepoint == '\n' || codepoint == '\t' || codepoint == '\b' || codepoint == 0x1b) {
            if (!plain.empty()) {
                items.push_back(Item{Kind::Plain, plain, 0});
                plain.clear();
            }
            items.push_back(Item{Kind::Special, {}, keysym_for_codepoint(codepoint)});
        } else {
            plain += ch;
        }
    }
    if (!plain.empty()) items.push_back(Item{Kind::Plain, plain, 0});

    const auto shift_keycode = keymap.shift_keycode();
    const auto shift_mask = keymap.shift_mask();
    std::uint32_t depressed = 0;

    for (const auto &item : items) {
        if (item.kind == Kind::Special) {
            const auto keycode = keymap.find_keysym(item.keysym);
            if (!keycode) return false;
            if (!inject_key_raw(state, *keycode, true)) return false;
            if (!inject_key_raw(state, *keycode, false)) return false;
            continue;
        }
        for (const char ch : item.text) {
            const auto codepoint = static_cast<std::uint32_t>(static_cast<unsigned char>(ch));
            // Non-ASCII bytes cannot be produced by a single keysym on a
            // plain layout; they require text-input-v3.
            if (codepoint > 0X7F) return false;
            const auto resolution = keymap.resolve_char(codepoint);
            if (!resolution) return false;
            if (resolution->needs_shift) {
                if (!shift_keycode) return false;
                if (!(depressed & shift_mask)) {
                    if (!inject_key_raw(state, *shift_keycode, true)) return false;
                    depressed |= shift_mask;
                    if (!inject_modifiers(state, depressed, 0, 0, 0)) return false;
                }
            } else if (depressed & shift_mask) {
                if (!inject_key_raw(state, *shift_keycode, false)) return false;
                depressed &= ~shift_mask;
                if (!inject_modifiers(state, depressed, 0, 0, 0)) return false;
            }
            if (!inject_key_raw(state, resolution->keycode, true)) return false;
            if (!inject_key_raw(state, resolution->keycode, false)) return false;
        }
    }

    if (shift_keycode && (depressed & shift_mask)) {
        if (!inject_key_raw(state, *shift_keycode, false)) return false;
        if (!inject_modifiers(state, 0, 0, 0, 0)) return false;
    }
    return true;
}

}   // namespace skl::wcas::proxy
