#pragma once

#include "harness/session_context.h"

#include "skl/wcas/api.h"
#include "skl/wcas/types.h"

#include "ABIX/Interface/Declaration.h"

#include <memory>
#include <string>

namespace skl::wcas {

// FocusBackend owns the activation policy; KeyboardController only injects keys.
#define SKL_WCAS_FOCUS_BACKEND_METHODS(X)              \
    X(name, std::string, str, {})                      \
    X(available, bool, bool_t, {})                     \
    X(set_allowed, result<void>, res_void_t, {bool_t}) \
    X(allowed, bool, bool_t, {})                       \
    X(apply, result<void>, res_void_t, {})

SKL_ABIX_INTERFACE_UNIQUE_DECLARE(
    FocusBackend, SKL_WCAS_FOCUS_BACKEND_METHODS, SKL_ABIX_VERSION_NUMBER(0, 1, 0), SKL_WCAS_API);

#undef SKL_WCAS_FOCUS_BACKEND_METHODS

/// Internal focus runtime. A transport that cannot activate returns
/// available() == false; set_allowed() remains a policy flag but does not
/// synthesize activation.
class FocusRuntime {
public:
    explicit FocusRuntime(SessionContext *state);
    ~FocusRuntime();
    FocusRuntime(const FocusRuntime &) = delete;
    FocusRuntime &operator=(const FocusRuntime &) = delete;
    FocusRuntime(FocusRuntime &&) = delete;
    FocusRuntime &operator=(FocusRuntime &&) = delete;

    [[nodiscard]] bool available() const;
    [[nodiscard]] result<void> set_allowed(bool value);
    [[nodiscard]] bool allowed() const;
    [[nodiscard]] result<void> apply();

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

}   // namespace skl::wcas
