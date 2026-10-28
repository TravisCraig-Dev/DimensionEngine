#include "Dimension/Core/Application.h"
#include "Dimension/Core/Profiler.h"
#include "Dimension/Core/Input.h"
#include "Dimension/Core/Log.h"
#include "Dimension/ImGui/ImGuiLayer.h"
#include "Dimension/Renderer/SceneRenderer.h"
#include "Dimension/Scene/ComponentRegistry.h"
#include "Dimension/Asset/AssetManager.h"
#include "Dimension/Audio/AudioEngine.h"
#include <GLFW/glfw3.h>
#include <chrono>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#ifdef _WIN32
#include <windows.h>
#endif

namespace Dimension
{
    static Application* s_App = nullptr;

    Application& Application::Get() { return *s_App; }

    Application::Application(const ApplicationSpec& spec) : m_Spec(spec)
    {
        s_App = this;
#ifdef _WIN32
        // Enable ANSI colors in the console log.
        HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD mode = 0;
        if (GetConsoleMode(out, &mode)) SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
#endif
        Log::Info("Dimension Engine starting ({})", spec.Name);
        ComponentRegistry::RegisterBuiltins();

        m_Window = MakeScope<Window>(spec.Window);
        m_Window->OnFilesDropped = [this](std::vector<fs::path> files) {
            for (auto& l : m_Layers) l->OnFilesDropped(files);
        };
        m_Window->OnCloseRequested = [this] {
            for (auto& l : m_Layers)
                if (!l->OnCloseRequested()) return false;
            return true;
        };

        RenderDeviceSpec deviceSpec;
        deviceSpec.EnableValidation = spec.Validation;
        deviceSpec.VSync = spec.VSync;
        m_Device = MakeScope<RenderDevice>(*m_Window, deviceSpec);
        Renderer::Init();
        AudioEngine::Init();
        if (spec.EnableImGui) m_ImGui = MakeScope<ImGuiLayer>(*m_Window, spec.Docking);
    }

    Application::~Application()
    {
        m_Device->WaitIdle();
        for (auto it = m_Layers.rbegin(); it != m_Layers.rend(); ++it) (*it)->OnDetach();
        m_Layers.clear();
        Input::StopAllVibration();
        AssetManager::Shutdown();
        m_ImGui.reset();
        Renderer::Shutdown();
        AudioEngine::Shutdown();
        m_Device.reset();
        m_Window.reset();
        s_App = nullptr;
    }

    void Application::PushLayer(Scope<Layer> layer)
    {
        layer->OnAttach();
        m_Layers.push_back(std::move(layer));
    }

    void Application::PopAllLayers()
    {
        m_Device->WaitIdle();
        for (auto it = m_Layers.rbegin(); it != m_Layers.rend(); ++it) (*it)->OnDetach();
        m_Layers.clear();
        m_Device->WaitIdle();
    }

    void Application::Close() { m_Running = false; }

