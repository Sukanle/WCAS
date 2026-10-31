#include "SKL/WCAS/Private/SDM/Channel.h"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#ifndef _WIN32
#  include <sys/wait.h>
#  include <unistd.h>
#endif

namespace sdm = skl::wcas::sdm;

namespace {

constexpr std::uint32_t kWidth = 64;
constexpr std::uint32_t kHeight = 64;
constexpr std::uint32_t kStride = kWidth * 3;
constexpr std::size_t kBytes = static_cast<std::size_t>(kStride) * kHeight;

sdm::FrameInfo info() {
    sdm::FrameInfo result;
    result.width = kWidth;
    result.height = kHeight;
    result.stride = kStride;
    result.format = sdm::PixelFormat::Bgr8;
    result.timestamp_ns = 42;
    return result;
}

void test_single_process() {
    const std::string name = sdm::Channel::unique_name("wcas-sdm-test");
    std::string error;

    sdm::Config config;
    config.slot_count = 3;
    config.slot_bytes = static_cast<std::uint32_t>(kBytes);
    auto writer = sdm::Channel::create(name, config, error);
    assert(writer);
    assert(error.empty());
    auto reader = sdm::Channel::open(name, error);
    assert(reader);
    assert(error.empty());

    sdm::FrameView view;
    assert(!reader->read_latest(view, error));

    std::vector<std::uint8_t> frame(kBytes, 0);
    for (std::uint32_t seq = 1; seq <= 3; ++seq) {
        std::memset(frame.data(), static_cast<int>(seq), frame.size());
        assert(writer->write(frame.data(), frame.size(), info(), error));
        assert(error.empty());
    }

    assert(reader->wait_for_frame(0, std::chrono::milliseconds(100)));
    assert(reader->read_latest(view, error));
    assert(error.empty());
    assert(view.seq == 3);
    assert(view.size == kBytes);
    assert(view.data[0] == 3);
    reader->release(view);
    assert(view.data == nullptr);

    std::memset(frame.data(), 9, frame.size());
    assert(writer->write(frame.data(), frame.size(), info(), error));
    assert(reader->read_latest(view, error));
    assert(view.seq == 4);
    assert(view.data[0] == 9);
    reader->release(view);

    const std::uint32_t latest = writer->latest_seq();
    assert(latest == 4);
    assert(!reader->wait_for_frame(latest, std::chrono::milliseconds(1)));

    writer.reset();
    sdm::Channel::remove(name);
}

#ifndef _WIN32
void test_cross_process() {
    const std::string name = sdm::Channel::unique_name("wcas-sdm-fork");
    std::string error;
    sdm::Config config;
    config.slot_count = 2;
    config.slot_bytes = static_cast<std::uint32_t>(kBytes);

    auto writer = sdm::Channel::create(name, config, error);
    assert(writer);

    int ready[2] = {-1, -1};
    assert(::pipe(ready) == 0);
    const pid_t child = ::fork();
    assert(child >= 0);

    if (child == 0) {
        ::close(ready[0]);
        auto reader = sdm::Channel::open(name, error);
        if (!reader) _exit(2);
        const char byte = '1';
        (void)::write(ready[1], &byte, 1);
        ::close(ready[1]);
        if (!reader->wait_for_frame(0, std::chrono::seconds(2))) _exit(3);
        sdm::FrameView view;
        if (!reader->read_latest(view, error)) _exit(4);
        const bool ok = view.seq == 1 && view.size == kBytes && view.data[0] == 77;
        reader->release(view);
        _exit(ok ? 0 : 5);
    }

    ::close(ready[1]);
    char byte = 0;
    assert(::read(ready[0], &byte, 1) == 1);
    ::close(ready[0]);

    std::vector<std::uint8_t> frame(kBytes, 77);
    assert(writer->write(frame.data(), frame.size(), info(), error));

    int status = 0;
    assert(::waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 0);

    writer.reset();
    sdm::Channel::remove(name);
}
#endif

}   // namespace

int main() {
    test_single_process();
#ifndef _WIN32
    test_cross_process();
#endif
    std::cout << "sdm channel: ok\n";
    return 0;
}
