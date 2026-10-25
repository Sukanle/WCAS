// Unit coverage for the capture slot scheduler: three-slot lifecycle,
// latest-frame selection and the all-slots-busy drop policy. Uses a memfd as a
// stand-in DMA-BUF so the test needs neither a compositor nor a GPU.

#include "SKL/WCAS/Private/Proxy/CaptureScheduler.h"
#include "SKL/WCAS/Private/Proxy/CaptureSlot.h"
#include "SKL/WCAS/Private/Proxy/Reactor.h"

#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include <opencv2/core.hpp>

namespace {

constexpr uint32_t drm_argb8888 = 0X34325241U;

int make_linear_dmabuf(int width, int height, uint32_t *stride) {
    *stride = static_cast<uint32_t>(width) * 4u;
    const size_t size = static_cast<size_t>(*stride) * static_cast<size_t>(height);
    const int fd = ::memfd_create("wcas-sched-test", MFD_CLOEXEC);
    assert(fd >= 0);
    assert(::ftruncate(fd, static_cast<off_t>(size)) == 0);
    void *base = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    assert(base != MAP_FAILED);
    for (int y = 0; y < height; ++y) {
        auto *row = static_cast<unsigned char *>(base) + static_cast<size_t>(y) * *stride;
        for (int x = 0; x < width; ++x) {
            const int i = y * width + x;
            row[x * 4 + 0] = static_cast<unsigned char>(i);
            row[x * 4 + 1] = static_cast<unsigned char>(i + 10);
            row[x * 4 + 2] = static_cast<unsigned char>(i + 20);
            row[x * 4 + 3] = 255;
        }
    }
    ::munmap(base, size);
    return fd;
}

skl::wcas::proxy::CaptureJob make_job(int memfd, int width, int height, uint32_t stride) {
    skl::wcas::proxy::CaptureJob job;
    job.fd = ::dup(memfd);
    assert(job.fd >= 0);
    job.width = width;
    job.height = height;
    job.format = drm_argb8888;
    job.stride = stride;
    job.offset = 0;
    return job;
}

void check_pixels(const cv::Mat &image, int width, int height) {
    assert(image.rows == height && image.cols == width);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int i = y * width + x;
            const auto px = image.at<cv::Vec3b>(y, x);
            assert(px[0] == static_cast<unsigned char>(i));
            assert(px[1] == static_cast<unsigned char>(i + 10));
            assert(px[2] == static_cast<unsigned char>(i + 20));
        }
    }
}

}   // namespace

int main() {
    using namespace skl::wcas::proxy;
    const int width = 4;
    const int height = 3;
    uint32_t stride = 0;
    const int memfd = make_linear_dmabuf(width, height, &stride);

    Reactor reactor;
    CaptureScheduler scheduler;
    assert(scheduler.valid());
    assert(scheduler.slot_states().size() == 3);
    assert(scheduler.has_free_slot());

    // First tick: a memfd has no producer fence, so the slot submits at once.
    assert(scheduler.on_timer_tick(make_job(memfd, width, height, stride), reactor));
    assert(scheduler.total_captures() == 1);
    assert(scheduler.total_completed() == 1);

    auto snapshot = scheduler.take_latest_snapshot();
    assert(snapshot.has_value());
    check_pixels(*snapshot, width, height);

    // Fill all three slots without consuming: the fourth tick must be dropped.
    assert(scheduler.on_timer_tick(make_job(memfd, width, height, stride), reactor));
    assert(scheduler.on_timer_tick(make_job(memfd, width, height, stride), reactor));
    assert(scheduler.on_timer_tick(make_job(memfd, width, height, stride), reactor));
    assert(!scheduler.has_free_slot());
    assert(!scheduler.on_timer_tick(make_job(memfd, width, height, stride), reactor));

    // Consuming the latest snapshot frees a slot for the next tick.
    snapshot = scheduler.take_latest_snapshot();
    assert(snapshot.has_value());
    check_pixels(*snapshot, width, height);
    assert(scheduler.has_free_slot());
    assert(scheduler.on_timer_tick(make_job(memfd, width, height, stride), reactor));

    ::close(memfd);
    std::printf("capture scheduler unit: ok\n");
    return 0;
}
