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
        // Per-pixel surface data for screen-space reflections and GI (see MeshSurface.glsl's outSurface).
        static constexpr VkFormat kSurfaceFormat = VK_FORMAT_R32G32B32A32_UINT;
        // Order-independent transparency: weighted color and alpha sums, and the product of (1 - alpha).
        static constexpr VkFormat kOitAccumFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
        static constexpr VkFormat kOitRevealFormat = VK_FORMAT_R16_SFLOAT;

        SceneRenderTarget(uint32_t width, uint32_t height);
        ~SceneRenderTarget();

        void Resize(uint32_t width, uint32_t height);
        // MSAA: 1 (off), 2, 4 or 8 samples (clamped to what the GPU supports). With more than one, the scene is drawn
        // into multisampled color, id and depth images that resolve into GetHdr(), GetIds() and GetDepth().
        void SetSamples(uint32_t samples);
        uint32_t GetSamples() const { return m_Samples; }
        const Image& GetHdrMS() const { return m_HdrMS; }
        const Image& GetIdsMS() const { return m_IdsMS; }
        const Image& GetDepthMS() const { return m_DepthMS; }
        const Image& GetSurfaceMS() const { return m_SurfaceMS; }
        // Screen-space reflections: the surface buffer the main pass writes, and the traced reflections (HDR).
        const Image& GetSurface() const { return m_Surface; }
        uint32_t GetSurfaceBindless() const { return m_SurfaceBindless; }
        const Image& GetOitAccum() const { return m_OitAccum; }
        const Image& GetOitReveal() const { return m_OitReveal; }
        uint32_t GetOitAccumBindless() const { return m_OitAccumBindless; }
        uint32_t GetOitRevealBindless() const { return m_OitRevealBindless; }
        const Image& GetReflections() const { return m_Reflections; }
        uint32_t GetReflectionsBindless() const { return m_ReflectionsBindless; }
        const Image& GetIndirect() const { return m_Indirect; } // screen-space GI: this frame's half-resolution trace
        uint32_t GetIndirectBindless() const { return m_IndirectBindless; }
        // Screen-space GI's temporal filter: two full-resolution history images (one read, one written each frame).
        const Image& GetGiHistory(uint32_t i) const { return m_GiHistory[i]; }
        uint32_t GetGiHistoryBindless(uint32_t i) const { return m_GiHistoryBindless[i]; }
        struct GiState
        {
            uint32_t Written = 0; // history image written last
            bool Valid = false;
            glm::mat4 PrevViewProj{ 1.0f };
        } Gi;
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
        // Depth of field, half resolution HDR: [0] color + CoC, [1] blurred.
        const Image& GetDof(uint32_t i) const { return m_Dof[i]; }
        uint32_t GetDofBindless(uint32_t i) const { return m_DofBindless[i]; }

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

        // Temporal anti-aliasing: two history images (one read, one written each frame) and what they were made with.
        const Image& GetTaaHistory(uint32_t i) const { return m_TaaHistory[i]; }
        uint32_t GetTaaHistoryBindless(uint32_t i) const { return m_TaaHistoryBindless[i]; }
        struct TemporalState
        {
            uint32_t Frame = 0;   // jitter sequence position
            uint32_t Written = 0; // history image written last
            bool Valid = false;   // history holds a previous frame
            glm::mat4 PrevViewProj{ 1.0f };
            uint32_t CameraCuts = 0; // the scene's count when the history was made (another camera: start over)
        } Temporal;

    private:
        void Create();
        void Destroy();

        uint32_t m_Width, m_Height;
        uint32_t m_Samples = 1;
        Image m_Hdr, m_Ids, m_Depth, m_Output;
        Image m_HdrMS, m_IdsMS, m_DepthMS, m_SurfaceMS; // only with MSAA
        Image m_Surface, m_Reflections, m_Indirect;
        Image m_OitAccum, m_OitReveal;
        uint32_t m_OitAccumBindless = 0, m_OitRevealBindless = 0;
        Image m_TaaHistory[2];
        uint32_t m_TaaHistoryBindless[2] = {};
        Image m_GiHistory[2];
        uint32_t m_GiHistoryBindless[2] = {};
        uint32_t m_SurfaceBindless = 0, m_ReflectionsBindless = 0, m_IndirectBindless = 0;
        uint32_t m_HdrBindless = 0;
        static constexpr uint32_t kMaxBloomLevels = 6;
        std::vector<Image> m_Bloom;
        std::vector<uint32_t> m_BloomBindless;
        Image m_Ldr;
        uint32_t m_LdrBindless = 0;
        uint32_t m_DepthBindless = 0;
        Image m_AO[2];
        uint32_t m_AOBindless[2] = {};
        Image m_Dof[2];
        uint32_t m_DofBindless[2] = {};
        uint64_t m_ImGuiTexture = 0;

        std::optional<glm::uvec2> m_PickRequest;
        Buffer m_PickBuffers[kFramesInFlight];
        uint64_t m_PickFrame[kFramesInFlight] = {};
    };
}
