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
        m_Hdr = Image({ m_Width, m_Height, kHdrFormat, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 1, "SceneHDR" });
        m_Ids = Image({ m_Width, m_Height, kIdFormat, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 1, "SceneIds" });
        m_Depth = Image({ m_Width, m_Height, kDepthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, 1, "SceneDepth" });
        m_Output = Image({ m_Width, m_Height, kOutputFormat,
                           VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 1, "SceneOutput" });

        auto& device = RenderDevice::Get();
        m_HdrBindless = device.RegisterTexture(m_Hdr.GetView(), device.GetClampSampler());

        // Establish the between-frames layouts so the output can be sampled even before the first render.
        VkImage hdr = m_Hdr.GetHandle(), out = m_Output.GetHandle();
        device.ImmediateSubmit([&](VkCommandBuffer cmd) {
            Vk::TransitionImage(cmd, hdr, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            Vk::TransitionImage(cmd, out, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        });
    }

    void SceneRenderTarget::Destroy()
    {
        auto& device = RenderDevice::Get();
        device.UnregisterTexture(m_HdrBindless);
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
