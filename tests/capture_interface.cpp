// Contract test for CaptureInfo / CaptureSource. The fake provider proves that
// a capture backend can be a plain struct, bound independently of the concrete
// auto-capture implementation used by ControlledApp.
#include "skl/wcas/capture.h"

#include <cassert>
#include <cstdio>
#include <iterator>
#include <string>

#include <opencv2/core.hpp>

namespace {

using namespace skl::wcas;
using skl::abix::model::TypeId;
using skl::abix::poly::interface_entry;
using skl::abix::poly::InterfaceMethod;
using skl::abix::poly::InterfaceRegistry;
using skl::abix::poly::InterfaceStatus;

struct FakeCapture {
    std::string app_id;
    std::string title;
    int captures = 0;
};

std::uint64_t fake_id(const void *) { return 7; }
std::string fake_name(const void *) { return "fake-shm"; }
bool fake_connected(const void *) { return true; }
CaptureKind fake_kind(const void *) { return CaptureKind::SHM; }
CaptureCapability fake_capabilities(const void *) { return CaptureCapability::SHM | CaptureCapability::WINDOWGEOMETRY; }

result<void> fake_set_target(void *self, std::string app_id, std::string title) {
    auto *capture = static_cast<FakeCapture *>(self);
    capture->app_id = std::move(app_id);
    capture->title = std::move(title);
    return boost::outcome_v2::success();
}

result<cv::Mat> fake_capture(const void *self) {
    auto *capture = const_cast<FakeCapture *>(static_cast<const FakeCapture *>(self));
    ++capture->captures;
    return cv::Mat{cv::Mat::zeros(4, 5, CV_8UC3)};
}

optional<cv::Size> fake_frame_size(const void *) { return cv::Size{5, 4}; }

// The member-entry macro needs member functions; wrap the free fake adapters
// in a class so the test exercises the same binding style as capture_source.cpp.
class FakeCaptureDevice {
public:
    std::uint64_t id() const { return fake_id(nullptr); }
    std::string name() const { return fake_name(nullptr); }
    bool connected() const { return fake_connected(nullptr); }
    CaptureKind kind() const { return fake_kind(nullptr); }
    CaptureCapability capabilities() const { return fake_capabilities(nullptr); }

    result<void> set_target(std::string app_id, std::string title) {
        return fake_set_target(&state_, std::move(app_id), std::move(title));
    }
    result<cv::Mat> capture() const { return fake_capture(&state_); }
    optional<cv::Size> frame_size() const { return fake_frame_size(nullptr); }

    FakeCapture state_;
};

const InterfaceMethod kDeviceInfoMethods[] = {
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(CaptureInfoTraits, id, &FakeCaptureDevice::id),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(CaptureInfoTraits, name, &FakeCaptureDevice::name),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(CaptureInfoTraits, connected, &FakeCaptureDevice::connected),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(CaptureInfoTraits, kind, &FakeCaptureDevice::kind),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(CaptureInfoTraits, capabilities, &FakeCaptureDevice::capabilities),
};

const InterfaceMethod kDeviceSourceMethods[] = {
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(CaptureSourceTraits, set_target, &FakeCaptureDevice::set_target),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(CaptureSourceTraits, capture, &FakeCaptureDevice::capture),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(CaptureSourceTraits, frame_size, &FakeCaptureDevice::frame_size),
};

constexpr TypeId kFakeType = skl::abix::model::canonical_type_id("test::FakeCaptureDevice");

}   // namespace

int main() {
    InterfaceRegistry<4> registry;
    assert(registry.add({CaptureInfoTraits::id, kFakeType, kDeviceInfoMethods,
               static_cast<std::uint32_t>(std::size(kDeviceInfoMethods)), CaptureInfoTraits::version, 0})
           == InterfaceStatus::ok);
    assert(registry.add({CaptureSourceTraits::id, kFakeType, kDeviceSourceMethods,
               static_cast<std::uint32_t>(std::size(kDeviceSourceMethods)), CaptureSourceTraits::version, 0})
           == InterfaceStatus::ok);

    FakeCaptureDevice device;
    CaptureInfo info(registry, &device, kFakeType);
    CaptureSource source(registry, &device, kFakeType);
    assert(info.valid());
    assert(source.valid());
    assert(info.id() == 7);
    assert(info.name() == "fake-shm");
    assert(info.connected());
    assert(info.kind() == CaptureKind::SHM);
    assert((info.capabilities() & CaptureCapability::SHM) != CaptureCapability::UNKNOWN);

    assert(source.set_target(std::string("app"), std::string("title")).has_value());
    assert(device.state_.app_id == "app");
    assert(device.state_.title == "title");

    auto frame = source.capture();
    assert(frame.has_value());
    assert(frame.value().cols == 5);
    assert(frame.value().rows == 4);
    assert(device.state_.captures == 1);

    const auto size = source.frame_size();
    assert(size.has_value());
    assert(size->width == 5);
    assert(size->height == 4);

    // A type that did not register the contract must fail explicitly.
    constexpr TypeId unknown_type = skl::abix::model::canonical_type_id("test::UnknownCapture");
    CaptureSource missing(registry, &device, unknown_type);
    assert(!missing.valid());
    assert(missing.status() == InterfaceStatus::interface_not_found);

    std::puts("capture interface: ok");
    return 0;
}
