#pragma once
#include "Dimension/Platform/AppIcon.h"
#include <string>

namespace Dimension
{
    // A splash screen shown while an application loads (the editor uses it until its first frame with the scene is
    // ready): a small borderless window with the engine's logo (Engine/Resources/SplashLogo.png), a title, a subtitle
    // (the project), the current loading step and a moving progress bar. It runs on a thread of its own, so it stays drawn and movable while the
    // main thread is busy. Windows draws it with GDI; Linux shows Render's picture in an X11 window (also under
    // Wayland, through XWayland; nothing without an X server). Elsewhere the calls do nothing.
    namespace SplashScreen
    {
        void Show(const std::string& title, const std::string& subtitle);
        // The loading step shown under the title ("Compiling scripts..."). Any thread.
        void SetStatus(const std::string& status);
        // Closes it (waits for its thread). Safe to call when it is not shown.
        void Close();
        bool IsShown();

        // The layout both versions share, in pixels at scale 1 (times the screen's DPI scale).
        namespace Layout
        {
            inline constexpr int Width = 560, Height = 320;
            inline constexpr int AccentHeight = 4;                         // the line along the top
            inline constexpr int LogoX = 20, LogoY = 8, LogoWidth = 520;   // (its height follows the image's aspect)
            inline constexpr int TextLeft = 36, TextRight = Width - 36;
            inline constexpr int TitleTop = 186, TitleSize = 18;
            inline constexpr int SubtitleTop = 212, SubtitleSize = 14;
            inline constexpr int BarTop = Height - 44, BarHeight = 4;
            inline constexpr int StatusTop = BarTop - 26, StatusSize = 13;
        }

        // The picture under the text: the background color, the accent line and the logo, Width x Height times `scale`.
        AppIcon::Image Background(float scale);

        // The splash screen's picture as Linux shows it (Layout::Width x Height times `scale`): Background with the
        // text in the built-in font, `phase` (0..1) the moving bar's position. Any platform (tests use it).
        AppIcon::Image Render(float scale, const std::string& title, const std::string& subtitle,
                              const std::string& status, float phase);
    }
}
