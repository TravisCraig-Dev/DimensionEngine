#include "Dimension/Project/InputActions.h"
#include "Dimension/Project/Project.h"
#include "Dimension/Core/Input.h"
#include "Dimension/Core/Log.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>
#include <unordered_map>

namespace Dimension::InputActions
{
    namespace
    {
        struct State
        {
            float Value = 0.0f, Previous = 0.0f;
        };
        std::unordered_map<std::string, State> s_States; // by lower-case name
        std::set<std::string> s_Warned;

        std::string Lower(std::string s)
        {
            for (auto& c : s) c = (char)std::tolower((unsigned char)c);
            return s;
        }

        const State* Find(const std::string& name)
        {
            auto it = s_States.find(Lower(name));
            if (it != s_States.end()) return &it->second;
            // Known but not sampled yet (no frame has run): not pressed.
            for (const InputAction& a : Current())
                if (Lower(a.Name) == Lower(name)) return nullptr;
            if (s_Warned.insert(Lower(name)).second) Log::Warn("No input action named '{}' (Project Settings > Input)", name);
            return nullptr;
        }

        InputBinding KeyB(Key k, float scale = 1.0f) { return { InputBinding::Kind::Key, (int)k, scale }; }
        InputBinding PadB(GamepadButton b) { return { InputBinding::Kind::GamepadButton, (int)b, 1.0f }; }
        InputBinding AxisB(GamepadAxis a, float scale = 1.0f) { return { InputBinding::Kind::GamepadAxis, (int)a, scale }; }
    }

    std::vector<InputAction> Defaults()
    {
        return {
            { "Horizontal", true, 0.15f, { KeyB(Key::D), KeyB(Key::A, -1.0f), KeyB(Key::Right), KeyB(Key::Left, -1.0f), AxisB(GamepadAxis::LeftX) } },
            // The stick's +Y is down: scaled by -1 so up is positive, as W.
            { "Vertical", true, 0.15f, { KeyB(Key::W), KeyB(Key::S, -1.0f), KeyB(Key::Up), KeyB(Key::Down, -1.0f), AxisB(GamepadAxis::LeftY, -1.0f) } },
            { "Jump", false, 0.15f, { KeyB(Key::Space), PadB(GamepadButton::A) } },
            { "Fire", false, 0.15f, { { InputBinding::Kind::MouseButton, (int)MouseButton::Left, 1.0f }, AxisB(GamepadAxis::RightTrigger) } },
            { "Submit", false, 0.15f, { KeyB(Key::Enter), PadB(GamepadButton::A) } },
            { "Cancel", false, 0.15f, { KeyB(Key::Escape), PadB(GamepadButton::B) } },
        };
    }

    const std::vector<InputAction>& Current()
    {
        static const std::vector<InputAction> defaults = Defaults();
        if (Project::IsOpen() && Project::GetConfig().InputActionsSet) return Project::GetConfig().InputActions;
        return defaults;
    }

    float Evaluate(const InputAction& action, const Source& source)
    {
        float sum = 0.0f;
        bool pressed = false;
        for (const InputBinding& b : action.Bindings)
        {
            float v = 0.0f;
            if (b.Type == InputBinding::Kind::GamepadAxis)
            {
                float raw = source.Axis ? source.Axis(b.Code) : 0.0f;
                float dz = std::clamp(action.DeadZone, 0.0f, 0.99f);
                float mag = std::abs(raw);
                v = mag <= dz ? 0.0f : std::copysign((mag - dz) / (1.0f - dz), raw) * b.Scale;
            }
            else if (source.Down && source.Down(b.Type, b.Code))
                v = b.Scale;
            sum += v;
            // A button: any control past half way in its direction.
            if (v >= 0.5f || (b.Type != InputBinding::Kind::GamepadAxis && v != 0.0f)) pressed = true;
        }
        if (!action.Axis) return pressed ? 1.0f : 0.0f;
        return std::clamp(sum, -1.0f, 1.0f);
    }

    void Update(const Source& source)
    {
        std::unordered_map<std::string, State> next;
        for (const InputAction& a : Current())
        {
            std::string key = Lower(a.Name);
            State s;
            if (auto it = s_States.find(key); it != s_States.end()) s.Previous = it->second.Value;
            s.Value = Evaluate(a, source);
            next[key] = s;
        }
        s_States = std::move(next);
    }

