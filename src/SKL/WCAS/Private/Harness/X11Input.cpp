#include <stdlib.h>

#include <string>
#include <thread>
#include <vector>
#include <algorithm>
#include <string_view>

#include "SKL/WCAS/Error.h"

#include "SKL/WCAS/Private/Proxy/Input.h"
#include "SKL/WCAS/Private/Proxy/Keymap.h"
#include "SKL/WCAS/Private/Proxy/Keysym.h"
#include "SKL/WCAS/Private/Harness/X11Input.h"

#ifdef WCAS_HAVE_XTEST
#  include <X11/Xlib.h>
#  include <X11/extensions/XTest.h>
#  include <X11/keysym.h>
#endif

namespace skl::wcas {
namespace {

std::error_code code(errc value) { return ::skl::make_error_code(value); }

#ifdef WCAS_HAVE_XTEST
using DisplayHandle = Display;

std::vector<uint32_t> decode_utf8(std::string_view text) {
    std::vector<uint32_t> codepoints;
    for (size_t i = 0; i < text.size();) {
        const auto first = static_cast<unsigned char>(text[i]);
        if (first < 0x80) {
            codepoints.push_back(first);
            ++i;
        } else if ((first & 0xE0) == 0xC0 && i + 1 < text.size()) {
            codepoints.push_back(
                (static_cast<uint32_t>(first & 0x1F) << 6) | (static_cast<unsigned char>(text[i + 1]) & 0x3F));
            i += 2;
        } else if ((first & 0xF0) == 0xE0 && i + 2 < text.size()) {
            codepoints.push_back((static_cast<uint32_t>(first & 0x0F) << 12)
                                 | (static_cast<uint32_t>(static_cast<unsigned char>(text[i + 1]) & 0x3F) << 6)
                                 | (static_cast<unsigned char>(text[i + 2]) & 0x3F));
            i += 3;
        } else if ((first & 0xF8) == 0xF0 && i + 3 < text.size()) {
            codepoints.push_back((static_cast<uint32_t>(first & 0x07) << 18)
                                 | (static_cast<uint32_t>(static_cast<unsigned char>(text[i + 1]) & 0x3F) << 12)
                                 | (static_cast<uint32_t>(static_cast<unsigned char>(text[i + 2]) & 0x3F) << 6)
                                 | (static_cast<unsigned char>(text[i + 3]) & 0x3F));
            i += 4;
        } else {
            ++i;
        }
    }
    return codepoints;
}
#endif

}   // namespace

X11InputDevice::X11InputDevice(SessionContext *state)
    : state_(state) {}
X11InputDevice::~X11InputDevice() {
#ifdef WCAS_HAVE_XTEST
    if (display_ != nullptr) XCloseDisplay(static_cast<DisplayHandle *>(display_));
#endif
    display_ = nullptr;
}

DeviceKind X11InputDevice::kind() { return DeviceKind::KeyboardMouse; }
uint64_t X11InputDevice::id() { return 1; }
std::string X11InputDevice::name() { return "x11-xtest"; }
DeviceCapability X11InputDevice::capabilities() {
    return DeviceCapability::Pointer | DeviceCapability::Keyboard | DeviceCapability::Text;
}

bool X11InputDevice::connected() const { return state_ && state_->bridge && state_->bridge->running(); }

bool X11InputDevice::ensure_display() const {
#ifdef WCAS_HAVE_XTEST
    if (display_ != nullptr) return true;
    if (!state_ || !state_->bridge || !state_->bridge->running()) return false;

    const std::string &xauthority = state_->bridge->xauthority();
    const char *previous = std::getenv("XAUTHORITY");
    const std::string saved = previous != nullptr ? previous : "";
    if (!xauthority.empty()) ::setenv("XAUTHORITY", xauthority.c_str(), 1);
    auto *display = XOpenDisplay(state_->bridge->display().c_str());
    if (!xauthority.empty()) {
        if (previous != nullptr)
            ::setenv("XAUTHORITY", saved.c_str(), 1);
        else
            ::unsetenv("XAUTHORITY");
    }
    if (display == nullptr) return false;
    display_ = display;
    screen_ = DefaultScreen(display);
    root_ = RootWindow(display, screen_);
    return true;
#else
    return false;
#endif
}

bool X11InputDevice::target_origin(int &x, int &y) const {
#ifdef WCAS_HAVE_XTEST
    if (!ensure_display()) return false;
    auto *display = static_cast<DisplayHandle *>(display_);
    Window root = 0;
    Window parent = 0;
    Window *children = nullptr;
    unsigned int count = 0;
    if (!XQueryTree(display, root_, &root, &parent, &children, &count)) return false;

    Window best = 0;
    unsigned long best_area = 0;
    for (unsigned int i = 0; i < count; ++i) {
        XWindowAttributes attributes{};
        if (!XGetWindowAttributes(display, children[i], &attributes)) continue;
        if (attributes.map_state != IsViewable) continue;
        XWindowAttributes ignored{};
        (void)ignored;
        Window child = 0;
        unsigned int width = 0;
        unsigned int height = 0;
        int window_x = 0;
        int window_y = 0;
        unsigned int border = 0;
        unsigned int depth = 0;
        if (!XGetGeometry(display, children[i], &root, &window_x, &window_y, &width, &height, &border, &depth))
            continue;
        const auto area = static_cast<unsigned long>(width) * static_cast<unsigned long>(height);
        if (area > best_area) {
            best_area = area;
            best = children[i];
        }
        (void)child;
    }
    if (children != nullptr) XFree(children);
    if (best == 0) return false;

    int dest_x = 0;
    int dest_y = 0;
    Window child = 0;
    if (!XTranslateCoordinates(display, best, root_, 0, 0, &dest_x, &dest_y, &child)) return false;
    x = dest_x;
    y = dest_y;
    return true;
#else
    (void)x;
    (void)y;
    return false;
#endif
}

bool X11InputDevice::send_key(uint32_t keycode, bool down) const {
#ifdef WCAS_HAVE_XTEST
    if (!ensure_display()) return false;
    auto *display = static_cast<DisplayHandle *>(display_);
    XTestFakeKeyEvent(display, keycode, down ? True : False, CurrentTime);
    XFlush(display);
    return true;
#else
    (void)keycode;
    (void)down;
    return false;
#endif
}

result<void> X11InputDevice::move(cv::Point2d pos) const {
#ifdef WCAS_HAVE_XTEST
    if (!ensure_display()) return code(errc::input_injection_failed);
    auto *display = static_cast<DisplayHandle *>(display_);
    int origin_x = 0;
    int origin_y = 0;
    if (!target_origin(origin_x, origin_y)) return code(errc::input_injection_failed);
    XTestFakeMotionEvent(
        display, screen_, origin_x + static_cast<int>(pos.x), origin_y + static_cast<int>(pos.y), CurrentTime);
    XFlush(display);
    return boost::outcome_v2::success();
#else
    (void)pos;
    return code(errc::feature_not_implemented);
#endif
}

result<void> X11InputDevice::click(cv::Point2d pos, PointerButton button, uint64_t count) const {
#ifdef WCAS_HAVE_XTEST
    auto moved = move(pos);
    if (!moved) return moved;
    auto *display = static_cast<DisplayHandle *>(display_);
    const unsigned int native = button == PointerButton::right ? 3u : button == PointerButton::middle ? 2u : 1u;
    for (uint64_t i = 0; i < count; ++i) {
        XTestFakeButtonEvent(display, native, True, CurrentTime);
        XTestFakeButtonEvent(display, native, False, CurrentTime);
        XFlush(display);
    }
    return boost::outcome_v2::success();
#else
    (void)pos;
    (void)button;
    (void)count;
    return code(errc::feature_not_implemented);
#endif
}

result<void> X11InputDevice::scroll(cv::Point2d pos, cv::Point2i dpos) const {
#ifdef WCAS_HAVE_XTEST
    auto moved = move(pos);
    if (!moved) return moved;
    auto *display = static_cast<DisplayHandle *>(display_);
    const auto press = [&](unsigned int button, int count) {
        for (int i = 0; i < count; ++i) {
            XTestFakeButtonEvent(display, button, True, CurrentTime);
            XTestFakeButtonEvent(display, button, False, CurrentTime);
        }
    };
    if (dpos.y != 0) press(dpos.y > 0 ? 5u : 4u, std::abs(dpos.y));
    if (dpos.x != 0) press(dpos.x > 0 ? 7u : 6u, std::abs(dpos.x));
    XFlush(display);
    return boost::outcome_v2::success();
#else
    (void)pos;
    (void)dpos;
    return code(errc::feature_not_implemented);
#endif
}

result<void> X11InputDevice::abs_drag(cv::Point2d from, cv::Point2d to) const {
#ifdef WCAS_HAVE_XTEST
    auto moved = move(from);
    if (!moved) return moved;
    auto *display = static_cast<DisplayHandle *>(display_);
    XTestFakeButtonEvent(display, 1, True, CurrentTime);
    XFlush(display);
    auto target = move(to);
    XTestFakeButtonEvent(display, 1, False, CurrentTime);
    XFlush(display);
    return target;
#else
    (void)from;
    (void)to;
    return code(errc::feature_not_implemented);
#endif
}

result<void> X11InputDevice::rel_drag(cv::Point2d from, cv::Point2d to) const {
#ifdef WCAS_HAVE_XTEST
    if (!ensure_display()) return code(errc::input_injection_failed);
    auto *display = static_cast<DisplayHandle *>(display_);
    XTestFakeRelativeMotionEvent(
        display, static_cast<int>(to.x - from.x), static_cast<int>(to.y - from.y), CurrentTime);
    XFlush(display);
    return boost::outcome_v2::success();
#else
    (void)from;
    (void)to;
    return code(errc::feature_not_implemented);
#endif
}

result<void> X11InputDevice::press_drag(cv::Point2d from, cv::Point2d to, std::chrono::milliseconds hold) const {
#ifdef WCAS_HAVE_XTEST
    auto moved = move(from);
    if (!moved) return moved;
    auto *display = static_cast<DisplayHandle *>(display_);
    XTestFakeButtonEvent(display, 1, True, CurrentTime);
    XFlush(display);
    if (hold.count() > 0) std::this_thread::sleep_for(hold);
    auto target = move(to);
    XTestFakeButtonEvent(display, 1, False, CurrentTime);
    XFlush(display);
    return target;
#else
    (void)from;
    (void)to;
    (void)hold;
    return code(errc::feature_not_implemented);
#endif
}

result<void> X11InputDevice::key(std::string combination, std::chrono::milliseconds hold) const {
#ifdef WCAS_HAVE_XTEST
    if (!ensure_display()) return code(errc::input_injection_failed);
    auto *display = static_cast<DisplayHandle *>(display_);
    const auto parsed = proxy::parse_key_combination(combination);
    if (parsed.key.empty()) return code(errc::input_injection_failed);

    std::vector<KeyCode> held;
    for (const auto &modifier : parsed.modifiers) {
        const auto keysym = proxy::keysym_for_key(modifier);
        if (!keysym) return code(errc::input_injection_failed);
        const auto keycode = XKeysymToKeycode(display, *keysym);
        if (keycode == 0) return code(errc::input_injection_failed);
        if (!send_key(keycode, true)) return code(errc::input_injection_failed);
        held.push_back(keycode);
    }

    const auto keysym = proxy::keysym_for_key(parsed.key);
    if (!keysym) return code(errc::input_injection_failed);
    const auto keycode = XKeysymToKeycode(display, *keysym);
    if (keycode == 0) return code(errc::input_injection_failed);
    if (!send_key(keycode, true)) return code(errc::input_injection_failed);
    if (hold.count() > 0) std::this_thread::sleep_for(hold);
    if (!send_key(keycode, false)) return code(errc::input_injection_failed);
    for (auto it = held.rbegin(); it != held.rend(); ++it) {
        if (!send_key(*it, false)) return code(errc::input_injection_failed);
    }
    return boost::outcome_v2::success();
#else
    (void)combination;
    (void)hold;
    return code(errc::feature_not_implemented);
#endif
}

result<void> X11InputDevice::type_text(std::string text) const {
#ifdef WCAS_HAVE_XTEST
    if (!ensure_display()) return code(errc::input_injection_failed);
    auto *display = static_cast<DisplayHandle *>(display_);
    for (const auto codepoint : decode_utf8(text)) {
        bool shift = false;
        uint32_t keysym = proxy::keysym_for_codepoint(codepoint);
        if (codepoint >= 'A' && codepoint <= 'Z') {
            keysym = codepoint + ('a' - 'A');
            shift = true;
        }
        const auto keycode = XKeysymToKeycode(display, keysym);
        if (keycode == 0) return code(errc::input_injection_failed);
        const auto shift_keycode = XKeysymToKeycode(display, XK_Shift_L);
        if (shift && shift_keycode == 0) return code(errc::input_injection_failed);
        if (shift && !send_key(shift_keycode, true)) return code(errc::input_injection_failed);
        if (!send_key(keycode, true) || !send_key(keycode, false)) return code(errc::input_injection_failed);
        if (shift && !send_key(shift_keycode, false)) return code(errc::input_injection_failed);
    }
    XFlush(display);
    return boost::outcome_v2::success();
#else
    (void)text;
    return code(errc::feature_not_implemented);
#endif
}

}   // namespace skl::wcas