    void Application::Run()
    {
        auto last = std::chrono::steady_clock::now();
        int frames = 0;
        if (!m_Spec.ScreenshotPath.empty() && m_Spec.MaxFrames <= 0) m_Spec.MaxFrames = 120;
        Buffer screenshot;
        VkExtent2D screenshotExtent{};
        while (m_Running && !m_Window->ShouldClose())
        {
            m_Window->PollEvents();
            if (m_Window->IsMinimized())
            {
                m_Window->WaitEvents();
                continue;
            }

            auto now = std::chrono::steady_clock::now();
            m_FrameTime = std::chrono::duration<float>(now - last).count();
            last = now;
            Input::NewFrame();
            Profiler::BeginFrame();

            bool begun;
            {
                DIM_PROFILE_SCOPE("Wait for GPU");
                begun = m_Device->BeginFrame();
            }
            if (!begun) continue;
            {
                std::vector<Profiler::Section> gpu;
                for (const auto& t : m_Device->GetGpuTimings()) gpu.push_back({ t.Name, t.Depth, t.Ms, 1 });
                Profiler::SetGpuSections(std::move(gpu));
            }
            const FrameContext& frame = m_Device->Frame();
            VkCommandBuffer cmd = frame.Cmd;

            {
                DIM_PROFILE_SCOPE("Update");
                for (auto& l : m_Layers) l->OnUpdate(m_FrameTime);
            }
            if (m_ImGui)
            {
                DIM_PROFILE_SCOPE("Editor UI");
                m_ImGui->BeginFrame();
                for (auto& l : m_Layers) l->OnImGui();
                m_ImGui->EndFrame();
            }
            {
                DIM_PROFILE_SCOPE("Render");
                for (auto& l : m_Layers) l->OnRender(cmd);
            }

            // Compose into the swapchain image, then draw ImGui on top.
            Vk::TransitionImage(cmd, frame.SwapchainImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkClearColorValue black{ { 0.0f, 0.0f, 0.0f, 1.0f } };
            VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            vkCmdClearColorImage(cmd, frame.SwapchainImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
            for (auto& l : m_Layers) l->OnCompose(cmd, frame.SwapchainImage, frame.SwapchainExtent);
            Vk::TransitionImage(cmd, frame.SwapchainImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

            if (m_ImGui)
            {
                int uiTimer = m_Device->BeginGpuTimer(cmd, "Editor UI");
                VkRenderingAttachmentInfo color{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
                color.imageView = frame.SwapchainView;
                color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                VkRenderingInfo info{ VK_STRUCTURE_TYPE_RENDERING_INFO };
                info.renderArea = { { 0, 0 }, frame.SwapchainExtent };
                info.layerCount = 1;
                info.colorAttachmentCount = 1;
                info.pColorAttachments = &color;
                vkCmdBeginRendering(cmd, &info);
                m_ImGui->Render(cmd);
                vkCmdEndRendering(cmd);
                m_Device->EndGpuTimer(cmd, uiTimer);
            }
            bool lastFrame = m_Spec.MaxFrames > 0 && frames + 1 >= m_Spec.MaxFrames;
            if (lastFrame && !m_Spec.ScreenshotPath.empty())
            {
                screenshotExtent = frame.SwapchainExtent;
                screenshot = Buffer(VkDeviceSize(screenshotExtent.width) * screenshotExtent.height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, MemoryUsage::GpuToCpu);
                Vk::TransitionImage(cmd, frame.SwapchainImage, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                VkBufferImageCopy region{};
                region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                region.imageExtent = { screenshotExtent.width, screenshotExtent.height, 1 };
                vkCmdCopyImageToBuffer(cmd, frame.SwapchainImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, screenshot.GetHandle(), 1, &region);
                Vk::TransitionImage(cmd, frame.SwapchainImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
            }
            else
                Vk::TransitionImage(cmd, frame.SwapchainImage, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
            {
                DIM_PROFILE_SCOPE("Submit");
                m_Device->EndFrame();
            }

            if (lastFrame)
            {
                m_Running = false;
                if (screenshot.IsValid())
                {
                    m_Device->WaitIdle();
                    auto* px = (uint8_t*)screenshot.GetMapped();
                    size_t count = size_t(screenshotExtent.width) * screenshotExtent.height;
                    bool bgra = m_Device->GetSwapchainFormat() == VK_FORMAT_B8G8R8A8_UNORM || m_Device->GetSwapchainFormat() == VK_FORMAT_B8G8R8A8_SRGB;
                    for (size_t i = 0; i < count; ++i)
                    {
                        if (bgra) std::swap(px[i * 4], px[i * 4 + 2]);
                        px[i * 4 + 3] = 255;
                    }
                    stbi_write_png(m_Spec.ScreenshotPath.c_str(), (int)screenshotExtent.width, (int)screenshotExtent.height, 4, px, (int)screenshotExtent.width * 4);
                    Log::Info("Saved screenshot to {}", m_Spec.ScreenshotPath);
                }
            }
            ++frames;
        }
    }
}
