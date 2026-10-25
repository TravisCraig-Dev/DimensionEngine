#pragma once
#include "Dimension/Renderer/Vulkan.h"
#include <span>

namespace Dimension
{
    enum class BlendMode { Opaque, Alpha, Additive };

    struct VertexAttribute
    {
        uint32_t Location;
        VkFormat Format;
        uint32_t Offset;
    };

    struct ColorAttachmentDesc
    {
        VkFormat Format;
        BlendMode Blend = BlendMode::Opaque;
        bool Write = true;
    };

    struct GraphicsPipelineDesc
    {
        std::span<const uint32_t> VertexSpirv;
        std::span<const uint32_t> FragmentSpirv;
        uint32_t VertexStride = 0; // 0 = no vertex buffer
        std::vector<VertexAttribute> Attributes;
        VkPrimitiveTopology Topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkCullModeFlags CullMode = VK_CULL_MODE_NONE;
        VkPolygonMode PolygonMode = VK_POLYGON_MODE_FILL;
        bool DepthTest = false;
        bool DepthWrite = false;
        VkCompareOp DepthCompare = VK_COMPARE_OP_LESS_OR_EQUAL;
        bool DepthBias = false; // constant/slope bias for shadow maps
        std::vector<ColorAttachmentDesc> ColorAttachments;
        VkFormat DepthFormat = VK_FORMAT_UNDEFINED;
        const char* DebugName = nullptr;
        VkSampleCountFlagBits Samples = VK_SAMPLE_COUNT_1_BIT; // must match the attachments' (MSAA)
    };

    // Graphics pipeline using the device-wide pipeline layout and dynamic rendering.
    class Pipeline
    {
    public:
        Pipeline() = default;
        explicit Pipeline(const GraphicsPipelineDesc& desc);
        ~Pipeline();
        Pipeline(Pipeline&& o) noexcept { std::swap(m_Pipeline, o.m_Pipeline); }
        Pipeline& operator=(Pipeline&& o) noexcept { std::swap(m_Pipeline, o.m_Pipeline); return *this; }
        Pipeline(const Pipeline&) = delete;
        Pipeline& operator=(const Pipeline&) = delete;

        void Bind(VkCommandBuffer cmd) const { vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_Pipeline); }
        VkPipeline GetHandle() const { return m_Pipeline; }

    private:
        VkPipeline m_Pipeline = VK_NULL_HANDLE;
    };
}
