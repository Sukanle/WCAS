#include "SKL/WCAS/Error.h"

#include "SKL/EPCD/Interface/CategoryRegistry.h"

#include <cstddef>
#include <cstdio>
#include <string>

namespace skl::wcas {
namespace {

constexpr std::size_t epcd_registry_capacity = 8;
using epcd_registry_type = ::skl::epcd::iface::CategoryRegistry<epcd_registry_capacity>;

epcd_registry_type &registry() noexcept {
    static epcd_registry_type instance;
    return instance;
}

::skl::epcd::iface::CategoryStatus registration_status() noexcept {
    static const auto status = registry().add_static(WcasError_domain);
    return status;
}

bool registration_available(::skl::epcd::iface::CategoryStatus status) noexcept {
    return status == ::skl::epcd::iface::CategoryStatus::ok
        || status == ::skl::epcd::iface::CategoryStatus::duplicate_domain;
}

}   // namespace

std::string error_message(const ::skl::epcd::ErrorCode &code) {
    if (code.ok()) return "success";

    if (code.domain == WcasError_domain.descriptor.domain) {
        if (const char *text = ::skl::epcd::message(WcasError_domain, code.value); text != nullptr) return text;
    }

    char buffer[96] = {};
    std::snprintf(buffer, sizeof(buffer), "error domain 0x%016llx code %d",
        static_cast<unsigned long long>(code.domain), static_cast<int>(code.value));
    return buffer;
}

bool register_errors() noexcept { return registration_available(registration_status()); }

bool errors_registered() noexcept { return registration_available(registration_status()); }

[[maybe_unused]] const bool wcas_epcd_registration = register_errors();

}   // namespace skl::wcas
