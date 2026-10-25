// Compile/contract check for the X11 input provider. Actual XTest injection is
// environment-dependent; this test only pins the provider surface and the
// no-display failure behavior.
#include "SKL/WCAS/Private/Harness/X11Input.h"

#include <assert.h>
#include <stdio.h>

using namespace skl::wcas;

int main() {
    assert(X11InputDevice::kind() == DeviceKind::KeyboardMouse);
    assert(X11InputDevice::id() == 1);
    assert(!X11InputDevice::name().empty());
    const auto caps = X11InputDevice::capabilities();
    assert((caps & DeviceCapability::Pointer) == DeviceCapability::Pointer);
    assert((caps & DeviceCapability::Keyboard) == DeviceCapability::Keyboard);

    X11InputDevice device(nullptr);
    assert(!device.connected());
    assert(!device.move(cv::Point2d{1, 2}).has_value());

    std::puts("x11 input: ok");
    return 0;
}
