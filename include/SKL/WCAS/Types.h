#pragma once

#include <optional>

#include "SKL/ABIX/Model/ABI.h"

#include "opencv2/core.hpp"   // IWYU pragma: keep
#include "boost/outcome.hpp"


namespace skl::wcas {

/// Every public operation reports failure through this result type. It is kept
/// in its own header so the controller contract can refer to it without pulling
/// in the whole harness (and so `wcas.h` and `controller.h` do not include each
/// other).
template<typename T>
using result = boost::outcome_v2::std_result<T>;

template<typename T>
using optional = std::optional<T>;

enum class PointerButton : uint8_t {
    left,
    right,
    middle,
};

inline constexpr auto i32 = skl::abix::model::canonical_type_id("prim:i32");
inline constexpr auto u32 = skl::abix::model::canonical_type_id("prim:u32");
inline constexpr auto u64 = skl::abix::model::canonical_type_id("prim:u64");
inline constexpr auto f64 = skl::abix::model::canonical_type_id("prim:fieee64");
inline constexpr auto bool_t = skl::abix::model::canonical_type_id("prim:bool");
inline constexpr auto PtrBtn = skl::abix::model::canonical_type_id("PointerButton");
inline constexpr auto res_void_t = skl::abix::model::canonical_type_id("result<void>");
inline constexpr auto str = skl::abix::model::canonical_type_id("std::string");
inline constexpr auto ms = skl::abix::model::canonical_type_id("std::chrono::milliseconds");

struct reg_cv {
    struct Point {
        static constexpr auto D2 = skl::abix::model::canonical_type_id("cv::Point2d");
        static constexpr auto I2 = skl::abix::model::canonical_type_id("cv::Point2i");
    };
    static constexpr auto Size = skl::abix::model::canonical_type_id("cv::Size");
    static constexpr auto Mat = skl::abix::model::canonical_type_id("cv::Mat");
    static constexpr auto rect = skl::abix::model::canonical_type_id("cv::cv::Rect");
};

struct reg_device {
    static constexpr auto kind = skl::abix::model::canonical_type_id("skl::wcas::DeviceKind");
    static constexpr auto capability = skl::abix::model::canonical_type_id("skl::wcas::DeviceCapability");
};

struct reg_capture {
    static constexpr auto kind = skl::abix::model::canonical_type_id("skl::wcas::CaptureKind");
    static constexpr auto capability = skl::abix::model::canonical_type_id("skl::wcas::CaptureCapability");
};

struct reg_window {
    static constexpr auto list = skl::abix::model::canonical_type_id("std::vector<skl::wcas::WindowInfo>");
};

struct reg_launch {
    static constexpr auto kind = skl::abix::model::canonical_type_id("skl::wcas::LaunchKind");
};

struct reg_target {
    static constexpr auto id = skl::abix::model::canonical_type_id("skl::wcas::TargetId");
};

struct reg_transport {
    static constexpr auto kind = skl::abix::model::canonical_type_id("skl::wcas::TransportKind");
    static constexpr auto capability = skl::abix::model::canonical_type_id("skl::wcas::TransportCapability");
};

struct reg_dmabuf {
    static constexpr auto kind = skl::abix::model::canonical_type_id("skl::wcas::DmaBufDecoderKind");
    static constexpr auto descriptor = skl::abix::model::canonical_type_id("skl::wcas::DmaBufDescriptor*");
    static constexpr auto decoded = skl::abix::model::canonical_type_id("result<skl::wcas::DmaBufDecoded>");
};

struct reg_frame {
    static constexpr auto status = skl::abix::model::canonical_type_id("skl::wcas::SinkStatus");
    static constexpr auto view = skl::abix::model::canonical_type_id("skl::wcas::FrameView*");
    static constexpr auto state = skl::abix::model::canonical_type_id("skl::wcas::FrameState*");
};

}   // namespace skl::wcas
