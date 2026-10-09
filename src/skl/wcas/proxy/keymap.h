#pragma once

#include "conn.h"

#include <xkbcommon/xkbcommon.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace skl::wcas::proxy {

/// Keysym a character is injected with. Control characters map to their
/// dedicated keysyms (`\n` -> Return, `\t` -> Tab, `\b` -> BackSpace,
/// `\x1b` -> Escape); everything else goes through `xkb_utf32_to_keysym`.
std::uint32_t keysym_for_codepoint(std::uint32_t codepoint);

/// RAII wrapper around an xkbcommon keymap compiled from the app's own
/// `wl_keyboard.keymap` text. The app's layout is authoritative: resolving
/// keysyms through it is what makes non-US layouts and Unicode input work,
/// instead of a hand-written evdev table.
class Keymap {
public:
    Keymap() = default;
    ~Keymap();
    Keymap(const Keymap &) = delete;
    Keymap &operator=(const Keymap &) = delete;
    Keymap(Keymap &&other) noexcept;
    Keymap &operator=(Keymap &&other) noexcept;

    [[nodiscard]] bool valid() const noexcept { return keymap_ != nullptr; }

    /// Parse `text` (the keymap memfd contents, NUL-terminated region) into a
    /// compiled keymap. Returns an invalid Keymap when parsing fails.
    static Keymap parse(const std::string &text);

    [[nodiscard]] std::uint32_t min_keycode() const noexcept;
    [[nodiscard]] std::uint32_t max_keycode() const noexcept;

    /// Keysyms produced by `keycode` at `layout`/`level`.
    [[nodiscard]] std::vector<std::uint32_t> key_syms(
        std::uint32_t keycode, std::uint32_t layout, std::uint32_t level) const;

    /// Resolve a literal character to a keycode, preferring the unshifted
    /// level and falling back to Shift.
    struct CharResolution {
        std::uint32_t keycode = 0;
        bool needs_shift = false;
    };
    [[nodiscard]] std::optional<CharResolution> resolve_char(std::uint32_t codepoint) const;

    /// Keycode for the left Shift key, if the layout defines one.
    [[nodiscard]] std::optional<std::uint32_t> shift_keycode() const;

    /// Mask to OR into `wl_keyboard.modifiers.depressed` while Shift is held.
    [[nodiscard]] std::uint32_t shift_mask() const;

    /// Keycode producing `keysym` at either level, if any.
    [[nodiscard]] std::optional<std::uint32_t> find_keysym(std::uint32_t keysym) const;

    /// Modifier mask for an xkb modifier name ("Shift", "Control", "Mod1",
    /// "Mod4"). Returns std::nullopt when the layout does not define it.
    [[nodiscard]] std::optional<std::uint32_t> modifier_mask(const char *name) const;

private:
    struct ContextDeleter {
        void operator()(xkb_context *value) const noexcept;
    };
    struct KeymapDeleter {
        void operator()(xkb_keymap *value) const noexcept;
    };
    struct StateDeleter {
        void operator()(xkb_state *value) const noexcept;
    };

    using ContextPtr = std::unique_ptr<xkb_context, ContextDeleter>;
    using KeymapPtr = std::unique_ptr<xkb_keymap, KeymapDeleter>;
    using StatePtr = std::unique_ptr<xkb_state, StateDeleter>;

    ContextPtr context_;
    KeymapPtr keymap_;
    /// Per-query scratch state; only used from the injection thread.
    mutable StatePtr state_;
};

/// Read the keymap text captured from `wl_keyboard.keymap`. Returns
/// std::nullopt when no keymap has been captured yet or when the mapping
/// cannot be established.
std::optional<std::string> read_keymap_text(const ConnState &state);

}   // namespace skl::wcas::proxy
