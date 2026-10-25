#pragma once

#include <stdint.h>
#include <stddef.h>

#include <chrono>
#include <memory>
#include <string>

#include "SKL/WCAS/API.h"
#include "SKL/WCAS/Types.h"

#include "SKL/ABIX/Interface/Declaration.h"

namespace skl::wcas {

/// What kind of input device a provider represents. `unknown` is returned by a
/// default-constructed Controller and must not be used as a provider kind.
enum class DeviceKind : uint8_t {
    KeyboardMouse = 0,
    Gamepad = 1,
    VirtualPointer = 2,
    VirtualKeyboard = 3,
    Unknown = 0XFF,
};

enum class DeviceCapability : uint8_t {
    Unknown = 0X00,
    Pointer = 0X01,
    Keyboard = 0x02,
    Text = 0x04,
    Gamepad = 0x08,
    Rumble = 0x10,
};

SKL_WCAS_ENABLE_BITMASK_OPERATORS(DeviceCapability);

// Contracts are grouped by capability, not by concrete class. A keyboard/mouse
// provider implements DeviceInfo + PointerController + KeyboardController; a
// gamepad provider implements DeviceInfo + GamepadController. Adding a new
// device therefore never changes an existing interface identity or slot order.
//
// Slot order is an ABI: append new methods in a new version only, never insert
// or reorder entries in an existing list.
#define SKL_WCAS_DEVICE_INFO_METHODS(X)       \
    X(id, uint64_t, u64, {})                  \
    X(name, std::string, str, {})             \
    X(connected, bool, bool_t, {})            \
    X(kind, DeviceKind, reg_device::kind, {}) \
    X(capabilities, DeviceCapability, reg_device::capability, {})

#define SKL_WCAS_POINTER_METHODS(X)                                               \
    X(move, result<void>, res_void_t, {reg_cv::Point::D2})                        \
    X(click, result<void>, res_void_t, {reg_cv::Point::D2, PtrBtn, u64})          \
    X(scroll, result<void>, res_void_t, {reg_cv::Point::D2, reg_cv::Point::I2})   \
    X(abs_drag, result<void>, res_void_t, {reg_cv::Point::D2, reg_cv::Point::D2}) \
    X(rel_drag, result<void>, res_void_t, {reg_cv::Point::D2, reg_cv::Point::D2}) \
    X(press_drag, result<void>, res_void_t, {reg_cv::Point::D2, reg_cv::Point::D2, ms})

#define SKL_WCAS_KEYBOARD_METHODS(X)            \
    X(key, result<void>, res_void_t, {str, ms}) \
    X(type_text, result<void>, res_void_t, {str})

#define SKL_WCAS_GAMEPAD_METHODS(X)                    \
    X(button, result<void>, res_void_t, {u32, bool_t}) \
    X(axis, result<void>, res_void_t, {u32, f64})      \
    X(rumble, result<void>, res_void_t, {f64, f64, ms})

SKL_ABIX_INTERFACE_UNIQUE_DECLARE(
    DeviceInfo, SKL_WCAS_DEVICE_INFO_METHODS, SKL_ABIX_VERSION_NUMBER(0, 1, 0), SKL_WCAS_API);
SKL_ABIX_INTERFACE_UNIQUE_DECLARE(
    PointerController, SKL_WCAS_POINTER_METHODS, SKL_ABIX_VERSION_NUMBER(0, 1, 0), SKL_WCAS_API);
SKL_ABIX_INTERFACE_UNIQUE_DECLARE(
    KeyboardController, SKL_WCAS_KEYBOARD_METHODS, SKL_ABIX_VERSION_NUMBER(0, 1, 0), SKL_WCAS_API);
SKL_ABIX_INTERFACE_UNIQUE_DECLARE(
    GamepadController, SKL_WCAS_GAMEPAD_METHODS, SKL_ABIX_VERSION_NUMBER(0, 1, 0), SKL_WCAS_API);

#undef SKL_WCAS_DEVICE_INFO_METHODS
#undef SKL_WCAS_POINTER_METHODS
#undef SKL_WCAS_KEYBOARD_METHODS
#undef SKL_WCAS_GAMEPAD_METHODS

class ControlledApp;

/// A device-independent controller handle.
///
/// The handle composes the capability proxies (pointer, keyboard, gamepad) and
/// a common `DeviceInfo` proxy. A backend that does not implement a capability
/// leaves that proxy unbound; calling the corresponding method returns
/// `errc::feature_not_implemented` instead of calling an unbound slot.
///
/// A Controller holds a pointer to the ControlledApp it was created from and
/// must not outlive it.
class SKL_WCAS_API Controller {
public:
    Controller() noexcept;
    Controller(Controller &&) noexcept;
    Controller &operator=(Controller &&) noexcept;
    ~Controller();
    Controller(const Controller &) = delete;
    Controller &operator=(const Controller &) = delete;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] bool has_pointer() const noexcept;
    [[nodiscard]] bool has_keyboard() const noexcept;
    [[nodiscard]] bool has_gamepad() const noexcept;

    // DeviceInfo
    [[nodiscard]] uint64_t device_id() const noexcept;
    [[nodiscard]] DeviceKind kind() const noexcept;
    [[nodiscard]] std::string name() const;
    [[nodiscard]] DeviceCapability capabilities() const noexcept;
    [[nodiscard]] bool connected() const noexcept;

    // PointerController
    [[nodiscard]] result<void> move(cv::Point2d pos) const;
    [[nodiscard]] result<void> click(
        cv::Point2d pos, PointerButton button = PointerButton::left, size_t count = 1) const;
    [[nodiscard]] result<void> scroll(cv::Point2d pos, cv::Point dpos) const;
    [[nodiscard]] result<void> abs_drag(cv::Point2d from, cv::Point2d to) const;
    [[nodiscard]] result<void> rel_drag(cv::Point2d from, cv::Point2d to) const;
    /// Press, hold for `hold`, move to `to`, then release. A zero hold behaves
    /// like `abs_drag`.
    [[nodiscard]] result<void> press_drag(cv::Point2d from, cv::Point2d to, std::chrono::milliseconds hold) const;

    // KeyboardController
    [[nodiscard]] result<void> key(std::string combination, std::chrono::milliseconds hold) const;
    [[nodiscard]] result<void> type_text(std::string text) const;

    // GamepadController
    [[nodiscard]] result<void> button(uint32_t button, bool pressed) const;
    [[nodiscard]] result<void> axis(uint32_t axis, double value) const;
    [[nodiscard]] result<void> rumble(
        double low_frequency, double high_frequency, std::chrono::milliseconds duration) const;

private:
    friend class ControlledApp;
    struct Impl;
    explicit Controller(std::unique_ptr<Impl> impl) noexcept;
    [[nodiscard]] static std::unique_ptr<Controller> create(ControlledApp &app);
    std::unique_ptr<Impl> _impl;
};

}   // namespace skl::wcas
