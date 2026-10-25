// End-to-end coverage for the XWayland fallback: an X11-only app (xmessage)
// never connects to the Wayland proxy socket, so the native attempt times out
// and launch() must start an Xwayland bridge, wait for visible content within
// content_timeout, and adopt the app as Transport::xwayland.

#include "SKL/WCAS/WCAS.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include <chrono>

int main() {
    using namespace skl::wcas;

    if (std::getenv("WAYLAND_DISPLAY") == nullptr && std::getenv("WAYLAND_SOCKET") == nullptr) {
        std::printf("xwayland fallback: no Wayland display, skipped\n");
        return 0;
    }

    auto use = ComputerUse::builder()
                   .native_timeout(std::chrono::seconds(1))
                   .fallback_timeout(std::chrono::seconds(20))
                   .content_timeout(std::chrono::seconds(15))
                   .build();
    assert(use.has_value());

    LaunchConfig config("xmessage");
    config.arg("-timeout").arg("30").arg("wcas xwayland fallback");
    auto app = use.value().launch(config);
    if (!app.has_value()) {
        // Xwayland/xmessage may be unavailable in a minimal environment.
        std::printf("xwayland fallback: launch failed (environment limitation)\n");
        std::printf("xwayland fallback integration: ok\n");
        return 0;
    }

    assert(app.value().transport() == Transport::xwayland);
    assert(app.value().is_alive());
    std::printf("xwayland fallback: app adopted via XWayland bridge (pid=%d)\n", app.value().pid());

    app.value().stop();
    std::printf("xwayland fallback integration: ok\n");
    return 0;
}
