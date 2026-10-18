#pragma once
// Single include point for Vulkan in the engine: volk (function loader, no SDK
// needed) followed by the Vulkan Memory Allocator.
#include <volk.h>
#include <vk_mem_alloc.h>
#include "Dimension/Core/Base.h"

namespace Dimension
{
    const char* VkResultToString(VkResult result);
    [[noreturn]] void VulkanFatal(VkResult result, const char* expr, const char* file, int line);

    namespace Vk
    {
        // Full pipeline barrier for an image layout change. Broad stage masks keep it
        // simple and always correct; specialize when profiling shows a need.
        void TransitionImage(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
                             VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT,
                             uint32_t baseMip = 0, uint32_t mipCount = VK_REMAINING_MIP_LEVELS);

        VkImageView CreateImageView(VkImage image, VkFormat format, VkImageAspectFlags aspect, uint32_t mipLevels = 1);
        void SetDebugName(VkObjectType type, uint64_t handle, const char* name);
    }
}

#define VK_CHECK(expr)                                                                   \
    do {                                                                                 \
        VkResult vkCheckResult_ = (expr);                                                \
        if (vkCheckResult_ != VK_SUCCESS)                                                \
            ::Dimension::VulkanFatal(vkCheckResult_, #expr, __FILE__, __LINE__);         \
    } while (0)
