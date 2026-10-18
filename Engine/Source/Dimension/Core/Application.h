#pragma once
#include "Dimension/Core/Base.h"
#include "Dimension/Core/Window.h"
#include "Dimension/Renderer/RenderDevice.h"

namespace Dimension
{
    class ImGuiLayer;

    // A layer receives the frame callbacks in this order:
    //   OnUpdate -> OnImGui -> OnRender (off-screen work) -> OnCompose (write to the swapchain image)
    // after which the Application draws ImGui on top and presents.
    class Layer
    {
    public:
        virtual ~Layer() = default;
        virtual void OnAttach() {}
        virtual void OnDetach() {}
        virtual void OnUpdate(float dt) {}
        virtual void OnImGui() {}
        virtual void OnRender(VkCommandBuffer cmd) {}
        // Swapchain image is in TRANSFER_DST_OPTIMAL layout and must be left in it.
        virtual void OnCompose(VkCommandBuffer cmd, VkImage swapchainImage, VkExtent2D extent) {}
        virtual void OnFilesDropped(const std::vector<fs::path>& files) {}
        virtual bool OnCloseRequested() { return true; }
    };

    struct ApplicationSpec
    {
        std::string Name = "Dimension";
        WindowSpec Window;
        bool EnableImGui = true;
        bool Docking = true;
        bool VSync = true;
        bool Validation =
#ifdef DIM_DEBUG
            true;
#else
            false;
#endif
        int MaxFrames = 0; // > 0: exit after N frames (automated smoke tests)
        std::string ScreenshotPath; // if set, saves the last frame as PNG (defaults MaxFrames to 120)
    };

    class Application
    {
    public:
        explicit Application(const ApplicationSpec& spec);
        virtual ~Application();

        void Run();
        void Close();
        void PushLayer(Scope<Layer> layer);
        void PopAllLayers(); // detaches and destroys layers (call before unloading a game module)

        static Application& Get();
        Window& GetWindow() { return *m_Window; }
        RenderDevice& GetDevice() { return *m_Device; }
        const ApplicationSpec& GetSpec() const { return m_Spec; }
        float GetFrameTime() const { return m_FrameTime; }

    private:
        ApplicationSpec m_Spec;
        Scope<Window> m_Window;
        Scope<RenderDevice> m_Device;
        Scope<ImGuiLayer> m_ImGui;
        std::vector<Scope<Layer>> m_Layers;
        float m_FrameTime = 0.0f;
        bool m_Running = true;
    };
}
