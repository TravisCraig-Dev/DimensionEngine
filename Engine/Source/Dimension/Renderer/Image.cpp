#include "Dimension/Renderer/Image.h"
#include "Dimension/Renderer/RenderDevice.h"

namespace Dimension
{
    bool Image::IsDepthFormat(VkFormat f)
    {
        return f == VK_FORMAT_D32_SFLOAT || f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D16_UNORM ||
               f == VK_FORMAT_D32_SFLOAT_S8_UINT;
    }

    Image::Image(const ImageDesc& desc)
        : m_Format(desc.Format), m_Width(desc.Width), m_Height(desc.Height), m_MipLevels(desc.MipLevels)
    {
        VkImageCreateInfo info{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = desc.Format;
        info.extent = { desc.Width, desc.Height, 1 };
        info.mipLevels = desc.MipLevels;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = desc.Usage;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VmaAllocationCreateInfo alloc{};
        alloc.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        if (desc.Usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT))
            alloc.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
        VK_CHECK(vmaCreateImage(RenderDevice::Get().GetAllocator(), &info, &alloc, &m_Image, &m_Allocation, nullptr));

        VkImageAspectFlags aspect = IsDepthFormat(desc.Format) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
        m_View = Vk::CreateImageView(m_Image, desc.Format, aspect, desc.MipLevels);
        if (desc.DebugName) Vk::SetDebugName(VK_OBJECT_TYPE_IMAGE, (uint64_t)m_Image, desc.DebugName);
    }

    Image::~Image() { Release(); }

    VkImageView Image::GetMipView(uint32_t mip)
    {
        if (mip >= m_MipLevels) return VK_NULL_HANDLE;
        if (m_MipViews.size() < m_MipLevels) m_MipViews.resize(m_MipLevels, VK_NULL_HANDLE);
        if (!m_MipViews[mip])
            m_MipViews[mip] = Vk::CreateImageView(m_Image, m_Format, IsDepthFormat(m_Format) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 1, mip);
        return m_MipViews[mip];
    }

    Image::Image(Image&& o) noexcept { *this = std::move(o); }

    Image& Image::operator=(Image&& o) noexcept
    {
        if (this != &o)
        {
            Release();
            std::swap(m_Image, o.m_Image);
            std::swap(m_View, o.m_View);
            std::swap(m_MipViews, o.m_MipViews);
            std::swap(m_Allocation, o.m_Allocation);
            std::swap(m_Format, o.m_Format);
            std::swap(m_Width, o.m_Width);
            std::swap(m_Height, o.m_Height);
            std::swap(m_MipLevels, o.m_MipLevels);
        }
        return *this;
    }

    void Image::Release()
    {
        if (!m_Image) return;
        VkImage image = m_Image;
        VkImageView view = m_View;
        VmaAllocation allocation = m_Allocation;
        std::vector<VkImageView> mipViews = std::move(m_MipViews);
        m_MipViews.clear();
        RenderDevice::Get().DeferRelease([image, view, allocation, mipViews] {
            auto& d = RenderDevice::Get();
            for (VkImageView v : mipViews)
                if (v) vkDestroyImageView(d.GetDevice(), v, nullptr);
            vkDestroyImageView(d.GetDevice(), view, nullptr);
            vmaDestroyImage(d.GetAllocator(), image, allocation);
        });
        m_Image = VK_NULL_HANDLE;
        m_View = VK_NULL_HANDLE;
        m_Allocation = nullptr;
    }
}
