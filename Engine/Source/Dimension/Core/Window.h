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
        bool Fullscreen = false; // borderless fullscreen on the primary monitor
        bool Hidden = false;     // created hidden (the editor shows it once loaded, behind its splash screen)
        // Linux: the window's X11 class and Wayland app id, which desktops match to a .desktop entry of that name
        // (its menu entry and icon). Empty: GLFW's default.
        std::string AppId;
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
        // Shows a window created Hidden (maximized if its spec asked) and brings it to the front.
        void Show();
        bool IsVisible() const;
        glm::uvec2 GetFramebufferSize() const;
        bool IsMinimized() const;
        bool IsFocused() const;
        void SetFullscreen(bool fullscreen);
        // The window's client size in screen coordinates while windowed (fullscreen: the size it goes back to).
        void SetSize(uint32_t width, uint32_t height);
        // The monitor the window is on (the primary one if unsure): its current mode and its DPI (96 at 100% scale).
        void GetMonitorMode(int& width, int& height, int& refreshRate) const;
        float GetDpi() const;
        // Window/taskbar icon from RGBA images of different sizes (the system picks the closest). Empty = the
        // executable's own icon.
        struct IconImage { int Width, Height; const uint8_t* Rgba; };
        void SetIcon(const std::vector<IconImage>& images);
        // The system title bar's colors (0xRRGGBB): background, caption text, and dark mode for its buttons. Custom
        // colors need Windows 11; Windows 10 only takes the dark mode. No effect on other platforms.
        void SetTitleBarColors(uint32_t background, uint32_t text, bool dark);
        bool IsFullscreen() const { return m_Fullscreen; }
        GLFWwindow* GetNative() const { return m_Window; }

        std::function<void(std::vector<fs::path>)> OnFilesDropped;
        std::function<bool()> OnCloseRequested; // return false to veto closing

    private:
        GLFWwindow* m_Window = nullptr;
        bool m_Fullscreen = false;
        bool m_ShowMaximized = false;
        int m_WindowedRect[4] = { 100, 100, 1280, 720 }; // restored when leaving fullscreen
    };
}
