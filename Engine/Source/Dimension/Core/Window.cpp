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
        glfwSetScrollCallback(m_Window, [](GLFWwindow*, double dx, double dy) { Input::AddScroll((float)dy, (float)dx); });
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

    void Window::SetIcon(const std::vector<IconImage>& images)
    {
        std::vector<GLFWimage> glfwImages;
        for (auto& i : images) glfwImages.push_back({ i.Width, i.Height, const_cast<unsigned char*>(i.Rgba) });
        glfwSetWindowIcon(m_Window, (int)glfwImages.size(), glfwImages.empty() ? nullptr : glfwImages.data());
    }

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

    void Window::SetSize(uint32_t width, uint32_t height)
    {
        width = std::max(width, 64u);
        height = std::max(height, 64u);
        if (m_Fullscreen)
        {
            m_WindowedRect[2] = (int)width;
            m_WindowedRect[3] = (int)height;
            return;
        }
        glfwSetWindowSize(m_Window, (int)width, (int)height);
    }

    void Window::GetMonitorMode(int& width, int& height, int& refreshRate) const
    {
        GLFWmonitor* monitor = glfwGetWindowMonitor(m_Window);
        if (!monitor)
        {
            // The monitor holding the window's middle.
            int wx, wy, ww, wh, count = 0;
            glfwGetWindowPos(m_Window, &wx, &wy);
            glfwGetWindowSize(m_Window, &ww, &wh);
            GLFWmonitor** monitors = glfwGetMonitors(&count);
            for (int i = 0; i < count && !monitor; ++i)
            {
                int mx, my;
                glfwGetMonitorPos(monitors[i], &mx, &my);
                const GLFWvidmode* m = glfwGetVideoMode(monitors[i]);
                int cx = wx + ww / 2, cy = wy + wh / 2;
                if (m && cx >= mx && cy >= my && cx < mx + m->width && cy < my + m->height) monitor = monitors[i];
            }
            if (!monitor) monitor = glfwGetPrimaryMonitor();
        }
        const GLFWvidmode* mode = monitor ? glfwGetVideoMode(monitor) : nullptr;
        width = mode ? mode->width : 0;
        height = mode ? mode->height : 0;
        refreshRate = mode ? mode->refreshRate : 0;
    }

    float Window::GetDpi() const
    {
        float sx = 1.0f, sy = 1.0f;
        glfwGetWindowContentScale(m_Window, &sx, &sy);
        return 96.0f * sx;
    }

    bool Window::IsMinimized() const
    {
        auto s = GetFramebufferSize();
        return s.x == 0 || s.y == 0 || glfwGetWindowAttrib(m_Window, GLFW_ICONIFIED);
    }
}
