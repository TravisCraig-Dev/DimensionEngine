#include "Dimension/Platform/GamepadHaptics.h"

#ifdef _WIN32
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Gaming.Input.h>
#include <algorithm>
#include <mutex>
#include <optional>
#include <vector>

namespace Dimension::GamepadHaptics
{
    namespace
    {
        namespace wgi = winrt::Windows::Gaming::Input;

        // Windows.Gaming.Input fills its gamepad list asynchronously in desktop apps (Gamepads() is empty until the
        // first GamepadAdded), so the list is kept from the events, which arrive on worker threads.
        struct State
        {
            std::mutex Mutex;
            std::vector<wgi::Gamepad> Pads;
            winrt::event_token Added{}, Removed{};
            bool Started = false, Failed = false;
        };
        State& S()
        {
            static State s;
            return s;
        }

        bool Start()
        {
            State& s = S();
            if (s.Started || s.Failed) return s.Started;
            try
            {
                // The main thread may already be in an apartment (C# scripting makes it MTA): keep whichever it is.
                try { winrt::init_apartment(winrt::apartment_type::multi_threaded); }
                catch (const winrt::hresult_error&) {}
                s.Added = wgi::Gamepad::GamepadAdded([](const auto&, const wgi::Gamepad& pad) {
                    std::lock_guard lock(S().Mutex);
                    for (const auto& p : S().Pads)
                        if (p == pad) return;
                    S().Pads.push_back(pad);
                });
                s.Removed = wgi::Gamepad::GamepadRemoved([](const auto&, const wgi::Gamepad& pad) {
                    std::lock_guard lock(S().Mutex);
                    std::erase(S().Pads, pad);
                });
                std::lock_guard lock(s.Mutex);
                for (const auto& pad : wgi::Gamepad::Gamepads())
                    if (std::find(s.Pads.begin(), s.Pads.end(), pad) == s.Pads.end()) s.Pads.push_back(pad);
                s.Started = true;
            }
            catch (...)
            {
                s.Failed = true; // no Windows.Gaming.Input (e.g. Windows Server without the media features)
            }
            return s.Started;
        }

        std::optional<wgi::Gamepad> At(int index)
        {
            if (index < 0 || !Start()) return std::nullopt;
            std::lock_guard lock(S().Mutex);
            if (index >= (int)S().Pads.size()) return std::nullopt;
            return S().Pads[index];
        }
    }

    bool Set(int index, float low, float high, float leftTrigger, float rightTrigger)
    {
        auto pad = At(index);
        if (!pad) return false;
        try
        {
            wgi::GamepadVibration v{};
            v.LeftMotor = low;
            v.RightMotor = high;
            v.LeftTrigger = leftTrigger;
            v.RightTrigger = rightTrigger;
            pad->Vibration(v);
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    int Count()
    {
        if (!Start()) return 0;
        std::lock_guard lock(S().Mutex);
        return (int)S().Pads.size();
    }

    void Shutdown()
    {
        State& s = S();
        if (!s.Started) return;
        try
        {
            wgi::Gamepad::GamepadAdded(s.Added);
            wgi::Gamepad::GamepadRemoved(s.Removed);
            std::lock_guard lock(s.Mutex);
            for (auto& pad : s.Pads) pad.Vibration(wgi::GamepadVibration{});
            s.Pads.clear();
        }
        catch (...) {}
        s.Started = false;
    }
}
#else
namespace Dimension::GamepadHaptics
{
    bool Set(int, float, float, float, float) { return false; }
    int Count() { return 0; }
    void Shutdown() {}
}
#endif
