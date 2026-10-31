// Experimental, independent Vulkan capture layer for WCAS.
//
// It hooks swapchain creation/presentation and copies each presented image into
// a linear, DMA-BUF-exportable image, then sends the fd to a Unix socket. The
// approach is inspired by obs-vkcapture, but this is a separate implementation
// and does not link to OBS or copy its code.
//
// Usage:
//   VK_LAYER_PATH=<build>/experimental/vk-capture \
//   VK_INSTANCE_LAYERS=VK_LAYER_WCAS_vk_capture \
//   WCAS_VK_SOCKET=/tmp/wcas-vk.sock <vulkan-app>
//
// Limitations:
//   * 64-bit Linux only, Vulkan only.
//   * Requires VK_KHR_external_memory_fd and a linear-exportable format.
//   * One queue family (0) is assumed for the copy command pool.
//   * No frame pacing/backpressure: a slow receiver drops frames.

#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#ifndef VK_LAYER_EXPORT
#  define VK_LAYER_EXPORT __attribute__((visibility("default")))
#endif

namespace {

constexpr const char *kLayerName = "VK_LAYER_WCAS_vk_capture";
constexpr const char *kSocketEnv = "WCAS_VK_SOCKET";

struct InstanceState {
    PFN_vkGetInstanceProcAddr next_gipa = nullptr;
};

struct SwapState;

struct DeviceState {
    VkInstance instance = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    PFN_vkGetDeviceProcAddr next_gdpa = nullptr;
    PFN_vkGetInstanceProcAddr next_gipa = nullptr;
    VkQueue queue = VK_NULL_HANDLE;
    std::unordered_map<VkSwapchainKHR, SwapState *> swaps;

    PFN_vkDestroyDevice DestroyDevice = nullptr;
    PFN_vkCreateSwapchainKHR CreateSwapchainKHR = nullptr;
    PFN_vkDestroySwapchainKHR DestroySwapchainKHR = nullptr;
    PFN_vkGetSwapchainImagesKHR GetSwapchainImagesKHR = nullptr;
    PFN_vkQueuePresentKHR QueuePresentKHR = nullptr;
    PFN_vkGetDeviceQueue GetDeviceQueue = nullptr;
    PFN_vkCreateImage CreateImage = nullptr;
    PFN_vkDestroyImage DestroyImage = nullptr;
    PFN_vkGetImageMemoryRequirements2 GetImageMemoryRequirements2 = nullptr;
    PFN_vkBindImageMemory2 BindImageMemory2 = nullptr;
    PFN_vkAllocateMemory AllocateMemory = nullptr;
    PFN_vkFreeMemory FreeMemory = nullptr;
    PFN_vkGetMemoryFdKHR GetMemoryFdKHR = nullptr;
    PFN_vkGetImageSubresourceLayout GetImageSubresourceLayout = nullptr;
    PFN_vkCreateCommandPool CreateCommandPool = nullptr;
    PFN_vkDestroyCommandPool DestroyCommandPool = nullptr;
    PFN_vkAllocateCommandBuffers AllocateCommandBuffers = nullptr;
    PFN_vkResetCommandPool ResetCommandPool = nullptr;
    PFN_vkBeginCommandBuffer BeginCommandBuffer = nullptr;
    PFN_vkEndCommandBuffer EndCommandBuffer = nullptr;
    PFN_vkCmdPipelineBarrier CmdPipelineBarrier = nullptr;
    PFN_vkCmdCopyImage CmdCopyImage = nullptr;
    PFN_vkQueueSubmit QueueSubmit = nullptr;
    PFN_vkCreateFence CreateFence = nullptr;
    PFN_vkDestroyFence DestroyFence = nullptr;
    PFN_vkWaitForFences WaitForFences = nullptr;
    PFN_vkResetFences ResetFences = nullptr;
};

struct SwapState {
    DeviceState *device = nullptr;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    std::vector<VkImage> images;

    VkImage export_image = VK_NULL_HANDLE;
    VkDeviceMemory export_memory = VK_NULL_HANDLE;
    int export_fd = -1;
    uint32_t stride = 0;
    uint32_t bytes_per_pixel = 0;

    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;

