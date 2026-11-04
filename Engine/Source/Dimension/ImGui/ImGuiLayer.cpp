#include "Dimension/ImGui/ImGuiLayer.h"
#include "Dimension/Renderer/RenderDevice.h"
#include "Dimension/Core/Window.h"
#include "Dimension/Core/Log.h"
#include "Dimension/Core/Input.h"
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>
#include <ImGuizmo.h>

namespace Dimension
{
    static bool s_Active = false;

    ImGuiLayer::ImGuiLayer(Window& window, bool docking)
    {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        if (docking) io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        io.ConfigWindowsMoveFromTitleBarOnly = true;

        // Prefer Segoe UI on Windows; fall back to the built-in font.
        const char* font = "C:/Windows/Fonts/segoeui.ttf";
        if (std::filesystem::exists(font)) io.Fonts->AddFontFromFileTTF(font, 16.0f);
        else io.Fonts->AddFontDefault();
        ApplyEditorStyle();

        ImGui_ImplGlfw_InitForVulkan(window.GetNative(), true);

        auto& device = RenderDevice::Get();
        VkFormat format = device.GetSwapchainFormat();
        ImGui_ImplVulkan_InitInfo info{};
        info.ApiVersion = VK_API_VERSION_1_3;
        info.Instance = device.GetInstance();
        info.PhysicalDevice = device.GetPhysicalDevice();
        info.Device = device.GetDevice();
        info.QueueFamily = device.GetGraphicsQueueFamily();
        info.Queue = device.GetGraphicsQueue();
        info.DescriptorPoolSize = 1024;
        info.MinImageCount = 2;
        info.ImageCount = std::max(2u, device.GetSwapchainImageCount());
        info.UseDynamicRendering = true;
        info.PipelineInfoMain.PipelineRenderingCreateInfo = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
        info.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
        info.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &format;
        info.CheckVkResultFn = [](VkResult r) {
            if (r != VK_SUCCESS) Log::Error("ImGui Vulkan error: {}", VkResultToString(r));
        };
        ImGui_ImplVulkan_Init(&info);
        s_Active = true;
    }

    ImGuiLayer::~ImGuiLayer()
    {
        RenderDevice::Get().WaitIdle();
        s_Active = false;
        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
    }

    void ImGuiLayer::BeginFrame()
    {
        ImGui_ImplVulkan_NewFrame();
        // A game's hidden cursor: ImGui would otherwise show it again every frame. A locked one moves freely out of
        // sight, so the editor's windows must not react to it (clicks while mouse-looking).
        ImGuiIO& io = ImGui::GetIO();
        CursorMode cursor = Input::GetEffectiveCursorMode();
        if (cursor != CursorMode::Normal) io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
        else io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
        if (cursor == CursorMode::Locked) io.ConfigFlags |= ImGuiConfigFlags_NoMouse;
        else io.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        ImGuizmo::BeginFrame();
    }

    void ImGuiLayer::EndFrame() { ImGui::Render(); }

