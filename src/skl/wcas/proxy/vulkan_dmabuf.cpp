#include "vulkan_dmabuf.h"

#include <vulkan/vulkan.h>

#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

namespace skl::wcas::proxy {
namespace {

constexpr std::uint32_t drm_argb8888 = 0X34325241U;
constexpr std::uint32_t drm_xrgb8888 = 0X34325258U;
constexpr std::uint32_t drm_abgr8888 = 0X34324241U;
constexpr std::uint32_t drm_xbgr8888 = 0X34324258U;

VkFormat fourcc_to_vk(std::uint32_t fourcc) {
    switch (fourcc) {
        case drm_argb8888:
        case drm_xrgb8888: return VK_FORMAT_B8G8R8A8_UNORM;
        case drm_abgr8888:
        case drm_xbgr8888: return VK_FORMAT_R8G8B8A8_UNORM;
        default:           return VK_FORMAT_UNDEFINED;
    }
}

// ── sync_file (DMA_BUF_IOCTL_EXPORT_SYNC_FILE) ─────────────────────────
// _IOWR('b', 2, struct dma_buf_export_sync_file) = 0xC0086202
struct DmaBufExportSyncFile {
    std::uint32_t flags;
    std::int32_t fd;
};
constexpr std::uint32_t dma_buf_sync_read = 1U << 0;
constexpr unsigned long dma_buf_ioctl_export_sync_file =
    (3ul << 30) | (sizeof(DmaBufExportSyncFile) << 16) | (static_cast<unsigned long>('b') << 8) | 2UL;

struct ExportedSync {
    int fd = -1;
    /// True when a real producer fence exists and must be waited on.
    bool active = false;
};

ExportedSync export_sync_file(int dmabuf_fd) {
    ExportedSync result;
    DmaBufExportSyncFile exported{dma_buf_sync_read, -1};
    if (::ioctl(dmabuf_fd, dma_buf_ioctl_export_sync_file, &exported) != 0) return result;
    if (exported.fd < 0) return result;
    result.fd = exported.fd;
    pollfd ready{exported.fd, POLLIN, 0};
    const int polled = ::poll(&ready, 1, 0);
    // poll() == 0 means the fence has not signaled yet -> a real producer fence.
    result.active = polled <= 0;
    return result;
}

// ── shared Vulkan device ───────────────────────────────────────────────
// A single instance/device is critical on NVIDIA: importing the same DMA-BUF
// into two independent instances crashes the driver.
struct Device {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    std::uint32_t queue_family = 0;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    bool valid = false;
    /// Reused host-visible staging buffer (mirrors the Rust cached staging).
    /// Kept across captures and only recreated when a larger size is required.
    /// Guarded by `g_device_mutex`.
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_memory = VK_NULL_HANDLE;
    VkDeviceSize staging_size = 0;
};

/// The device is shared by every session; this serializes imports and guards
/// the cached staging buffer. It is a file-scope mutex (not a Device member)
/// so `Device` stays movable for the static initializer below.
std::mutex g_device_mutex;

const std::array<const char *, 3> kRequiredExtensions = {
    "VK_KHR_external_memory_fd",
    "VK_EXT_external_memory_dma_buf",
    "VK_EXT_image_drm_format_modifier",
};

Device &global_device() {
    static Device device = [] {
        Device d;
        VkApplicationInfo app_info{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app_info.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        instance_info.pApplicationInfo = &app_info;
        if (vkCreateInstance(&instance_info, nullptr, &d.instance) != VK_SUCCESS) return d;

        std::uint32_t physical_count = 0;
        vkEnumeratePhysicalDevices(d.instance, &physical_count, nullptr);
        if (physical_count == 0) return d;
        std::vector<VkPhysicalDevice> physicals(physical_count);
        vkEnumeratePhysicalDevices(d.instance, &physical_count, physicals.data());

        // Prefer a discrete GPU: on hybrid systems the app's DMA-BUFs come
        // from it, and it has the best transfer throughput.
        const auto rank = [](VkPhysicalDeviceType type) {
            switch (type) {
                case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return 0;
                case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 1;
                case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    return 2;
                default:                                     return 3;
            }
        };
        std::stable_sort(physicals.begin(), physicals.end(), [&](VkPhysicalDevice a, VkPhysicalDevice b) {
            VkPhysicalDeviceProperties pa{}, pb{};
            vkGetPhysicalDeviceProperties(a, &pa);
            vkGetPhysicalDeviceProperties(b, &pb);
            return rank(pa.deviceType) < rank(pb.deviceType);
        });

        for (const auto physical : physicals) {
            std::uint32_t extension_count = 0;
            vkEnumerateDeviceExtensionProperties(physical, nullptr, &extension_count, nullptr);
            std::vector<VkExtensionProperties> extensions(extension_count);
            vkEnumerateDeviceExtensionProperties(physical, nullptr, &extension_count, extensions.data());
            bool supported = true;
            for (const auto *required : kRequiredExtensions) {
                bool found = false;
                for (const auto &extension : extensions) {
                    if (std::strcmp(extension.extensionName, required) == 0) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    supported = false;
                    break;
                }
            }
            if (!supported) continue;

            std::uint32_t family_count = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, nullptr);
            std::vector<VkQueueFamilyProperties> families(family_count);
            vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, families.data());
            std::uint32_t family = UINT32_MAX;
            for (std::uint32_t i = 0; i < family_count; ++i) {
                if (families[i].queueFlags & VK_QUEUE_TRANSFER_BIT) {
                    family = i;
                    break;
                }
            }
            if (family == UINT32_MAX) continue;

            std::vector<const char *> enabled(kRequiredExtensions.begin(), kRequiredExtensions.end());
            enabled.push_back("VK_KHR_external_semaphore_fd");
            float priority = 1.0F;
            VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
            queue_info.queueFamilyIndex = family;
            queue_info.queueCount = 1;
            queue_info.pQueuePriorities = &priority;
            VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
            device_info.queueCreateInfoCount = 1;
            device_info.pQueueCreateInfos = &queue_info;
            device_info.enabledExtensionCount = static_cast<std::uint32_t>(enabled.size());
            device_info.ppEnabledExtensionNames = enabled.data();
            if (vkCreateDevice(physical, &device_info, nullptr, &d.device) != VK_SUCCESS) continue;

            d.physical = physical;
            d.queue_family = family;
            vkGetDeviceQueue(d.device, family, 0, &d.queue);

            VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            pool_info.queueFamilyIndex = family;
            if (vkCreateCommandPool(d.device, &pool_info, nullptr, &d.command_pool) != VK_SUCCESS) {
                continue;
            }
            d.valid = true;
            break;
        }
        return d;
    }();
    return device;
}

std::uint32_t find_memory_type(
    const VkPhysicalDeviceMemoryProperties &properties, std::uint32_t type_bits, VkMemoryPropertyFlags required) {
    for (std::uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
        if ((type_bits & (1U << i)) && (properties.memoryTypes[i].propertyFlags & required) == required) {
            return i;
        }
    }
    return UINT32_MAX;
}

/// Ensure `d.staging` is a host-visible buffer of at least `required` bytes,
/// growing the cached allocation when needed. Caller holds `g_device_mutex`.
bool ensure_staging(Device &d, VkDeviceSize required) {
    if (d.staging != VK_NULL_HANDLE && d.staging_size >= required) return true;
    if (d.staging != VK_NULL_HANDLE) {
        vkDestroyBuffer(d.device, d.staging, nullptr);
        d.staging = VK_NULL_HANDLE;
    }
    if (d.staging_memory != VK_NULL_HANDLE) {
        vkFreeMemory(d.device, d.staging_memory, nullptr);
        d.staging_memory = VK_NULL_HANDLE;
    }
    d.staging_size = 0;
    // Reserve at least 4K UHD so ordinary captures reuse one allocation.
    const VkDeviceSize floor_size = 3'840ULL * 2'160ULL * 4ULL;
    const VkDeviceSize alloc_size = required > floor_size ? required : floor_size;

    VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buffer_info.size = alloc_size;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(d.device, &buffer_info, nullptr, &d.staging) != VK_SUCCESS) {
        d.staging = VK_NULL_HANDLE;
        return false;
    }
    VkMemoryRequirements buffer_requirements{};
    vkGetBufferMemoryRequirements(d.device, d.staging, &buffer_requirements);
    VkPhysicalDeviceMemoryProperties memory_properties{};
    vkGetPhysicalDeviceMemoryProperties(d.physical, &memory_properties);
    const std::uint32_t buffer_type = find_memory_type(memory_properties, buffer_requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (buffer_type == UINT32_MAX) {
        vkDestroyBuffer(d.device, d.staging, nullptr);
        d.staging = VK_NULL_HANDLE;
        return false;
    }
    VkMemoryAllocateInfo buffer_alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    buffer_alloc.allocationSize = buffer_requirements.size;
    buffer_alloc.memoryTypeIndex = buffer_type;
    if (vkAllocateMemory(d.device, &buffer_alloc, nullptr, &d.staging_memory) != VK_SUCCESS
        || vkBindBufferMemory(d.device, d.staging, d.staging_memory, 0) != VK_SUCCESS) {
        if (d.staging_memory != VK_NULL_HANDLE) {
            vkFreeMemory(d.device, d.staging_memory, nullptr);
            d.staging_memory = VK_NULL_HANDLE;
        }
        vkDestroyBuffer(d.device, d.staging, nullptr);
        d.staging = VK_NULL_HANDLE;
        return false;
    }
    d.staging_size = alloc_size;
    return true;
}

}   // namespace

