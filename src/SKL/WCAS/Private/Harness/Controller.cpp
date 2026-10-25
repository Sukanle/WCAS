#include <stdint.h>

#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <utility>
#include <iterator>
#include <algorithm>

#include "SKL/WCAS/Error.h"
#include "SKL/WCAS/Controller.h"

#include "SKL/WCAS/Private/Debug.h"
#include "SKL/WCAS/Private/Harness/ControlledAppImpl.h"
#include "SKL/WCAS/Private/Harness/X11Input.h"
#include "SKL/WCAS/Private/Proxy/Conn.h"
#include "SKL/WCAS/Private/Proxy/Input.h"
#include "SKL/WCAS/Private/Proxy/Keymap.h"
#include "SKL/WCAS/Private/Proxy/Keysym.h"

#include <fmt/base.h>

namespace skl::wcas {
namespace {

using skl::abix::poly::InterfaceMethod;
using skl::abix::poly::InterfaceRegistry;
using skl::abix::poly::InterfaceStatus;

std::error_code code(errc value) { return make_error_code(value); }

uint32_t input_target(const std::shared_ptr<proxy::ConnState> &session) {
    return session ? proxy::target_surface(session) : 0;
}

// Callers express pointer positions in frame (buffer) pixels, but wl_pointer
// expects surface-local (logical) pixels. Divide by the surface scale, which is
// 1.0 when the app does not use buffer scale / wp_viewport.
double to_logical(const std::shared_ptr<proxy::ConnState> &session, uint32_t surface, double value) {
    const double scale = proxy::input_scale(session, surface);
    return scale > 0.0 ? value / scale : value;
}

// The keyboard/mouse provider is a plain class: no base class, no virtual
// functions. Its members are the adapters themselves, so `InterfaceMethod`
// tables can be built with SKL_ABIX_INTERFACE_MEMBER_ENTRY instead of one free
// function per slot.
class KeyboardMouseDevice {
public:
    KeyboardMouseDevice(ControlledApp *app, ControlledApp::Impl *state) noexcept
        : app_(app)
        , state_(state) {}

    // DeviceInfo
    //
    // Keep these members non-noexcept: Clang currently sees the `const` and
    // `const noexcept` member_adapter partial specializations as ambiguous
    // when SKL_ABIX_INTERFACE_MEMBER_ENTRY takes their address. GCC accepts
    // both, but omitting noexcept is portable and has no effect on the adapter
    // function type used by the interface.
    [[nodiscard]] static DeviceKind kind() noexcept { return DeviceKind::KeyboardMouse; }
    [[nodiscard]] static uint64_t id() noexcept { return 0; }
    [[nodiscard]] static std::string_view name() { return "keyboard-mouse"; }
    [[nodiscard]] static DeviceCapability capabilities() noexcept {
        return DeviceCapability::Pointer | DeviceCapability::Keyboard | DeviceCapability::Text;
    }
    [[nodiscard]] bool connected() const noexcept;

    // PointerController
    [[nodiscard]] result<void> move(cv::Point2d pos) const;
    [[nodiscard]] result<void> click(cv::Point2d pos, PointerButton button, uint64_t count) const;
    [[nodiscard]] result<void> scroll(cv::Point2d pos, cv::Point2i dpos) const;
    [[nodiscard]] result<void> abs_drag(cv::Point2d from, cv::Point2d to) const;
    [[nodiscard]] result<void> rel_drag(cv::Point2d from, cv::Point2d to) const;
    [[nodiscard]] result<void> press_drag(cv::Point2d from, cv::Point2d to, std::chrono::milliseconds hold) const;

    // KeyboardController
    [[nodiscard]] result<void> key(std::string combination, std::chrono::milliseconds hold) const;
    [[nodiscard]] result<void> type_text(std::string text) const;

private:
    [[nodiscard]] bool ensure_session() const;
    [[nodiscard]] std::shared_ptr<proxy::ConnState> session() const;
    [[nodiscard]] result<void> drag(cv::Point2d from, cv::Point2d to, std::chrono::milliseconds hold) const;

