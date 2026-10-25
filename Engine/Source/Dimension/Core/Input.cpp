#include "Dimension/Core/Input.h"
#include <GLFW/glfw3.h>
#include <array>
#include <cmath>

namespace Dimension
{
    namespace
    {
        GLFWwindow* s_Window = nullptr;
        std::array<bool, GLFW_KEY_LAST + 1> s_Keys{}, s_PrevKeys{};
        std::array<bool, 8> s_Mouse{}, s_PrevMouse{};
        glm::vec2 s_MousePos{}, s_PrevMousePos{};
        float s_Scroll = 0.0f, s_ScrollAccum = 0.0f;
        bool s_GameInput = true;
        bool s_UICapture = false;
        bool KeyboardOpen() { return s_GameInput && !s_UICapture; }
        std::string s_TypedAccum, s_Typed;
        std::array<uint8_t, GLFW_KEY_LAST + 1> s_KeyTypedAccum{}, s_KeyTyped{}; // presses + repeats this frame
        int s_ModsAccum = 0, s_Mods = 0; // modifiers seen with key presses this frame
        const std::string s_Empty;

        constexpr int kMaxPads = 4;
        struct PadState
        {
            bool Connected = false;
            std::array<bool, 15> Buttons{}, PrevButtons{};
            std::array<float, 6> Axes{};
        };
        std::array<PadState, kMaxPads> s_Pads;
    }

    void Input::Init(GLFWwindow* window)
    {
        s_Window = window;
        if (window) glfwSetInputMode(window, GLFW_LOCK_KEY_MODS, GLFW_TRUE); // report Num Lock in key mods
    }

    void Input::NewFrame()
    {
        if (!s_Window) return;
        s_PrevKeys = s_Keys;
        s_PrevMouse = s_Mouse;
        for (int k = GLFW_KEY_SPACE; k <= GLFW_KEY_LAST; ++k) s_Keys[k] = glfwGetKey(s_Window, k) == GLFW_PRESS;
        for (int b = 0; b < 8; ++b) s_Mouse[b] = glfwGetMouseButton(s_Window, b) == GLFW_PRESS;
        double x, y;
        glfwGetCursorPos(s_Window, &x, &y);
        s_PrevMousePos = s_MousePos;
        s_MousePos = { (float)x, (float)y };
        s_Scroll = s_ScrollAccum;
        s_ScrollAccum = 0.0f;
        s_Typed = std::move(s_TypedAccum);
        s_TypedAccum.clear();
        s_KeyTyped = s_KeyTypedAccum;
        s_KeyTypedAccum.fill(0);
        s_Mods = s_ModsAccum;
        s_ModsAccum = 0;

        for (int p = 0; p < kMaxPads; ++p)
        {
            PadState& pad = s_Pads[p];
            pad.PrevButtons = pad.Buttons;
            GLFWgamepadstate state;
            pad.Connected = glfwJoystickIsGamepad(GLFW_JOYSTICK_1 + p) && glfwGetGamepadState(GLFW_JOYSTICK_1 + p, &state);
            if (!pad.Connected)
            {
                pad.Buttons.fill(false);
                pad.Axes.fill(0.0f);
                continue;
            }
            for (int b = 0; b < 15; ++b) pad.Buttons[b] = state.buttons[b] == GLFW_PRESS;
            for (int a = 0; a < 6; ++a)
            {
                float v = state.axes[a];
                if (a >= 4) v = v * 0.5f + 0.5f;          // triggers: -1..1 -> 0..1
                else if (std::abs(v) < 0.15f) v = 0.0f;   // stick dead zone
                pad.Axes[a] = v;
            }
        }
    }

    bool Input::IsGamepadConnected(int pad) { return pad >= 0 && pad < kMaxPads && s_Pads[pad].Connected; }
    std::string Input::GetGamepadName(int pad)
    {
        const char* name = IsGamepadConnected(pad) ? glfwGetGamepadName(GLFW_JOYSTICK_1 + pad) : nullptr;
        return name ? name : "";
    }
    bool Input::IsGamepadButtonDown(GamepadButton b, int pad)
    {
        return s_GameInput && IsGamepadConnected(pad) && s_Pads[pad].Buttons[(int)b];
    }
    bool Input::IsGamepadButtonPressed(GamepadButton b, int pad)
    {
        return s_GameInput && IsGamepadConnected(pad) && s_Pads[pad].Buttons[(int)b] && !s_Pads[pad].PrevButtons[(int)b];
    }
    float Input::GetGamepadAxis(GamepadAxis a, int pad) { return s_GameInput && IsGamepadConnected(pad) ? s_Pads[pad].Axes[(int)a] : 0.0f; }

    void Input::AddScroll(float dy) { s_ScrollAccum += dy; }

