#pragma once
#include "Dimension/Platform/AppIcon.h"
#include <string>

namespace Dimension
{
    // A splash screen shown while an application loads (the editor uses it until its first frame with the scene is
    // ready): a small borderless window with the executable's icon, a title, a subtitle (the project), the current
    // loading step and a moving progress bar. It runs on a thread of its own, so it stays drawn and movable while the
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

        // The splash screen's picture as Linux shows it (560 x 300 times `scale`, the screen's DPI scale): the built-in
        // icon and font, `phase` (0..1) the moving bar's position. Any platform (tests use it).
        AppIcon::Image Render(float scale, const std::string& title, const std::string& subtitle,
                              const std::string& status, float phase);
    }
}
