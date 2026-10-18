#include "Dimension/Renderer/Texture.h"
#include "Dimension/Renderer/RenderDevice.h"
#include "Dimension/ImGui/ImGuiLayer.h"
#include "Dimension/Core/Log.h"
#include <algorithm>
#include <cmath>
#include <cstring>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

namespace Dimension
{
    Texture2D::Texture2D(const void* rgba8, uint32_t width, uint32_t height, const TextureSettings& settings, const char* debugName)
    {
        auto& device = RenderDevice::Get();
        uint32_t mips = settings.GenerateMips ? (uint32_t)std::floor(std::log2((float)std::max(width, height))) + 1 : 1;
        VkFormat format = settings.SRGB ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
        m_Image = Image(ImageDesc{ width, height, format,
                                   VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                   mips, debugName });

        VkDeviceSize size = VkDeviceSize(width) * height * 4;
        Buffer staging(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, MemoryUsage::CpuToGpu);
        std::memcpy(staging.GetMapped(), rgba8, size);

        VkImage image = m_Image.GetHandle();
        device.ImmediateSubmit([&](VkCommandBuffer cmd) {
            Vk::TransitionImage(cmd, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkBufferImageCopy copy{};
            copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            copy.imageExtent = { width, height, 1 };
            vkCmdCopyBufferToImage(cmd, staging.GetHandle(), image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

            // Generate mip chain by successive blits.
            int32_t w = (int32_t)width, h = (int32_t)height;
            for (uint32_t mip = 1; mip < mips; ++mip)
            {
                Vk::TransitionImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                    VK_IMAGE_ASPECT_COLOR_BIT, mip - 1, 1);
                int32_t nw = std::max(1, w / 2), nh = std::max(1, h / 2);
                VkImageBlit blit{};
                blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, mip - 1, 0, 1 };
                blit.srcOffsets[1] = { w, h, 1 };
                blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, 1 };
                blit.dstOffsets[1] = { nw, nh, 1 };
                vkCmdBlitImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               1, &blit, VK_FILTER_LINEAR);
                Vk::TransitionImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                    VK_IMAGE_ASPECT_COLOR_BIT, mip - 1, 1);
                w = nw;
                h = nh;
            }
            Vk::TransitionImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                VK_IMAGE_ASPECT_COLOR_BIT, mips - 1, 1);
        });

        m_Sampler = settings.Filter == TextureFilter::Point ? device.GetNearestSampler() : device.GetLinearSampler();
        m_BindlessIndex = device.RegisterTexture(m_Image.GetView(), m_Sampler);
    }

    Texture2D::~Texture2D()
    {
        RenderDevice::Get().UnregisterTexture(m_BindlessIndex);
        if (m_ImGuiId)
        {
            uint64_t id = m_ImGuiId;
            RenderDevice::Get().DeferRelease([id] { ImGuiLayer::RemoveTexture(id); });
        }
    }

    uint64_t Texture2D::GetImGuiId()
    {
        if (!m_ImGuiId) m_ImGuiId = ImGuiLayer::AddTexture(m_Sampler, m_Image.GetView());
        return m_ImGuiId;
    }

    Ref<Texture2D> Texture2D::LoadFromFile(const fs::path& path, const TextureSettings& settings)
    {
        int w, h, channels;
        std::string utf8 = path.string();
        stbi_uc* pixels = stbi_load(utf8.c_str(), &w, &h, &channels, 4);
        if (!pixels)
        {
            Log::Error("Failed to load texture '{}': {}", utf8, stbi_failure_reason());
            return nullptr;
        }
        auto name = path.filename().string();
        auto texture = MakeRef<Texture2D>(pixels, (uint32_t)w, (uint32_t)h, settings, name.c_str());
        stbi_image_free(pixels);
        return texture;
    }

    Ref<Texture2D> Texture2D::CreateSolid(uint32_t rgba, const char* debugName)
    {
        TextureSettings s;
        s.GenerateMips = false;
        return MakeRef<Texture2D>(&rgba, 1, 1, s, debugName);
    }
}
