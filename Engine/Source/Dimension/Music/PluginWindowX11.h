#pragma once
// Top-level X11 windows for plug-in editors on Linux (Vst3Host.cpp embeds the plug-ins' views in them). Kept apart
// because Xlib's macros (None, Status, Bool...) clash with other headers. Linux only.
#include <cstdint>
#include <functional>
#include <string>

namespace Dimension::Music::X11
{
    struct WindowHandle;
    // False when there is no X display (a Wayland session without XWayland, or none at all).
    bool Available();
    WindowHandle* Create(const std::string& title, int width, int height, std::function<void()> onClose, std::function<void(int, int)> onResize);
    void Destroy(WindowHandle* w);
    void Show(WindowHandle* w);
    void SetClientSize(WindowHandle* w, int width, int height);
    uint64_t NativeId(WindowHandle* w); // the X window id (what plug-ins attach to)
    // Handles pending X events of these windows (close, resize).
    void Pump();
}
