#pragma once

#include <chrono>
#include <string>

#include "SKL/WCAS/Types.h"
#include "SKL/WCAS/Private/Harness/SessionContext.h"

namespace skl::wcas {

/// X11/XTest input provider. It is selected for the XWayland transport and
/// implements the same DeviceInfo/Pointer/Keyboard contracts as the Wayland
/// private-seat provider. Display/X11 types stay inside x11_input.cpp.
class X11InputDevice {
public:
    explicit X11InputDevice(SessionContext *state);
    ~X11InputDevice();
    X11InputDevice(const X11InputDevice &) = delete;
    X11InputDevice &operator=(const X11InputDevice &) = delete;

    // DeviceInfo
    [[nodiscard]] static DeviceKind kind();
    [[nodiscard]] static uint64_t id();
    [[nodiscard]] static std::string name();
    [[nodiscard]] static DeviceCapability capabilities();
    [[nodiscard]] bool connected() const;

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
    [[nodiscard]] bool ensure_display() const;
    [[nodiscard]] bool target_origin(int &x, int &y) const;
    [[nodiscard]] bool send_key(uint32_t keycode, bool down) const;

    SessionContext *state_ = nullptr;
    mutable void *display_ = nullptr;   // Display*
    mutable int screen_ = 0;
    mutable unsigned long root_ = 0;
};

}   // namespace skl::wcas
