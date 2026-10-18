#include "Dimension/Renderer/Pipeline.h"
#include "Dimension/Renderer/RenderDevice.h"

namespace Dimension
{
    static VkShaderModule CreateModule(std::span<const uint32_t> code)
    {
        VkShaderModuleCreateInfo info{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        info.codeSize = code.size_bytes();
        info.pCode = code.data();
        VkShaderModule module;
        VK_CHECK(vkCreateShaderModule(RenderDevice::Get().GetDevice(), &info, nullptr, &module));
        return module;
    }

    Pipeline::Pipeline(const GraphicsPipelineDesc& desc)
    {
        VkDevice device = RenderDevice::Get().GetDevice();
        VkShaderModule vs = CreateModule(desc.VertexSpirv);
        VkShaderModule fs = CreateModule(desc.FragmentSpirv);

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", nullptr };
        stages[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", nullptr };

        VkVertexInputBindingDescription binding{ 0, desc.VertexStride, VK_VERTEX_INPUT_RATE_VERTEX };
        std::vector<VkVertexInputAttributeDescription> attributes;
        for (auto& a : desc.Attributes) attributes.push_back({ a.Location, 0, a.Format, a.Offset });
        VkPipelineVertexInputStateCreateInfo vertexInput{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        if (desc.VertexStride > 0)
        {
            vertexInput.vertexBindingDescriptionCount = 1;
            vertexInput.pVertexBindingDescriptions = &binding;
            vertexInput.vertexAttributeDescriptionCount = (uint32_t)attributes.size();
            vertexInput.pVertexAttributeDescriptions = attributes.data();
        }

        VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        ia.topology = desc.Topology;

        VkPipelineViewportStateCreateInfo viewport{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        viewport.viewportCount = 1;
        viewport.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo raster{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        raster.polygonMode = desc.PolygonMode;
        raster.cullMode = desc.CullMode;
        raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth = 1.0f;
        if (desc.DepthBias)
        {
            raster.depthBiasEnable = VK_TRUE;
            raster.depthBiasConstantFactor = 1.25f;
            raster.depthBiasSlopeFactor = 1.75f;
        }

        VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineDepthStencilStateCreateInfo depth{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
        depth.depthTestEnable = desc.DepthTest;
        depth.depthWriteEnable = desc.DepthWrite;
        depth.depthCompareOp = desc.DepthCompare;

        std::vector<VkPipelineColorBlendAttachmentState> blends;
        std::vector<VkFormat> colorFormats;
        for (auto& att : desc.ColorAttachments)
        {
            VkPipelineColorBlendAttachmentState b{};
            b.colorWriteMask = att.Write ? (VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT) : 0;
            if (att.Blend != BlendMode::Opaque)
            {
                b.blendEnable = VK_TRUE;
                b.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
                b.dstColorBlendFactor = att.Blend == BlendMode::Additive ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                b.colorBlendOp = VK_BLEND_OP_ADD;
                b.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
                b.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                b.alphaBlendOp = VK_BLEND_OP_ADD;
            }
            blends.push_back(b);
            colorFormats.push_back(att.Format);
        }
        VkPipelineColorBlendStateCreateInfo blend{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        blend.attachmentCount = (uint32_t)blends.size();
        blend.pAttachments = blends.data();

        VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_LINE_WIDTH };
        VkPipelineDynamicStateCreateInfo dynamic{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        dynamic.dynamicStateCount = 3;
        dynamic.pDynamicStates = dynamicStates;

        VkPipelineRenderingCreateInfo rendering{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
        rendering.colorAttachmentCount = (uint32_t)colorFormats.size();
        rendering.pColorAttachmentFormats = colorFormats.data();
        rendering.depthAttachmentFormat = desc.DepthFormat;

        VkGraphicsPipelineCreateInfo info{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        info.pNext = &rendering;
        info.stageCount = 2;
        info.pStages = stages;
        info.pVertexInputState = &vertexInput;
        info.pInputAssemblyState = &ia;
        info.pViewportState = &viewport;
        info.pRasterizationState = &raster;
        info.pMultisampleState = &ms;
        info.pDepthStencilState = &depth;
        info.pColorBlendState = &blend;
        info.pDynamicState = &dynamic;
        info.layout = RenderDevice::Get().GetPipelineLayout();
        VK_CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &m_Pipeline));
        if (desc.DebugName) Vk::SetDebugName(VK_OBJECT_TYPE_PIPELINE, (uint64_t)m_Pipeline, desc.DebugName);

        vkDestroyShaderModule(device, vs, nullptr);
        vkDestroyShaderModule(device, fs, nullptr);
    }

    Pipeline::~Pipeline()
    {
        if (!m_Pipeline) return;
        VkPipeline p = m_Pipeline;
        RenderDevice::Get().DeferRelease([p] { vkDestroyPipeline(RenderDevice::Get().GetDevice(), p, nullptr); });
    }
}
