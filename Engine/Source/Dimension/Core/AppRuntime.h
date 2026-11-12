#pragma once
#include <cstdint>
#include <functional>

namespace Dimension::AppRuntime
{
    // What runs the game (like Unity's Application.platform): the Player, the editor, or nothing (tests, tools).
    enum class Host : int { None = 0, Player = 1, Editor = 2 };
    void SetHost(Host host);
    Host GetHost();

    // A script asked the game to close (Unity's Application.Quit): the Player closes after this frame; the editor
    // ignores it, as Unity's does. The Application takes the request once a frame.
    void RequestQuit();
    bool TakeQuitRequest();

    // Frames per second the Application holds itself to (sleeping out the rest of each frame); -1 or 0: as fast as
    // it goes (or the display's rate with VSync).
    void SetTargetFrameRate(int framesPerSecond);
    int GetTargetFrameRate();
    // A host's own cap on top of the game's (the editor's background and idle frame rates); 0: none. The
    // Application holds the lower of the two.
    void SetFrameRateCap(int framesPerSecond);
    int GetFrameRateCap();

    // Real seconds since the program started.
    double GetRealtimeSinceStartup();

    // The game window's state, reported by the Application each frame; running scenes pass changes on to scripts
    // (OnApplicationFocus, and OnApplicationPause while minimized). Focused and not minimized until told otherwise.
    void ReportWindowState(bool focused, bool minimized);
    bool IsFocused();
    bool IsMinimized();
    // Called when the reported state changes (focus and minimized as they are now, and which of them changed), even on
    // frames the Application skips while minimized. Returns an id for RemoveWindowListener.
    using WindowListener = std::function<void(bool focused, bool focusChanged, bool minimized, bool minimizedChanged)>;
    int AddWindowListener(WindowListener listener);
    void RemoveWindowListener(int id);
}
