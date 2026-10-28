#pragma once
#include "Dimension/Renderer/Vulkan.h"
#include <span>

namespace Dimension
{
    // Multiply: result = src * dst. Premultiplied: result = src + dst * (1 - src alpha).
    // Accumulate adds color and alpha (weighted blended OIT sums); Revealage multiplies the target by 1 - source.
    enum class BlendMode { Opaque, Alpha, Additive, Multiply, Premultiplied, Accumulate, Revealage };

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
        bool DepthBias = false; // constant/slope bias (shadow maps push away; negative values pull toward the camera)
        float DepthBiasConstant = 1.25f, DepthBiasSlope = 1.75f;
        std::vector<ColorAttachmentDesc> ColorAttachments;
        VkFormat DepthFormat = VK_FORMAT_UNDEFINED;
        const char* DebugName = nullptr;
        VkSampleCountFlagBits Samples = VK_SAMPLE_COUNT_1_BIT; // must match the attachments' (MSAA)
    };

    // Graphics (or compute) pipeline using the device-wide pipeline layout and dynamic rendering.
    class Pipeline
    {
    public:
        Pipeline() = default;
        explicit Pipeline(const GraphicsPipelineDesc& desc);
        // A compute pipeline; it uses the same layout, so bindless textures, uniforms and push constants work as in graphics.
        static Pipeline Compute(std::span<const uint32_t> spirv, const char* debugName = nullptr);
        ~Pipeline();
        Pipeline(Pipeline&& o) noexcept { std::swap(m_Pipeline, o.m_Pipeline); std::swap(m_BindPoint, o.m_BindPoint); }
        Pipeline& operator=(Pipeline&& o) noexcept { std::swap(m_Pipeline, o.m_Pipeline); std::swap(m_BindPoint, o.m_BindPoint); return *this; }
        Pipeline(const Pipeline&) = delete;
        Pipeline& operator=(const Pipeline&) = delete;

        void Bind(VkCommandBuffer cmd) const { vkCmdBindPipeline(cmd, m_BindPoint, m_Pipeline); }
        VkPipeline GetHandle() const { return m_Pipeline; }
        VkPipelineBindPoint GetBindPoint() const { return m_BindPoint; }

    private:
        VkPipeline m_Pipeline = VK_NULL_HANDLE;
        VkPipelineBindPoint m_BindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    };
}
