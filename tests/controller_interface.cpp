// Contract test for the capability-split controller interfaces.
//
// It deliberately uses a fake provider and the public InterfaceRegistry: the
// point is to prove that DeviceInfo / PointerController / KeyboardController /
// GamepadController can be implemented by a plain struct, bound independently,
// and that an unimplemented capability fails cleanly.
#include "skl/wcas/controller.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <string>

namespace {

using skl::abix::model::TypeId;
using skl::abix::poly::interface_entry;
using skl::abix::poly::InterfaceMethod;
using skl::abix::poly::InterfaceRegistry;
using skl::abix::poly::InterfaceStatus;
using namespace skl::wcas;

struct FakeDevice {
    bool connected = true;
    int moves = 0;
    int clicks = 0;
    int keys = 0;
    int buttons = 0;
    std::string last_text;
};

DeviceKind fake_kind(const void *) { return DeviceKind::Gamepad; }
uint64_t fake_id(const void *) { return 42; }
std::string fake_name(const void *) { return "fake-gamepad"; }
DeviceCapability fake_capabilities(const void *) { return DeviceCapability::Gamepad | DeviceCapability::Rumble; }
bool fake_connected(const void *self) { return static_cast<const FakeDevice *>(self)->connected; }

result<void> fake_move(const void *self, cv::Point2d) {
    ++static_cast<FakeDevice *>(const_cast<void *>(self))->moves;
    return boost::outcome_v2::success();
}
result<void> fake_click(const void *self, cv::Point2d, PointerButton, uint64_t) {
    ++static_cast<FakeDevice *>(const_cast<void *>(self))->clicks;
    return boost::outcome_v2::success();
}
result<void> fake_scroll(const void *, cv::Point2d, cv::Point2i) { return boost::outcome_v2::success(); }
result<void> fake_abs_drag(const void *, cv::Point2d, cv::Point2d) { return boost::outcome_v2::success(); }
result<void> fake_rel_drag(const void *, cv::Point2d, cv::Point2d) { return boost::outcome_v2::success(); }
result<void> fake_press_drag(const void *, cv::Point2d, cv::Point2d, std::chrono::milliseconds) {
    return boost::outcome_v2::success();
}
result<void> fake_key(const void *self, std::string, std::chrono::milliseconds) {
    ++static_cast<FakeDevice *>(const_cast<void *>(self))->keys;
    return boost::outcome_v2::success();
}
result<void> fake_type_text(const void *self, std::string text) {
    static_cast<FakeDevice *>(const_cast<void *>(self))->last_text = std::move(text);
    return boost::outcome_v2::success();
}
result<void> fake_button(const void *self, uint32_t, bool) {
    ++static_cast<FakeDevice *>(const_cast<void *>(self))->buttons;
    return boost::outcome_v2::success();
}
result<void> fake_axis(const void *, uint32_t, double) { return boost::outcome_v2::success(); }
result<void> fake_rumble(const void *, double, double, std::chrono::milliseconds) {
    return boost::outcome_v2::success();
}

const InterfaceMethod kInfoMethods[] = {
    {          DeviceInfoTraits::signatures[DeviceInfoTraits::slots.id],           interface_entry(&fake_id),DeviceInfoTraits::slots.id,
     0                                      },
    {        DeviceInfoTraits::signatures[DeviceInfoTraits::slots.kind],         interface_entry(&fake_kind),
     DeviceInfoTraits::slots.kind, 0        },
    {        DeviceInfoTraits::signatures[DeviceInfoTraits::slots.name],         interface_entry(&fake_name),
     DeviceInfoTraits::slots.name, 0        },
    {DeviceInfoTraits::signatures[DeviceInfoTraits::slots.capabilities], interface_entry(&fake_capabilities),
     DeviceInfoTraits::slots.capabilities, 0},
    {   DeviceInfoTraits::signatures[DeviceInfoTraits::slots.connected],    interface_entry(&fake_connected),
     DeviceInfoTraits::slots.connected, 0   },
};

const InterfaceMethod kPointerMethods[] = {
    {      PointerControllerTraits::signatures[PointerControllerTraits::slots.move],       interface_entry(&fake_move),
     PointerControllerTraits::slots.move, 0      },
    {     PointerControllerTraits::signatures[PointerControllerTraits::slots.click],      interface_entry(&fake_click),
     PointerControllerTraits::slots.click, 0     },
    {    PointerControllerTraits::signatures[PointerControllerTraits::slots.scroll],     interface_entry(&fake_scroll),
     PointerControllerTraits::slots.scroll, 0    },
    {  PointerControllerTraits::signatures[PointerControllerTraits::slots.abs_drag],   interface_entry(&fake_abs_drag),
     PointerControllerTraits::slots.abs_drag, 0  },
    {  PointerControllerTraits::signatures[PointerControllerTraits::slots.rel_drag],   interface_entry(&fake_rel_drag),
     PointerControllerTraits::slots.rel_drag, 0  },
    {PointerControllerTraits::signatures[PointerControllerTraits::slots.press_drag], interface_entry(&fake_press_drag),
     PointerControllerTraits::slots.press_drag, 0},
};

const InterfaceMethod kKeyboardMethods[] = {
    {      KeyboardControllerTraits::signatures[KeyboardControllerTraits::slots.key],       interface_entry(&fake_key),
     KeyboardControllerTraits::slots.key, 0      },
    {KeyboardControllerTraits::signatures[KeyboardControllerTraits::slots.type_text], interface_entry(&fake_type_text),
     KeyboardControllerTraits::slots.type_text, 0},
};

const InterfaceMethod kGamepadMethods[] = {
    {GamepadControllerTraits::signatures[GamepadControllerTraits::slots.button], interface_entry(&fake_button),
     GamepadControllerTraits::slots.button, 0},
    {  GamepadControllerTraits::signatures[GamepadControllerTraits::slots.axis],   interface_entry(&fake_axis),
     GamepadControllerTraits::slots.axis, 0  },
    {GamepadControllerTraits::signatures[GamepadControllerTraits::slots.rumble], interface_entry(&fake_rumble),
     GamepadControllerTraits::slots.rumble, 0},
};

constexpr TypeId kFakeType = skl::abix::model::canonical_type_id("test::FakeDevice");

template<size_t N>
void add(InterfaceRegistry<8> &registry, skl::abix::poly::InterfaceId id, uint32_t version,
    const InterfaceMethod (&methods)[N]) {
    const auto status = registry.add({id, kFakeType, methods, static_cast<uint32_t>(N), version, 0});
    assert(status == InterfaceStatus::ok);
}

}   // namespace

