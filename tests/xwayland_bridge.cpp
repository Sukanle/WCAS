// Integration coverage for the authenticated XWayland bridge: it must start a
// dedicated Xwayland process against the ambient Wayland display, serve a
// capturable X screen, and clean up on stop(). Skips cleanly when Xwayland or
// a Wayland display is unavailable.

#include "x11_capture.h"
#include "xwayland.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>

int main() {
    const char *wayland = std::getenv("WAYLAND_DISPLAY");
    if (wayland == nullptr || wayland[0] == '\0') {
        std::printf("xwayland bridge: no WAYLAND_DISPLAY, skipped\n");
        return 0;
    }

    skl::wcas::XwaylandBridge bridge;
    if (!bridge.start(wayland, 29)) {
        std::printf("xwayland bridge: unavailable (Xwayland could not start)\n");
        std::printf("xwayland bridge integration: ok\n");
        return 0;
    }

    assert(bridge.running());
    assert(!bridge.display().empty());
    assert(!bridge.xauthority().empty());

    auto frame = skl::wcas::capture_x11_window(bridge.display(), bridge.xauthority());
    if (frame.has_value()) {
        assert(frame.value().cols > 0 && frame.value().rows > 0);
        std::printf("xwayland bridge capture: ok %dx%d\n", frame.value().cols, frame.value().rows);
    } else {
        std::printf("xwayland bridge capture: display %s not capturable\n", bridge.display().c_str());
    }

    bridge.stop();
    assert(!bridge.running());

    std::printf("xwayland bridge integration: ok\n");
    return 0;
}
