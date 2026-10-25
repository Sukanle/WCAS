#pragma once

#include <memory>
#include <string>

#include "SKL/WCAS/API.h"
#include "SKL/WCAS/Types.h"

#include "SKL/WCAS/Private/Harness/SessionContext.h"

#include "SKL/ABIX/Interface/Declaration.h"

namespace skl::wcas {

// HealthMonitor is a read-only capability: it reports process/session state and
// the native proxy counters. It never captures or changes selection.
#define SKL_WCAS_HEALTH_MONITOR_METHODS(X) \
    X(name, std::string, str, {})          \
    X(available, bool, bool_t, {})         \
    X(alive, bool, bool_t, {})             \
    X(connected, bool, bool_t, {})         \
    X(commit_count, uint64_t, u64, {})     \
    X(presented_frames, uint64_t, u64, {})

SKL_ABIX_INTERFACE_UNIQUE_DECLARE(
    HealthMonitor, SKL_WCAS_HEALTH_MONITOR_METHODS, SKL_ABIX_VERSION_NUMBER(0, 1, 0), SKL_WCAS_API);

#undef SKL_WCAS_HEALTH_MONITOR_METHODS

class HealthRuntime {
public:
    explicit HealthRuntime(SessionContext *state);
    ~HealthRuntime();
    HealthRuntime(const HealthRuntime &) = delete;
    HealthRuntime &operator=(const HealthRuntime &) = delete;
    HealthRuntime(HealthRuntime &&) = delete;
    HealthRuntime &operator=(HealthRuntime &&) = delete;

    [[nodiscard]] bool available() const;
    [[nodiscard]] bool alive() const;
    [[nodiscard]] bool connected() const;
    [[nodiscard]] uint64_t commit_count() const;
    [[nodiscard]] uint64_t presented_frames() const;

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

}   // namespace skl::wcas