    void ImGuiLayer::Render(VkCommandBuffer cmd) { ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd); }

    uint64_t ImGuiLayer::AddTexture(VkSampler sampler, VkImageView view)
    {
        if (!s_Active) return 0;
        return (uint64_t)ImGui_ImplVulkan_AddTexture(sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }

    void ImGuiLayer::RemoveTexture(uint64_t id)
    {
        if (s_Active && id) ImGui_ImplVulkan_RemoveTexture((VkDescriptorSet)id);
    }

    bool ImGuiLayer::IsActive() { return s_Active; }

    static int s_Theme = ImGuiLayer::ThemeDark;
    bool ImGuiLayer::IsLightTheme() { return s_Theme == ThemeLight; }

    void ImGuiLayer::ApplyEditorStyle(int theme, uint32_t accentRgb)
    {
        s_Theme = theme;
        ImGuiStyle& style = ImGui::GetStyle();
        style.WindowRounding = 2.0f;
        style.FrameRounding = 3.0f;
        style.GrabRounding = 3.0f;
        style.TabRounding = 3.0f;
        style.PopupRounding = 3.0f;
        style.ScrollbarRounding = 6.0f;
        style.FramePadding = ImVec2(6, 4);
        style.ItemSpacing = ImVec2(8, 5);
        style.WindowPadding = ImVec2(8, 8);
        style.WindowBorderSize = 1.0f;
        style.TabBorderSize = 0.0f;
        style.IndentSpacing = 14.0f;

        ImVec4* c = style.Colors;
        auto rgb = [](int r, int g, int b, float a = 1.0f) { return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a); };
        ImVec4 accent = rgb((accentRgb >> 16) & 0xFF, (accentRgb >> 8) & 0xFF, accentRgb & 0xFF);
        auto withAlpha = [](ImVec4 v, float a) { v.w = a; return v; };
        if (theme == ThemeLight || theme == ThemeMidnight)
        {
            // Both are built from a few base tones: background, panel, field, raised (buttons), and text.
            bool light = theme == ThemeLight;
            auto tone = [&](int grey, int blueShift = 0) { return rgb(grey, grey + blueShift / 2, grey + blueShift); };
            int shift = light ? 0 : 14;
            ImVec4 bg = light ? tone(194) : tone(30, shift), panel = light ? tone(200) : tone(34, shift);
            ImVec4 field = light ? tone(232) : tone(22, shift), raised = light ? tone(222) : tone(48, shift);
            ImVec4 raisedHover = light ? tone(236) : tone(62, shift), dark = light ? tone(150) : tone(16, shift);
            ImVec4 text = light ? tone(16) : tone(214, 10);
            c[ImGuiCol_Text] = text;
            c[ImGuiCol_TextDisabled] = light ? tone(100) : tone(120, shift);
            c[ImGuiCol_WindowBg] = bg;
            c[ImGuiCol_ChildBg] = bg;
            c[ImGuiCol_PopupBg] = panel;
            c[ImGuiCol_Border] = dark;
            c[ImGuiCol_FrameBg] = field;
            c[ImGuiCol_FrameBgHovered] = light ? tone(242) : tone(30, shift);
            c[ImGuiCol_FrameBgActive] = light ? tone(250) : tone(18, shift);
            c[ImGuiCol_TitleBg] = light ? tone(180) : tone(24, shift);
            c[ImGuiCol_TitleBgActive] = c[ImGuiCol_TitleBg];
            c[ImGuiCol_MenuBarBg] = light ? tone(184) : tone(24, shift);
            c[ImGuiCol_ScrollbarBg] = light ? tone(186) : tone(26, shift);
            c[ImGuiCol_ScrollbarGrab] = light ? tone(150) : tone(70, shift);
            c[ImGuiCol_CheckMark] = light ? tone(30) : tone(210, 10);
            c[ImGuiCol_SliderGrab] = light ? tone(130) : tone(110, shift);
            c[ImGuiCol_SliderGrabActive] = accent;
            c[ImGuiCol_Button] = raised;
            c[ImGuiCol_ButtonHovered] = raisedHover;
            c[ImGuiCol_ButtonActive] = accent;
            c[ImGuiCol_Header] = light ? tone(176) : tone(44, shift);
            c[ImGuiCol_HeaderHovered] = light ? tone(186) : tone(56, shift);
            c[ImGuiCol_HeaderActive] = accent;
            c[ImGuiCol_Separator] = dark;
            c[ImGuiCol_Tab] = light ? tone(176) : tone(26, shift);
            c[ImGuiCol_TabHovered] = raisedHover;
            c[ImGuiCol_TabSelected] = bg;
            c[ImGuiCol_TabSelectedOverline] = accent;
            c[ImGuiCol_TabDimmed] = c[ImGuiCol_Tab];
            c[ImGuiCol_TabDimmedSelected] = bg;
            c[ImGuiCol_DockingPreview] = withAlpha(accent, 0.7f);
            c[ImGuiCol_DockingEmptyBg] = dark;
            c[ImGuiCol_TextSelectedBg] = withAlpha(accent, 0.45f);
            c[ImGuiCol_NavCursor] = accent;
            c[ImGuiCol_ResizeGrip] = rgb(0, 0, 0, 0);
            c[ImGuiCol_PlotHistogram] = accent;
            return;
        }

        // Dark grey palette close to Unity's Professional skin.
        c[ImGuiCol_Text] = rgb(210, 210, 210);
        c[ImGuiCol_TextDisabled] = rgb(128, 128, 128);
        c[ImGuiCol_WindowBg] = rgb(56, 56, 56);
        c[ImGuiCol_ChildBg] = rgb(56, 56, 56);
        c[ImGuiCol_PopupBg] = rgb(46, 46, 46);
        c[ImGuiCol_Border] = rgb(30, 30, 30);
        c[ImGuiCol_FrameBg] = rgb(42, 42, 42);
        c[ImGuiCol_FrameBgHovered] = rgb(50, 50, 50);
        c[ImGuiCol_FrameBgActive] = rgb(36, 36, 36);
        c[ImGuiCol_TitleBg] = rgb(40, 40, 40);
        c[ImGuiCol_TitleBgActive] = rgb(40, 40, 40);
        c[ImGuiCol_MenuBarBg] = rgb(40, 40, 40);
        c[ImGuiCol_ScrollbarBg] = rgb(50, 50, 50);
        c[ImGuiCol_ScrollbarGrab] = rgb(90, 90, 90);
        c[ImGuiCol_CheckMark] = rgb(200, 200, 200);
        c[ImGuiCol_SliderGrab] = rgb(130, 130, 130);
        c[ImGuiCol_SliderGrabActive] = rgb(160, 160, 160);
        c[ImGuiCol_Button] = rgb(70, 70, 70);
        c[ImGuiCol_ButtonHovered] = rgb(85, 85, 85);
        c[ImGuiCol_ButtonActive] = accent;
        c[ImGuiCol_Header] = rgb(66, 66, 66);
        c[ImGuiCol_HeaderHovered] = rgb(80, 80, 80);
        c[ImGuiCol_HeaderActive] = accent;
        c[ImGuiCol_Separator] = rgb(35, 35, 35);
        c[ImGuiCol_Tab] = rgb(45, 45, 45);
        c[ImGuiCol_TabHovered] = rgb(70, 70, 70);
        c[ImGuiCol_TabSelected] = rgb(60, 60, 60);
        c[ImGuiCol_TabSelectedOverline] = accent;
        c[ImGuiCol_TabDimmed] = rgb(45, 45, 45);
        c[ImGuiCol_TabDimmedSelected] = rgb(56, 56, 56);
        c[ImGuiCol_DockingPreview] = withAlpha(accent, 0.7f);
        c[ImGuiCol_DockingEmptyBg] = rgb(30, 30, 30);
        c[ImGuiCol_TextSelectedBg] = withAlpha(accent, 0.5f);
        c[ImGuiCol_NavCursor] = accent;
        c[ImGuiCol_ResizeGrip] = rgb(0, 0, 0, 0);
    }
}
