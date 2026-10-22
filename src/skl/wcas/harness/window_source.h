#pragma once

#include "harness/session_context.h"
#include "skl/wcas/wcas.h"
#include "skl/wcas/window.h"

#include <memory>

namespace skl::wcas {

/// Internal window runtime: owns the WindowEnumerator / WindowResolver proxies
/// and the provider that implements them for one ControlledApp.
class WindowRuntime {
public:
    explicit WindowRuntime(SessionContext *state);
    ~WindowRuntime();
    WindowRuntime(const WindowRuntime &) = delete;
    WindowRuntime &operator=(const WindowRuntime &) = delete;
    WindowRuntime(WindowRuntime &&) = delete;
    WindowRuntime &operator=(WindowRuntime &&) = delete;

    [[nodiscard]] result<std::vector<WindowInfo>> list() const;
    [[nodiscard]] result<Rect> find_by_pid(int32_t pid) const;
    [[nodiscard]] result<Rect> find_by_size_and_title(int32_t width, int32_t height, std::string title) const;

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

}   // namespace skl::wcas