    ControlledApp *app_ = nullptr;
    ControlledApp::Impl *state_ = nullptr;
};

bool KeyboardMouseDevice::connected() const noexcept {
    return state_ && state_->session && state_->session->connected.load();
}

bool KeyboardMouseDevice::ensure_session() const {
    if (!app_ || !state_) return false;
    if (!app_->ensure_active_session()) return false;
    return state_->session && state_->session->connected.load();
}

std::shared_ptr<proxy::ConnState> KeyboardMouseDevice::session() const { return state_ ? state_->session : nullptr; }

result<void> KeyboardMouseDevice::move(cv::Point2d pos) const {
    if (!ensure_session()) return code(errc::input_injection_failed);
    auto state = session();
    if (!state) return code(errc::input_injection_failed);

    const auto target = input_target(state);
    if (!target) return code(errc::input_injection_failed);
    const double lx = to_logical(state, target, pos.x);
    const double ly = to_logical(state, target, pos.y);
    if (!proxy::ensure_pointer_focus(state, target, lx, ly)) return code(errc::input_injection_failed);

    if (!proxy::inject_pointer_motion(state, lx, ly)) return code(errc::input_injection_failed);
    if (!proxy::inject_pointer_frame(state)) return code(errc::input_injection_failed);
    return boost::outcome_v2::success();
}

result<void> KeyboardMouseDevice::click(cv::Point2d pos, PointerButton button, uint64_t count) const {
    if (!ensure_session()) {
        debug_trace(DebugCategory::Input, "click: no active native session");
        return code(errc::input_injection_failed);
    }
    auto state = session();
    if (!state) {
        debug_trace(DebugCategory::Input, "click: session disappeared");
        return code(errc::input_injection_failed);
    }

    // Linux evdev BTN_ codes: left, right, middle.
    static constexpr uint32_t button_map[] = {0x110, 0x111, 0x112};
    const auto index = static_cast<size_t>(button);
    if (index >= std::size(button_map)) return code(errc::input_injection_failed);
    const auto native = button_map[index];

    const auto target = input_target(state);
    if (!target) {
        debug_trace(DebugCategory::Input, fmt::format("click: no target surface (preferred={} pointer_obj={})",
                                              state->preferred_surface, state->pointer_obj));
        return code(errc::input_injection_failed);
    }
    debug_trace(DebugCategory::Input,
        fmt::format("click target={} pointer_obj={} pointer_focus={} pos=({:.1f}, {:.1f}) button={}", target,
            state->pointer_obj, state->pointer_focus, pos.x, pos.y, native));
    const double lx = to_logical(state, target, pos.x);
    const double ly = to_logical(state, target, pos.y);
    if (!proxy::ensure_pointer_focus(state, target, lx, ly)) {
        debug_trace(DebugCategory::Input, "click: ensure_pointer_focus failed (no wl_pointer or enter/leave)");
        return code(errc::input_injection_failed);
    }

    if (!proxy::inject_pointer_motion(state, lx, ly)) {
        debug_trace(DebugCategory::Input, "click: pointer motion failed");
        return code(errc::input_injection_failed);
    }
    for (uint64_t i = 0; i < count; ++i) {
        if (!proxy::inject_pointer_button(state, native, true)) {
            debug_trace(DebugCategory::Input, "click: pointer button press failed");
            return code(errc::input_injection_failed);
        }
        if (!proxy::inject_pointer_frame(state)) return code(errc::input_injection_failed);
        if (!proxy::inject_pointer_button(state, native, false)) {
            debug_trace(DebugCategory::Input, "click: pointer button release failed");
            return code(errc::input_injection_failed);
        }
        if (!proxy::inject_pointer_frame(state)) return code(errc::input_injection_failed);
    }
    return boost::outcome_v2::success();
}

result<void> KeyboardMouseDevice::scroll(cv::Point2d pos, cv::Point2i dpos) const {
    if (!ensure_session()) return code(errc::input_injection_failed);
    auto state = session();
    if (!state) return code(errc::input_injection_failed);

    const auto target = input_target(state);
    if (!target) return code(errc::input_injection_failed);
    if (!proxy::ensure_pointer_focus(state, target, to_logical(state, target, pos.x), to_logical(state, target, pos.y)))
        return code(errc::input_injection_failed);

    // axis 0 = vertical, axis 1 = horizontal. Source 0 = wheel, 3 = wheel_tilt.
    if (dpos.y != 0) {
        if (!proxy::inject_pointer_axis_source(state, 0)) return code(errc::input_injection_failed);
        if (!proxy::inject_pointer_axis(state, 0, static_cast<double>(dpos.y) * 15.0))
            return code(errc::input_injection_failed);
        if (!proxy::inject_pointer_frame(state)) return code(errc::input_injection_failed);
    }
    if (dpos.x != 0) {
        if (!proxy::inject_pointer_axis_source(state, 3)) return code(errc::input_injection_failed);
        if (!proxy::inject_pointer_axis(state, 1, static_cast<double>(dpos.x) * 15.0))
            return code(errc::input_injection_failed);
        if (!proxy::inject_pointer_frame(state)) return code(errc::input_injection_failed);
    }
    return boost::outcome_v2::success();
}

result<void> KeyboardMouseDevice::key(std::string combination, std::chrono::milliseconds hold) const {
    if (!ensure_session()) return code(errc::input_injection_failed);
    auto state = session();
    if (!state || !state->keyboard_obj) return code(errc::input_injection_failed);

    const auto surface = input_target(state);
    if (!surface) return code(errc::input_injection_failed);
    // Wine maps keyboard focus to the window through the surface that carries
    // the xdg_toplevel, so enter that one rather than the rendering subsurface.
    const auto focus_surface = proxy::keyboard_surface(state, surface);
    debug_trace(DebugCategory::Input,
        fmt::format("key '{}' session_pid={} keyboard_obj={} target_surface={} focus_surface={}", combination,
            static_cast<long>(state->owner_pid), state->keyboard_obj, surface, focus_surface));

    const auto parsed = proxy::parse_key_combination(combination);
    if (parsed.key.empty()) return code(errc::input_injection_failed);
    const auto keymap_text = proxy::read_keymap_text(*state);
    if (!keymap_text) return code(errc::input_injection_failed);
    const auto keymap = proxy::Keymap::parse(*keymap_text);
    if (!keymap.valid()) return code(errc::input_injection_failed);

    if (!proxy::ensure_keyboard_focus(state, focus_surface)) return code(errc::input_injection_failed);

    // Resolve every modifier and the key itself against the app's own layout;
    // there is no hard-coded keycode table to fall back to.
    std::vector<uint32_t> held;
    uint32_t depressed = 0;
    for (const auto &name : parsed.modifiers) {
        const auto xkb_name = proxy::xkb_modifier_name(name);
        if (!xkb_name) return code(errc::input_injection_failed);
        const auto mask = keymap.modifier_mask(*xkb_name);
        const auto keysym = proxy::keysym_for_key(name);
        if (!mask || !keysym) return code(errc::input_injection_failed);
        const auto keycode = keymap.find_keysym(*keysym);
        if (!keycode) return code(errc::input_injection_failed);
        if (!proxy::inject_key_raw(state, *keycode, true)) return code(errc::input_injection_failed);
        depressed |= *mask;
        held.push_back(*keycode);
        if (!proxy::inject_modifiers(state, depressed, 0, 0, 0)) return code(errc::input_injection_failed);
    }

    const auto keysym = proxy::keysym_for_key(parsed.key);
    if (!keysym) return code(errc::input_injection_failed);
    const auto keycode = keymap.find_keysym(*keysym);
    if (!keycode) return code(errc::input_injection_failed);

    bool ok = proxy::inject_key_raw(state, *keycode, true);
    if (ok && hold.count() > 0) std::this_thread::sleep_for(hold);
    if (ok) ok = proxy::inject_key_raw(state, *keycode, false);
    for (auto it = held.rbegin(); it != held.rend(); ++it) {
        if (!proxy::inject_key_raw(state, *it, false)) ok = false;
    }
    if (!held.empty() && !proxy::inject_modifiers(state, 0, 0, 0, 0)) ok = false;
    if (!ok) return code(errc::input_injection_failed);
    return boost::outcome_v2::success();
}

result<void> KeyboardMouseDevice::type_text(std::string text) const {
    if (!ensure_session()) return code(errc::input_injection_failed);
    auto state = session();
    if (!state) return code(errc::input_injection_failed);
    // Layout-independent when text-input-v3 is enabled, keymap-resolved
    // otherwise; both paths live in the input layer.
    if (!proxy::inject_text(state, text)) return code(errc::input_injection_failed);
    return boost::outcome_v2::success();
}

result<void> KeyboardMouseDevice::drag(cv::Point2d from, cv::Point2d to, std::chrono::milliseconds hold) const {
    if (!ensure_session()) return code(errc::input_injection_failed);
    auto state = session();
    if (!state) return code(errc::input_injection_failed);

    const auto target = input_target(state);
    if (!target) return code(errc::input_injection_failed);
    const double from_lx = to_logical(state, target, from.x);
    const double from_ly = to_logical(state, target, from.y);
    const double to_lx = to_logical(state, target, to.x);
    const double to_ly = to_logical(state, target, to.y);
    if (!proxy::ensure_pointer_focus(state, target, from_lx, from_ly)) return code(errc::input_injection_failed);

    if (!proxy::inject_pointer_motion(state, from_lx, from_ly)) return code(errc::input_injection_failed);
    if (!proxy::inject_pointer_frame(state)) return code(errc::input_injection_failed);
    if (!proxy::inject_pointer_button(state, 0x110, true)) return code(errc::input_injection_failed);
    if (!proxy::inject_pointer_frame(state)) return code(errc::input_injection_failed);
    if (hold.count() > 0) std::this_thread::sleep_for(hold);
    if (!proxy::inject_pointer_motion(state, to_lx, to_ly)) return code(errc::input_injection_failed);
    if (!proxy::inject_pointer_frame(state)) return code(errc::input_injection_failed);
    if (!proxy::inject_pointer_button(state, 0x110, false)) return code(errc::input_injection_failed);
    if (!proxy::inject_pointer_frame(state)) return code(errc::input_injection_failed);
    return boost::outcome_v2::success();
}

result<void> KeyboardMouseDevice::abs_drag(cv::Point2d from, cv::Point2d to) const {
    return drag(from, to, std::chrono::milliseconds{0});
}

result<void> KeyboardMouseDevice::press_drag(cv::Point2d from, cv::Point2d to, std::chrono::milliseconds hold) const {
    return drag(from, to, hold);
}

result<void> KeyboardMouseDevice::rel_drag(cv::Point2d from, cv::Point2d to) const {
    if (!ensure_session()) return code(errc::input_injection_failed);
    auto state = session();
    if (!state) return code(errc::input_injection_failed);

    const auto target = input_target(state);
    if (!target) return code(errc::input_injection_failed);
    const double from_lx = to_logical(state, target, from.x);
    const double from_ly = to_logical(state, target, from.y);
    const double to_lx = to_logical(state, target, to.x);
    const double to_ly = to_logical(state, target, to.y);
    if (!proxy::ensure_pointer_focus(state, target, from_lx, from_ly)) return code(errc::input_injection_failed);

    // Move without pressing a button: the app sees motion only, so a
    // pointer-locked camera follows the relative delta and nothing is seen as a
    // click (unlike abs_drag).
    if (!proxy::inject_pointer_motion(state, to_lx, to_ly)) return code(errc::input_injection_failed);
    if (!proxy::inject_pointer_frame(state)) return code(errc::input_injection_failed);
    proxy::inject_relative_motion(state, to_lx - from_lx, to_ly - from_ly);
    return boost::outcome_v2::success();
}

#define SKL_WCAS_KM_BIND_DEVICE_INFO_LIST(X)                        \
    X(DeviceInfoTraits, id, &KeyboardMouseDevice::id)               \
    X(DeviceInfoTraits, kind, &KeyboardMouseDevice::kind)           \
    X(DeviceInfoTraits, name, &KeyboardMouseDevice::name)           \
    X(DeviceInfoTraits, connected, &KeyboardMouseDevice::connected) \
    X(DeviceInfoTraits, capabilities, &KeyboardMouseDevice::capabilities)

SKL_ABIX_INTERFACE_MEMBER_TABLE(DeviceInfo, SKL_WCAS_KM_BIND_DEVICE_INFO_LIST)

#define SKL_WCAS_KM_BIND_POINTER_CTLER_LIST(X)                           \
    X(PointerControllerTraits, move, &KeyboardMouseDevice::move)         \
    X(PointerControllerTraits, click, &KeyboardMouseDevice::click)       \
    X(PointerControllerTraits, scroll, &KeyboardMouseDevice::scroll)     \
    X(PointerControllerTraits, abs_drag, &KeyboardMouseDevice::abs_drag) \
    X(PointerControllerTraits, rel_drag, &KeyboardMouseDevice::rel_drag) \
    X(PointerControllerTraits, press_drag, &KeyboardMouseDevice::press_drag)

SKL_ABIX_INTERFACE_MEMBER_TABLE(PointerController, SKL_WCAS_KM_BIND_POINTER_CTLER_LIST)

#define SKL_WCAS_KM_BIND_KEYBOARD_CTLER_LIST(X)                 \
    X(KeyboardControllerTraits, key, &KeyboardMouseDevice::key) \
    X(KeyboardControllerTraits, type_text, &KeyboardMouseDevice::type_text)

SKL_ABIX_INTERFACE_MEMBER_TABLE(KeyboardController, SKL_WCAS_KM_BIND_KEYBOARD_CTLER_LIST)

static_assert(std::size(DeviceInfoMethods) == DeviceInfoTraits::signatures.size());
static_assert(std::size(PointerControllerMethods) == PointerControllerTraits::signatures.size());
static_assert(std::size(KeyboardControllerMethods) == KeyboardControllerTraits::signatures.size());

const InterfaceMethod kX11InfoMethods[] = {
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(DeviceInfoTraits, id, &X11InputDevice::id),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(DeviceInfoTraits, kind, &X11InputDevice::kind),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(DeviceInfoTraits, name, &X11InputDevice::name),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(DeviceInfoTraits, capabilities, &X11InputDevice::capabilities),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(DeviceInfoTraits, connected, &X11InputDevice::connected),
};

const InterfaceMethod kX11PointerMethods[] = {
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(PointerControllerTraits, move, &X11InputDevice::move),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(PointerControllerTraits, click, &X11InputDevice::click),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(PointerControllerTraits, scroll, &X11InputDevice::scroll),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(PointerControllerTraits, abs_drag, &X11InputDevice::abs_drag),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(PointerControllerTraits, rel_drag, &X11InputDevice::rel_drag),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(PointerControllerTraits, press_drag, &X11InputDevice::press_drag),
};

const InterfaceMethod kX11KeyboardMethods[] = {
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(KeyboardControllerTraits, key, &X11InputDevice::key),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(KeyboardControllerTraits, type_text, &X11InputDevice::type_text),
};

static_assert(std::size(kX11InfoMethods) == DeviceInfoTraits::signatures.size());
static_assert(std::size(kX11PointerMethods) == PointerControllerTraits::signatures.size());
static_assert(std::size(kX11KeyboardMethods) == KeyboardControllerTraits::signatures.size());

constexpr auto kX11InputType = skl::abix::model::canonical_type_id("skl::wcas::X11InputDevice");

InterfaceStatus register_x11_input(InterfaceRegistry<8> &registry) {
    InterfaceStatus status = registry.add({DeviceInfoTraits::id, kX11InputType, kX11InfoMethods,
        static_cast<uint32_t>(std::size(kX11InfoMethods)), DeviceInfoTraits::version, 0});
    if (status != InterfaceStatus::ok) return status;

    status = registry.add({PointerControllerTraits::id, kX11InputType, kX11PointerMethods,
        static_cast<uint32_t>(std::size(kX11PointerMethods)), PointerControllerTraits::version, 0});
    if (status != InterfaceStatus::ok) return status;

    return registry.add({KeyboardControllerTraits::id, kX11InputType, kX11KeyboardMethods,
        static_cast<uint32_t>(std::size(kX11KeyboardMethods)), KeyboardControllerTraits::version, 0});
}

constexpr auto kKeyboardMouseType = skl::abix::model::canonical_type_id("skl::wcas::KeyboardMouse");

InterfaceStatus register_keyboard_mouse(InterfaceRegistry<8> &registry) {
    InterfaceStatus status = registry.add(SKL_ABIX_INTERFACE_IMPLEMENTATION(DeviceInfo, kKeyboardMouseType, 0));
    if (status != InterfaceStatus::ok) return status;

    status = registry.add(SKL_ABIX_INTERFACE_IMPLEMENTATION(PointerController, kKeyboardMouseType, 0));
    if (status != InterfaceStatus::ok) return status;

    return registry.add(SKL_ABIX_INTERFACE_IMPLEMENTATION(KeyboardController, kKeyboardMouseType, 0));
}

// The registry only describes providers: it maps (concrete TypeId, interface,
// version) to static adapter tables. It holds no per-object state, so one
// process-wide instance is enough. Keeping it out of Controller::Impl avoids
// 464 bytes per controller; it is only needed while binding.
//
// InterfaceRegistry is neither copyable nor movable (it owns an atomic lookup
// counter), so initialize it in place through a small holder. The holder's
// constructor runs during thread-safe function-local static initialization;
// afterwards the registry is read-only.
struct ControllerRegistry {
    ControllerRegistry() {
        if (register_keyboard_mouse(registry) != InterfaceStatus::ok) return;
        if (register_x11_input(registry) != InterfaceStatus::ok) return;
        // Register future providers here, for example:
        // (void)register_gamepad(registry);
    }

