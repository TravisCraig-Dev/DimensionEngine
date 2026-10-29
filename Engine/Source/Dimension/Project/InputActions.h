#pragma once
#include <functional>
#include <string>
#include <vector>

namespace Dimension
{
    // A control an input action listens to. A key, mouse button or gamepad button counts `Scale` while held (axes:
    // +1 for one way, -1 for the other); a gamepad axis (sticks, triggers) counts its value times `Scale`.
    struct InputBinding
    {
        enum class Kind : int { Key, MouseButton, GamepadButton, GamepadAxis };
        Kind Type = Kind::Key;
        int Code = 0; // Key / MouseButton / GamepadButton / GamepadAxis value
        float Scale = 1.0f;
        bool operator==(const InputBinding&) const = default;
    };

    // A named action (Project Settings > Input, like Unity's Input Manager): a button ("Jump": on or off) or an axis
    // ("Horizontal": -1..1, its bindings added up), read from scripts by name instead of by key.
    struct InputAction
    {
        std::string Name;
        bool Axis = false;
        float DeadZone = 0.15f; // gamepad axes: values below it count as 0 (the rest rescaled to 0..1)
        std::vector<InputBinding> Bindings;
        bool operator==(const InputAction&) const = default;
    };

    namespace InputActions
    {
        // Horizontal and Vertical (WASD, arrows, left stick), Jump (Space, A), Fire (left mouse, right trigger),
        // Submit (Enter, A) and Cancel (Escape, B): the actions of a project that defines none.
        std::vector<InputAction> Defaults();
        // The open project's actions (its own, else the defaults).
        const std::vector<InputAction>& Current();

        // Where values come from: whether a key / mouse button / gamepad button is held, and a gamepad axis's value.
        struct Source
        {
            std::function<bool(InputBinding::Kind, int)> Down;
            std::function<float(int)> Axis;
        };
        // An action's value: a button's 1 or 0, an axis's sum of its bindings (dead zone applied) clamped to -1..1.
        float Evaluate(const InputAction& action, const Source& source);
        // Once a frame (after the input is read): every action sampled, keeping the last frame's for Down and Up.
        void Update();                       // from the live input (Input::)
        void Update(const Source& source);   // from another source (tests, replays)

        // This frame's state of an action by name (case-insensitive); unknown names read as not pressed / 0 (with a
        // warning, once per name).
        bool GetButton(const std::string& name);
        bool GetButtonDown(const std::string& name); // pressed this frame
        bool GetButtonUp(const std::string& name);   // released this frame
        float GetAxis(const std::string& name);

        // Names for the Project Settings pickers (and the saved project file): keys, mouse and gamepad controls.
        const std::vector<std::pair<int, const char*>>& Controls(InputBinding::Kind kind);
        const char* ControlName(InputBinding::Kind kind, int code);
    }
}
