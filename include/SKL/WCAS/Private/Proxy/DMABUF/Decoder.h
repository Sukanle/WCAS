#pragma once

#include "SKL/WCAS/API.h"
#include "SKL/WCAS/Types.h"

#include "SKL/ABIX/Interface/Declaration.h"

#include <stdint.h>

#include <string>

namespace skl::wcas {

enum class DmaBufDecoderKind : uint32_t {
    Auto = 0,
    Vulkan = 1,
    Egl = 2,
    Mmap = 3,
    Unknown = 0XFF,
};

/// One DMA-BUF plane. The fd is borrowed for the duration of decode().
struct DmaBufDescriptor {
    int32_t fd = -1;
    int32_t width = 0;
    int32_t height = 0;
    uint32_t stride = 0;
    uint64_t modifier = 0;
    uint32_t format = 0;
    uint32_t offset = 0;
};

struct DmaBufDecoded {
    cv::Mat image;
    DmaBufDecoderKind decoder = DmaBufDecoderKind::Unknown;
    /// Vulkan's readback can succeed on a modifier the driver does not list as
    /// supported; the pixels are then plausible but unverified.
    bool modifier_supported = false;
};

// DmaBufDecoder is an internal process-boundary contract: providers operate on
// borrowed fds and return a cv::Mat. A module-facing version should use POD
// frame descriptors and owner handles instead of cv::Mat.
#define SKL_WCAS_DMABUF_DECODER_METHODS(X)           \
    X(name, std::string, str, {})                    \
    X(kind, DmaBufDecoderKind, reg_dmabuf::kind, {}) \
    X(available, bool, bool_t, {})                   \
    X(supports, bool, bool_t, {u32, u64})            \
    X(decode, result<DmaBufDecoded>, reg_dmabuf::decoded, {reg_dmabuf::descriptor})

SKL_ABIX_INTERFACE_UNIQUE_DECLARE(
    DmaBufDecoder, SKL_WCAS_DMABUF_DECODER_METHODS, SKL_ABIX_VERSION_NUMBER(0, 1, 0), SKL_WCAS_API);

#undef SKL_WCAS_DMABUF_DECODER_METHODS

}   // namespace skl::wcas
