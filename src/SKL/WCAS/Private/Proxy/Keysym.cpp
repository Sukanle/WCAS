#include <ctype.h>

#include <algorithm>

#include "SKL/WCAS/Private/Proxy/Keysym.h"

namespace skl::wcas::proxy {
namespace {

std::string trimmed(const std::string &value) {
    const auto begin = value.find_first_not_of(" \t\n\r\f\v");
    if (begin == std::string::npos) return {};
    const auto end = value.find_last_not_of(" \t\n\r\f\v");
    return value.substr(begin, end - begin + 1);
}

std::string lowered(std::string value) {
    std::transform(
        value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

}   // namespace

optional<uint32_t> keysym_for_key(const std::string &name) {
    const auto text = trimmed(name);
    if (text.empty()) return std::nullopt;

    if (text.size() == 1) {
        const auto character = static_cast<unsigned char>(text[0]);
        // Single characters are keyed by their lower-case ASCII value, which
        // matches the Rust reference (`A` and `a` both resolve to 0x61).
        if (character >= 'A' && character <= 'Z') return static_cast<uint32_t>(character + 0x20);
        return static_cast<uint32_t>(character);
    }

    const auto key = lowered(text);
    if (key == "return" || key == "enter") return 0xff0du;
    if (key == "tab") return 0xff09u;
    if (key == "escape" || key == "esc") return 0xff1bu;
    if (key == "backspace") return 0xff08u;
    if (key == "delete" || key == "del") return 0xffffu;
    if (key == "insert") return 0xff63u;
    if (key == "home") return 0xff50u;
    if (key == "end") return 0xff57u;
    if (key == "pageup" || key == "page_up") return 0xff55u;
    if (key == "pagedown" || key == "page_down") return 0xff56u;
    if (key == "left") return 0xff51u;
    if (key == "up") return 0xff52u;
    if (key == "right") return 0xff53u;
    if (key == "down") return 0xff54u;
    if (key == "shift" || key == "shiftleft" || key == "shift_left") return 0xffe1u;
    if (key == "shiftright" || key == "shift_right") return 0xffe2u;
    if (key == "control" || key == "ctrl" || key == "controlleft" || key == "control_left") return 0xffe3u;
    if (key == "controlright" || key == "control_right") return 0xffe4u;
    if (key == "alt" || key == "altleft" || key == "alt_left" || key == "option") return 0xffe9u;
    if (key == "altright" || key == "alt_right") return 0xffeau;
    if (key == "super"
        || key == "cmd"
        || key == "command"
        || key == "meta"
        || key == "superleft"
        || key == "super_left")
        return 0xffebu;
    if (key == "superright" || key == "super_right") return 0xffecu;
    if (key == "space") return 0x0020u;
    if (key == "capslock" || key == "caps_lock") return 0xffe5u;

    if (key.size() > 1 && key[0] == 'f') {
        const auto digits = key.substr(1);
        if (digits.find_first_not_of("0123456789") == std::string::npos) {
            const auto number = std::stoul(digits);
            if (number >= 1 && number <= 24) return static_cast<uint32_t>(0xffbeu + number - 1);
        }
    }

    return std::nullopt;
}

}   // namespace skl::wcas::proxy
