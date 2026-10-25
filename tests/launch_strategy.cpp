#include "SKL/WCAS/Private/Harness/LaunchStrategy.h"

#include <assert.h>
#include <stdio.h>

using namespace skl::wcas;

int main() {
    auto strategies = launch_strategies();
    assert(strategies.size() >= 2);
    assert(strategies[0].valid());
    assert(strategies[0].kind() == LaunchKind::NativeWayland);
    assert(!strategies[0].requires_x11());
    assert(strategies[0].priority() > 0);
    assert(strategies[1].valid());
    assert(strategies[1].kind() == LaunchKind::XWayland);
    assert(strategies[1].requires_x11());
    assert(strategies[0].priority() >= strategies[1].priority());
    assert(!strategies[0].name().empty());
    std::puts("launch strategy: ok");
    return 0;
}
