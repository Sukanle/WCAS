// Contract test for the DMA-BUF decoder interface. Actual Vulkan/EGL paths are
// hardware/environment dependent; this test pins the provider/binding shape and
// the decode result contract.
#include "SKL/WCAS/Private/Proxy/DMABUF/Decoder.h"
#include "SKL/WCAS/Error.h"

#include <assert.h>
#include <stdio.h>
#include <iterator>
#include <string>

#include <opencv2/core.hpp>

namespace {

using namespace skl::wcas;
using skl::abix::model::TypeId;
using skl::abix::poly::InterfaceMethod;
using skl::abix::poly::InterfaceRegistry;
using skl::abix::poly::InterfaceStatus;

class FakeDecoder {
public:
    std::string name() const { return "fake"; }
    DmaBufDecoderKind kind() const { return DmaBufDecoderKind::Mmap; }
    bool available() const { return true; }
    bool supports(uint32_t fourcc, uint64_t modifier) const { return fourcc == 0x34325241U && modifier == 0; }
    result<DmaBufDecoded> decode(const DmaBufDescriptor *descriptor) const {
        if (descriptor == nullptr || descriptor->fd < 0) return skl::wcas::from_std_errc(std::errc::invalid_argument);
        return DmaBufDecoded{
            cv::Mat(descriptor->height, descriptor->width, CV_8UC3, cv::Scalar(1, 2, 3)), kind(), true};
    }
};

const InterfaceMethod kMethods[] = {
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(DmaBufDecoderTraits, name, &FakeDecoder::name),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(DmaBufDecoderTraits, kind, &FakeDecoder::kind),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(DmaBufDecoderTraits, available, &FakeDecoder::available),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(DmaBufDecoderTraits, supports, &FakeDecoder::supports),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(DmaBufDecoderTraits, decode, &FakeDecoder::decode),
};

constexpr TypeId kFakeType = skl::abix::model::canonical_type_id("test::FakeDmaBufDecoder");

}   // namespace

int main() {
    InterfaceRegistry<4> registry;
    const auto status = registry.add({DmaBufDecoderTraits::id, kFakeType, kMethods,
        static_cast<uint32_t>(std::size(kMethods)), DmaBufDecoderTraits::version, 0});
    assert(status == InterfaceStatus::ok);

    FakeDecoder provider;
    DmaBufDecoder decoder(registry, &provider, kFakeType);
    assert(decoder.valid());
    assert(decoder.name() == "fake");
    assert(decoder.kind() == DmaBufDecoderKind::Mmap);
    assert(decoder.available());
    assert(decoder.supports(0x34325241U, 0));
    assert(!decoder.supports(0x34325258U, 0));

    DmaBufDescriptor descriptor;
    descriptor.fd = 3;
    descriptor.width = 5;
    descriptor.height = 4;
    descriptor.stride = 20;
    descriptor.format = 0x34325241U;
    const auto decoded = decoder.decode(&descriptor);
    assert(decoded.has_value());
    assert(decoded.value().image.cols == 5);
    assert(decoded.value().image.rows == 4);
    assert(decoded.value().decoder == DmaBufDecoderKind::Mmap);

    constexpr TypeId unknown_type = skl::abix::model::canonical_type_id("test::UnknownDmaBufDecoder");
    DmaBufDecoder missing(registry, &provider, unknown_type);
    assert(!missing.valid());
    assert(missing.status() == InterfaceStatus::interface_not_found);

    std::puts("dmabuf decoder interface: ok");
    return 0;
}
