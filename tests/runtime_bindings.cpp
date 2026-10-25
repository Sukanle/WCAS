// Production binding smoke test: construct each capability runtime with an
// empty SessionContext. A wrong method table shows up as feature_not_implemented
// before any real capture/window/input path is reached.
#include "SKL/WCAS/Error.h"
#include "SKL/WCAS/Private/Harness/CaptureSource.h"
#include "SKL/WCAS/Private/Harness/FocusBackend.h"
#include "SKL/WCAS/Private/Harness/HealthMonitor.h"
#include "SKL/WCAS/Private/Harness/SessionContext.h"
#include "SKL/WCAS/Private/Harness/TargetSelector.h"
#include "SKL/WCAS/Private/Harness/WindowSource.h"

#include <assert.h>
#include <stdio.h>

using namespace skl::wcas;

int main() {
    SessionContext context;

    CaptureRuntime capture(&context);
    auto frame = capture.capture();
    assert(!frame.has_value());
    assert(frame.error() == skl::make_error_code(errc::frame_capture_failed));

    WindowRuntime windows(&context);
    auto list = windows.list();
    assert(list.has_value());
    assert(list.value().empty());

    FocusRuntime focus(&context);
    assert(focus.available());
    assert(focus.set_allowed(true).has_value());
    assert(focus.apply().has_value());

    HealthRuntime health(&context);
    assert(health.available());
    assert(health.commit_count() == 0);
    assert(health.presented_frames() == 0);

    TargetRuntime target(&context);
    auto selected = target.select(0);
    assert(!selected.has_value());
    assert(selected.error() == skl::make_error_code(errc::unspecified));
    assert(target.selected_target() == TargetId{});

    std::puts("runtime bindings: ok");
    return 0;
}