bool vulkan_dmabuf_available() { return global_device().valid; }

std::optional<VulkanFrame> vulkan_import_dmabuf(int fd, std::uint32_t width, std::uint32_t height, std::uint32_t stride,
    std::uint64_t modifier, std::uint32_t fourcc, std::uint32_t plane_offset) {
    auto &d = global_device();
    if (!d.valid || fd < 0 || width == 0 || height == 0) return std::nullopt;
    const VkFormat format = fourcc_to_vk(fourcc);
    if (format == VK_FORMAT_UNDEFINED) return std::nullopt;

    // The device queue, command pool and cached staging buffer are shared
    // across sessions, so serialize the whole import.
    std::lock_guard<std::mutex> device_lock(g_device_mutex);

    // Vulkan takes ownership of the fd handed to vkAllocateMemory. Import a
    // duplicate so the caller's fd (the retained committed frame, or a capture
    // job's duplicated plane) stays valid for later captures; reusing a
    // consumed fd made every capture after the first fail.
    const int import_fd = ::dup(fd);
    if (import_fd < 0) return std::nullopt;
    bool import_fd_owned_by_vulkan = false;

    // Export the producer fence before the driver may take ownership of the fd
    // during vkAllocateMemory.
    const auto exported = export_sync_file(fd);
    VkSemaphore wait_semaphore = VK_NULL_HANDLE;
    if (exported.fd >= 0 && exported.active) {
        auto import_semaphore =
            reinterpret_cast<PFN_vkImportSemaphoreFdKHR>(vkGetDeviceProcAddr(d.device, "vkImportSemaphoreFdKHR"));
        VkSemaphoreCreateInfo semaphore_info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if (import_semaphore && vkCreateSemaphore(d.device, &semaphore_info, nullptr, &wait_semaphore) == VK_SUCCESS) {
            VkImportSemaphoreFdInfoKHR import_info{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR};
            import_info.semaphore = wait_semaphore;
            import_info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
            import_info.fd = exported.fd;
            if (import_semaphore(d.device, &import_info) != VK_SUCCESS) {
                vkDestroySemaphore(d.device, wait_semaphore, nullptr);
                wait_semaphore = VK_NULL_HANDLE;
                ::close(exported.fd);
            }
            // On success the fd ownership moves to Vulkan; do not close it.
        } else
            ::close(exported.fd);
    } else if (exported.fd >= 0)
        ::close(exported.fd);
    else
        vkDeviceWaitIdle(d.device);

    // ── import the DMA-BUF as a Vulkan image ──────────────────────────
    VkSubresourceLayout plane_layout{};
    plane_layout.offset = plane_offset;
    plane_layout.rowPitch = stride;
    VkImageDrmFormatModifierExplicitCreateInfoEXT modifier_info{
        VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT};
    modifier_info.drmFormatModifier = modifier;
    modifier_info.drmFormatModifierPlaneCount = 1;
    modifier_info.pPlaneLayouts = &plane_layout;

    VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image_info.pNext = &modifier_info;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = format;
    image_info.extent = {width, height, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    image_info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory image_memory = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;

    auto cleanup = [&] {
        // The staging buffer is cached in the device and intentionally not
        // destroyed here; only its transient one-time command buffer is.
        if (fence) vkDestroyFence(d.device, fence, nullptr);
        if (command) vkFreeCommandBuffers(d.device, d.command_pool, 1, &command);
        if (image) vkDestroyImage(d.device, image, nullptr);
        if (image_memory) vkFreeMemory(d.device, image_memory, nullptr);
        if (wait_semaphore) vkDestroySemaphore(d.device, wait_semaphore, nullptr);
        // Only close the duplicate while Vulkan has not taken ownership of it.
        if (import_fd >= 0 && !import_fd_owned_by_vulkan) ::close(import_fd);
    };

    if (vkCreateImage(d.device, &image_info, nullptr, &image) != VK_SUCCESS) {
        cleanup();
        return std::nullopt;
    }

    auto get_fd_properties =
        reinterpret_cast<PFN_vkGetMemoryFdPropertiesKHR>(vkGetDeviceProcAddr(d.device, "vkGetMemoryFdPropertiesKHR"));
    if (!get_fd_properties) {
        cleanup();
        return std::nullopt;
    }
    VkMemoryFdPropertiesKHR fd_properties{VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
    if (get_fd_properties(d.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &fd_properties) != VK_SUCCESS) {
        cleanup();
        return std::nullopt;
    }

    VkPhysicalDeviceMemoryProperties memory_properties{};
    vkGetPhysicalDeviceMemoryProperties(d.physical, &memory_properties);
    VkMemoryRequirements image_requirements{};
    vkGetImageMemoryRequirements(d.device, image, &image_requirements);
    const std::uint32_t image_type =
        find_memory_type(memory_properties, image_requirements.memoryTypeBits & fd_properties.memoryTypeBits, 0);
    if (image_type == UINT32_MAX) {
        cleanup();
        return std::nullopt;
    }

    VkImportMemoryFdInfoKHR import_memory{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
    import_memory.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    import_memory.fd = import_fd;
    VkMemoryAllocateInfo image_alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    image_alloc.pNext = &import_memory;
    image_alloc.allocationSize = image_requirements.size;
    image_alloc.memoryTypeIndex = image_type;
    if (vkAllocateMemory(d.device, &image_alloc, nullptr, &image_memory) != VK_SUCCESS) {
        cleanup();
        return std::nullopt;
    }
    // Ownership of `import_fd` moved to Vulkan; it is released with the memory.
    import_fd_owned_by_vulkan = true;
    if (vkBindImageMemory(d.device, image, image_memory, 0) != VK_SUCCESS) {
        cleanup();
        return std::nullopt;
    }

    // ── staging buffer (cached across captures) ───────────────────────
    const VkDeviceSize staging_size = static_cast<VkDeviceSize>(width) * height * 4U;
    if (!ensure_staging(d, staging_size)) {
        cleanup();
        return std::nullopt;
    }

    // ── record and submit the copy ────────────────────────────────────
    VkCommandBufferAllocateInfo command_alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    command_alloc.commandPool = d.command_pool;
    command_alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_alloc.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(d.device, &command_alloc, &command) != VK_SUCCESS) {
        cleanup();
        return std::nullopt;
    }
    VkCommandBufferBeginInfo begin_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(command, &begin_info);

    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
        nullptr, 1, &barrier);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {width, height, 1};
    vkCmdCopyImageToBuffer(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, d.staging, 1, &region);
    vkEndCommandBuffer(command);

    VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (vkCreateFence(d.device, &fence_info, nullptr, &fence) != VK_SUCCESS) {
        cleanup();
        return std::nullopt;
    }
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    if (wait_semaphore) {
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &wait_semaphore;
        submit.pWaitDstStageMask = &wait_stage;
    }
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    if (vkQueueSubmit(d.queue, 1, &submit, fence) != VK_SUCCESS
        || vkWaitForFences(d.device, 1, &fence, VK_TRUE, 5'000'000'000ULL) != VK_SUCCESS) {
        cleanup();
        return std::nullopt;
    }

    VulkanFrame frame;
    frame.width = width;
    frame.height = height;
    frame.pixels.resize(static_cast<std::size_t>(staging_size));
    void *mapped = nullptr;
    if (vkMapMemory(d.device, d.staging_memory, 0, staging_size, 0, &mapped) != VK_SUCCESS) {
        cleanup();
        return std::nullopt;
    }
    std::memcpy(frame.pixels.data(), mapped, static_cast<std::size_t>(staging_size));
    vkUnmapMemory(d.device, d.staging_memory);

    cleanup();
    return frame;
}

namespace {

// DMA_BUF_IOCTL_SYNC = _IOW('b', 0, struct dma_buf_sync)
struct DmaBufSyncFlags {
    std::uint64_t flags;
};
constexpr std::uint64_t dma_buf_sync_start = 0U << 2;
constexpr std::uint64_t dma_buf_sync_end = 1U << 2;
constexpr unsigned long dma_buf_ioctl_sync =
    (1ul << 30) | (sizeof(DmaBufSyncFlags) << 16) | (static_cast<unsigned long>('b') << 8) | 0ul;

bool dma_buf_sync_ioctl(int fd, std::uint64_t flags) {
    DmaBufSyncFlags sync{flags};
    return ::ioctl(fd, dma_buf_ioctl_sync, &sync) == 0;
}

/// Decode one plane of a 32-bit RGB buffer into a BGR `cv::Mat`.
bool decode_plane(const unsigned char *base, std::size_t map_size, std::int32_t width, std::int32_t height,
    std::uint32_t stride, std::uint32_t format, std::uint32_t offset, cv::Mat &out) {
    const bool bgra = format == drm_argb8888 || format == drm_xrgb8888;
    const bool rgba = format == drm_abgr8888 || format == drm_xbgr8888;
    if (!bgra && !rgba) return false;
    if (width <= 0 || height <= 0) return false;
    if (stride < static_cast<std::uint32_t>(width) * 4U) return false;

    cv::Mat result(height, width, CV_8UC3);
    for (int y = 0; y < height; ++y) {
        const std::size_t row = static_cast<std::size_t>(offset) + static_cast<std::size_t>(y) * stride;
        if (row + static_cast<std::size_t>(width) * 4 > map_size) return false;
        const auto *pixel = base + row;
        for (int x = 0; x < width; ++x) {
            auto &dst = result.at<cv::Vec3b>(y, x);
            if (bgra)
                dst = cv::Vec3b(pixel[0], pixel[1], pixel[2]);
            else
                dst = cv::Vec3b(pixel[2], pixel[1], pixel[0]);
            pixel += 4;
        }
    }
    out = std::move(result);
    return true;
}

}   // namespace

int export_dmabuf_sync_file(int dmabuf_fd, bool *active) {
    const auto exported = export_sync_file(dmabuf_fd);
    if (active) *active = exported.active;
    return exported.fd;
}

bool capture_dmabuf_plane(int fd, std::int32_t width, std::int32_t height, std::uint32_t stride, std::uint64_t modifier,
    std::uint32_t format, std::uint32_t offset, cv::Mat &out) {
    if (fd < 0 || width <= 0 || height <= 0) return false;

    // Prefer the GPU import path: it is the only one that can read tiled or
    // otherwise non-mappable buffers. The staging buffer comes back tightly
    // packed, so decode it as a zero-offset plane.
    if (auto gpu = vulkan_import_dmabuf(fd, static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height),
            stride, modifier, format, offset)) {
        if (decode_plane(gpu->pixels.data(), gpu->pixels.size(), width, height, static_cast<std::uint32_t>(width) * 4,
                format, 0, out)) {
            return true;
        }
    }

    const auto map_size =
        static_cast<std::size_t>(offset) + static_cast<std::size_t>(stride) * static_cast<std::size_t>(height);
    if (map_size == 0) return false;

    const bool synced = dma_buf_sync_ioctl(fd, dma_buf_sync_read | dma_buf_sync_start);

    void *mapped = ::mmap(nullptr, map_size, PROT_READ, MAP_SHARED, fd, 0);
    if (mapped == MAP_FAILED) {
        if (synced) dma_buf_sync_ioctl(fd, dma_buf_sync_read | dma_buf_sync_end);
        return false;
    }
    const bool ok =
        decode_plane(static_cast<const unsigned char *>(mapped), map_size, width, height, stride, format, offset, out);
    ::munmap(mapped, map_size);
    if (synced) dma_buf_sync_ioctl(fd, dma_buf_sync_read | dma_buf_sync_end);
    return ok;
}

}   // namespace skl::wcas::proxy
