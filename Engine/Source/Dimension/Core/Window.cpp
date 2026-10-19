#include "Dimension/Core/Window.h"
#include "Dimension/Core/Input.h"
#include "Dimension/Core/Log.h"
#include <volk.h>
#include <GLFW/glfw3.h>

namespace Dimension
{
    Window::Window(const WindowSpec& spec)
    {
        glfwSetErrorCallback([](int code, const char* desc) { Log::Error("GLFW error {}: {}", code, desc); });
        DIM_ASSERT(glfwInit(), "Failed to initialize GLFW");
        DIM_ASSERT(glfwVulkanSupported(), "Vulkan is not supported on this system");

        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        glfwWindowHint(GLFW_MAXIMIZED, spec.Maximized ? GLFW_TRUE : GLFW_FALSE);
        m_Window = glfwCreateWindow((int)spec.Width, (int)spec.Height, spec.Title.c_str(), nullptr, nullptr);
        DIM_ASSERT(m_Window, "Failed to create window");
        glfwSetWindowUserPointer(m_Window, this);
        if (spec.Fullscreen) SetFullscreen(true);

        // Installed before ImGui so its GLFW backend chains to these callbacks.
        glfwSetScrollCallback(m_Window, [](GLFWwindow*, double, double dy) { Input::AddScroll((float)dy); });
        glfwSetCharCallback(m_Window, [](GLFWwindow*, unsigned int cp) { Input::AddChar(cp); });
        glfwSetKeyCallback(m_Window, [](GLFWwindow*, int key, int, int action, int mods) { Input::AddKeyEvent(key, action, mods); });
        glfwSetDropCallback(m_Window, [](GLFWwindow* w, int count, const char** paths) {
            auto* self = (Window*)glfwGetWindowUserPointer(w);
            if (!self->OnFilesDropped) return;
            std::vector<fs::path> files;
            for (int i = 0; i < count; ++i)
                files.emplace_back(std::u8string_view((const char8_t*)paths[i]));
            self->OnFilesDropped(std::move(files));
        });
        glfwSetWindowCloseCallback(m_Window, [](GLFWwindow* w) {
            auto* self = (Window*)glfwGetWindowUserPointer(w);
            if (self->OnCloseRequested && !self->OnCloseRequested()) glfwSetWindowShouldClose(w, GLFW_FALSE);
        });
        Input::Init(m_Window);
    }

    Window::~Window()
    {
        glfwDestroyWindow(m_Window);
        glfwTerminate();
    }

    void Window::PollEvents() { glfwPollEvents(); }
    void Window::WaitEvents() { glfwWaitEvents(); }
    bool Window::ShouldClose() const { return glfwWindowShouldClose(m_Window); }
    void Window::SetShouldClose(bool close) { glfwSetWindowShouldClose(m_Window, close); }
    void Window::SetTitle(const std::string& title) { glfwSetWindowTitle(m_Window, title.c_str()); }

    glm::uvec2 Window::GetFramebufferSize() const
    {
        int w, h;
        glfwGetFramebufferSize(m_Window, &w, &h);
        return { (uint32_t)w, (uint32_t)h };
    }

    bool Window::IsFocused() const { return glfwGetWindowAttrib(m_Window, GLFW_FOCUSED) == GLFW_TRUE; }

    void Window::SetFullscreen(bool fullscreen)
    {
        if (fullscreen == m_Fullscreen) return;
        if (fullscreen)
        {
            glfwGetWindowPos(m_Window, &m_WindowedRect[0], &m_WindowedRect[1]);
            glfwGetWindowSize(m_Window, &m_WindowedRect[2], &m_WindowedRect[3]);
            GLFWmonitor* monitor = glfwGetPrimaryMonitor();
            const GLFWvidmode* mode = monitor ? glfwGetVideoMode(monitor) : nullptr;
            if (!mode) return;
            // Matching the desktop mode gives a borderless fullscreen window (no display mode switch).
            glfwSetWindowMonitor(m_Window, monitor, 0, 0, mode->width, mode->height, mode->refreshRate);
        }
        else
            glfwSetWindowMonitor(m_Window, nullptr, m_WindowedRect[0], m_WindowedRect[1], m_WindowedRect[2], m_WindowedRect[3], 0);
        m_Fullscreen = fullscreen;
    }

    bool Window::IsMinimized() const
    {
        auto s = GetFramebufferSize();
        return s.x == 0 || s.y == 0 || glfwGetWindowAttrib(m_Window, GLFW_ICONIFIED);
    }
}
