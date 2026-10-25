#ifndef SKL_WCAS_CAPTURE_H
#define SKL_WCAS_CAPTURE_H

#include "SKL/WCAS/API.h"
#include "SKL/WCAS/Types.h"

#include "SKL/ABIX/Interface/Declaration.h"

#include <stdint.h>

#include <string>

#include <opencv2/core.hpp>

namespace skl::wcas {

/// Capture implementation selected by the caller. The lowercase spellings are
/// kept as aliases for the pre-interface `CaptureBackend` names.
enum class CaptureKind : uint8_t {
    CLIENTBUF = 0,
    client_buffer = CLIENTBUF,
    KMS = 1,
    kms = KMS,
    SHM = 2,
    shm = SHM,
    DMABUF = 3,
    dmabuf = DMABUF,
    X11 = 4,
    x11 = X11,
    UNKNOWN = 0XFF,
};

using CaptureBackend = CaptureKind;

/// What a capture source can do. This is deliberately separate from
/// `DeviceCapability`: input devices and frame sources are different domains.
enum class CaptureCapability : uint8_t {
    UNKNOWN = 0X00,
    CLIENTBUF = 0X01,
    SHM = 0X02,
    DMABUF = 0X04,
    KMS = 0X08,
    X11 = 0X10,
    BACKGROUND = 0X20,
    WINDOWGEOMETRY = 0X40,
};

SKL_WCAS_ENABLE_BITMASK_OPERATORS(CaptureCapability);

struct SKL_WCAS_API CaptureTarget {
    optional<std::string> app_id;
    optional<std::string> title;
};

// A capture source has a common info contract (like DeviceInfo) plus the
// capture operations. `set_target` uses empty strings for "no constraint"
// because ABIX's declaration macro is intended for simple value parameters.
#define SKL_WCAS_CAPTURE_INFO_METHODS(X)        \
    X(id, uint64_t, u64, {})                    \
    X(name, std::string, str, {})               \
    X(connected, bool, bool_t, {})              \
    X(kind, CaptureKind, reg_capture::kind, {}) \
    X(capabilities, CaptureCapability, reg_capture::capability, {})

#define SKL_WCAS_CAPTURE_SOURCE_METHODS(X)              \
    X(set_target, result<void>, res_void_t, {str, str}) \
    X(capture, result<cv::Mat>, reg_cv::Mat, {})        \
    X(frame_size, optional<cv::Size>, reg_cv::Size, {})

SKL_ABIX_INTERFACE_UNIQUE_DECLARE(
    CaptureInfo, SKL_WCAS_CAPTURE_INFO_METHODS, SKL_ABIX_VERSION_NUMBER(0, 1, 0), SKL_WCAS_API);
SKL_ABIX_INTERFACE_UNIQUE_DECLARE(
    CaptureSource, SKL_WCAS_CAPTURE_SOURCE_METHODS, SKL_ABIX_VERSION_NUMBER(0, 1, 0), SKL_WCAS_API);

#undef SKL_WCAS_CAPTURE_INFO_METHODS
#undef SKL_WCAS_CAPTURE_SOURCE_METHODS

}   // namespace skl::wcas

#endif   // SKL_WCAS_CAPTURE_H
