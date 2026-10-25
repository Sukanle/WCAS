// Unit coverage for the DMA-BUF sync probe and the tracked-fd registry. Uses a
// memfd: a valid fd that is not a real DMA-BUF, so the ioctls fail cleanly
// while the probe structure and reporting are still exercised.

#include "SKL/WCAS/DMABUF.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <assert.h>
#include <stdio.h>
#include <string>

int main() {
    using namespace skl::wcas;

    const int memfd = ::memfd_create("wcas-probe-test", MFD_CLOEXEC);
    assert(memfd >= 0);
    assert(::ftruncate(memfd, 4'096) == 0);

    DmaBufSyncProbe probe(memfd);
    const auto results = probe.run_all();
    assert(!results.basic_info.empty());
    assert(results.results.size() >= 10);

    const auto report = results.report();
    assert(report.find("DMA-BUF 同步能力测试报告") != std::string::npos);
    assert(report.find("合计:") != std::string::npos);
    // A live fd must always validate.
    assert(results.results.front().passed);

    // Tracking helpers dedupe and clear.
    clear_tracked_dmabuf_fds();
    track_dmabuf_fd(memfd);
    track_dmabuf_fd(memfd);
    track_dmabuf_fd(42);
    assert(tracked_dmabuf_fds().size() == 2);
    clear_tracked_dmabuf_fds();
    assert(tracked_dmabuf_fds().empty());

    ::close(memfd);
    std::printf("dmabuf probe unit: ok\n");
    return 0;
}
