#include "harness/health_monitor.h"

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

class FakeHealth {
public:
    std::string name() const { return "fake-health"; }
    bool available() const { return true; }
    bool alive() const { return alive_; }
    bool connected() const { return connected_; }
    uint64_t commit_count() const { return commits_; }
    uint64_t presented_frames() const { return presented_; }

    bool alive_ = true;
    bool connected_ = true;
    uint64_t commits_ = 12;
    uint64_t presented_ = 7;
};

const InterfaceMethod kMethods[] = {
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(HealthMonitorTraits, name, &FakeHealth::name),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(HealthMonitorTraits, available, &FakeHealth::available),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(HealthMonitorTraits, alive, &FakeHealth::alive),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(HealthMonitorTraits, connected, &FakeHealth::connected),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(HealthMonitorTraits, commit_count, &FakeHealth::commit_count),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(HealthMonitorTraits, presented_frames, &FakeHealth::presented_frames),
};

constexpr TypeId kFakeType = skl::abix::model::canonical_type_id("test::FakeHealth");

}   // namespace

int main() {
    InterfaceRegistry<2> registry;
    assert(registry.add({HealthMonitorTraits::id, kFakeType, kMethods, static_cast<std::uint32_t>(std::size(kMethods)),
               HealthMonitorTraits::version, 0})
           == InterfaceStatus::ok);

    FakeHealth provider;
    HealthMonitor health(registry, &provider, kFakeType);
    assert(health.valid());
    assert(health.name() == "fake-health");
    assert(health.available());
    assert(health.alive());
    assert(health.connected());
    assert(health.commit_count() == 12);
    assert(health.presented_frames() == 7);

    provider.alive_ = false;
    provider.connected_ = false;
    assert(!health.alive());
    assert(!health.connected());

    constexpr TypeId unknown_type = skl::abix::model::canonical_type_id("test::UnknownHealth");
    HealthMonitor missing(registry, &provider, unknown_type);
    assert(!missing.valid());
    assert(missing.status() == InterfaceStatus::interface_not_found);

    std::puts("health monitor: ok");
    return 0;
}
