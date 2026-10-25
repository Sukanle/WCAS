#include "SKL/WCAS/Private/Harness/FocusBackend.h"

#include <assert.h>
#include <stdio.h>
#include <iterator>
#include <string>

namespace {

using namespace skl::wcas;
using skl::abix::model::TypeId;
using skl::abix::poly::InterfaceMethod;
using skl::abix::poly::InterfaceRegistry;
using skl::abix::poly::InterfaceStatus;

class FakeFocus {
public:
    std::string name() const { return "fake-focus"; }
    bool available() const { return true; }
    result<void> set_allowed(bool value) {
        allowed_ = value;
        return boost::outcome_v2::success();
    }
    bool allowed() const { return allowed_; }
    result<void> apply() {
        ++applied_;
        return boost::outcome_v2::success();
    }
    int applied() const { return applied_; }

private:
    bool allowed_ = false;
    int applied_ = 0;
};

const InterfaceMethod kMethods[] = {
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(FocusBackendTraits, name, &FakeFocus::name),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(FocusBackendTraits, available, &FakeFocus::available),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(FocusBackendTraits, set_allowed, &FakeFocus::set_allowed),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(FocusBackendTraits, allowed, &FakeFocus::allowed),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(FocusBackendTraits, apply, &FakeFocus::apply),
};

constexpr TypeId kFakeType = skl::abix::model::canonical_type_id("test::FakeFocus");

}   // namespace

int main() {
    InterfaceRegistry<2> registry;
    assert(registry.add({FocusBackendTraits::id, kFakeType, kMethods, static_cast<uint32_t>(std::size(kMethods)),
               FocusBackendTraits::version, 0})
           == InterfaceStatus::ok);

    FakeFocus provider;
    FocusBackend backend(registry, &provider, kFakeType);
    assert(backend.valid());
    assert(backend.name() == "fake-focus");
    assert(backend.available());
    assert(!backend.allowed());
    assert(backend.set_allowed(true).has_value());
    assert(backend.allowed());
    assert(provider.allowed());
    assert(backend.apply().has_value());
    assert(provider.applied() == 1);

    constexpr TypeId unknown_type = skl::abix::model::canonical_type_id("test::UnknownFocus");
    FocusBackend missing(registry, &provider, unknown_type);
    assert(!missing.valid());
    assert(missing.status() == InterfaceStatus::interface_not_found);

    std::puts("focus backend: ok");
    return 0;
}
