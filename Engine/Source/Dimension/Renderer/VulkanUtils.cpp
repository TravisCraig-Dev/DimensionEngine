#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#define VMA_IMPLEMENTATION
#include "Dimension/Renderer/Vulkan.h"
#include "Dimension/Renderer/RenderDevice.h"
#include "Dimension/Core/Log.h"

namespace Dimension
{
    const char* VkResultToString(VkResult result)
    {
        switch (result)
        {
#define DIM_VK_CASE(x) case x: return #x;
            DIM_VK_CASE(VK_SUCCESS) DIM_VK_CASE(VK_NOT_READY) DIM_VK_CASE(VK_TIMEOUT) DIM_VK_CASE(VK_INCOMPLETE)
            DIM_VK_CASE(VK_SUBOPTIMAL_KHR) DIM_VK_CASE(VK_ERROR_OUT_OF_HOST_MEMORY) DIM_VK_CASE(VK_ERROR_OUT_OF_DEVICE_MEMORY)
            DIM_VK_CASE(VK_ERROR_INITIALIZATION_FAILED) DIM_VK_CASE(VK_ERROR_DEVICE_LOST) DIM_VK_CASE(VK_ERROR_MEMORY_MAP_FAILED)
            DIM_VK_CASE(VK_ERROR_LAYER_NOT_PRESENT) DIM_VK_CASE(VK_ERROR_EXTENSION_NOT_PRESENT) DIM_VK_CASE(VK_ERROR_FEATURE_NOT_PRESENT)
            DIM_VK_CASE(VK_ERROR_INCOMPATIBLE_DRIVER) DIM_VK_CASE(VK_ERROR_TOO_MANY_OBJECTS) DIM_VK_CASE(VK_ERROR_FORMAT_NOT_SUPPORTED)
            DIM_VK_CASE(VK_ERROR_FRAGMENTED_POOL) DIM_VK_CASE(VK_ERROR_OUT_OF_POOL_MEMORY) DIM_VK_CASE(VK_ERROR_SURFACE_LOST_KHR)
            DIM_VK_CASE(VK_ERROR_NATIVE_WINDOW_IN_USE_KHR) DIM_VK_CASE(VK_ERROR_OUT_OF_DATE_KHR)
#undef DIM_VK_CASE
        default: return "VK_ERROR_UNKNOWN";
        }
    }

    void VulkanFatal(VkResult result, const char* expr, const char* file, int line)
    {
        FatalError(std::format("Vulkan call failed with {}: {}", VkResultToString(result), expr), file, line);
    }

    namespace Vk
    {
        void TransitionImage(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
                             VkImageAspectFlags aspect, uint32_t baseMip, uint32_t mipCount)
        {
            VkImageMemoryBarrier2 barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
            barrier.oldLayout = from;
            barrier.newLayout = to;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = image;
            barrier.subresourceRange = { aspect, baseMip, mipCount, 0, 1 };

            VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            dep.imageMemoryBarrierCount = 1;
            dep.pImageMemoryBarriers = &barrier;
            vkCmdPipelineBarrier2(cmd, &dep);
        }

        VkImageView CreateImageView(VkImage image, VkFormat format, VkImageAspectFlags aspect, uint32_t mipLevels, uint32_t baseMip)
        {
            VkImageViewCreateInfo info{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
            info.image = image;
            info.viewType = VK_IMAGE_VIEW_TYPE_2D;
            info.format = format;
            info.subresourceRange = { aspect, baseMip, mipLevels, 0, 1 };
            VkImageView view;
            VK_CHECK(vkCreateImageView(RenderDevice::Get().GetDevice(), &info, nullptr, &view));
            return view;
        }

        void SetDebugName(VkObjectType type, uint64_t handle, const char* name)
        {
            if (!vkSetDebugUtilsObjectNameEXT || !name) return;
            VkDebugUtilsObjectNameInfoEXT info{ VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT };
            info.objectType = type;
            info.objectHandle = handle;
            info.pObjectName = name;
            vkSetDebugUtilsObjectNameEXT(RenderDevice::Get().GetDevice(), &info);
        }
    }
}