int main() {
    InterfaceRegistry<8> registry;
    add(registry, DeviceInfoTraits::id, DeviceInfoTraits::version, kInfoMethods);
    add(registry, PointerControllerTraits::id, PointerControllerTraits::version, kPointerMethods);
    add(registry, KeyboardControllerTraits::id, KeyboardControllerTraits::version, kKeyboardMethods);
    add(registry, GamepadControllerTraits::id, GamepadControllerTraits::version, kGamepadMethods);

    FakeDevice device;

    DeviceInfo info(registry, &device, kFakeType);
    assert(info.valid());
    assert(info.id() == 42);
    assert(info.kind() == DeviceKind::Gamepad);
    assert(info.name() == "fake-gamepad");
    assert((info.capabilities() & DeviceCapability::Gamepad) != DeviceCapability::Unknown);
    assert(info.connected());

    PointerController pointer(registry, &device, kFakeType);
    assert(pointer.valid());
    (void)pointer.move(cv::Point2d{1.0, 2.0});
    (void)pointer.click(cv::Point2d{1.0, 2.0}, PointerButton::left, 2);
    (void)pointer.press_drag(cv::Point2d{1.0, 2.0}, cv::Point2d{3.0, 4.0}, std::chrono::milliseconds{5});
    assert(device.moves == 1);
    assert(device.clicks == 1);

    KeyboardController keyboard(registry, &device, kFakeType);
    assert(keyboard.valid());
    (void)keyboard.key(std::string("ctrl+s"), std::chrono::milliseconds{5});
    (void)keyboard.type_text(std::string("hello"));
    assert(device.keys == 1);
    assert(device.last_text == "hello");

    GamepadController gamepad(registry, &device, kFakeType);
    assert(gamepad.valid());
    (void)gamepad.button(0, true);
    (void)gamepad.axis(1, 0.5);
    (void)gamepad.rumble(0.25, 0.75, std::chrono::milliseconds{10});
    assert(device.buttons == 1);

    // A provider that does not implement a capability is not callable through
    // it: binding fails instead of caching an unsafe slot.
    constexpr TypeId unknown_type = skl::abix::model::canonical_type_id("test::UnknownDevice");
    GamepadController missing(registry, &device, unknown_type);
    assert(!missing.valid());
    assert(missing.status() == InterfaceStatus::interface_not_found);

    std::puts("controller interface: ok");
    return 0;
}
