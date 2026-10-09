#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include <opencv2/core.hpp>

namespace skl::wcas::proxy {

/// A frame read back through the GPU. `pixels` is tightly packed
/// (`width * height * 4`) in the source channel order of the imported format.
struct VulkanFrame {
    std::vector<std::uint8_t> pixels;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

/// True when a Vulkan device exposing the DMA-BUF import extensions is
/// available. False when the process has no usable Vulkan driver.
bool vulkan_dmabuf_available();

/// Import `fd` as a Vulkan image, GPU-copy it into a host-visible staging
/// buffer and read the pixels back. Returns `nullopt` on any failure (no
/// Vulkan, unsupported format, driver rejects the modifier) so callers can
/// fall back to a linear mmap. This is the only path that can read tiled or
/// otherwise non-mappable buffers.
std::optional<VulkanFrame> vulkan_import_dmabuf(int fd, std::uint32_t width, std::uint32_t height, std::uint32_t stride,
    std::uint64_t modifier, std::uint32_t fourcc, std::uint32_t plane_offset);

/// Export the producer fence of a DMA-BUF as a sync fd. Returns -1 when the
/// driver does not support `DMA_BUF_IOCTL_EXPORT_SYNC_FILE`. When the returned
/// fd is >= 0, `*active` reports whether a real fence must be waited on.
int export_dmabuf_sync_file(int dmabuf_fd, bool *active);

/// Capture a single-plane DMA-BUF into a BGR `cv::Mat`. Tries the Vulkan
/// import path first (tiled/non-mappable buffers) and falls back to a linear
/// mmap. Returns false when the format is unsupported or neither path works.
bool capture_dmabuf_plane(int fd, std::int32_t width, std::int32_t height, std::uint32_t stride, std::uint64_t modifier,
    std::uint32_t format, std::uint32_t offset, cv::Mat &out);

}   // namespace skl::wcas::proxy
