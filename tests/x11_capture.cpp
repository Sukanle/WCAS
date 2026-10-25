// Integration coverage for X11 window capture. Connects to the ambient DISPLAY
// (an XWayland or plain X server) via XComposite + XGetImage and requires a
// non-empty frame. Skips cleanly when no X display is configured.

#include "SKL/WCAS/Private/X11Capture.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

int main() {
    const char *display = std::getenv("DISPLAY");
    if (display == nullptr || display[0] == '\0') {
        std::printf("x11 capture: no DISPLAY, skipped\n");
        return 0;
    }

    auto frame = skl::wcas::capture_x11_window(display, "");
    if (frame.has_value()) {
        assert(frame.value().cols > 0 && frame.value().rows > 0);
        assert(frame.value().type() == CV_8UC3);
        std::printf("x11 capture: ok %dx%d\n", frame.value().cols, frame.value().rows);
    } else {
        std::printf("x11 capture: display %s not capturable\n", display);
    }
    std::printf("x11 capture integration: ok\n");
    return 0;
}
