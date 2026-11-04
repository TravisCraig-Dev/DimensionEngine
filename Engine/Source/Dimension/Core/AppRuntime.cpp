#include "Dimension/Core/AppRuntime.h"
#include <atomic>
#include <chrono>
#include <map>

namespace Dimension::AppRuntime
{
    namespace
    {
        Host s_Host = Host::None;
        std::atomic<bool> s_QuitRequested{ false };
        int s_TargetFrameRate = -1;
        bool s_Focused = true, s_Minimized = false;
        const auto s_Start = std::chrono::steady_clock::now();
        std::map<int, WindowListener> s_Listeners;
        int s_NextListener = 1;
    }

    void SetHost(Host host) { s_Host = host; }
    Host GetHost() { return s_Host; }

    void RequestQuit() { s_QuitRequested = true; }
    bool TakeQuitRequest() { return s_QuitRequested.exchange(false); }

    void SetTargetFrameRate(int framesPerSecond) { s_TargetFrameRate = framesPerSecond > 0 ? framesPerSecond : -1; }
    int GetTargetFrameRate() { return s_TargetFrameRate; }

    double GetRealtimeSinceStartup() { return std::chrono::duration<double>(std::chrono::steady_clock::now() - s_Start).count(); }

    void ReportWindowState(bool focused, bool minimized)
    {
        bool focusChanged = focused != s_Focused, minimizedChanged = minimized != s_Minimized;
        s_Focused = focused;
        s_Minimized = minimized;
        if (!focusChanged && !minimizedChanged) return;
        auto listeners = s_Listeners; // copy: a listener may remove itself
        for (auto& [id, listener] : listeners) listener(focused, focusChanged, minimized, minimizedChanged);
    }
    int AddWindowListener(WindowListener listener)
    {
        int id = s_NextListener++;
        s_Listeners[id] = std::move(listener);
        return id;
    }
    void RemoveWindowListener(int id) { s_Listeners.erase(id); }
    bool IsFocused() { return s_Focused; }
    bool IsMinimized() { return s_Minimized; }
}
