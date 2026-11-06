#include "Dimension/Renderer/Texture.h"
#include "Dimension/Renderer/RenderDevice.h"
#include "Dimension/ImGui/ImGuiLayer.h"
#include "Dimension/Core/Log.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>
#include <glm/gtc/packing.hpp>

#include "Dimension/Asset/AssetPrefetch.h"
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

namespace Dimension
{
    Texture2D::Texture2D(const void* rgba8, uint32_t width, uint32_t height, const TextureSettings& settings, const char* debugName)
    {
        Create(rgba8, 4, settings.SRGB ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM, width, height, settings, debugName);
    }

    bool Texture2D::Update(VkCommandBuffer cmd, const void* rgba8, uint32_t width, uint32_t height)
    {
        if (!cmd || width != GetWidth() || height != GetHeight() || m_Image.GetMipLevels() != 1 || IsHDR()) return false;
        VkDeviceSize size = VkDeviceSize(width) * height * 4;
        TransientAllocation staging = RenderDevice::Get().AllocateTransient(size, 16);
        if (!staging) return false;
        std::memcpy(staging.Data, rgba8, size);
        VkImage image = m_Image.GetHandle();
        Vk::TransitionImage(cmd, image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy copy{};
        copy.bufferOffset = staging.Offset;
        copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        copy.imageExtent = { width, height, 1 };
        vkCmdCopyBufferToImage(cmd, staging.Buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        Vk::TransitionImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        return true;
    }

    void Texture2D::BlitFrom(VkCommandBuffer cmd, const Texture2D& source)
    {
        VkImage src = source.m_Image.GetHandle(), dst = m_Image.GetHandle();
        Vk::TransitionImage(cmd, src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1);
        Vk::TransitionImage(cmd, dst, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1);
        VkImageBlit blit{};
        blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        blit.srcOffsets[1] = { (int32_t)source.GetWidth(), (int32_t)source.GetHeight(), 1 };
        blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        blit.dstOffsets[1] = { (int32_t)GetWidth(), (int32_t)GetHeight(), 1 };
        vkCmdBlitImage(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
        Vk::TransitionImage(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1);
        Vk::TransitionImage(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1);
    }

    Texture2D::Texture2D(const float* rgba32f, uint32_t width, uint32_t height, const TextureSettings& settings, const char* debugName)
    {
        std::vector<uint16_t> half(size_t(width) * height * 4);
        for (size_t i = 0; i < half.size(); ++i) half[i] = glm::packHalf1x16(std::min(rgba32f[i], 65000.0f));
        Create(half.data(), 8, VK_FORMAT_R16G16B16A16_SFLOAT, width, height, settings, debugName);
        m_IrradianceSH = Environment::ComputeIrradianceSH(rgba32f, width, height);
    }

    void Texture2D::Create(const void* pixels, uint32_t bytesPerPixel, VkFormat format, uint32_t width, uint32_t height,
                           const TextureSettings& settings, const char* debugName)
    {
        auto& device = RenderDevice::Get();
        uint32_t mips = settings.GenerateMips ? (uint32_t)std::floor(std::log2((float)std::max(width, height))) + 1 : 1;
        m_Image = Image(ImageDesc{ width, height, format,
                                   VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                   mips, debugName });

        VkDeviceSize size = VkDeviceSize(width) * height * bytesPerPixel;
        Buffer staging(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, MemoryUsage::CpuToGpu);
        std::memcpy(staging.GetMapped(), pixels, size);

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
        // Decoding goes through the import cache.
        auto image = DecodedImage::Decode(path);
        if (!image)
        {
            Log::Error("Failed to load texture '{}': {}", path.string(), stbi_failure_reason());
            return nullptr;
        }
        return FromDecoded(*image, settings, path.filename().string());
    }

    Ref<Texture2D> Texture2D::LoadFromMemory(const void* data, size_t size, const TextureSettings& settings, const std::string& name)
    {
        int w, h, channels;
        stbi_uc* pixels = stbi_load_from_memory((const stbi_uc*)data, (int)size, &w, &h, &channels, 4);
        if (!pixels)
        {
            Log::Error("Failed to decode texture '{}': {}", name, stbi_failure_reason());
            return nullptr;
        }
        auto texture = MakeRef<Texture2D>(pixels, (uint32_t)w, (uint32_t)h, settings, name.c_str());
        stbi_image_free(pixels);
        return texture;
    }

    Ref<Texture2D> Texture2D::FromDecoded(const DecodedImage& image, const TextureSettings& settings, const std::string& name)
    {
        if (image.IsHdr()) return MakeRef<Texture2D>(image.Rgba32f.data(), image.Width, image.Height, settings, name.c_str());
        return MakeRef<Texture2D>(image.Rgba8.data(), image.Width, image.Height, settings, name.c_str());
    }

    Ref<Texture2D> Texture2D::CreateSolid(uint32_t rgba, const char* debugName)
    {
        TextureSettings s;
        s.GenerateMips = false;
        return MakeRef<Texture2D>(&rgba, 1, 1, s, debugName);
    }
}