    InterfaceRegistry<8> registry;
};

const InterfaceRegistry<8> &controller_registry() {
    static const ControllerRegistry holder;
    return holder.registry;
}

}   // namespace

struct Controller::Impl {
    Impl(ControlledApp &app, ControlledApp::Impl *state)
        : device(&app, state)
        , x11_device(state) {}

    KeyboardMouseDevice device;
    X11InputDevice x11_device;
    PointerController pointer;
    KeyboardController keyboard;
    GamepadController gamepad;
    DeviceInfo info;
};

Controller::Controller() noexcept = default;
Controller::Controller(std::unique_ptr<Impl> impl) noexcept
    : _impl(std::move(impl)) {}
Controller::Controller(Controller &&) noexcept = default;
Controller &Controller::operator=(Controller &&) noexcept = default;
Controller::~Controller() = default;

std::unique_ptr<Controller> Controller::create(ControlledApp &app) {
    if (!app._impl) return nullptr;

    auto impl = std::make_unique<Impl>(app, app._impl.get());
    // controller_registry() is fully populated during static initialization and
    // never mutated afterwards; concurrent find() calls are read-only and safe.
    const auto &registry = controller_registry();

    const bool x11 = app._impl->transport == Transport::xwayland;
    const auto type = x11 ? kX11InputType : kKeyboardMouseType;
    void *object = x11 ? static_cast<void *>(&impl->x11_device) : static_cast<void *>(&impl->device);
    const auto bind = [&](auto &proxy) { return proxy.bind(registry, object, type); };
    if (bind(impl->info) != InterfaceStatus::ok) return nullptr;
    if (bind(impl->pointer) != InterfaceStatus::ok) return nullptr;
    if (bind(impl->keyboard) != InterfaceStatus::ok) return nullptr;

    // A gamepad backend is optional. It registers GamepadControllerTraits::id
    // with its own concrete TypeId and method table; until then the proxy stays
    // unbound and the facade reports feature_not_implemented.
    (void)bind(impl->gamepad);

    return std::unique_ptr<Controller>(new Controller(std::move(impl)));
}

bool Controller::valid() const noexcept { return _impl && _impl->info.valid(); }
bool Controller::has_pointer() const noexcept { return _impl && _impl->pointer.valid(); }
bool Controller::has_keyboard() const noexcept { return _impl && _impl->keyboard.valid(); }
bool Controller::has_gamepad() const noexcept { return _impl && _impl->gamepad.valid(); }

uint64_t Controller::device_id() const noexcept { return valid() ? _impl->info.id() : 0; }
DeviceKind Controller::kind() const noexcept { return valid() ? _impl->info.kind() : DeviceKind::Unknown; }
std::string Controller::name() const { return valid() ? _impl->info.name() : std::string{}; }
DeviceCapability Controller::capabilities() const noexcept {
    return valid() ? _impl->info.capabilities() : DeviceCapability::Unknown;
}
bool Controller::connected() const noexcept { return valid() && _impl->info.connected(); }

result<void> Controller::move(cv::Point2d pos) const {
    if (!has_pointer()) return code(errc::feature_not_implemented);
    return _impl->pointer.move(pos);
}

result<void> Controller::click(cv::Point2d pos, PointerButton button, size_t count) const {
    if (!has_pointer()) return code(errc::feature_not_implemented);
    return _impl->pointer.click(pos, button, static_cast<uint64_t>(count));
}

result<void> Controller::scroll(cv::Point2d pos, cv::Point dpos) const {
    if (!has_pointer()) return code(errc::feature_not_implemented);
    return _impl->pointer.scroll(pos, dpos);
}

result<void> Controller::abs_drag(cv::Point2d from, cv::Point2d to) const {
    if (!has_pointer()) return code(errc::feature_not_implemented);
    return _impl->pointer.abs_drag(from, to);
}

result<void> Controller::rel_drag(cv::Point2d from, cv::Point2d to) const {
    if (!has_pointer()) return code(errc::feature_not_implemented);
    return _impl->pointer.rel_drag(from, to);
}

result<void> Controller::press_drag(cv::Point2d from, cv::Point2d to, std::chrono::milliseconds hold) const {
    if (!has_pointer()) return code(errc::feature_not_implemented);
    return _impl->pointer.press_drag(from, to, hold);
}

result<void> Controller::key(std::string combination, std::chrono::milliseconds hold) const {
    if (!has_keyboard()) return code(errc::feature_not_implemented);
    return _impl->keyboard.key(std::move(combination), hold);
}

result<void> Controller::type_text(std::string text) const {
    if (!has_keyboard()) return code(errc::feature_not_implemented);
    return _impl->keyboard.type_text(std::move(text));
}

result<void> Controller::button(uint32_t button, bool pressed) const {
    if (!has_gamepad()) return code(errc::feature_not_implemented);
    return _impl->gamepad.button(button, pressed);
}

result<void> Controller::axis(uint32_t axis, double value) const {
    if (!has_gamepad()) return code(errc::feature_not_implemented);
    return _impl->gamepad.axis(axis, value);
}

result<void> Controller::rumble(double low_frequency, double high_frequency, std::chrono::milliseconds duration) const {
    if (!has_gamepad()) return code(errc::feature_not_implemented);
    return _impl->gamepad.rumble(low_frequency, high_frequency, duration);
}

}   // namespace skl::wcas