    void Input::AddKeyEvent(int key, int action, int mods)
    {
        if (action == GLFW_PRESS || action == GLFW_REPEAT) s_ModsAccum |= mods & (GLFW_MOD_SHIFT | GLFW_MOD_CONTROL | GLFW_MOD_ALT | GLFW_MOD_SUPER);
        // With Num Lock off the keypad navigates, as everywhere else on Windows.
        if (!(mods & GLFW_MOD_NUM_LOCK))
            switch (key)
            {
            case GLFW_KEY_KP_4: key = GLFW_KEY_LEFT; break;
            case GLFW_KEY_KP_6: key = GLFW_KEY_RIGHT; break;
            case GLFW_KEY_KP_8: key = GLFW_KEY_UP; break;
            case GLFW_KEY_KP_2: key = GLFW_KEY_DOWN; break;
            case GLFW_KEY_KP_7: key = GLFW_KEY_HOME; break;
            case GLFW_KEY_KP_1: key = GLFW_KEY_END; break;
            case GLFW_KEY_KP_DECIMAL: key = GLFW_KEY_DELETE; break;
            default: break;
            }
        if (key >= 0 && key <= GLFW_KEY_LAST && (action == GLFW_PRESS || action == GLFW_REPEAT) && s_KeyTypedAccum[key] < 255)
            ++s_KeyTypedAccum[key];
    }

    std::string Input::GetClipboardText()
    {
        const char* text = s_Window ? glfwGetClipboardString(s_Window) : nullptr;
        return text ? text : "";
    }

    void Input::SetClipboardText(const std::string& text)
    {
        if (s_Window) glfwSetClipboardString(s_Window, text.c_str());
    }

    void Input::AddChar(unsigned int cp)
    {
        // UTF-8 encode.
        if (cp < 0x80) s_TypedAccum += (char)cp;
        else if (cp < 0x800) { s_TypedAccum += (char)(0xC0 | (cp >> 6)); s_TypedAccum += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { s_TypedAccum += (char)(0xE0 | (cp >> 12)); s_TypedAccum += (char)(0x80 | ((cp >> 6) & 0x3F)); s_TypedAccum += (char)(0x80 | (cp & 0x3F)); }
        else { s_TypedAccum += (char)(0xF0 | (cp >> 18)); s_TypedAccum += (char)(0x80 | ((cp >> 12) & 0x3F)); s_TypedAccum += (char)(0x80 | ((cp >> 6) & 0x3F)); s_TypedAccum += (char)(0x80 | (cp & 0x3F)); }
    }

    const std::string& Input::GetTypedText() { return KeyboardOpen() ? s_Typed : s_Empty; }
    void Input::SetKeyboardCapturedByUI(bool captured) { s_UICapture = captured; }
    bool Input::IsKeyboardCapturedByUI() { return s_UICapture; }
    void Input::SetGameInputEnabled(bool enabled) { s_GameInput = enabled; }
    bool Input::IsGameInputEnabled() { return s_GameInput; }

    bool Input::RawKeyDown(Key key) { return s_Keys[(int)key]; }
    bool Input::RawMouseDown(MouseButton b) { return s_Mouse[(int)b]; }

    bool Input::IsKeyDown(Key key) { return KeyboardOpen() && s_Keys[(int)key]; }
    bool Input::IsKeyPressed(Key key) { return KeyboardOpen() && s_Keys[(int)key] && !s_PrevKeys[(int)key]; }
    int Input::GetModifiers()
    {
        if (!KeyboardOpen()) return 0;
        int held = (s_Keys[GLFW_KEY_LEFT_SHIFT] || s_Keys[GLFW_KEY_RIGHT_SHIFT] ? ModShift : 0) |
                   (s_Keys[GLFW_KEY_LEFT_CONTROL] || s_Keys[GLFW_KEY_RIGHT_CONTROL] ? ModControl : 0) |
                   (s_Keys[GLFW_KEY_LEFT_ALT] || s_Keys[GLFW_KEY_RIGHT_ALT] ? ModAlt : 0) |
                   (s_Keys[GLFW_KEY_LEFT_SUPER] || s_Keys[GLFW_KEY_RIGHT_SUPER] ? ModSuper : 0);
        return held | s_Mods;
    }

    bool Input::IsKeyTyped(Key key) { return KeyboardOpen() && (s_KeyTyped[(int)key] > 0 || (s_Keys[(int)key] && !s_PrevKeys[(int)key])); }
    bool Input::IsMouseDown(MouseButton b) { return s_GameInput && s_Mouse[(int)b]; }
    bool Input::IsMousePressed(MouseButton b) { return s_GameInput && s_Mouse[(int)b] && !s_PrevMouse[(int)b]; }
    glm::vec2 Input::GetMousePosition() { return s_MousePos; }
    glm::vec2 Input::GetMouseDelta() { return s_MousePos - s_PrevMousePos; }
    float Input::GetScrollDelta() { return s_Scroll; }
}
