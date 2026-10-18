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

    // Polled input, sampled once per frame. Used by gameplay scripts and the editor camera.
    class Input
    {
    public:
        static bool IsKeyDown(Key key);
        static bool IsKeyPressed(Key key); // went down this frame
        static bool IsMouseDown(MouseButton button);
        static bool IsMousePressed(MouseButton button);
        static glm::vec2 GetMousePosition(); // window pixels, top-left origin
        static glm::vec2 GetMouseDelta();
        static float GetScrollDelta();
        // UTF-8 text typed this frame (respects the game-input gate).
        static const std::string& GetTypedText();

        // When disabled (e.g. editor not in play mode or game view unfocused) all queries return "nothing pressed".
        static void SetGameInputEnabled(bool enabled);
        static bool IsGameInputEnabled();

        // Engine-internal
        static void Init(GLFWwindow* window);
        static void NewFrame();
        static void AddScroll(float dy);
        static void AddChar(unsigned int codepoint);
        // Raw access that ignores the game-input gate (editor tools).
        static bool RawKeyDown(Key key);
        static bool RawMouseDown(MouseButton button);
    };
}
