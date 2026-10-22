// Contract test for WindowEnumerator / WindowResolver. A plain fake provider
// proves both interfaces can be registered and bound independently.
#include "skl/wcas/window.h"

#include <cassert>
#include <cstdio>
#include <iterator>
#include <string>
#include <vector>

namespace {

using namespace skl::wcas;
using skl::abix::model::TypeId;
using skl::abix::poly::InterfaceMethod;
using skl::abix::poly::InterfaceRegistry;
using skl::abix::poly::InterfaceStatus;

class FakeWindowProvider {
public:
    std::string name() const { return "fake-window"; }
    bool connected() const { return true; }

    result<std::vector<WindowInfo>> list() const {
        WindowInfo first;
        first.app_id = "demo";
        first.title = "Demo Window";
        first.width = 1'280;
        first.height = 720;
        first.pid = 42;
        first.selected = true;
        first.origin = WindowOrigin::Toplevel;
        first.surface_id = 7;
        return boost::outcome_v2::success(std::vector<WindowInfo>{std::move(first)});
    }

    result<Rect> find_by_pid(int32_t pid) const {
        if (pid != 42) return std::make_error_code(std::errc::invalid_argument);
        return Rect{10, 20, 1'280, 720};
    }

    result<Rect> find_by_size_and_title(int32_t width, int32_t height, std::string title) const {
        if (width != 1'280 || height != 720 || title != "Demo Window")
            return std::make_error_code(std::errc::invalid_argument);
        return Rect{10, 20, 1'280, 720};
    }
};

const InterfaceMethod kEnumeratorMethods[] = {
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(WindowEnumeratorTraits, name, &FakeWindowProvider::name),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(WindowEnumeratorTraits, connected, &FakeWindowProvider::connected),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(WindowEnumeratorTraits, list, &FakeWindowProvider::list),
};

const InterfaceMethod kResolverMethods[] = {
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(WindowResolverTraits, name, &FakeWindowProvider::name),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(WindowResolverTraits, connected, &FakeWindowProvider::connected),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(WindowResolverTraits, find_by_pid, &FakeWindowProvider::find_by_pid),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(
        WindowResolverTraits, find_by_size_and_title, &FakeWindowProvider::find_by_size_and_title),
};

constexpr TypeId kFakeType = skl::abix::model::canonical_type_id("test::FakeWindowProvider");

}   // namespace

int main() {
    InterfaceRegistry<4> registry;
    assert(registry.add({WindowEnumeratorTraits::id, kFakeType, kEnumeratorMethods,
               static_cast<std::uint32_t>(std::size(kEnumeratorMethods)), WindowEnumeratorTraits::version, 0})
           == InterfaceStatus::ok);
    assert(registry.add({WindowResolverTraits::id, kFakeType, kResolverMethods,
               static_cast<std::uint32_t>(std::size(kResolverMethods)), WindowResolverTraits::version, 0})
           == InterfaceStatus::ok);

    FakeWindowProvider provider;
    WindowEnumerator enumerator(registry, &provider, kFakeType);
    WindowResolver resolver(registry, &provider, kFakeType);
    assert(enumerator.valid());
    assert(resolver.valid());
    assert(enumerator.connected());
    assert(resolver.connected());

    auto windows = enumerator.list();
    assert(windows.has_value());
    assert(windows.value().size() == 1);
    assert(windows.value()[0].app_id == "demo");
    assert(windows.value()[0].title == "Demo Window");
    assert(windows.value()[0].width == 1'280);
    assert(windows.value()[0].height == 720);
    assert(windows.value()[0].selected);

    const auto by_pid = resolver.find_by_pid(42);
    assert(by_pid.has_value());
    assert(by_pid.value() == Rect(10, 20, 1'280, 720));

    const auto by_title = resolver.find_by_size_and_title(1'280, 720, std::string("Demo Window"));
    assert(by_title.has_value());
    assert(by_title.value() == Rect(10, 20, 1'280, 720));

    constexpr TypeId unknown_type = skl::abix::model::canonical_type_id("test::UnknownWindowProvider");
    WindowEnumerator missing(registry, &provider, unknown_type);
    assert(!missing.valid());
    assert(missing.status() == InterfaceStatus::interface_not_found);

    std::puts("window interface: ok");
    return 0;
}
