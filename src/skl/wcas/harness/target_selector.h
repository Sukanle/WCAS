#pragma once

#include "harness/session_context.h"

#include "skl/wcas/api.h"
#include "skl/wcas/types.h"

#include "ABIX/Interface/Declaration.h"

#include <memory>
#include <string>

namespace skl::wcas {

// TargetSelector owns the persistent window/surface selection. It does not
// enumerate windows or implement capture/focus; those remain separate
// capabilities.
#define SKL_WCAS_TARGET_SELECTOR_METHODS(X)                \
    X(name, std::string, str, {})                          \
    X(available, bool, bool_t, {})                         \
    X(select, result<void>, res_void_t, {i32})             \
    X(selected, uint32_t, u32, {})                         \
    X(clear, result<void>, res_void_t, {})                 \
    X(select_target, result<void>, res_void_t, {u64, u32}) \
    X(selected_target, TargetId, reg_target::id, {})

SKL_ABIX_INTERFACE_UNIQUE_DECLARE(
    TargetSelector, SKL_WCAS_TARGET_SELECTOR_METHODS, SKL_ABIX_VERSION_NUMBER(0, 1, 0), SKL_WCAS_API);

#undef SKL_WCAS_TARGET_SELECTOR_METHODS

class TargetRuntime {
public:
    explicit TargetRuntime(SessionContext *state);
    ~TargetRuntime();
    TargetRuntime(const TargetRuntime &) = delete;
    TargetRuntime &operator=(const TargetRuntime &) = delete;
    TargetRuntime(TargetRuntime &&) = delete;
    TargetRuntime &operator=(TargetRuntime &&) = delete;

    [[nodiscard]] result<void> select(int32_t index);
    [[nodiscard]] uint32_t selected() const;
    [[nodiscard]] result<void> clear();
    [[nodiscard]] result<void> select_target(TargetId target);
    [[nodiscard]] TargetId selected_target() const;

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

}   // namespace skl::wcas
