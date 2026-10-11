#pragma once

#include "conn.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace skl::wcas::proxy {

/// Surface synthetic input should be routed to: the keyboard-focused surface
/// when known, otherwise the most recently committed one. Mirrors the Rust
/// reference `target_surface`. Returns 0 when the app has no surface yet.
std::uint32_t target_surface(const std::shared_ptr<ConnState> &state);

/// Surface to send `wl_keyboard.enter` for. Wine maps keyboard focus to a window
/// through the surface that carries the `xdg_toplevel` role, so focusing the
/// rendering subsurface (what `target_surface` returns) can make the app drop
/// key events. Walks up the subsurface parent chain from `preferred` to the
/// surface that owns a toplevel, falling back to any toplevel surface and then
/// to `preferred`.
std::uint32_t keyboard_surface(const std::shared_ptr<ConnState> &state, std::uint32_t preferred);

/// Send wl_pointer.leave event to the app.
bool inject_pointer_leave(const std::shared_ptr<ConnState> &state, std::uint32_t surface_id);

/// Send wl_pointer.enter event to the app.
bool inject_pointer_enter(const std::shared_ptr<ConnState> &state, std::uint32_t surface_id, double sx, double sy);

/// Ensure the synthetic pointer has focus on `surface`, sending leave/enter as
/// needed. Returns false when the app has no wl_pointer or focus cannot be
/// established.
bool ensure_pointer_focus(const std::shared_ptr<ConnState> &state, std::uint32_t surface_id, double x, double y);

/// Send wl_pointer.motion event to the app.
bool inject_pointer_motion(const std::shared_ptr<ConnState> &state, double sx, double sy);

/// Send wl_pointer.button event to the app (pressed=true for press, false for release).
bool inject_pointer_button(const std::shared_ptr<ConnState> &state, std::uint32_t button, bool pressed);

/// Send wl_pointer.axis event to the app.
/// axis: 0=vertical, 1=horizontal
bool inject_pointer_axis(const std::shared_ptr<ConnState> &state, std::uint32_t axis, double value);

/// Send wl_pointer.frame event to the app.
/// Frame marks the end of a pointer event sequence.
bool inject_pointer_frame(const std::shared_ptr<ConnState> &state);

/// Send wl_pointer.axis_source event.
/// source: 0=wheel, 1=finger, 2=continuous, 3=wheel_tilt
bool inject_pointer_axis_source(const std::shared_ptr<ConnState> &state, std::uint32_t source);

/// Ensure synthetic keyboard events are routed to `surface`, sending
/// leave/enter as needed. Repeating an enter for the already-focused surface
/// is a protocol violation, so focus is tracked.
bool ensure_keyboard_focus(const std::shared_ptr<ConnState> &state, std::uint32_t surface_id);

/// Inject a raw wl_keyboard.key event. `xkb_keycode` is xkbcommon's keycode
/// (evdev + 8); the wire value is translated back to evdev.
bool inject_key_raw(const std::shared_ptr<ConnState> &state, std::uint32_t xkb_keycode, bool pressed);

/// Send a wl_keyboard.modifiers event.
bool inject_modifiers(const std::shared_ptr<ConnState> &state, std::uint32_t depressed, std::uint32_t latched,
    std::uint32_t locked, std::uint32_t group);

/// Type `text` into the app. Prefers text-input-v3 `commit_string` when the
/// app has it enabled (layout-independent and Unicode-correct); otherwise
/// resolves each character through the app's own xkb keymap.
bool inject_text(const std::shared_ptr<ConnState> &state, const std::string &text);

/// Parser for key combinations such as `ctrl+shift+p`.
struct KeyCombination {
    std::string key;
    std::vector<std::string> modifiers;
};
KeyCombination parse_key_combination(const std::string &combination);

/// XKB modifier name ("Shift", "Control", "Mod1", "Mod4") for an agent-facing
/// modifier name. Returns std::nullopt for unknown names.
std::optional<const char *> xkb_modifier_name(const std::string &name);

/// Acquire and return the next serial number for input events.
inline std::uint32_t next_serial(const std::shared_ptr<ConnState> &state) {
    return state->next_serial.fetch_add(1, std::memory_order_relaxed);
}

/// Returns a monotonically increasing timestamp in milliseconds.
std::uint32_t input_timestamp_ms();

}   // namespace skl::wcas::proxy
