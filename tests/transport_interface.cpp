#include "skl/wcas/frame.h"
// Compile/contract test for the reserved TransportBackend descriptor. It does
// not implement an X11 or platform backend; it only pins the capability bits
// and ABIX binding shape for future providers.
#include "skl/wcas/transport.h"

#include <cassert>
#include <cstdio>
#include <iterator>
#include <string>

namespace {

using namespace skl::wcas;
using skl::abix::model::TypeId;
using skl::abix::poly::InterfaceMethod;
using skl::abix::poly::InterfaceRegistry;
using skl::abix::poly::InterfaceStatus;

class FakeTransport {
public:
    std::string name() const { return "fake-transport"; }
    TransportKind kind() const { return TransportKind::Custom; }
    bool available() const { return true; }
    TransportCapability capabilities() const {
        return TransportCapability::Capture | TransportCapability::WindowEnumeration;
    }
};

const InterfaceMethod kMethods[] = {
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(TransportBackendTraits, name, &FakeTransport::name),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(TransportBackendTraits, kind, &FakeTransport::kind),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(TransportBackendTraits, available, &FakeTransport::available),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(TransportBackendTraits, capabilities, &FakeTransport::capabilities),
};

constexpr TypeId kFakeType = skl::abix::model::canonical_type_id("test::FakeTransport");

}   // namespace

int main() {
    InterfaceRegistry<2> registry;
    assert(registry.add({TransportBackendTraits::id, kFakeType, kMethods,
               static_cast<std::uint32_t>(std::size(kMethods)), TransportBackendTraits::version, 0})
           == InterfaceStatus::ok);

    FakeTransport provider;
    TransportBackend backend(registry, &provider, kFakeType);
    assert(backend.valid());
    assert(backend.name() == "fake-transport");
    assert(backend.kind() == TransportKind::Custom);
    assert(backend.available());
    const auto caps = backend.capabilities();
    assert((caps & TransportCapability::Capture) == TransportCapability::Capture);
    assert((caps & TransportCapability::WindowEnumeration) == TransportCapability::WindowEnumeration);
    assert(static_cast<uint64_t>(caps & TransportCapability::Pointer) == 0);

    TargetId target{7, TargetKind::WaylandSurface};
    assert(target == (TargetId{7, TargetKind::WaylandSurface}));
    assert(target != (TargetId{7, TargetKind::X11Window}));
    SessionHandle session{1, TransportKind::Custom};
    assert(session.value == 1);
    LaunchSpec launch;
    launch.program = "demo";
    launch.args = {"--flag"};
    assert(launch.program == "demo" && launch.args.size() == 1);
    FrameDescriptor descriptor{};
    descriptor.width = 4;
    assert(descriptor.width == 4);

    constexpr TypeId unknown_type = skl::abix::model::canonical_type_id("test::UnknownTransport");
    TransportBackend missing(registry, &provider, unknown_type);
    assert(!missing.valid());
    assert(missing.status() == InterfaceStatus::interface_not_found);

    std::puts("transport interface: ok");
    return 0;
}
