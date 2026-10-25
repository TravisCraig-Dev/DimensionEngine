#pragma once
#include "Dimension/Renderer/Vulkan.h"
#include <vector>

namespace Dimension
{
    struct ImageDesc
    {
        uint32_t Width = 1, Height = 1;
        VkFormat Format = VK_FORMAT_R8G8B8A8_UNORM;
        VkImageUsageFlags Usage = VK_IMAGE_USAGE_SAMPLED_BIT;
        uint32_t MipLevels = 1;
        const char* DebugName = nullptr;
    };

    // RAII 2D image + default view. Destruction is deferred past in-flight frames.
    class Image
    {
    public:
        Image() = default;
        explicit Image(const ImageDesc& desc);
        ~Image();
        Image(Image&& other) noexcept;
        Image& operator=(Image&& other) noexcept;
        Image(const Image&) = delete;
        Image& operator=(const Image&) = delete;

        VkImage GetHandle() const { return m_Image; }
        VkImageView GetView() const { return m_View; }
        VkFormat GetFormat() const { return m_Format; }
        VkExtent2D GetExtent() const { return { m_Width, m_Height }; }
        uint32_t GetWidth() const { return m_Width; }
        uint32_t GetHeight() const { return m_Height; }
        uint32_t GetMipLevels() const { return m_MipLevels; }
        // A view of a single mip level (render target for that level); created on first use.
        VkImageView GetMipView(uint32_t mip);
        bool IsValid() const { return m_Image != VK_NULL_HANDLE; }

        static bool IsDepthFormat(VkFormat format);

    private:
        void Release();

        VkImage m_Image = VK_NULL_HANDLE;
        VkImageView m_View = VK_NULL_HANDLE;
        std::vector<VkImageView> m_MipViews;
        VmaAllocation m_Allocation = nullptr;
        VkFormat m_Format = VK_FORMAT_UNDEFINED;
        uint32_t m_Width = 0, m_Height = 0, m_MipLevels = 1;
    };
}
