#pragma once
#include <glm/glm.hpp>
#include <string>

struct GLFWwindow;

namespace Dimension
{
    // Values match GLFW key codes.
    enum class Key : int
    {
        Space = 32, Apostrophe = 39, Comma = 44, Minus = 45, Period = 46, Slash = 47,
        D0 = 48, D1, D2, D3, D4, D5, D6, D7, D8, D9,
        A = 65, B, C, D, E, F, G, H, I, J, K, L, M, N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
        Escape = 256, Enter, Tab, Backspace, Insert, Delete, Right, Left, Down, Up, PageUp, PageDown, Home, End,
        F1 = 290, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
        LeftShift = 340, LeftControl, LeftAlt, LeftSuper, RightShift, RightControl, RightAlt, RightSuper,
    };

    enum class MouseButton : int { Left = 0, Right = 1, Middle = 2 };

    // Gamepad buttons and axes in the standard (Xbox-style) layout; values match GLFW's gamepad mapping.
    enum class GamepadButton : int
    {
        A = 0, B, X, Y, LeftBumper, RightBumper, Back, Start, Guide, LeftThumb, RightThumb, DpadUp, DpadRight, DpadDown, DpadLeft
    };
    enum class GamepadAxis : int { LeftX = 0, LeftY, RightX, RightY, LeftTrigger, RightTrigger }; // sticks -1..1 (+Y down), triggers 0..1

    // The mouse cursor while the game has input: shown, hidden over the window, or hidden and held in place (mouse
    // look: GetMouseDelta keeps reporting movement).
    enum class CursorMode : int { Normal = 0, Hidden = 1, Locked = 2 };

    // Modifier bits (match GLFW).
    enum KeyModifier : int { ModShift = 1, ModControl = 2, ModAlt = 4, ModSuper = 8 };

    // Polled input, sampled once per frame. Used by gameplay scripts and the editor camera.
    class Input
    {
    public:
        static bool IsKeyDown(Key key);
        static bool IsKeyPressed(Key key); // went down this frame
        static bool IsKeyTyped(Key key);   // went down or auto-repeated this frame (text editing)
        // Modifiers held now or during any key press this frame (catches taps shorter than a frame).
        static int GetModifiers();
        static bool IsMouseDown(MouseButton button);
        static bool IsMousePressed(MouseButton button);
        static glm::vec2 GetMousePosition(); // window pixels, top-left origin
        static glm::vec2 GetMouseDelta();
        static float GetScrollDelta();
        // Gamepads (up to 4, any controller SDL's mapping database knows). Sticks have a 0.15 dead zone.
        static bool IsGamepadConnected(int pad = 0);
        static std::string GetGamepadName(int pad = 0);
        static bool IsGamepadButtonDown(GamepadButton button, int pad = 0);
        static bool IsGamepadButtonPressed(GamepadButton button, int pad = 0); // went down this frame
        static float GetGamepadAxis(GamepadAxis axis, int pad = 0);
        // Rumble (XInput controllers): low-frequency (left, heavy) and high-frequency (right, light) motor speeds 0..1.
        // A positive duration stops it after that many seconds; 0 keeps it until changed. Silent while game input is off.
        static void SetGamepadVibration(float lowFrequency, float highFrequency, float duration = 0.0f, int pad = 0);
        static void StopAllVibration();
        // UTF-8 text typed this frame (respects the game-input gate).
        static const std::string& GetTypedText();
        static std::string GetClipboardText();
        static void SetClipboardText(const std::string& text);

        // When disabled (e.g. editor not in play mode or game view unfocused) all queries return "nothing pressed".
        static void SetGameInputEnabled(bool enabled);
        static bool IsGameInputEnabled();
        // While a UI text field has focus, gameplay sees no keyboard (keys, typed text, modifiers); the UI reads it first.
        static void SetKeyboardCapturedByUI(bool captured);
        static bool IsKeyboardCapturedByUI();

        // The cursor the game asks for. It applies only while game input is enabled and the cursor is not released
        // (the editor releases it on Escape until the Game view is clicked again); otherwise the cursor is normal.
        static void SetCursorMode(CursorMode mode);
        static CursorMode GetCursorMode();
        static void SetCursorReleased(bool released);
        static bool IsCursorReleased();
        // The mode in effect now (applied to the window at NewFrame).
        static CursorMode GetEffectiveCursorMode();

        // Engine-internal
        static void Init(GLFWwindow* window);
        static void NewFrame();
        static void AddScroll(float dy);
        static void AddChar(unsigned int codepoint);
        static void AddKeyEvent(int key, int action, int mods);
        // Raw access that ignores the game-input gate (editor tools).
        static bool RawKeyDown(Key key);
        static bool RawMouseDown(MouseButton button);
        // Automated runs (the Player's --click): sets a mouse button's state for this frame (replacing the real one).
        static void SimulateMouseButton(MouseButton button, bool down);
        static void SimulateKey(Key key, bool down); // likewise for a key (tests)
    };
}
