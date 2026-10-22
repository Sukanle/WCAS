// Regression guard for the production CaptureRuntime binding. An unbound or
// signature-mismatched CaptureSource reports feature_not_implemented; this test
// proves the real AutoCaptureSource binding is valid before any frame exists.
#include "harness/capture_source.h"
#include "harness/session_context.h"

#include "skl/wcas/error.h"

#include <cassert>
#include <cstdio>

using namespace skl::wcas;

int main() {
    SessionContext context;   // native transport, no session yet
    CaptureRuntime capture(&context);
    auto result = capture.capture();
    assert(!result.has_value());
    assert(result.error() == skl::make_error_code(errc::frame_capture_failed));
    assert(!capture.frame_size().has_value());
    std::puts("capture runtime: ok");
    return 0;
}
