#include "SKL/WCAS/Error.h"

#include "SKL/EPCD/Interface/CategoryRegistry.h"

#include <cstddef>

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

const error_category &error_category::instance() noexcept {
    static error_category instance;
    return instance;
}

const char *error_category::name() const noexcept { return "skl.wcas"; }

std::string error_category::message(int ev) const {
    const char *text = ::skl::epcd::message(WcasError_domain, ev);
    return text != nullptr ? text : "Invalid error, illegal out-of-bounds";
}

const char *error_category::module() const noexcept { return "wcas"; }

std::error_condition error_category::default_error_condition(int ev) const noexcept {
    if (ev == 0) return {0, std::generic_category()};

    const auto condition = ::skl::epcd::default_condition(WcasError_domain, ev);
    switch (::skl::epcd::to_generic_condition(condition)) {
        case ::skl::epcd::GenericCondition::address_in_use: return std::make_error_condition(std::errc::address_in_use);
        case ::skl::epcd::GenericCondition::protocol_not_supported:
            return std::make_error_condition(std::errc::protocol_not_supported);
        case ::skl::epcd::GenericCondition::permission_denied:
            return std::make_error_condition(std::errc::permission_denied);
        case ::skl::epcd::GenericCondition::io_error:      return std::make_error_condition(std::errc::io_error);
        case ::skl::epcd::GenericCondition::timed_out:     return std::make_error_condition(std::errc::timed_out);
        case ::skl::epcd::GenericCondition::not_supported: return std::make_error_condition(std::errc::not_supported);
        case ::skl::epcd::GenericCondition::no_child_process:
            return std::make_error_condition(std::errc::no_child_process);
        default: return {ev, *this};
    }
}

bool register_errors() noexcept { return registration_available(registration_status()); }

bool errors_registered() noexcept { return registration_available(registration_status()); }

[[maybe_unused]] const bool wcas_epcd_registration = register_errors();

}   // namespace skl::wcas

namespace skl {
std::error_code make_error_code(wcas::errc ec) noexcept {
    return {static_cast<int>(ec), wcas::error_category::instance()};
}
}   // namespace skl
