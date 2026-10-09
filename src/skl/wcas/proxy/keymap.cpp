#include "keymap.h"

#include <sys/mman.h>

#include <algorithm>
#include <cstring>

namespace skl::wcas::proxy {

std::uint32_t keysym_for_codepoint(std::uint32_t codepoint) {
    switch (codepoint) {
        case '\n': return XKB_KEY_Return;
        case '\t': return XKB_KEY_Tab;
        case '\b': return XKB_KEY_BackSpace;
        case 0x1b: return XKB_KEY_Escape;
        default:   return xkb_utf32_to_keysym(codepoint);
    }
}

Keymap::~Keymap() = default;

Keymap::Keymap(Keymap &&other) noexcept = default;
Keymap &Keymap::operator=(Keymap &&other) noexcept = default;

void Keymap::ContextDeleter::operator()(xkb_context *value) const noexcept {
    if (value) xkb_context_unref(value);
}

void Keymap::KeymapDeleter::operator()(xkb_keymap *value) const noexcept {
    if (value) xkb_keymap_unref(value);
}

void Keymap::StateDeleter::operator()(xkb_state *value) const noexcept {
    if (value) xkb_state_unref(value);
}

Keymap Keymap::parse(const std::string &text) {
    Keymap result;
    if (text.empty()) return result;
    result.context_.reset(xkb_context_new(XKB_CONTEXT_NO_FLAGS));
    if (!result.context_) return Keymap{};
    result.keymap_.reset(xkb_keymap_new_from_string(
        result.context_.get(), text.c_str(), XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS));
    if (!result.keymap_) return Keymap{};
    result.state_.reset(xkb_state_new(result.keymap_.get()));
    if (!result.state_) return Keymap{};
    return result;
}

std::uint32_t Keymap::min_keycode() const noexcept { return keymap_ ? xkb_keymap_min_keycode(keymap_.get()) : 0; }

std::uint32_t Keymap::max_keycode() const noexcept { return keymap_ ? xkb_keymap_max_keycode(keymap_.get()) : 0; }

std::vector<std::uint32_t> Keymap::key_syms(std::uint32_t keycode, std::uint32_t layout, std::uint32_t level) const {
    std::vector<std::uint32_t> symbols;
    if (!keymap_) return symbols;
    const xkb_keysym_t *syms = nullptr;
    const auto count = xkb_keymap_key_get_syms_by_level(keymap_.get(), static_cast<xkb_keycode_t>(keycode),
        static_cast<xkb_layout_index_t>(layout), static_cast<xkb_level_index_t>(level), &syms);
    for (int i = 0; i < count; ++i)
        symbols.push_back(static_cast<std::uint32_t>(syms[i]));
    return symbols;
}

std::optional<Keymap::CharResolution> Keymap::resolve_char(std::uint32_t codepoint) const {
    if (!keymap_) return std::nullopt;
    const auto target = keysym_for_codepoint(codepoint);
    if (!target) return std::nullopt;
    for (auto keycode = min_keycode(); keycode <= max_keycode(); ++keycode) {
        for (const auto symbol : key_syms(keycode, 0, 0)) {
            if (symbol == target) return CharResolution{keycode, false};
        }
        for (const auto symbol : key_syms(keycode, 0, 1)) {
            if (symbol == target) return CharResolution{keycode, true};
        }
    }
    return std::nullopt;
}

std::optional<std::uint32_t> Keymap::find_keysym(std::uint32_t keysym) const {
    if (!keymap_) return std::nullopt;
    for (auto keycode = min_keycode(); keycode <= max_keycode(); ++keycode) {
        for (const auto symbol : key_syms(keycode, 0, 0)) {
            if (symbol == keysym) return keycode;
        }
        for (const auto symbol : key_syms(keycode, 0, 1)) {
            if (symbol == keysym) return keycode;
        }
    }
    return std::nullopt;
}

std::optional<std::uint32_t> Keymap::shift_keycode() const { return find_keysym(XKB_KEY_Shift_L); }

std::uint32_t Keymap::shift_mask() const {
    const auto index = modifier_mask("Shift");
    return index ? *index : 1u;
}

std::optional<std::uint32_t> Keymap::modifier_mask(const char *name) const {
    if (!keymap_ || !name) return std::nullopt;
    const auto index = xkb_keymap_mod_get_index(keymap_.get(), name);
    if (index == XKB_MOD_INVALID) return std::nullopt;
    if (index >= 32) return std::nullopt;
    return 1u << index;
}

std::optional<std::string> read_keymap_text(const ConnState &state) {
    if (state.keymap_fd < 0 || state.keymap_size == 0) return std::nullopt;
    // MAP_PRIVATE: the keymap memfd is owned by the app and may be sealed;
    // the mapping is read-only and never written back.
    void *mapped = ::mmap(nullptr, state.keymap_size, PROT_READ, MAP_PRIVATE, state.keymap_fd, 0);
    if (mapped == MAP_FAILED) return std::nullopt;
    const auto *bytes = static_cast<const char *>(mapped);
    std::size_t length = state.keymap_size;
    // The keymap buffer may be padded with NULs; stop at the first NUL.
    for (std::size_t i = 0; i < state.keymap_size; ++i) {
        if (bytes[i] == '\0') {
            length = i;
            break;
        }
    }
    std::string text(bytes, length);
    ::munmap(mapped, state.keymap_size);
    return text;
}

}   // namespace skl::wcas::proxy
