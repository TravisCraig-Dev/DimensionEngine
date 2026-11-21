#pragma once
#include "Dimension/Renderer/Image.h"
#include "Dimension/Renderer/Environment.h"
#include <optional>

namespace Dimension
{
    enum class TextureFilter : int32_t { Bilinear = 0, Point = 1 };

    struct TextureSettings
    {
        TextureFilter Filter = TextureFilter::Bilinear;
        bool SRGB = true;
        bool GenerateMips = true;
    };

    // Sampled 2D texture registered in the bindless table.
    class Texture2D
    {
    public:
        Texture2D(const void* rgba8, uint32_t width, uint32_t height, const TextureSettings& settings, const char* debugName = nullptr);
        // HDR texture (RGBA16F) from linear float RGBA; also computes its irradiance SH (environment maps).
        Texture2D(const float* rgba32f, uint32_t width, uint32_t height, const TextureSettings& settings, const char* debugName);
        ~Texture2D();
        Texture2D(const Texture2D&) = delete;
        Texture2D& operator=(const Texture2D&) = delete;

        static Ref<Texture2D> LoadFromFile(const fs::path& path, const TextureSettings& settings);
        // Decodes an encoded image (PNG, JPEG, ...) held in memory.
        static Ref<Texture2D> LoadFromMemory(const void* data, size_t size, const TextureSettings& settings, const std::string& name);
        static Ref<Texture2D> CreateSolid(uint32_t rgba, const char* debugName);
        static Ref<Texture2D> FromDecoded(const struct DecodedImage& image, const TextureSettings& settings, const std::string& name);

        uint32_t GetBindlessIndex() const { return m_BindlessIndex; }
        bool IsHDR() const { return m_IrradianceSH.has_value(); }
        const std::optional<Environment::SH9>& GetIrradianceSH() const { return m_IrradianceSH; }
        uint32_t GetMipLevels() const { return m_Image.GetMipLevels(); }
        uint32_t GetWidth() const { return m_Image.GetWidth(); }
        uint32_t GetHeight() const { return m_Image.GetHeight(); }
        const Image& GetImage() const { return m_Image; }

        // New pixels for an RGBA8 texture without mips (same size), copied in by `cmd` (recorded outside render passes;
        // the GPU's earlier reads finish first). False when it does not fit that.
        bool Update(VkCommandBuffer cmd, const void* rgba8, uint32_t width, uint32_t height);
        // New pixels for a rectangle of an RGBA8 texture's top level (`rgba8`: width x height, rows packed), copied in by
        // `cmd` outside render passes; its smaller mips are redrawn over the same area. False when it does not fit.
        bool UpdateRegion(VkCommandBuffer cmd, uint32_t x, uint32_t y, uint32_t width, uint32_t height, const void* rgba8);
        // Copies another texture's top level onto this one's, scaled to fit (both in SHADER_READ_ONLY between frames).
        void BlitFrom(VkCommandBuffer cmd, const Texture2D& source);

        // ImGui texture id for this texture (created on first use; editor thumbnails).
        uint64_t GetImGuiId();

    private:
        Image m_Image;
        VkSampler m_Sampler = VK_NULL_HANDLE;
        uint32_t m_BindlessIndex = 0;
        uint64_t m_ImGuiId = 0;
        std::optional<Environment::SH9> m_IrradianceSH;

        void Create(const void* pixels, uint32_t bytesPerPixel, VkFormat format, uint32_t width, uint32_t height,
                    const TextureSettings& settings, const char* debugName);
    };
}
