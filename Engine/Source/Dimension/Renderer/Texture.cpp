#include "Dimension/Renderer/Texture.h"
#include "Dimension/Renderer/RenderDevice.h"
#include "Dimension/ImGui/ImGuiLayer.h"
#include "Dimension/Core/Log.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <atomic>
#include <format>
#include <mutex>
#include <thread>
#include <vector>
#include <glm/gtc/packing.hpp>
#include <bc7enc.h>

#include "Dimension/Asset/AssetPrefetch.h"
#include "Dimension/Asset/ImportCache.h"
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
        if (!cmd || width != GetWidth() || height != GetHeight() || m_Image.GetMipLevels() != 1 || IsHDR() || m_Compressed) return false;
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

    bool Texture2D::UpdateRegion(VkCommandBuffer cmd, uint32_t x, uint32_t y, uint32_t width, uint32_t height, const void* rgba8)
    {
        if (!cmd || width == 0 || height == 0 || x + width > GetWidth() || y + height > GetHeight() || IsHDR() || m_Compressed) return false;
        VkDeviceSize size = VkDeviceSize(width) * height * 4;
        TransientAllocation staging = RenderDevice::Get().AllocateTransient(size, 16);
        if (!staging) return false;
        std::memcpy(staging.Data, rgba8, size);
        VkImage image = m_Image.GetHandle();
        uint32_t mips = m_Image.GetMipLevels();
        Vk::TransitionImage(cmd, image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy copy{};
        copy.bufferOffset = staging.Offset;
        copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        copy.imageOffset = { (int32_t)x, (int32_t)y, 0 };
        copy.imageExtent = { width, height, 1 };
        vkCmdCopyBufferToImage(cmd, staging.Buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        // Each smaller level over the area: the texels covering it, from the level above (as the chain was made).
        uint32_t x0 = x, y0 = y, x1 = x + width, y1 = y + height;
        for (uint32_t mip = 1; mip < mips; ++mip)
        {
            Vk::TransitionImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, mip - 1, 1);
            uint32_t srcW = std::max(1u, GetWidth() >> (mip - 1)), srcH = std::max(1u, GetHeight() >> (mip - 1));
            uint32_t dstW = std::max(1u, GetWidth() >> mip), dstH = std::max(1u, GetHeight() >> mip);
            x0 = std::min(x0 / 2, dstW - 1), y0 = std::min(y0 / 2, dstH - 1);
            x1 = std::min((x1 + 1) / 2, dstW), y1 = std::min((y1 + 1) / 2, dstH);
            VkImageBlit blit{};
            blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, mip - 1, 0, 1 };
            blit.srcOffsets[0] = { (int32_t)std::min(x0 * 2, srcW - 1), (int32_t)std::min(y0 * 2, srcH - 1), 0 };
            blit.srcOffsets[1] = { (int32_t)std::min(x1 * 2, srcW), (int32_t)std::min(y1 * 2, srcH), 1 };
            blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, 1 };
            blit.dstOffsets[0] = { (int32_t)x0, (int32_t)y0, 0 };
            blit.dstOffsets[1] = { (int32_t)x1, (int32_t)y1, 1 };
            vkCmdBlitImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
            Vk::TransitionImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, mip - 1, 1);
        }
        Vk::TransitionImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, mips - 1, 1);
        return true;
    }

    void Texture2D::BlitFrom(VkCommandBuffer cmd, const Texture2D& source)
    {
        if (m_Compressed || source.m_Compressed) return; // (blits can't write or read block-compressed images)
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

    Texture2D::Texture2D(const CompressedImage& image, const TextureSettings& settings, const char* debugName)
    {
        auto& device = RenderDevice::Get();
        uint32_t width = image.Width, height = image.Height, mips = (uint32_t)image.Levels.size();
        m_Compressed = true;
        m_Image = Image(ImageDesc{ width, height, settings.SRGB ? VK_FORMAT_BC7_SRGB_BLOCK : VK_FORMAT_BC7_UNORM_BLOCK,
                                   VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, std::max(mips, 1u), debugName });
        VkDeviceSize size = 0;
        for (const auto& level : image.Levels) size += (level.size() + 15) & ~VkDeviceSize(15);
        Buffer staging(std::max<VkDeviceSize>(size, 16), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, MemoryUsage::CpuToGpu);
        std::vector<VkBufferImageCopy> copies;
        VkDeviceSize offset = 0;
        for (uint32_t mip = 0; mip < mips; ++mip)
        {
            std::memcpy((uint8_t*)staging.GetMapped() + offset, image.Levels[mip].data(), image.Levels[mip].size());
            VkBufferImageCopy copy{};
            copy.bufferOffset = offset;
            copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, 1 };
            copy.imageExtent = { std::max(1u, width >> mip), std::max(1u, height >> mip), 1 }; // (partial blocks at the edges)
            copies.push_back(copy);
            offset += (image.Levels[mip].size() + 15) & ~VkDeviceSize(15);
        }
        VkImage handle = m_Image.GetHandle();
        device.ImmediateSubmit([&](VkCommandBuffer cmd) {
            Vk::TransitionImage(cmd, handle, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            vkCmdCopyBufferToImage(cmd, staging.GetHandle(), handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, (uint32_t)copies.size(), copies.data());
            Vk::TransitionImage(cmd, handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        });
        m_Sampler = settings.Filter == TextureFilter::Point ? device.GetNearestSampler() : device.GetLinearSampler();
        m_BindlessIndex = device.RegisterTexture(m_Image.GetView(), m_Sampler);
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

    namespace
    {
        // Runs body(row) for rows 0..count-1, shared out to every core (pixel work on large images).
        template<typename Body>
        void ParallelRows(uint32_t count, Body body)
        {
            unsigned n = std::min<unsigned>(std::max(1u, std::thread::hardware_concurrency()), count);
            std::atomic<uint32_t> next{ 0 };
            auto work = [&] {
                for (uint32_t row; (row = next++) < count;) body(row);
            };
            std::vector<std::thread> pool;
            for (unsigned t = 1; t < n; ++t) pool.emplace_back(work);
            work();
            for (auto& t : pool) t.join();
        }

        // Halves an image (2 x 2 boxes; an odd last row or column counts once), colour in linear light when sRGB.
        template<typename T, typename ToLinear, typename FromLinear>
        std::vector<T> Halve(const std::vector<T>& src, uint32_t w, uint32_t h, ToLinear toLinear, FromLinear fromLinear)
        {
            uint32_t nw = std::max(1u, w / 2), nh = std::max(1u, h / 2);
            std::vector<T> out((size_t)nw * nh * 4);
            auto row = [&](uint32_t y) {
                for (uint32_t x = 0; x < nw; ++x)
                    for (int c = 0; c < 4; ++c)
                    {
                        float sum = 0.0f;
                        for (uint32_t dy = 0; dy < 2; ++dy)
                            for (uint32_t dx = 0; dx < 2; ++dx)
                            {
                                uint32_t sx = std::min(x * 2 + dx, w - 1), sy = std::min(y * 2 + dy, h - 1);
                                sum += toLinear(src[((size_t)sy * w + sx) * 4 + c], c);
                            }
                        out[((size_t)y * nw + x) * 4 + c] = fromLinear(sum * 0.25f, c);
                    }
            };
            if ((size_t)nw * nh >= 65536) ParallelRows(nh, row);
            else for (uint32_t y = 0; y < nh; ++y) row(y);
            return out;
        }

        // (sRGB colour averaged as light, not as its encoded values, which would darken it; alpha as it is)
        const std::array<float, 256> kSrgbToLinear = [] {
            std::array<float, 256> t{};
            for (int i = 0; i < 256; ++i)
            {
                float c = i / 255.0f;
                t[i] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
            }
            return t;
        }();

        std::vector<uint8_t> HalveRgba8(const std::vector<uint8_t>& px, uint32_t w, uint32_t h, bool srgb)
        {
            auto toLinear = [&](uint8_t v, int c) { return srgb && c < 3 ? kSrgbToLinear[v] : v / 255.0f; };
            auto fromLinear = [&](float v, int c) {
                if (srgb && c < 3) v = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
                return (uint8_t)std::clamp((int)std::lround(v * 255.0f), 0, 255);
            };
            return Halve(px, w, h, toLinear, fromLinear);
        }

        // Halves until neither side is over `maxSize` (0: as it is).
        void ShrinkToFit(std::vector<uint8_t>& px, uint32_t& w, uint32_t& h, uint32_t maxSize, bool srgb)
        {
            for (; maxSize && std::max(w, h) > maxSize && std::max(w, h) > 1; w = std::max(1u, w / 2), h = std::max(1u, h / 2))
                px = HalveRgba8(px, w, h, srgb);
        }
    }

    std::string Texture2D::CompressedCacheVariant(const TextureSettings& s)
    {
        return std::format("bc7 srgb{} mips{} max{}", s.SRGB ? 1 : 0, s.GenerateMips ? 1 : 0, s.MaxSize);
    }

    Ref<Texture2D> Texture2D::LoadFromFile(const fs::path& path, const TextureSettings& settings, const DecodedImage* decoded)
    {
        std::string name = path.filename().string();
        bool compress = UseCompression(settings);
        if (compress)
        {
            CompressedImage blocks;
            if (ImportCache::LoadCompressedImage(path, CompressedCacheVariant(settings), blocks)) return MakeRef<Texture2D>(blocks, settings, name.c_str());
        }
        // Decoding goes through the import cache.
        std::optional<DecodedImage> own;
        if (!decoded)
        {
            own = DecodedImage::Decode(path);
            if (!own)
            {
                Log::Error("Failed to load texture '{}': {}", path.string(), stbi_failure_reason());
                return nullptr;
            }
            decoded = &*own;
        }
        if (compress)
            if (auto blocks = Compress(*decoded, settings))
            {
                ImportCache::StoreCompressedImage(path, CompressedCacheVariant(settings), *blocks);
                return MakeRef<Texture2D>(*blocks, settings, name.c_str());
            }
        return FromDecoded(*decoded, settings, name);
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
        DecodedImage image;
        image.Width = (uint32_t)w;
        image.Height = (uint32_t)h;
        image.Rgba8.assign(pixels, pixels + (size_t)w * h * 4);
        stbi_image_free(pixels);
        // (embedded images aren't files the cache can key, so they compress at every load)
        if (UseCompression(settings))
            if (auto blocks = Compress(image, settings)) return MakeRef<Texture2D>(*blocks, settings, name.c_str());
        return FromDecoded(image, settings, name);
    }

    Ref<Texture2D> Texture2D::FromDecoded(const DecodedImage& image, const TextureSettings& settings, const std::string& name)
    {
        uint32_t w = image.Width, h = image.Height;
        bool fits = settings.MaxSize == 0 || std::max(w, h) <= settings.MaxSize;
        if (image.IsHdr())
        {
            if (fits) return MakeRef<Texture2D>(image.Rgba32f.data(), w, h, settings, name.c_str());
            std::vector<float> px = image.Rgba32f;
            auto same = [](float v, int) { return v; };
            for (; std::max(w, h) > settings.MaxSize && std::max(w, h) > 1; w = std::max(1u, w / 2), h = std::max(1u, h / 2)) px = Halve(px, w, h, same, same);
            return MakeRef<Texture2D>(px.data(), w, h, settings, name.c_str());
        }
        if (fits) return MakeRef<Texture2D>(image.Rgba8.data(), w, h, settings, name.c_str());
        std::vector<uint8_t> px = image.Rgba8;
        ShrinkToFit(px, w, h, settings.MaxSize, settings.SRGB);
        return MakeRef<Texture2D>(px.data(), w, h, settings, name.c_str());
    }

    bool Texture2D::UseCompression(const TextureSettings& settings)
    {
        return settings.Compress && RenderDevice::Get().SupportsTextureCompressionBC();
    }

    std::optional<CompressedImage> Texture2D::Compress(const DecodedImage& image, const TextureSettings& settings)
    {
        if (image.IsHdr() || image.Rgba8.empty() || image.Width == 0 || image.Height == 0) return std::nullopt;
        static std::once_flag init;
        std::call_once(init, [] { bc7enc_compress_block_init(); });
        bc7enc_compress_block_params params;
        bc7enc_compress_block_params_init(&params);
        // (colour error judged as seen for sRGB images; channel by channel for data such as normals and masks)
        if (!settings.SRGB) bc7enc_compress_block_params_init_linear_weights(&params);

        uint32_t w = image.Width, h = image.Height;
        std::vector<uint8_t> px = image.Rgba8;
        ShrinkToFit(px, w, h, settings.MaxSize, settings.SRGB);
        CompressedImage out;
        out.Width = w;
        out.Height = h;
        for (;;)
        {
            // One level: 4 x 4 blocks (the edges padded by repeating the last row and column), rows of blocks shared out
            // to every core.
            uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
            std::vector<uint8_t> level((size_t)bw * bh * 16);
            ParallelRows(bh, [&](uint32_t by) {
                uint8_t block[64];
                for (uint32_t bx = 0; bx < bw; ++bx)
                {
                    for (uint32_t y = 0; y < 4; ++y)
                        for (uint32_t x = 0; x < 4; ++x)
                        {
                            uint32_t sx = std::min(bx * 4 + x, w - 1), sy = std::min(by * 4 + y, h - 1);
                            std::memcpy(block + (y * 4 + x) * 4, &px[((size_t)sy * w + sx) * 4], 4);
                        }
                    bc7enc_compress_block(&level[((size_t)by * bw + bx) * 16], block, &params);
                }
            });
            out.Levels.push_back(std::move(level));
            if (!settings.GenerateMips || (w == 1 && h == 1)) break;
            px = HalveRgba8(px, w, h, settings.SRGB);
            w = std::max(1u, w / 2);
            h = std::max(1u, h / 2);
        }
        return out;
    }

    Ref<Texture2D> Texture2D::CreateSolid(uint32_t rgba, const char* debugName)
    {
        TextureSettings s;
        s.GenerateMips = false;
        return MakeRef<Texture2D>(&rgba, 1, 1, s, debugName);
    }
}