    void Update()
    {
        Source live;
        live.Down = [](InputBinding::Kind kind, int code) {
            switch (kind)
            {
            case InputBinding::Kind::Key: return Input::IsKeyDown((Key)code);
            case InputBinding::Kind::MouseButton: return Input::IsMouseDown((MouseButton)code);
            case InputBinding::Kind::GamepadButton: return Input::IsGamepadButtonDown((GamepadButton)code);
            default: return false;
            }
        };
        live.Axis = [](int code) { return Input::GetGamepadAxis((GamepadAxis)code); };
        Update(live);
    }

    bool GetButton(const std::string& name)
    {
        const State* s = Find(name);
        return s && s->Value != 0.0f;
    }
    bool GetButtonDown(const std::string& name)
    {
        const State* s = Find(name);
        return s && s->Value != 0.0f && s->Previous == 0.0f;
    }
    bool GetButtonUp(const std::string& name)
    {
        const State* s = Find(name);
        return s && s->Value == 0.0f && s->Previous != 0.0f;
    }
    float GetAxis(const std::string& name)
    {
        const State* s = Find(name);
        return s ? s->Value : 0.0f;
    }

    const std::vector<std::pair<int, const char*>>& Controls(InputBinding::Kind kind)
    {
        static const std::vector<std::pair<int, const char*>> keys = [] {
            std::vector<std::pair<int, const char*>> k;
            static const char* letters[] = { "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M",
                                             "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z" };
            for (int i = 0; i < 26; ++i) k.push_back({ (int)Key::A + i, letters[i] });
            static const char* digits[] = { "0", "1", "2", "3", "4", "5", "6", "7", "8", "9" };
            for (int i = 0; i < 10; ++i) k.push_back({ (int)Key::D0 + i, digits[i] });
            k.insert(k.end(), { { (int)Key::Space, "Space" }, { (int)Key::Enter, "Enter" }, { (int)Key::Escape, "Escape" }, { (int)Key::Tab, "Tab" },
                                { (int)Key::Backspace, "Backspace" }, { (int)Key::Up, "Up Arrow" }, { (int)Key::Down, "Down Arrow" },
                                { (int)Key::Left, "Left Arrow" }, { (int)Key::Right, "Right Arrow" }, { (int)Key::LeftShift, "Left Shift" },
                                { (int)Key::RightShift, "Right Shift" }, { (int)Key::LeftControl, "Left Ctrl" }, { (int)Key::RightControl, "Right Ctrl" },
                                { (int)Key::LeftAlt, "Left Alt" }, { (int)Key::RightAlt, "Right Alt" }, { (int)Key::Insert, "Insert" },
                                { (int)Key::Delete, "Delete" }, { (int)Key::Home, "Home" }, { (int)Key::End, "End" }, { (int)Key::PageUp, "Page Up" },
                                { (int)Key::PageDown, "Page Down" }, { (int)Key::Comma, "Comma" }, { (int)Key::Period, "Period" },
                                { (int)Key::Slash, "Slash" }, { (int)Key::Minus, "Minus" }, { (int)Key::Apostrophe, "Apostrophe" } });
            static const char* f[] = { "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12" };
            for (int i = 0; i < 12; ++i) k.push_back({ (int)Key::F1 + i, f[i] });
            return k;
        }();
        static const std::vector<std::pair<int, const char*>> mouse = { { 0, "Left Button" }, { 1, "Right Button" }, { 2, "Middle Button" } };
        static const std::vector<std::pair<int, const char*>> buttons = {
            { 0, "A" }, { 1, "B" }, { 2, "X" }, { 3, "Y" }, { 4, "Left Bumper" }, { 5, "Right Bumper" }, { 6, "Back" }, { 7, "Start" },
            { 8, "Guide" }, { 9, "Left Stick Press" }, { 10, "Right Stick Press" }, { 11, "D-pad Up" }, { 12, "D-pad Right" },
            { 13, "D-pad Down" }, { 14, "D-pad Left" } };
        static const std::vector<std::pair<int, const char*>> axes = { { 0, "Left Stick X" }, { 1, "Left Stick Y (down +)" }, { 2, "Right Stick X" },
                                                                       { 3, "Right Stick Y (down +)" }, { 4, "Left Trigger" }, { 5, "Right Trigger" } };
        switch (kind)
        {
        case InputBinding::Kind::MouseButton: return mouse;
        case InputBinding::Kind::GamepadButton: return buttons;
        case InputBinding::Kind::GamepadAxis: return axes;
        default: return keys;
        }
    }

    const char* ControlName(InputBinding::Kind kind, int code)
    {
        for (const auto& [c, name] : Controls(kind))
            if (c == code) return name;
        return "?";
    }
}
