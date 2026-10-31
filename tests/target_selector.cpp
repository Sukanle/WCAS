#include "SKL/WCAS/Private/Harness/TargetSelector.h"
#include "SKL/WCAS/Error.h"

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

class FakeTarget {
public:
    std::string name() const { return "fake-target"; }
    bool available() const { return true; }
    result<void> select(int32_t index) {
        if (index != 1) return skl::wcas::from_std_errc(std::errc::invalid_argument);
        selected_ = 77;
        return boost::outcome_v2::success();
    }
    uint32_t selected() const { return selected_; }
    result<void> clear() {
        selected_ = 0;
        target_ = {};
        return boost::outcome_v2::success();
    }
    result<void> select_target(uint64_t value, uint32_t kind) {
        target_ = TargetId{value, static_cast<TargetKind>(kind)};
        selected_ = static_cast<uint32_t>(value);
        return boost::outcome_v2::success();
    }
    TargetId selected_target() const { return target_; }

private:
    uint32_t selected_ = 0;
    TargetId target_{};
};

const InterfaceMethod kMethods[] = {
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(TargetSelectorTraits, name, &FakeTarget::name),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(TargetSelectorTraits, available, &FakeTarget::available),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(TargetSelectorTraits, select, &FakeTarget::select),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(TargetSelectorTraits, selected, &FakeTarget::selected),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(TargetSelectorTraits, clear, &FakeTarget::clear),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(TargetSelectorTraits, select_target, &FakeTarget::select_target),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(TargetSelectorTraits, selected_target, &FakeTarget::selected_target),
};

constexpr TypeId kFakeType = skl::abix::model::canonical_type_id("test::FakeTarget");

}   // namespace

int main() {
    InterfaceRegistry<2> registry;
    assert(registry.add({TargetSelectorTraits::id, kFakeType, kMethods, static_cast<uint32_t>(std::size(kMethods)),
               TargetSelectorTraits::version, 0})
           == InterfaceStatus::ok);

    FakeTarget provider;
    TargetSelector target(registry, &provider, kFakeType);
    assert(target.valid());
    assert(target.name() == "fake-target");
    assert(target.available());
    assert(target.selected() == 0);
    assert(target.select(1).has_value());
    assert(target.selected() == 77);
    assert(target.select_target(7, static_cast<uint32_t>(TargetKind::WaylandSurface)).has_value());
    assert(target.selected() == 7);
    assert(target.selected_target() == (TargetId{7, TargetKind::WaylandSurface}));
    assert(target.clear().has_value());
    assert(target.selected() == 0);
    assert(target.selected_target() == TargetId{});

    constexpr TypeId unknown_type = skl::abix::model::canonical_type_id("test::UnknownTarget");
    TargetSelector missing(registry, &provider, unknown_type);
    assert(!missing.valid());
    assert(missing.status() == InterfaceStatus::interface_not_found);

    std::puts("target selector: ok");
    return 0;
}
