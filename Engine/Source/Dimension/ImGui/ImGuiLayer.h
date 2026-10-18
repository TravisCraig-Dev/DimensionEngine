#pragma once
#include "Dimension/Renderer/Vulkan.h"

namespace Dimension
{
    class Window;

    // Dear ImGui (docking branch) on GLFW + Vulkan dynamic rendering.
    class ImGuiLayer
    {
    public:
        ImGuiLayer(Window& window, bool docking);
        ~ImGuiLayer();

        void BeginFrame();
        void EndFrame();                  // ImGui::Render()
        void Render(VkCommandBuffer cmd); // records draw data into the current rendering pass

        // Texture registration for ImGui::Image. Returns an ImTextureID value.
        static uint64_t AddTexture(VkSampler sampler, VkImageView view);
        static void RemoveTexture(uint64_t id);
        static bool IsActive();

        static void ApplyEditorStyle();
    };
}
