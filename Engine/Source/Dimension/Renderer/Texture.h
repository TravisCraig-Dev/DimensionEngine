#pragma once
#include "Dimension/Renderer/Image.h"

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
        ~Texture2D();
        Texture2D(const Texture2D&) = delete;
        Texture2D& operator=(const Texture2D&) = delete;

        static Ref<Texture2D> LoadFromFile(const fs::path& path, const TextureSettings& settings);
        static Ref<Texture2D> CreateSolid(uint32_t rgba, const char* debugName);

        uint32_t GetBindlessIndex() const { return m_BindlessIndex; }
        uint32_t GetWidth() const { return m_Image.GetWidth(); }
        uint32_t GetHeight() const { return m_Image.GetHeight(); }
        const Image& GetImage() const { return m_Image; }

        // ImGui texture id for this texture (created on first use; editor thumbnails).
        uint64_t GetImGuiId();

    private:
        Image m_Image;
        VkSampler m_Sampler = VK_NULL_HANDLE;
        uint32_t m_BindlessIndex = 0;
        uint64_t m_ImGuiId = 0;
    };
}
