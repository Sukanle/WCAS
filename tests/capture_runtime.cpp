// Regression guard for the production CaptureRuntime binding. An unbound or
// signature-mismatched CaptureSource reports feature_not_implemented; this test
// proves the real AutoCaptureSource binding is valid before any frame exists.
#include "SKL/WCAS/Private/Harness/CaptureSource.h"
#include "SKL/WCAS/Private/Harness/SessionContext.h"

#include "SKL/WCAS/Error.h"

#include <assert.h>
#include <stdio.h>

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
