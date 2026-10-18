#pragma once
#include "Dimension/Renderer/Image.h"
#include "Dimension/Renderer/Buffer.h"
#include "Dimension/Renderer/RenderDevice.h"
#include <optional>

namespace Dimension
{
    // Off-screen target for one camera view: HDR color, entity-id buffer (for
    // mouse picking), depth, and the final tonemapped RGBA8 output that is shown
    // in an editor panel or blitted to the swapchain.
    class SceneRenderTarget
    {
    public:
        static constexpr VkFormat kHdrFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
        static constexpr VkFormat kIdFormat = VK_FORMAT_R32_UINT;
        static constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;
        static constexpr VkFormat kOutputFormat = VK_FORMAT_R8G8B8A8_UNORM;
        static constexpr VkFormat kAOFormat = VK_FORMAT_R8_UNORM;

        SceneRenderTarget(uint32_t width, uint32_t height);
        ~SceneRenderTarget();

        void Resize(uint32_t width, uint32_t height);
        uint32_t GetWidth() const { return m_Width; }
        uint32_t GetHeight() const { return m_Height; }
        float GetAspect() const { return m_Height ? (float)m_Width / (float)m_Height : 1.0f; }

        const Image& GetHdr() const { return m_Hdr; }
        const Image& GetIds() const { return m_Ids; }
        const Image& GetDepth() const { return m_Depth; }
        const Image& GetOutput() const { return m_Output; }
        uint32_t GetHdrBindless() const { return m_HdrBindless; }
        // Bloom mip chain (half resolution and smaller), HDR format.
        uint32_t GetBloomLevels() const { return (uint32_t)m_Bloom.size(); }
        const Image& GetBloom(uint32_t level) const { return m_Bloom[level]; }
        uint32_t GetBloomBindless(uint32_t level) const { return m_BloomBindless[level]; }
        // Tonemapped image before FXAA (same format as the output).
        const Image& GetLdr() const { return m_Ldr; }
        uint32_t GetLdrBindless() const { return m_LdrBindless; }
        // Depth sampled by SSAO (nearest filtering), and the half-resolution AO images: [0] raw, [1] blurred.
        uint32_t GetDepthBindless() const { return m_DepthBindless; }
        const Image& GetAO(uint32_t i) const { return m_AO[i]; }
        uint32_t GetAOBindless(uint32_t i) const { return m_AOBindless[i]; }

        // ImGui texture showing the output (SHADER_READ_ONLY layout between frames).
        uint64_t GetImGuiTexture();

        // Mouse picking: request the entity id under a pixel; the result arrives a
        // couple of frames later via PollPick (entity index + 1, 0 = nothing).
        void RequestPick(uint32_t x, uint32_t y);
        std::optional<uint32_t> PollPick();

        // Scales the output onto a swapchain image that is in TRANSFER_DST_OPTIMAL layout.
        void BlitOutputTo(VkCommandBuffer cmd, VkImage destination, VkExtent2D extent);

        // Used by SceneRenderer.
        void RecordPickCopy(VkCommandBuffer cmd);

    private:
        void Create();
        void Destroy();

        uint32_t m_Width, m_Height;
        Image m_Hdr, m_Ids, m_Depth, m_Output;
        uint32_t m_HdrBindless = 0;
        static constexpr uint32_t kMaxBloomLevels = 6;
        std::vector<Image> m_Bloom;
        std::vector<uint32_t> m_BloomBindless;
        Image m_Ldr;
        uint32_t m_LdrBindless = 0;
        uint32_t m_DepthBindless = 0;
        Image m_AO[2];
        uint32_t m_AOBindless[2] = {};
        uint64_t m_ImGuiTexture = 0;

        std::optional<glm::uvec2> m_PickRequest;
        Buffer m_PickBuffers[kFramesInFlight];
        uint64_t m_PickFrame[kFramesInFlight] = {};
    };
}
