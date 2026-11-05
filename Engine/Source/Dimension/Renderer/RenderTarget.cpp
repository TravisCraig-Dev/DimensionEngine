#include "Dimension/Renderer/RenderTarget.h"
#include "Dimension/ImGui/ImGuiLayer.h"
#include <algorithm>
#include <cstring>

namespace Dimension
{
    SceneRenderTarget::SceneRenderTarget(uint32_t width, uint32_t height)
        : m_Width(std::max(1u, width)), m_Height(std::max(1u, height))
    {
        for (auto& b : m_PickBuffers) b = Buffer(sizeof(uint32_t), VK_BUFFER_USAGE_TRANSFER_DST_BIT, MemoryUsage::GpuToCpu, "PickReadback");
        Create();
    }

    SceneRenderTarget::~SceneRenderTarget() { Destroy(); }

    void SceneRenderTarget::Create()
    {
        m_Hdr = Image({ m_Width, m_Height, kHdrFormat,
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 1,
                        "SceneHDR" }); // copied into reflection probe faces, and from the TAA result
        for (int i = 0; i < 2; ++i)
            m_TaaHistory[i] = Image({ m_Width, m_Height, kHdrFormat,
                                      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 1, "TAAHistory" });
        Temporal.Valid = false;
        for (int i = 0; i < 2; ++i)
            m_GiHistory[i] = Image({ m_Width, m_Height, kHdrFormat, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 1, "SSGIHistory" });
        Gi.Valid = false;
        m_Ids = Image({ m_Width, m_Height, kIdFormat, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 1, "SceneIds" });
        m_Depth = Image({ m_Width, m_Height, kDepthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 1, "SceneDepth" });
        m_Output = Image({ m_Width, m_Height, kOutputFormat,
                           VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 1, "SceneOutput" });
        if (m_Samples > 1)
        {
            auto samples = (VkSampleCountFlagBits)m_Samples;
            m_HdrMS = Image({ m_Width, m_Height, kHdrFormat, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, 1, "SceneHDR MSAA", samples });
            m_IdsMS = Image({ m_Width, m_Height, kIdFormat, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, 1, "SceneIds MSAA", samples });
            m_DepthMS = Image({ m_Width, m_Height, kDepthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, 1, "SceneDepth MSAA", samples });
            m_SurfaceMS = Image({ m_Width, m_Height, kSurfaceFormat, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, 1, "SceneSurface MSAA", samples });
        }
        m_Surface = Image({ m_Width, m_Height, kSurfaceFormat, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 1, "SceneSurface" });
        m_Reflections = Image({ m_Width, m_Height, kHdrFormat, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 1, "SSR" });
        m_Indirect = Image({ std::max(1u, m_Width / 2), std::max(1u, m_Height / 2), kHdrFormat, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 1, "SSGI" });
        m_OitAccum = Image({ m_Width, m_Height, kOitAccumFormat, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 1, "OITAccum" });
        m_OitReveal = Image({ m_Width, m_Height, kOitRevealFormat, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 1, "OITReveal" });

        m_Ldr = Image({ m_Width, m_Height, kOutputFormat, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 1, "SceneLDR" });
        uint32_t aw = std::max(1u, m_Width / 2), ah = std::max(1u, m_Height / 2);
        m_AO[0] = Image({ aw, ah, kAOFormat, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 1, "SSAO" });
        m_AO[1] = Image({ aw, ah, kAOFormat, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 1, "SSAOBlur" });
        m_Dof[0] = Image({ aw, ah, kHdrFormat, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 1, "DofPrefilter" });
        m_Dof[1] = Image({ aw, ah, kHdrFormat, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 1, "DofBlur" });
        uint32_t bw = m_Width / 2, bh = m_Height / 2;
        while (m_Bloom.size() < kMaxBloomLevels && bw >= 4 && bh >= 4)
        {
            m_Bloom.emplace_back(ImageDesc{ bw, bh, kHdrFormat, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 1, "Bloom" });
            bw /= 2;
            bh /= 2;
        }

        auto& device = RenderDevice::Get();
        m_HdrBindless = device.RegisterTexture(m_Hdr.GetView(), device.GetClampSampler());
        m_LdrBindless = device.RegisterTexture(m_Ldr.GetView(), device.GetClampSampler());
        m_OutputBindless = device.RegisterTexture(m_Output.GetView(), device.GetClampSampler());
        m_DepthBindless = device.RegisterTexture(m_Depth.GetView(), device.GetNearestClampSampler());
        m_SurfaceBindless = device.RegisterTexture(m_Surface.GetView(), device.GetNearestClampSampler());
        m_ReflectionsBindless = device.RegisterTexture(m_Reflections.GetView(), device.GetClampSampler());
        m_IndirectBindless = device.RegisterTexture(m_Indirect.GetView(), device.GetClampSampler());
        m_OitAccumBindless = device.RegisterTexture(m_OitAccum.GetView(), device.GetNearestClampSampler());
        m_OitRevealBindless = device.RegisterTexture(m_OitReveal.GetView(), device.GetNearestClampSampler());
        for (int i = 0; i < 2; ++i) m_TaaHistoryBindless[i] = device.RegisterTexture(m_TaaHistory[i].GetView(), device.GetClampSampler());
        for (int i = 0; i < 2; ++i) m_GiHistoryBindless[i] = device.RegisterTexture(m_GiHistory[i].GetView(), device.GetClampSampler());
        for (int i = 0; i < 2; ++i) m_AOBindless[i] = device.RegisterTexture(m_AO[i].GetView(), device.GetClampSampler());
        for (int i = 0; i < 2; ++i) m_DofBindless[i] = device.RegisterTexture(m_Dof[i].GetView(), device.GetClampSampler());
        for (auto& b : m_Bloom) m_BloomBindless.push_back(device.RegisterTexture(b.GetView(), device.GetClampSampler()));

        // Establish the between-frames layouts so the images can be sampled even before the first render.
        std::vector<VkImage> images{ m_Hdr.GetHandle(), m_Output.GetHandle(), m_Ldr.GetHandle(), m_AO[0].GetHandle(), m_AO[1].GetHandle(),
                                     m_Dof[0].GetHandle(), m_Dof[1].GetHandle(), m_Reflections.GetHandle(), m_Indirect.GetHandle(), m_TaaHistory[0].GetHandle(),
                                     m_TaaHistory[1].GetHandle(), m_OitAccum.GetHandle(), m_OitReveal.GetHandle(), m_GiHistory[0].GetHandle(),
                                     m_GiHistory[1].GetHandle() };
        for (auto& b : m_Bloom) images.push_back(b.GetHandle());
        device.ImmediateSubmit([&](VkCommandBuffer cmd) {
            for (VkImage image : images) Vk::TransitionImage(cmd, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        });
    }

    void SceneRenderTarget::Destroy()
    {
        auto& device = RenderDevice::Get();
        device.UnregisterTexture(m_HdrBindless);
        device.UnregisterTexture(m_LdrBindless);
        device.UnregisterTexture(m_OutputBindless);
        device.UnregisterTexture(m_DepthBindless);
        device.UnregisterTexture(m_SurfaceBindless);
        device.UnregisterTexture(m_ReflectionsBindless);
        device.UnregisterTexture(m_IndirectBindless);
        device.UnregisterTexture(m_OitAccumBindless);
        device.UnregisterTexture(m_OitRevealBindless);
        m_OitAccum = Image();
        m_OitReveal = Image();
        for (uint32_t index : m_TaaHistoryBindless) device.UnregisterTexture(index);
        m_TaaHistory[0] = Image();
        m_TaaHistory[1] = Image();
        for (uint32_t index : m_GiHistoryBindless) device.UnregisterTexture(index);
        m_GiHistory[0] = Image();
        m_GiHistory[1] = Image();
        m_Surface = Image();
        m_Reflections = Image();
        m_Indirect = Image();
        m_SurfaceMS = Image();
        for (uint32_t index : m_AOBindless) device.UnregisterTexture(index);
        for (uint32_t index : m_DofBindless) device.UnregisterTexture(index);
        m_Dof[0] = Image();
        m_Dof[1] = Image();
        m_AO[0] = Image();
        m_AO[1] = Image();
        for (uint32_t index : m_BloomBindless) device.UnregisterTexture(index);
        m_BloomBindless.clear();
        m_Bloom.clear();
        m_Ldr = Image();
        if (m_ImGuiTexture)
        {
            uint64_t id = m_ImGuiTexture;
            device.DeferRelease([id] { ImGuiLayer::RemoveTexture(id); });
            m_ImGuiTexture = 0;
        }
        m_Hdr = Image();
        m_Ids = Image();
        m_Depth = Image();
        m_Output = Image();
        m_HdrMS = Image();
        m_IdsMS = Image();
        m_DepthMS = Image();
    }

    void SceneRenderTarget::SetSamples(uint32_t samples)
    {
        // The largest supported count not above the request (color and depth both).
        const auto& limits = RenderDevice::Get().GetProperties().limits;
        VkSampleCountFlags supported = limits.framebufferColorSampleCounts & limits.framebufferDepthSampleCounts;
        uint32_t s = 1;
        for (uint32_t c : { 2u, 4u, 8u })
            if (c <= samples && (supported & c)) s = c;
        if (s == m_Samples) return;
        Destroy();
        m_Samples = s;
        m_PickRequest.reset();
        Create();
    }

    void SceneRenderTarget::Resize(uint32_t width, uint32_t height)
    {
        width = std::max(1u, width);
        height = std::max(1u, height);
        if (width == m_Width && height == m_Height) return;
        Destroy();
        m_Width = width;
        m_Height = height;
        m_PickRequest.reset();
        Create();
    }

    uint64_t SceneRenderTarget::GetImGuiTexture()
    {
        if (!m_ImGuiTexture) m_ImGuiTexture = ImGuiLayer::AddTexture(RenderDevice::Get().GetClampSampler(), m_Output.GetView());
        return m_ImGuiTexture;
    }

    void SceneRenderTarget::RequestPick(uint32_t x, uint32_t y)
    {
        if (x < m_Width && y < m_Height) m_PickRequest = glm::uvec2(x, y);
    }

    void SceneRenderTarget::RecordPickCopy(VkCommandBuffer cmd)
    {
        if (!m_PickRequest) return;
        auto& device = RenderDevice::Get();
        uint32_t slot = device.Frame().FrameIndex;
        VkBufferImageCopy region{};
        region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.imageOffset = { (int32_t)m_PickRequest->x, (int32_t)m_PickRequest->y, 0 };
        region.imageExtent = { 1, 1, 1 };
        Vk::TransitionImage(cmd, m_Ids.GetHandle(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        vkCmdCopyImageToBuffer(cmd, m_Ids.GetHandle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_PickBuffers[slot].GetHandle(), 1, &region);
        m_PickFrame[slot] = device.Frame().FrameNumber;
        m_PickRequest.reset();
    }

    void SceneRenderTarget::BlitOutputTo(VkCommandBuffer cmd, VkImage destination, VkExtent2D extent)
    {
        VkImage src = m_Output.GetHandle();
        Vk::TransitionImage(cmd, src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkImageBlit blit{};
        blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        blit.srcOffsets[1] = { (int32_t)m_Width, (int32_t)m_Height, 1 };
        blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        blit.dstOffsets[1] = { (int32_t)extent.width, (int32_t)extent.height, 1 };
        vkCmdBlitImage(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
        Vk::TransitionImage(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }

    std::optional<uint32_t> SceneRenderTarget::PollPick()
    {
        uint64_t completed = RenderDevice::Get().GetCompletedFrameNumber();
        for (uint32_t i = 0; i < kFramesInFlight; ++i)
        {
            if (m_PickFrame[i] && m_PickFrame[i] <= completed)
            {
                m_PickFrame[i] = 0;
                uint32_t value;
                std::memcpy(&value, m_PickBuffers[i].GetMapped(), sizeof(value));
                return value;
            }
        }
        return std::nullopt;
    }
}