    int socket_fd = -1;
    uint64_t sequence = 0;
    bool enabled = false;
};

struct FrameHeader {
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format;
    uint64_t sequence;
};

std::mutex g_mutex;
std::unordered_map<VkInstance, InstanceState> g_instances;
std::unordered_map<VkDevice, DeviceState *> g_devices;
PFN_vkGetInstanceProcAddr g_next_gipa = nullptr;
VkInstance g_last_instance = VK_NULL_HANDLE;

DeviceState *device_state(VkDevice device) {
    std::lock_guard lock(g_mutex);
    const auto it = g_devices.find(device);
    return it == g_devices.end() ? nullptr : it->second;
}

SwapState *swap_state(VkSwapchainKHR swapchain) {
    std::lock_guard lock(g_mutex);
    for (auto &[device, state] : g_devices) {
        (void)device;
        const auto it = state->swaps.find(swapchain);
        if (it != state->swaps.end()) return it->second;
    }
    return nullptr;
}

const VkLayerInstanceCreateInfo *find_instance_link(const void *next) {
    auto *cursor = static_cast<const VkBaseInStructure *>(next);
    while (cursor != nullptr) {
        if (cursor->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO) {
            const auto *info = reinterpret_cast<const VkLayerInstanceCreateInfo *>(cursor);
            if (info->function == VK_LAYER_LINK_INFO) return info;
        }
        cursor = cursor->pNext;
    }
    return nullptr;
}

const VkLayerDeviceCreateInfo *find_device_link(const void *next) {
    auto *cursor = static_cast<const VkBaseInStructure *>(next);
    while (cursor != nullptr) {
        if (cursor->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO) {
            const auto *info = reinterpret_cast<const VkLayerDeviceCreateInfo *>(cursor);
            if (info->function == VK_LAYER_LINK_INFO) return info;
        }
        cursor = cursor->pNext;
    }
    return nullptr;
}

uint32_t bytes_per_pixel(VkFormat format) {
    switch (format) {
        case VK_FORMAT_B8G8R8A8_UNORM:
        case VK_FORMAT_B8G8R8A8_SRGB:
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_R8G8B8A8_SRGB:            return 4;
        case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return 4;
        case VK_FORMAT_R16G16B16A16_UNORM:
        case VK_FORMAT_R16G16B16A16_SFLOAT:      return 8;
        default:                                 return 0;
    }
}

int connect_socket() {
    const char *path = std::getenv(kSocketEnv);
    if (path == nullptr || *path == '\0') return -1;
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, path, sizeof(address.sun_path) - 1);
    if (::connect(fd, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) < 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

void destroy_swap(SwapState *swap) {
    if (swap == nullptr) return;
    DeviceState *device = swap->device;
    if (swap->socket_fd >= 0) ::close(swap->socket_fd);
    if (device != nullptr) {
        if (swap->fence != VK_NULL_HANDLE && device->DestroyFence)
            device->DestroyFence(device->device, swap->fence, nullptr);
        if (swap->command_pool != VK_NULL_HANDLE && device->DestroyCommandPool)
            device->DestroyCommandPool(device->device, swap->command_pool, nullptr);
        if (swap->export_image != VK_NULL_HANDLE && device->DestroyImage)
            device->DestroyImage(device->device, swap->export_image, nullptr);
        if (swap->export_memory != VK_NULL_HANDLE && device->FreeMemory)
            device->FreeMemory(device->device, swap->export_memory, nullptr);
    }
    if (swap->export_fd >= 0) ::close(swap->export_fd);
    delete swap;
}

bool create_export_image(SwapState *swap) {
    DeviceState *device = swap->device;
    if (device->CreateImage == nullptr
        || device->DestroyImage == nullptr
        || device->GetImageMemoryRequirements2 == nullptr
        || device->BindImageMemory2 == nullptr
        || device->AllocateMemory == nullptr
        || device->FreeMemory == nullptr
        || device->GetMemoryFdKHR == nullptr
        || device->GetImageSubresourceLayout == nullptr
        || device->CreateCommandPool == nullptr
        || device->DestroyCommandPool == nullptr
        || device->AllocateCommandBuffers == nullptr
        || device->ResetCommandPool == nullptr
        || device->BeginCommandBuffer == nullptr
        || device->EndCommandBuffer == nullptr
        || device->CmdPipelineBarrier == nullptr
        || device->CmdCopyImage == nullptr
        || device->QueueSubmit == nullptr
        || device->CreateFence == nullptr
        || device->DestroyFence == nullptr
        || device->WaitForFences == nullptr
        || device->ResetFences == nullptr) {
        return false;
    }

    VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

    VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image_info.pNext = &external;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = swap->format;
    image_info.extent = {swap->extent.width, swap->extent.height, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_LINEAR;
    image_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (device->CreateImage(device->device, &image_info, nullptr, &swap->export_image) != VK_SUCCESS) return false;

    VkImageMemoryRequirementsInfo2 requirements_info{VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2};
    requirements_info.image = swap->export_image;
    VkMemoryDedicatedRequirements dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
    VkMemoryRequirements2 requirements{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
    requirements.pNext = &dedicated;
    device->GetImageMemoryRequirements2(device->device, &requirements_info, &requirements);

    VkExportMemoryAllocateInfo export_memory{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
    export_memory.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkMemoryDedicatedAllocateInfo dedicated_info{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    dedicated_info.pNext = &export_memory;
    dedicated_info.image = swap->export_image;

    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.pNext = &dedicated_info;
    allocate.allocationSize = requirements.memoryRequirements.size;

    VkPhysicalDeviceMemoryProperties memory_properties{};
    auto get_memory_properties = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
        device->next_gipa(device->instance, "vkGetPhysicalDeviceMemoryProperties"));
    if (get_memory_properties == nullptr) return false;
    get_memory_properties(device->physical, &memory_properties);

    bool found = false;
    for (uint32_t index = 0; index < memory_properties.memoryTypeCount; ++index) {
        if ((requirements.memoryRequirements.memoryTypeBits & (1u << index)) == 0) continue;
        const auto flags = memory_properties.memoryTypes[index].propertyFlags;
        if ((flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0 && (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0) {
            allocate.memoryTypeIndex = index;
            found = true;
            break;
        }
    }
    if (!found) return false;
    if (device->AllocateMemory(device->device, &allocate, nullptr, &swap->export_memory) != VK_SUCCESS) return false;

    VkBindImageMemoryInfo bind{VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO};
    bind.image = swap->export_image;
    bind.memory = swap->export_memory;
    if (device->BindImageMemory2(device->device, 1, &bind) != VK_SUCCESS) return false;

    VkMemoryGetFdInfoKHR fd_info{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
    fd_info.memory = swap->export_memory;
    fd_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    if (device->GetMemoryFdKHR(device->device, &fd_info, &swap->export_fd) != VK_SUCCESS) return false;

    VkImageSubresource subresource{};
    subresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    VkSubresourceLayout layout{};
    device->GetImageSubresourceLayout(device->device, swap->export_image, &subresource, &layout);
    swap->stride = static_cast<uint32_t>(layout.rowPitch);
    swap->bytes_per_pixel = bytes_per_pixel(swap->format);
    if (swap->stride == 0 || swap->bytes_per_pixel == 0) return false;

    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = 0;
    if (device->CreateCommandPool(device->device, &pool_info, nullptr, &swap->command_pool) != VK_SUCCESS) return false;
    VkCommandBufferAllocateInfo command_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    command_info.commandPool = swap->command_pool;
    command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_info.commandBufferCount = 1;
    if (device->AllocateCommandBuffers(device->device, &command_info, &swap->command_buffer) != VK_SUCCESS)
        return false;
    VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (device->CreateFence(device->device, &fence_info, nullptr, &swap->fence) != VK_SUCCESS) return false;

    swap->socket_fd = connect_socket();
    swap->enabled = swap->socket_fd >= 0;
    return true;
}

void copy_and_send(SwapState *swap, uint32_t index) {
    if (!swap->enabled || swap->device == nullptr || index >= swap->images.size()) return;
    DeviceState *device = swap->device;
    if (device->QueueSubmit == nullptr) return;

    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (device->BeginCommandBuffer(swap->command_buffer, &begin) != VK_SUCCESS) return;

    VkImageMemoryBarrier barriers[2]{};
    barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barriers[0].srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barriers[0].oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[0].image = swap->images[index];
    barriers[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    barriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barriers[1].srcAccessMask = 0;
    barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barriers[1].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barriers[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[1].image = swap->export_image;
    barriers[1].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    device->CmdPipelineBarrier(swap->command_buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 2, barriers);

    VkImageCopy copy{};
    copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.extent = {swap->extent.width, swap->extent.height, 1};
    device->CmdCopyImage(swap->command_buffer, swap->images[index], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        swap->export_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

    barriers[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barriers[0].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    barriers[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barriers[0].newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barriers[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barriers[1].dstAccessMask = 0;
    barriers[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barriers[1].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    device->CmdPipelineBarrier(swap->command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        0, 0, nullptr, 0, nullptr, 2, barriers);
    device->EndCommandBuffer(swap->command_buffer);

    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &swap->command_buffer;
    device->ResetFences(device->device, 1, &swap->fence);
    if (device->QueueSubmit(device->queue, 1, &submit, swap->fence) != VK_SUCCESS) return;
    device->WaitForFences(device->device, 1, &swap->fence, VK_TRUE, UINT64_MAX);
    device->ResetCommandPool(device->device, swap->command_pool, 0);

    FrameHeader header{};
    header.width = swap->extent.width;
    header.height = swap->extent.height;
    header.stride = swap->stride;
    header.format = static_cast<uint32_t>(swap->format);
    header.sequence = ++swap->sequence;

    const int send_fd = ::dup(swap->export_fd);
    if (send_fd < 0) return;
    iovec vector{&header, sizeof(header)};
    char control[CMSG_SPACE(sizeof(int))]{};
    msghdr message{};
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    cmsghdr *cmsg = CMSG_FIRSTHDR(&message);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    std::memcpy(CMSG_DATA(cmsg), &send_fd, sizeof(send_fd));
    if (::sendmsg(swap->socket_fd, &message, MSG_NOSIGNAL) < 0) {
        ::close(swap->socket_fd);
        swap->socket_fd = -1;
        swap->enabled = false;
    }
    ::close(send_fd);
}

VKAPI_ATTR VkResult VKAPI_CALL wcas_vk_CreateInstance(
    const VkInstanceCreateInfo *create_info, const VkAllocationCallbacks *allocator, VkInstance *instance) {
    const auto *link = find_instance_link(create_info->pNext);
    if (link == nullptr || link->u.pLayerInfo == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    const PFN_vkGetInstanceProcAddr next_gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    if (next_gipa == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    g_next_gipa = next_gipa;
    const auto next_create = reinterpret_cast<PFN_vkCreateInstance>(next_gipa(nullptr, "vkCreateInstance"));
    if (next_create == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    const VkResult result = next_create(create_info, allocator, instance);
    if (result == VK_SUCCESS && instance != nullptr) {
        std::lock_guard lock(g_mutex);
        g_instances[*instance] = InstanceState{next_gipa};
        g_last_instance = *instance;
    }
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL wcas_vk_CreateDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo *create_info,
    const VkAllocationCallbacks *allocator, VkDevice *device) {
    const auto *link = find_device_link(create_info->pNext);
    if (link == nullptr || link->u.pLayerInfo == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    const PFN_vkGetInstanceProcAddr next_gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    const PFN_vkGetDeviceProcAddr next_gdpa = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    if (next_gipa == nullptr || next_gdpa == nullptr) return VK_ERROR_INITIALIZATION_FAILED;

    VkInstance instance = VK_NULL_HANDLE;
    {
        std::lock_guard lock(g_mutex);
        for (const auto &[candidate, state] : g_instances) {
            (void)candidate;
            if (state.next_gipa == next_gipa) {
                instance = candidate;
                break;
            }
        }
    }
    const auto next_create = reinterpret_cast<PFN_vkCreateDevice>(next_gipa(instance, "vkCreateDevice"));
    if (next_create == nullptr) return VK_ERROR_INITIALIZATION_FAILED;

    // The host application does not need to know that we export DMA-BUFs, so
    // enable the device extensions required by the layer when available.
    std::vector<const char *> extension_names;
    for (uint32_t i = 0; i < create_info->enabledExtensionCount; ++i)
        extension_names.push_back(create_info->ppEnabledExtensionNames[i]);
    auto has_extension = [&](const char *name) {
        for (const char *existing : extension_names)
            if (std::strcmp(existing, name) == 0) return true;
        return false;
    };
    uint32_t available_count = 0;
    const auto enumerate = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
        next_gipa(instance, "vkEnumerateDeviceExtensionProperties"));
    if (enumerate != nullptr && enumerate(physical, nullptr, &available_count, nullptr) == VK_SUCCESS) {
        std::vector<VkExtensionProperties> available(available_count);
        if (enumerate(physical, nullptr, &available_count, available.data()) == VK_SUCCESS) {
            for (const char *required :
                {VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME}) {
                if (has_extension(required)) continue;
                for (const auto &property : available) {
                    if (std::strcmp(property.extensionName, required) == 0) {
                        extension_names.push_back(required);
                        break;
                    }
                }
            }
        }
    }
    VkDeviceCreateInfo adjusted = *create_info;
    if (extension_names.size() != create_info->enabledExtensionCount) {
        adjusted.enabledExtensionCount = static_cast<uint32_t>(extension_names.size());
        adjusted.ppEnabledExtensionNames = extension_names.data();
    }
    const VkResult result = next_create(physical, &adjusted, allocator, device);
    if (result != VK_SUCCESS) return result;

    auto *state = new DeviceState;
    state->instance = instance;
    state->device = *device;
    state->physical = physical;
    state->next_gipa = next_gipa;
    state->next_gdpa = next_gdpa;
    state->DestroyDevice = reinterpret_cast<PFN_vkDestroyDevice>(next_gdpa(*device, "vkDestroyDevice"));
    state->CreateSwapchainKHR = reinterpret_cast<PFN_vkCreateSwapchainKHR>(next_gdpa(*device, "vkCreateSwapchainKHR"));
    state->DestroySwapchainKHR =
        reinterpret_cast<PFN_vkDestroySwapchainKHR>(next_gdpa(*device, "vkDestroySwapchainKHR"));
    state->GetSwapchainImagesKHR =
        reinterpret_cast<PFN_vkGetSwapchainImagesKHR>(next_gdpa(*device, "vkGetSwapchainImagesKHR"));
    state->QueuePresentKHR = reinterpret_cast<PFN_vkQueuePresentKHR>(next_gdpa(*device, "vkQueuePresentKHR"));
    state->GetDeviceQueue = reinterpret_cast<PFN_vkGetDeviceQueue>(next_gdpa(*device, "vkGetDeviceQueue"));
    state->CreateImage = reinterpret_cast<PFN_vkCreateImage>(next_gdpa(*device, "vkCreateImage"));
    state->DestroyImage = reinterpret_cast<PFN_vkDestroyImage>(next_gdpa(*device, "vkDestroyImage"));
    state->GetImageMemoryRequirements2 =
        reinterpret_cast<PFN_vkGetImageMemoryRequirements2>(next_gdpa(*device, "vkGetImageMemoryRequirements2"));
    state->BindImageMemory2 = reinterpret_cast<PFN_vkBindImageMemory2>(next_gdpa(*device, "vkBindImageMemory2"));
    state->AllocateMemory = reinterpret_cast<PFN_vkAllocateMemory>(next_gdpa(*device, "vkAllocateMemory"));
    state->FreeMemory = reinterpret_cast<PFN_vkFreeMemory>(next_gdpa(*device, "vkFreeMemory"));
    state->GetMemoryFdKHR = reinterpret_cast<PFN_vkGetMemoryFdKHR>(next_gdpa(*device, "vkGetMemoryFdKHR"));
    if (state->GetMemoryFdKHR == nullptr)
        state->GetMemoryFdKHR = reinterpret_cast<PFN_vkGetMemoryFdKHR>(next_gipa(instance, "vkGetMemoryFdKHR"));
    state->GetImageSubresourceLayout =
        reinterpret_cast<PFN_vkGetImageSubresourceLayout>(next_gdpa(*device, "vkGetImageSubresourceLayout"));
    state->CreateCommandPool = reinterpret_cast<PFN_vkCreateCommandPool>(next_gdpa(*device, "vkCreateCommandPool"));
    state->DestroyCommandPool = reinterpret_cast<PFN_vkDestroyCommandPool>(next_gdpa(*device, "vkDestroyCommandPool"));
    state->AllocateCommandBuffers =
        reinterpret_cast<PFN_vkAllocateCommandBuffers>(next_gdpa(*device, "vkAllocateCommandBuffers"));
    state->ResetCommandPool = reinterpret_cast<PFN_vkResetCommandPool>(next_gdpa(*device, "vkResetCommandPool"));
    state->BeginCommandBuffer = reinterpret_cast<PFN_vkBeginCommandBuffer>(next_gdpa(*device, "vkBeginCommandBuffer"));
    state->EndCommandBuffer = reinterpret_cast<PFN_vkEndCommandBuffer>(next_gdpa(*device, "vkEndCommandBuffer"));
    state->CmdPipelineBarrier = reinterpret_cast<PFN_vkCmdPipelineBarrier>(next_gdpa(*device, "vkCmdPipelineBarrier"));
    state->CmdCopyImage = reinterpret_cast<PFN_vkCmdCopyImage>(next_gdpa(*device, "vkCmdCopyImage"));
    state->QueueSubmit = reinterpret_cast<PFN_vkQueueSubmit>(next_gdpa(*device, "vkQueueSubmit"));
    state->CreateFence = reinterpret_cast<PFN_vkCreateFence>(next_gdpa(*device, "vkCreateFence"));
    state->DestroyFence = reinterpret_cast<PFN_vkDestroyFence>(next_gdpa(*device, "vkDestroyFence"));
    state->WaitForFences = reinterpret_cast<PFN_vkWaitForFences>(next_gdpa(*device, "vkWaitForFences"));
    state->ResetFences = reinterpret_cast<PFN_vkResetFences>(next_gdpa(*device, "vkResetFences"));
    if (state->GetDeviceQueue != nullptr) state->GetDeviceQueue(*device, 0, 0, &state->queue);

    {
        std::lock_guard lock(g_mutex);
        g_devices[*device] = state;
    }
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL wcas_vk_CreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR *create_info,
    const VkAllocationCallbacks *allocator, VkSwapchainKHR *swapchain) {
    DeviceState *state = device_state(device);
    if (state == nullptr || state->CreateSwapchainKHR == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    const VkResult result = state->CreateSwapchainKHR(device, create_info, allocator, swapchain);
    if (result != VK_SUCCESS || std::getenv(kSocketEnv) == nullptr) return result;

    auto *swap = new SwapState;
    swap->device = state;
    swap->swapchain = *swapchain;
    swap->format = create_info->imageFormat;
    swap->extent = create_info->imageExtent;
    uint32_t count = 0;
    if (state->GetSwapchainImagesKHR(device, *swapchain, &count, nullptr) != VK_SUCCESS) {
        destroy_swap(swap);
        return result;
    }
    swap->images.resize(count);
    if (state->GetSwapchainImagesKHR(device, *swapchain, &count, swap->images.data()) != VK_SUCCESS) {
        destroy_swap(swap);
        return result;
    }
    if (!create_export_image(swap)) {
        destroy_swap(swap);
        return result;
    }
    {
        std::lock_guard lock(g_mutex);
        state->swaps[*swapchain] = swap;
    }
    return result;
}

VKAPI_ATTR void VKAPI_CALL wcas_vk_DestroySwapchainKHR(
    VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks *allocator) {
    DeviceState *state = device_state(device);
    if (state != nullptr) {
        SwapState *swap = nullptr;
        {
            std::lock_guard lock(g_mutex);
            const auto it = state->swaps.find(swapchain);
            if (it != state->swaps.end()) {
                swap = it->second;
                state->swaps.erase(it);
            }
        }
        if (swap != nullptr) {
            destroy_swap(swap);
        }
    }
    if (state != nullptr && state->DestroySwapchainKHR != nullptr)
        state->DestroySwapchainKHR(device, swapchain, allocator);
}

VKAPI_ATTR VkResult VKAPI_CALL wcas_vk_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *present_info) {
    DeviceState *state = nullptr;
    {
        std::lock_guard lock(g_mutex);
        for (auto &[device, candidate] : g_devices) {
            (void)device;
            if (candidate->queue == queue) {
                state = candidate;
                break;
            }
        }
    }
    if (state != nullptr && present_info->swapchainCount > 0 && present_info->pImageIndices != nullptr) {
        for (uint32_t i = 0; i < present_info->swapchainCount; ++i) {
            SwapState *swap = nullptr;
            {
                std::lock_guard lock(g_mutex);
                const auto it = state->swaps.find(present_info->pSwapchains[i]);
                if (it != state->swaps.end()) swap = it->second;
            }
            if (swap != nullptr) copy_and_send(swap, present_info->pImageIndices[i]);
        }
    }
    return state != nullptr && state->QueuePresentKHR != nullptr ? state->QueuePresentKHR(queue, present_info)
                                                                 : VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL wcas_vk_DestroyDevice(VkDevice device, const VkAllocationCallbacks *allocator) {
    DeviceState *state = nullptr;
    {
        std::lock_guard lock(g_mutex);
        const auto it = g_devices.find(device);
        if (it != g_devices.end()) {
            state = it->second;
            g_devices.erase(it);
        }
    }
    if (state == nullptr) return;
    std::vector<SwapState *> swaps;
    {
        std::lock_guard lock(g_mutex);
        for (auto &[swapchain, swap] : state->swaps) {
            (void)swapchain;
            swaps.push_back(swap);
        }
        state->swaps.clear();
    }
    for (SwapState *swap : swaps)
        destroy_swap(swap);
    const auto destroy = state->DestroyDevice;
    delete state;
    if (destroy != nullptr) destroy(device, allocator);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL layer_GetDeviceProcAddr(VkDevice device, const char *name) {
    DeviceState *state = device_state(device);
    if (std::strcmp(name, "vkCreateSwapchainKHR") == 0
        || std::strcmp(name, "vkDestroySwapchainKHR") == 0
        || std::strcmp(name, "vkQueuePresentKHR") == 0
        || std::strcmp(name, "vkDestroyDevice") == 0) {
        if (std::strcmp(name, "vkCreateSwapchainKHR") == 0)
            return reinterpret_cast<PFN_vkVoidFunction>(wcas_vk_CreateSwapchainKHR);
        if (std::strcmp(name, "vkDestroySwapchainKHR") == 0)
            return reinterpret_cast<PFN_vkVoidFunction>(wcas_vk_DestroySwapchainKHR);
        if (std::strcmp(name, "vkQueuePresentKHR") == 0)
            return reinterpret_cast<PFN_vkVoidFunction>(wcas_vk_QueuePresentKHR);
        return reinterpret_cast<PFN_vkVoidFunction>(wcas_vk_DestroyDevice);
    }
    return state != nullptr && state->next_gdpa != nullptr ? state->next_gdpa(device, name) : nullptr;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL layer_GetInstanceProcAddr(VkInstance instance, const char *name) {
    if (std::strcmp(name, "vkCreateInstance") == 0) return reinterpret_cast<PFN_vkVoidFunction>(wcas_vk_CreateInstance);
    if (std::strcmp(name, "vkCreateDevice") == 0) return reinterpret_cast<PFN_vkVoidFunction>(wcas_vk_CreateDevice);
    if (std::strcmp(name, "vkGetDeviceProcAddr") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(layer_GetDeviceProcAddr);
    if (g_next_gipa == nullptr) return nullptr;
    return g_next_gipa(instance, name);
}

}   // namespace

extern "C" VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(
    VkNegotiateLayerInterface *version) {
    if (version == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    version->loaderLayerInterfaceVersion = CURRENT_LOADER_LAYER_INTERFACE_VERSION;
    version->pfnGetInstanceProcAddr = layer_GetInstanceProcAddr;
    version->pfnGetDeviceProcAddr = layer_GetDeviceProcAddr;
    version->pfnGetPhysicalDeviceProcAddr = nullptr;
    return VK_SUCCESS;
}

extern "C" VK_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL wcas_vk_GetInstanceProcAddr(
    VkInstance instance, const char *name) {
    return layer_GetInstanceProcAddr(instance, name);
}

extern "C" VK_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL wcas_vk_GetDeviceProcAddr(
    VkDevice device, const char *name) {
    return layer_GetDeviceProcAddr(device, name);
}
