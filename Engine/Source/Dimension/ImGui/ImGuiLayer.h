#pragma once
#include "Dimension/Renderer/Vulkan.h"

struct ImFont;

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

        // Editor themes: 0 Dark (Unity's Professional skin), 1 Light (Personal skin), 2 Midnight (blue-black).
        // The accent (0xRRGGBB) colors selection, active items and focus highlights.
        enum Theme : int { ThemeDark = 0, ThemeLight = 1, ThemeMidnight = 2 };
        static constexpr uint32_t kDefaultAccent = 0x3A79BB;
        static void ApplyEditorStyle(int theme = ThemeDark, uint32_t accent = kDefaultAccent);
        static bool IsLightTheme();
        // A fixed-width font (code: Code Mode's editor): Cascadia Mono or Consolas on Windows, DejaVu Sans Mono on
        // Linux, else ImGui's own (fixed-width too). Null before the layer exists.
        static ::ImFont* GetMonoFont();
    };
}
