#include "SKL/WCAS/Error.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <map>
#include <array>
#include <mutex>
#include <string>
#include <vector>
#include <chrono>
#include <algorithm>

#ifdef _WIN32
#else
#  include <unistd.h>
#  include <sys/mman.h>
#  include <sys/stat.h>
#  include <sys/poll.h>
#  include <sys/ioctl.h>
#endif

#include "SKL/WCAS/Private/Proxy/DMABUF/EGL.h"
#include "SKL/WCAS/Private/Proxy/DMABUF/Decoder.h"
#include "SKL/WCAS/Private/Proxy/DMABUF/Vulkan.h"

#include <fmt/format.h>
#include <vulkan/vulkan.h>

namespace skl::wcas::proxy {
namespace {

constexpr uint32_t drm_argb8888 = 0X34325241U;
constexpr uint32_t drm_xrgb8888 = 0X34325258U;
constexpr uint32_t drm_abgr8888 = 0X34324241U;
constexpr uint32_t drm_xbgr8888 = 0X34324258U;

constexpr uint64_t drm_format_modifier_linear = 0U;
constexpr uint64_t drm_format_modifier_tiled = 1ULL << 0;

/// True when `modifier` describes a plain linear layout the CPU can walk row
/// by row. `DRM_FORMAT_MODIFIER_TILED` on its own is still linear (it only adds
/// a tiled aspect); anything carrying vendor bits describes a layout `decode_plane`
/// cannot interpret and must be rejected instead of silently mis-decoded.
bool modifier_is_linear(uint64_t modifier) {
    return modifier == drm_format_modifier_linear || modifier == drm_format_modifier_tiled;
}

/// Capture failures repeat once per captured frame, so a raw print would drown
/// the log. Each distinct message is emitted at most `k_trace_limit` times per
/// process, which is enough to see why a surface decodes badly without turning
/// a capture loop into thousands of lines.
constexpr int k_trace_limit = 4;

std::mutex g_trace_mutex;
std::map<std::string, int> g_trace_counts;

VkFormat fourcc_to_vk(uint32_t fourcc) {
    switch (fourcc) {
        case drm_argb8888:
        case drm_xrgb8888: return VK_FORMAT_B8G8R8A8_UNORM;
        case drm_abgr8888:
        case drm_xbgr8888: return VK_FORMAT_R8G8B8A8_UNORM;
        default:           return VK_FORMAT_UNDEFINED;
    }
}

/// Whether the physical device claims it can create a
/// `VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT` image with exactly this modifier,
/// format and `usage`. When this is false the driver is free to accept
/// `vkCreateImage` anyway and then interpret the buffer with a layout it does
/// not support, which decodes to a plausible-but-wrong picture instead of
/// failing — so the transfer path cannot be trusted without this check.
bool drm_modifier_usage_supported(
    VkPhysicalDevice physical, VkFormat format, uint64_t modifier, VkImageUsageFlags usage) {
    VkPhysicalDeviceImageDrmFormatModifierInfoEXT modifier_info{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT};
    modifier_info.drmFormatModifier = modifier;
    modifier_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkPhysicalDeviceImageFormatInfo2 format_info{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
    format_info.pNext = &modifier_info;
    format_info.format = format;
    format_info.type = VK_IMAGE_TYPE_2D;
    format_info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    format_info.usage = usage;
    VkImageFormatProperties2 properties{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
    return vkGetPhysicalDeviceImageFormatProperties2(physical, &format_info, &properties) == VK_SUCCESS;
}

// ── sync_file (DMA_BUF_IOCTL_EXPORT_SYNC_FILE) ─────────────────────────
// _IOWR('b', 2, struct dma_buf_export_sync_file) = 0xC0086202
struct DmaBufExportSyncFile {
    uint32_t flags;
    int32_t fd;
};
constexpr uint32_t dma_buf_sync_read = 1U << 0;
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
    uint32_t queue_family = 0;
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

        uint32_t physical_count = 0;
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
            uint32_t extension_count = 0;
            vkEnumerateDeviceExtensionProperties(physical, nullptr, &extension_count, nullptr);
            std::vector<VkExtensionProperties> extensions(extension_count);
            vkEnumerateDeviceExtensionProperties(physical, nullptr, &extension_count, extensions.data());
            bool supported = true;
            for (const auto *required : kRequiredExtensions) {
                bool found = false;
                for (const auto &extension : extensions) {
                    if (strcmp(extension.extensionName, required) == 0) {
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

            uint32_t family_count = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, nullptr);
            std::vector<VkQueueFamilyProperties> families(family_count);
            vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, families.data());
            uint32_t family = UINT32_MAX;
            for (uint32_t i = 0; i < family_count; ++i) {
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
            device_info.enabledExtensionCount = static_cast<uint32_t>(enabled.size());
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
            {
                VkPhysicalDeviceProperties props{};
                vkGetPhysicalDeviceProperties(d.physical, &props);
                trace_capture(fmt::format("vulkan device: {} (type={}) driver={}.{}.{} api={}.{}.{}", props.deviceName,
                    static_cast<int>(props.deviceType), VK_VERSION_MAJOR(props.driverVersion),
                    VK_VERSION_MINOR(props.driverVersion), VK_VERSION_PATCH(props.driverVersion),
                    VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion),
                    VK_VERSION_PATCH(props.apiVersion)));
            }
            break;
        }
        return d;
    }();
    return device;
}

uint32_t find_memory_type(
    const VkPhysicalDeviceMemoryProperties &properties, uint32_t type_bits, VkMemoryPropertyFlags required) {
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
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
    const uint32_t buffer_type = find_memory_type(memory_properties, buffer_requirements.memoryTypeBits,
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

/// `WCAS_DEBUG_DMABUF` (set by `--debug-dmabuf`) is the dedicated gate for the
/// per-buffer capture diagnostics below; `WCAS_DEBUG` (`--debug-all`) also turns
/// them on as the umbrella category. Off by default: every commit prints one of
/// these, so they would otherwise flood the log during normal playback.
bool dmabuf_trace_enabled() {
    static const bool enabled = ::getenv("WCAS_DEBUG_DMABUF") != nullptr || ::getenv("WCAS_DEBUG") != nullptr;
    return enabled;
}

void trace_capture(std::string message) {
    if (!dmabuf_trace_enabled()) return;
    int seen = 0;
    {
        std::lock_guard lock(g_trace_mutex);
        seen = ++g_trace_counts[message];
    }
    if (seen > k_trace_limit) return;
    if (seen == k_trace_limit)
        fmt::print(stderr, "[wcas:dma-buf] {} (further occurrences suppressed)\n", message);
    else
        fmt::print(stderr, "[wcas:dma-buf] {}\n", message);
}

bool vulkan_dmabuf_available() { return global_device().valid; }

optional<VulkanFrame> vulkan_import_dmabuf(int fd, uint32_t width, uint32_t height, uint32_t stride, uint64_t modifier,
    uint32_t fourcc, uint32_t plane_offset) {
    auto &d = global_device();
    // Every rejection below is silent by design, which makes "GPU import failed"
    // indistinguishable from "GPU read back garbage". Tag each one with the
    // buffer descriptor so the capture log says which modifier/fourcc failed.
    const std::string descriptor = fmt::format("{}x{} fourcc=0x{:08x} modifier=0x{:016x} stride={} offset={}", width,
        height, fourcc, modifier, stride, plane_offset);
    if (!d.valid) {
        trace_capture(fmt::format("vulkan device unavailable: {}", descriptor));
        return std::nullopt;
    }
    if (fd < 0 || width == 0 || height == 0) return std::nullopt;
    const VkFormat format = fourcc_to_vk(fourcc);
    if (format == VK_FORMAT_UNDEFINED) {
        trace_capture(fmt::format("unsupported fourcc for GPU import: {}", descriptor));
        return std::nullopt;
    }

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
    // `VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT` requires pNext to chain the
    // modifier; leaving it unset is invalid. The explicit struct pins the image
    // to the source buffer's real modifier *and* per-plane layout
    // (offset/rowPitch), which is what importing an existing DMA-BUF needs.

    // Whether the device+format actually support this modifier for the transfer
    // we are about to do. A successful vkCreateImage proves nothing on its own:
    // the driver accepts modifiers it lists as unsupported and then decodes the
    // wrong bytes. This is what decides whether the transfer route is legal, and
    // is reported so an unsupported readback is never mistaken for a good one.
    const bool transfer_supported =
        drm_modifier_usage_supported(d.physical, format, modifier, VK_IMAGE_USAGE_TRANSFER_SRC_BIT);

    VkSubresourceLayout plane_layout{};
    plane_layout.offset = plane_offset;
    plane_layout.rowPitch = stride;
    VkImageDrmFormatModifierExplicitCreateInfoEXT explicit_info{
        VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT};
    explicit_info.drmFormatModifier = modifier;
    explicit_info.drmFormatModifierPlaneCount = 1;
    explicit_info.pPlaneLayouts = &plane_layout;

    VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image_info.pNext = &explicit_info;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = format;
    image_info.extent = {width, height, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    image_info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    // NOT VK_IMAGE_LAYOUT_UNDEFINED: that permits the implementation to discard
    // the imported contents. For a tiled external buffer the transition is also
    // where the driver resolves the modifier's tile layout into the linear
    // layout the copy expects, so starting from UNDEFINED let a readback return
    // unresolved/cleared memory — a plausible picture with only high-contrast
    // edges surviving.
    image_info.initialLayout = VK_IMAGE_LAYOUT_GENERAL;

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

    const VkResult create_result = vkCreateImage(d.device, &image_info, nullptr, &image);
    if (create_result != VK_SUCCESS) {
        trace_capture(fmt::format("vkCreateImage failed (VkResult={}) - driver rejected the modifier: {}",
            static_cast<int>(create_result), descriptor));
        cleanup();
        return std::nullopt;
    }

    auto get_fd_properties =
        reinterpret_cast<PFN_vkGetMemoryFdPropertiesKHR>(vkGetDeviceProcAddr(d.device, "vkGetMemoryFdPropertiesKHR"));
    if (!get_fd_properties) {
        trace_capture(fmt::format("vkGetMemoryFdPropertiesKHR unavailable: {}", descriptor));
        cleanup();
        return std::nullopt;
    }
    VkMemoryFdPropertiesKHR fd_properties{VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
    const VkResult props_result =
        get_fd_properties(d.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &fd_properties);
    if (props_result != VK_SUCCESS) {
        trace_capture(fmt::format(
            "vkGetMemoryFdPropertiesKHR failed (VkResult={}): {}", static_cast<int>(props_result), descriptor));
        cleanup();
        return std::nullopt;
    }

    VkPhysicalDeviceMemoryProperties memory_properties{};
    vkGetPhysicalDeviceMemoryProperties(d.physical, &memory_properties);
    VkMemoryRequirements image_requirements{};
    vkGetImageMemoryRequirements(d.device, image, &image_requirements);
    const uint32_t image_type =
        find_memory_type(memory_properties, image_requirements.memoryTypeBits & fd_properties.memoryTypeBits, 0);
    if (image_type == UINT32_MAX) {
        trace_capture(fmt::format("no importable memory type (image bits=0x{:x} fd bits=0x{:x}): {}",
            image_requirements.memoryTypeBits, fd_properties.memoryTypeBits, descriptor));
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
    const VkResult alloc_result = vkAllocateMemory(d.device, &image_alloc, nullptr, &image_memory);
    if (alloc_result != VK_SUCCESS) {
        trace_capture(
            fmt::format("vkAllocateMemory failed (VkResult={}): {}", static_cast<int>(alloc_result), descriptor));
        cleanup();
        return std::nullopt;
    }
    // Ownership of `import_fd` moved to Vulkan; it is released with the memory.
    import_fd_owned_by_vulkan = true;
    const VkResult bind_result = vkBindImageMemory(d.device, image, image_memory, 0);
    if (bind_result != VK_SUCCESS) {
        trace_capture(
            fmt::format("vkBindImageMemory failed (VkResult={}): {}", static_cast<int>(bind_result), descriptor));
        cleanup();
        return std::nullopt;
    }

    // What the driver actually resolved the modifier to, versus what the client
    // advertised. A rowPitch mismatch means the explicit plane layout WCAS
    // passed was wrong and the copy would read the wrong bytes per row; a
    // *resolved modifier* different from the source one means the import
    // silently substituted a layout the buffer never used.
    {
        VkSubresourceLayout actual{};
        const VkImageSubresource color_subresource{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
        vkGetImageSubresourceLayout(d.device, image, &color_subresource, &actual);

        uint64_t resolved_modifier = 0;
        bool have_resolved_modifier = false;
        if (auto get_modifier = reinterpret_cast<PFN_vkGetImageDrmFormatModifierPropertiesEXT>(
                vkGetDeviceProcAddr(d.device, "vkGetImageDrmFormatModifierPropertiesEXT"))) {
            VkImageDrmFormatModifierPropertiesEXT resolved{VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT};
            if (get_modifier(d.device, image, &resolved) == VK_SUCCESS) {
                resolved_modifier = resolved.drmFormatModifier;
                have_resolved_modifier = true;
            }
        }
        trace_capture(fmt::format(
            "{} | modifier_supported={} | driver layout: rowPitch={} offset={} size={} "
            "| advertised stride={} offset={} | memreq size={} | resolved modifier=0x{:016x}{}",
            descriptor, transfer_supported ? "yes" : "NO", actual.rowPitch, actual.offset, actual.size, stride,
            plane_offset, image_requirements.size, resolved_modifier, have_resolved_modifier ? "" : " (unavailable)"));
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
        trace_capture(fmt::format("vkAllocateCommandBuffers failed: {}", descriptor));
        cleanup();
        return std::nullopt;
    }
    VkCommandBufferBeginInfo begin_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(command, &begin_info);

    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    // Make the memory reads that produced the imported contents visible to the
    // transfer; without the source access mask the copy can legally run before
    // them.
    barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
        nullptr, 1, &barrier);

    // The staging buffer is cached across captures, so anything the copy below does
    // not write keeps the PREVIOUS frame's pixels and shows up as ghosting. Fill it
    // with a flat sentinel first: every region the copy fails to cover then appears
    // as uniform grey, which turns a vague "the picture is wrong" report into a
    // map of exactly which parts of the readback are missing.
    vkCmdFillBuffer(command, d.staging, 0, staging_size, 0x007f007fu);

    VkBufferMemoryBarrier fill_done{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    fill_done.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    fill_done.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    fill_done.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    fill_done.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    fill_done.buffer = d.staging;
    fill_done.offset = 0;
    fill_done.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1,
        &fill_done, 0, nullptr);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {width, height, 1};
    vkCmdCopyImageToBuffer(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, d.staging, 1, &region);
    vkEndCommandBuffer(command);

    VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (vkCreateFence(d.device, &fence_info, nullptr, &fence) != VK_SUCCESS) {
        trace_capture(fmt::format("vkCreateFence failed: {}", descriptor));
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
    const VkResult submit_result = vkQueueSubmit(d.queue, 1, &submit, fence);
    const VkResult wait_result =
        submit_result == VK_SUCCESS ? vkWaitForFences(d.device, 1, &fence, VK_TRUE, 5'000'000'000ULL) : submit_result;
    if (wait_result != VK_SUCCESS) {
        trace_capture(fmt::format("GPU copy did not complete (submit={} wait={}): {}", static_cast<int>(submit_result),
            static_cast<int>(wait_result), descriptor));
        cleanup();
        return std::nullopt;
    }

    VulkanFrame frame;
    frame.width = width;
    frame.height = height;
    frame.modifier_supported = transfer_supported;
    frame.pixels.resize(static_cast<size_t>(staging_size));
    void *mapped = nullptr;
    const VkResult map_result = vkMapMemory(d.device, d.staging_memory, 0, staging_size, 0, &mapped);
    if (map_result != VK_SUCCESS) {
        trace_capture(
            fmt::format("vkMapMemory of staging failed (VkResult={}): {}", static_cast<int>(map_result), descriptor));
        cleanup();
        return std::nullopt;
    }
    memcpy(frame.pixels.data(), mapped, static_cast<size_t>(staging_size));
    vkUnmapMemory(d.device, d.staging_memory);

    cleanup();
    return frame;
}

namespace {

// DMA_BUF_IOCTL_SYNC = _IOW('b', 0, struct dma_buf_sync)
struct DmaBufSyncFlags {
    uint64_t flags;
};
constexpr uint64_t dma_buf_sync_start = 0U << 2;
constexpr uint64_t dma_buf_sync_end = 1U << 2;
constexpr unsigned long dma_buf_ioctl_sync =
    (1ul << 30) | (sizeof(DmaBufSyncFlags) << 16) | (static_cast<unsigned long>('b') << 8) | 0ul;

bool dma_buf_sync_ioctl(int fd, uint64_t flags) {
    DmaBufSyncFlags sync{flags};
    return ::ioctl(fd, dma_buf_ioctl_sync, &sync) == 0;
}

/// Decode one plane of a 32-bit RGB buffer into a BGR `cv::Mat`.
///
/// ARGB8888/ABGR8888 vs XRGB8888/XBGR8888 differ only in whether the fourth
/// byte carries alpha. The output is opaque BGR, so the byte is dropped either
/// way: a premultiplied translucent surface is not what the compositor shows,
/// and cannot be reconstructed from a single window buffer.
bool decode_plane(const unsigned char *base, size_t map_size, int32_t width, int32_t height, uint32_t stride,
    uint32_t format, uint32_t offset, cv::Mat &out) {
    const bool bgra = format == drm_argb8888 || format == drm_xrgb8888;
    const bool rgba = format == drm_abgr8888 || format == drm_xbgr8888;
    if (!bgra && !rgba) return false;
    if (width <= 0 || height <= 0) return false;
    if (stride < static_cast<uint32_t>(width) * 4U) return false;

    cv::Mat result(height, width, CV_8UC3);
    for (int y = 0; y < height; ++y) {
        const size_t row = static_cast<size_t>(offset) + static_cast<size_t>(y) * stride;
        if (row + static_cast<size_t>(width) * 4 > map_size) return false;
        const auto *pixel = base + row;
        for (int x = 0; x < width; ++x) {
            result.at<cv::Vec3b>(y, x) =
                bgra ? cv::Vec3b(pixel[0], pixel[1], pixel[2]) : cv::Vec3b(pixel[2], pixel[1], pixel[0]);
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

namespace {

/// Which decoder handles a DMA-BUF.
enum class CaptureKind {
    Auto,
    Vulkan,
    Egl
};

/// `WCAS_DMABUF_BACKEND=auto|vulkan|egl`, parsed once. `auto` picks the backend
/// whose decoder the device actually supports for the buffer's layout.
CaptureKind capture_backend() {
    static const CaptureKind backend = [] {
        const char *value = ::getenv("WCAS_DMABUF_BACKEND");
        if (value == nullptr) return CaptureKind::Auto;
        const std::string spec = value;
        if (spec == "vulkan") return CaptureKind::Vulkan;
        if (spec == "egl") return CaptureKind::Egl;
        if (spec != "auto") trace_capture(fmt::format("unknown WCAS_DMABUF_BACKEND='{}', using auto", spec));
        return CaptureKind::Auto;
    }();
    return backend;
}

/// Whether the Vulkan device advertises `modifier` for this fourcc and
/// `TRANSFER_SRC` usage. Used to decide when `auto` must route to EGL instead.
bool vulkan_modifier_supported(uint32_t fourcc, uint64_t modifier) {
    auto &d = global_device();
    if (!d.valid) return false;
    const VkFormat format = fourcc_to_vk(fourcc);
    if (format == VK_FORMAT_UNDEFINED) return false;
    return drm_modifier_usage_supported(d.physical, format, modifier, VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
}

/// `WCAS_CAPTURE_TIMING=1` enables per-capture latency lines. Off by default:
/// they are per-frame and intentionally not deduplicated, so they must be
/// opt-in. Printed directly (not via `trace_capture`) for the same reason.
bool capture_timing_enabled() {
    static const bool enabled = ::getenv("WCAS_CAPTURE_TIMING") != nullptr;
    return enabled;
}

using skl::abix::poly::InterfaceMethod;
using skl::abix::poly::InterfaceRegistry;
using skl::abix::poly::InterfaceStatus;

std::error_code decoder_error(errc value) { return make_error_code(value); }

std::string describe_dmabuf(const DmaBufDescriptor &descriptor) {
    return fmt::format("{}x{} fourcc=0x{:08x} modifier=0x{:016x} stride={} offset={}", descriptor.width,
        descriptor.height, descriptor.format, descriptor.modifier, descriptor.stride, descriptor.offset);
}

const char *decoder_name(DmaBufDecoderKind kind) {
    switch (kind) {
        case DmaBufDecoderKind::Auto:    return "auto";
        case DmaBufDecoderKind::Vulkan:  return "vulkan";
        case DmaBufDecoderKind::Egl:     return "egl";
        case DmaBufDecoderKind::Mmap:    return "cpu";
        case DmaBufDecoderKind::Unknown: break;
    }
    return "unknown";
}

class VulkanDecoder {
public:
    std::string name() const { return "vulkan"; }
    DmaBufDecoderKind kind() const { return DmaBufDecoderKind::Vulkan; }
    bool available() const { return vulkan_dmabuf_available(); }
    bool supports(uint32_t fourcc, uint64_t modifier) const {
        return fourcc_to_vk(fourcc) != VK_FORMAT_UNDEFINED
            && (modifier_is_linear(modifier) || vulkan_modifier_supported(fourcc, modifier));
    }

    result<DmaBufDecoded> decode(const DmaBufDescriptor *descriptor) const {
        if (descriptor == nullptr || descriptor->fd < 0 || descriptor->width <= 0 || descriptor->height <= 0)
            return decoder_error(errc::frame_capture_failed);
        if (!vulkan_dmabuf_available()) return decoder_error(errc::feature_not_implemented);
        auto gpu = vulkan_import_dmabuf(descriptor->fd, static_cast<uint32_t>(descriptor->width),
            static_cast<uint32_t>(descriptor->height), descriptor->stride, descriptor->modifier, descriptor->format,
            descriptor->offset);
        if (!gpu.has_value()) {
            trace_capture(fmt::format("{} | GPU import failed", describe_dmabuf(*descriptor)));
            return decoder_error(errc::frame_capture_failed);
        }
        cv::Mat image;
        if (!decode_plane(gpu->pixels.data(), gpu->pixels.size(), descriptor->width, descriptor->height,
                static_cast<uint32_t>(descriptor->width) * 4, descriptor->format, 0, image)) {
            trace_capture(fmt::format("{} | GPU readback rejected by decode_plane", describe_dmabuf(*descriptor)));
            return decoder_error(errc::frame_capture_failed);
        }
        trace_capture(fmt::format("{} | GPU transfer readback | modifier_supported={} | pixels=unverified",
            describe_dmabuf(*descriptor), gpu->modifier_supported ? "yes" : "NO"));
        return DmaBufDecoded{std::move(image), kind(), gpu->modifier_supported};
    }
};

class EglDecoder {
public:
    std::string name() const { return "egl"; }
    DmaBufDecoderKind kind() const { return DmaBufDecoderKind::Egl; }
    bool available() const { return egl_dmabuf_available(); }
    bool supports(uint32_t fourcc, uint64_t modifier) const {
        return egl_dmabuf_available() && egl_dmabuf_import_supported(fourcc, modifier);
    }

    result<DmaBufDecoded> decode(const DmaBufDescriptor *descriptor) const {
        if (descriptor == nullptr || descriptor->fd < 0 || descriptor->width <= 0 || descriptor->height <= 0)
            return decoder_error(errc::frame_capture_failed);
        if (!egl_dmabuf_available()) return decoder_error(errc::feature_not_implemented);
        auto pixels = egl_read_dmabuf(descriptor->fd, static_cast<uint32_t>(descriptor->width),
            static_cast<uint32_t>(descriptor->height), descriptor->stride, descriptor->modifier, descriptor->format,
            descriptor->offset);
        if (!pixels.has_value()) {
            trace_capture(fmt::format("{} | EGL readback failed", describe_dmabuf(*descriptor)));
            return decoder_error(errc::frame_capture_failed);
        }
        const uint32_t rgba_format =
            (descriptor->format == drm_argb8888 || descriptor->format == drm_abgr8888) ? drm_abgr8888 : drm_xbgr8888;
        cv::Mat image;
        if (!decode_plane(pixels->data(), pixels->size(), descriptor->width, descriptor->height,
                static_cast<uint32_t>(descriptor->width) * 4, rgba_format, 0, image)) {
            trace_capture(fmt::format("{} | EGL readback rejected by decode_plane", describe_dmabuf(*descriptor)));
            return decoder_error(errc::frame_capture_failed);
        }
        trace_capture(fmt::format("{} | EGL readback | pixels=rgba logical", describe_dmabuf(*descriptor)));
        return DmaBufDecoded{std::move(image), kind(), true};
    }
};

class MmapDecoder {
public:
    std::string name() const { return "mmap"; }
    DmaBufDecoderKind kind() const { return DmaBufDecoderKind::Mmap; }
    bool available() const { return true; }
    bool supports(uint32_t, uint64_t modifier) const { return modifier_is_linear(modifier); }

    result<DmaBufDecoded> decode(const DmaBufDescriptor *descriptor) const {
        if (descriptor == nullptr || descriptor->fd < 0 || descriptor->width <= 0 || descriptor->height <= 0)
            return decoder_error(errc::frame_capture_failed);
        if (!modifier_is_linear(descriptor->modifier)) return decoder_error(errc::feature_not_implemented);
        const auto map_size = static_cast<size_t>(descriptor->offset)
                            + static_cast<size_t>(descriptor->stride) * static_cast<size_t>(descriptor->height);
        if (map_size == 0) return decoder_error(errc::frame_capture_failed);

        const bool synced = dma_buf_sync_ioctl(descriptor->fd, dma_buf_sync_read | dma_buf_sync_start);
        void *mapped = ::mmap(nullptr, map_size, PROT_READ, MAP_SHARED, descriptor->fd, 0);
        if (mapped == MAP_FAILED) {
            if (synced) dma_buf_sync_ioctl(descriptor->fd, dma_buf_sync_read | dma_buf_sync_end);
            trace_capture(fmt::format("mmap of {} bytes failed: {}", map_size, describe_dmabuf(*descriptor)));
            return decoder_error(errc::frame_capture_failed);
        }
        cv::Mat image;
        const bool ok = decode_plane(static_cast<const unsigned char *>(mapped), map_size, descriptor->width,
            descriptor->height, descriptor->stride, descriptor->format, descriptor->offset, image);
        ::munmap(mapped, map_size);
        if (synced) dma_buf_sync_ioctl(descriptor->fd, dma_buf_sync_read | dma_buf_sync_end);
        if (!ok) {
            trace_capture(fmt::format("{} | CPU decode rejected the buffer", describe_dmabuf(*descriptor)));
            return decoder_error(errc::frame_capture_failed);
        }
        trace_capture(fmt::format("{} | OK via CPU mmap", describe_dmabuf(*descriptor)));
        return DmaBufDecoded{std::move(image), kind(), true};
    }
};

class AutoDecoder {
public:
    std::string name() const { return "auto"; }
    DmaBufDecoderKind kind() const { return DmaBufDecoderKind::Auto; }
    bool available() const { return true; }
    bool supports(uint32_t fourcc, uint64_t modifier) const {
        return mmap_.supports(fourcc, modifier)
            || vulkan_.supports(fourcc, modifier)
            || (egl_.available() && egl_.supports(fourcc, modifier));
    }

    result<DmaBufDecoded> decode(const DmaBufDescriptor *descriptor) const {
        if (descriptor == nullptr) return decoder_error(errc::frame_capture_failed);

        const auto mode = capture_backend();
        const bool linear = modifier_is_linear(descriptor->modifier);
        const bool vulkan_legal = mode == CaptureKind::Vulkan
                               || (mode == CaptureKind::Auto
                                   && (linear || vulkan_modifier_supported(descriptor->format, descriptor->modifier)));
        const bool egl_selected = mode == CaptureKind::Egl
                               || (mode == CaptureKind::Auto
                                   && !vulkan_legal
                                   && egl_.available()
                                   && egl_.supports(descriptor->format, descriptor->modifier));

        if (egl_selected) {
            auto decoded = egl_.decode(descriptor);
            if (decoded) return decoded;
            if (mode == CaptureKind::Egl) return decoder_error(errc::frame_capture_failed);
        }
        if (vulkan_legal) {
            auto decoded = vulkan_.decode(descriptor);
            if (decoded) return decoded;
        }
        if (linear) {
            auto decoded = mmap_.decode(descriptor);
            if (decoded) return decoded;
        }
        trace_capture(fmt::format("{} | all DMA-BUF decoders failed", describe_dmabuf(*descriptor)));
        return decoder_error(errc::frame_capture_failed);
    }

private:
    VulkanDecoder vulkan_;
    EglDecoder egl_;
    MmapDecoder mmap_;
};

const InterfaceMethod kVulkanMethods[] = {
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(DmaBufDecoderTraits, name, &VulkanDecoder::name),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(DmaBufDecoderTraits, kind, &VulkanDecoder::kind),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(DmaBufDecoderTraits, available, &VulkanDecoder::available),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(DmaBufDecoderTraits, supports, &VulkanDecoder::supports),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(DmaBufDecoderTraits, decode, &VulkanDecoder::decode),
};
const InterfaceMethod kEglMethods[] = {
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(DmaBufDecoderTraits, name, &EglDecoder::name),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(DmaBufDecoderTraits, kind, &EglDecoder::kind),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(DmaBufDecoderTraits, available, &EglDecoder::available),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(DmaBufDecoderTraits, supports, &EglDecoder::supports),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(DmaBufDecoderTraits, decode, &EglDecoder::decode),
};
const InterfaceMethod kMmapMethods[] = {
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(DmaBufDecoderTraits, name, &MmapDecoder::name),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(DmaBufDecoderTraits, kind, &MmapDecoder::kind),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(DmaBufDecoderTraits, available, &MmapDecoder::available),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(DmaBufDecoderTraits, supports, &MmapDecoder::supports),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(DmaBufDecoderTraits, decode, &MmapDecoder::decode),
};
const InterfaceMethod kAutoMethods[] = {
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(DmaBufDecoderTraits, name, &AutoDecoder::name),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(DmaBufDecoderTraits, kind, &AutoDecoder::kind),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(DmaBufDecoderTraits, available, &AutoDecoder::available),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(DmaBufDecoderTraits, supports, &AutoDecoder::supports),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(DmaBufDecoderTraits, decode, &AutoDecoder::decode),
};

static_assert(std::size(kVulkanMethods) == DmaBufDecoderTraits::signatures.size());
static_assert(std::size(kEglMethods) == DmaBufDecoderTraits::signatures.size());
static_assert(std::size(kMmapMethods) == DmaBufDecoderTraits::signatures.size());
static_assert(std::size(kAutoMethods) == DmaBufDecoderTraits::signatures.size());

constexpr auto kVulkanType = skl::abix::model::canonical_type_id("skl::wcas::VulkanDmaBufDecoder");
constexpr auto kEglType = skl::abix::model::canonical_type_id("skl::wcas::EglDmaBufDecoder");
constexpr auto kMmapType = skl::abix::model::canonical_type_id("skl::wcas::MmapDmaBufDecoder");
constexpr auto kAutoType = skl::abix::model::canonical_type_id("skl::wcas::AutoDmaBufDecoder");

InterfaceStatus register_dmabuf_decoders(InterfaceRegistry<8> &registry) {
    const auto add = [&](const auto &type, const InterfaceMethod *methods, size_t count) {
        return registry.add(
            {DmaBufDecoderTraits::id, type, methods, static_cast<uint32_t>(count), DmaBufDecoderTraits::version, 0});
    };
    if (const auto status = add(kVulkanType, kVulkanMethods, std::size(kVulkanMethods)); status != InterfaceStatus::ok)
        return status;
    if (const auto status = add(kEglType, kEglMethods, std::size(kEglMethods)); status != InterfaceStatus::ok)
        return status;
    if (const auto status = add(kMmapType, kMmapMethods, std::size(kMmapMethods)); status != InterfaceStatus::ok)
        return status;
    return add(kAutoType, kAutoMethods, std::size(kAutoMethods));
}

struct DmaBufDecoderRegistry {
    DmaBufDecoderRegistry() {
        if (register_dmabuf_decoders(registry) != InterfaceStatus::ok) return;
    }
    InterfaceRegistry<8> registry;
};

const InterfaceRegistry<8> &dmabuf_decoder_registry() {
    static const DmaBufDecoderRegistry holder;
    return holder.registry;
}

const DmaBufDecoder &auto_decoder() {
    static AutoDecoder provider;
    static const DmaBufDecoder decoder(dmabuf_decoder_registry(), &provider, kAutoType);
    return decoder;
}

}   // namespace

bool capture_dmabuf_plane(int fd, int32_t width, int32_t height, uint32_t stride, uint64_t modifier, uint32_t format,
    uint32_t offset, cv::Mat &out) {
    if (fd < 0 || width <= 0 || height <= 0) return false;

    DmaBufDescriptor descriptor{fd, width, height, stride, modifier, format, offset};
    const auto &decoder = auto_decoder();
    if (!decoder.valid()) return false;

    const auto capture_start = std::chrono::steady_clock::now();
    auto decoded = decoder.decode(&descriptor);
    if (!decoded) return false;

    out = std::move(decoded.value().image);
    if (capture_timing_enabled()) {
        const double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - capture_start).count();
        fmt::print(stderr, "[wcas] capture timing: backend={} {:.2f} ms {}x{}\n", decoder_name(decoded.value().decoder),
            ms, width, height);
    }
    return true;
}

}   // namespace skl::wcas::proxy
