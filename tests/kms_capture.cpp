// Coverage for the kernel-level (libdrmtap) capture path. Without
// CAP_SYS_ADMIN or a configured helper the call must fail cleanly rather than
// crash, so the test accepts either outcome but validates a successful frame.

#include "SKL/WCAS/DRM.h"

#include <assert.h>
#include <stdio.h>

int main() {
    using namespace skl::wcas;
    auto frame = capture_kms_screen(std::nullopt);
    if (frame.has_value()) {
        assert(frame.value().cols > 0 && frame.value().rows > 0);
        assert(frame.value().type() == CV_8UC3);
        printf("kms capture: ok %dx%d\n", frame.value().cols, frame.value().rows);
    } else {
        printf("kms capture: unavailable (no CAP_SYS_ADMIN or helper)\n");
    }

    // A crop region is clamped to the frame bounds and must not crash.
    cv::Rect crop(0, 0, 16, 16);
    auto cropped = capture_kms_screen(crop);
    if (cropped.has_value()) {
        assert(cropped.value().cols > 0 && cropped.value().rows > 0);
        printf("kms crop: ok %dx%d\n", cropped.value().cols, cropped.value().rows);
    }
    printf("kms capture integration: ok\n");
    return 0;
}
