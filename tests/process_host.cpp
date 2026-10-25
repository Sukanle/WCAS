#include "SKL/WCAS/Private/Harness/ProcessHost.h"

#include <assert.h>
#include <chrono>
#include <stdio.h>
#include <thread>

using namespace skl::wcas;

int main() {
    {
        ProcessSpec spec;
        spec.program = "/bin/true";
        spec.inherit_diagnostics = false;
        ProcessHost host;
        auto spawned = host.spawn(spec);
        assert(spawned.has_value());
        assert(host.pid() > 0);

        bool exited = false;
        for (int i = 0; i < 100 && !exited; ++i) {
            exited = host.poll_exited();
            if (!exited) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        assert(exited);
        assert(!host.alive());
    }

    {
        ProcessSpec spec;
        spec.program = "/bin/sleep";
        spec.args = {"30"};
        spec.inherit_diagnostics = false;
        ProcessHost host;
        assert(host.spawn(spec).has_value());
        assert(host.alive());
        host.terminate();
        assert(!host.alive());
        assert(host.pid() == -1);
    }

    std::puts("process host: ok");
    return 0;
}
