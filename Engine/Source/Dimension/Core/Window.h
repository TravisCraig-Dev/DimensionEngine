#pragma once
#include "Dimension/Core/Base.h"
#include <glm/glm.hpp>

struct GLFWwindow;

namespace Dimension
{
    struct WindowSpec
    {
        std::string Title = "Dimension";
        uint32_t Width = 1600, Height = 900;
        bool Maximized = false;
    };

    class Window
    {
    public:
        explicit Window(const WindowSpec& spec);
        ~Window();
        Window(const Window&) = delete;
        Window& operator=(const Window&) = delete;

        void PollEvents();
        void WaitEvents();
        bool ShouldClose() const;
        void SetShouldClose(bool close);
        void SetTitle(const std::string& title);
        glm::uvec2 GetFramebufferSize() const;
        bool IsMinimized() const;
        bool IsFocused() const;
        GLFWwindow* GetNative() const { return m_Window; }

        std::function<void(std::vector<fs::path>)> OnFilesDropped;
        std::function<bool()> OnCloseRequested; // return false to veto closing

    private:
        GLFWwindow* m_Window = nullptr;
    };
}
