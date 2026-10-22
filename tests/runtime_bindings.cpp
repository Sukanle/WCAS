// Production binding smoke test: construct each capability runtime with an
// empty SessionContext. A wrong method table shows up as feature_not_implemented
// before any real capture/window/input path is reached.
#include "harness/capture_source.h"
#include "harness/focus_backend.h"
#include "harness/health_monitor.h"
#include "harness/session_context.h"
#include "harness/target_selector.h"
#include "harness/window_source.h"

#include "skl/wcas/error.h"

#include <cassert>
#include <cstdio>

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
