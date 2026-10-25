#pragma once

#include <memory>

#include "SKL/WCAS/Capture.h"

#include "SKL/WCAS/Private/Harness/SessionContext.h"

namespace skl::wcas {

/// Internal capture runtime: owns the `CaptureInfo` / `CaptureSource` proxies
/// and the provider that implements them for one ControlledApp.
class CaptureRuntime {
public:
    explicit CaptureRuntime(SessionContext *state);
    ~CaptureRuntime();
    CaptureRuntime(const CaptureRuntime &) = delete;
    CaptureRuntime &operator=(const CaptureRuntime &) = delete;
    CaptureRuntime(CaptureRuntime &&) = delete;
    CaptureRuntime &operator=(CaptureRuntime &&) = delete;

    [[nodiscard]] result<cv::Mat> capture() const;
    [[nodiscard]] optional<cv::Size> frame_size() const;
    void set_target(const CaptureTarget &target);

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

}   // namespace skl::wcas
