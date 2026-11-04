#include "Dimension/Core/Input.h"
#include <GLFW/glfw3.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#ifdef _WIN32
#include <windows.h>
#include <Xinput.h>
#endif

namespace Dimension
{
    namespace
    {
        GLFWwindow* s_Window = nullptr;
        std::array<bool, GLFW_KEY_LAST + 1> s_Keys{}, s_PrevKeys{};
        std::array<bool, 8> s_Mouse{}, s_PrevMouse{};
        glm::vec2 s_MousePos{}, s_PrevMousePos{};
        float s_Scroll = 0.0f, s_ScrollAccum = 0.0f;
        float s_ScrollX = 0.0f, s_ScrollXAccum = 0.0f;
        bool s_GameInput = true;
        bool s_UICapture = false;
        CursorMode s_CursorRequested = CursorMode::Normal, s_CursorApplied = CursorMode::Normal;
        bool s_CursorReleased = false;
        bool KeyboardOpen() { return s_GameInput && !s_UICapture; }
        std::string s_TypedAccum, s_Typed;
        std::string s_InputStringAccum, s_InputString; // typed text with \b and \n
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

        struct RumbleState
        {
            float Low = 0.0f, High = 0.0f;
            double EndTime = 0.0;            // glfwGetTime() at which it stops (0 = until changed)
            float SentLow = -1.0f, SentHigh = -1.0f;
        };
        std::array<RumbleState, kMaxPads> s_Rumble;

#ifdef _WIN32
        // XInput is loaded at runtime (as GLFW does), so nothing extra is linked.
        struct XInputApi
        {
            DWORD (WINAPI* GetState)(DWORD, XINPUT_STATE*) = nullptr;
            DWORD (WINAPI* SetState)(DWORD, XINPUT_VIBRATION*) = nullptr;
        };
        const XInputApi& XInput()
        {
            static XInputApi api = [] {
                XInputApi a;
                HMODULE m = LoadLibraryA("xinput1_4.dll");
                if (!m) m = LoadLibraryA("xinput9_1_0.dll");
                if (m)
                {
                    a.GetState = reinterpret_cast<decltype(a.GetState)>(reinterpret_cast<void*>(GetProcAddress(m, "XInputGetState")));
                    a.SetState = reinterpret_cast<decltype(a.SetState)>(reinterpret_cast<void*>(GetProcAddress(m, "XInputSetState")));
                }
                return a;
            }();
            return api;
        }

        // GLFW tags XInput joysticks with a GUID starting "xinput" and lists them in user-index order, so pad p's
        // XInput slot is the n-th connected slot, n being p's rank among the XInput joysticks.
        int XInputSlot(int pad)
        {
            const XInputApi& x = XInput();
            if (!x.GetState) return -1;
            int rank = 0;
            for (int j = 0; j <= pad; ++j)
            {
                const char* guid = glfwGetJoystickGUID(GLFW_JOYSTICK_1 + j);
                bool isX = guid && std::strncmp(guid, "78696e707574", 12) == 0;
                if (j == pad && !isX) return -1;
                if (j < pad && isX) ++rank;
            }
            for (DWORD slot = 0; slot < XUSER_MAX_COUNT; ++slot)
            {
                XINPUT_STATE state{};
                if (x.GetState(slot, &state) == ERROR_SUCCESS && rank-- == 0) return (int)slot;
            }
            return -1;
        }

        void SendRumble(int pad, float low, float high)
        {
            int slot = XInputSlot(pad);
            if (slot < 0 || !XInput().SetState) return;
            XINPUT_VIBRATION v{ (WORD)std::lround(low * 65535.0f), (WORD)std::lround(high * 65535.0f) };
            XInput().SetState((DWORD)slot, &v);
        }
#else
        void SendRumble(int, float, float) {}
#endif

        void UpdateRumble()
        {
            double now = glfwGetTime();
            for (int p = 0; p < kMaxPads; ++p)
            {
                RumbleState& r = s_Rumble[p];
                if (r.EndTime > 0.0 && now >= r.EndTime) r = { 0.0f, 0.0f, 0.0, r.SentLow, r.SentHigh };
                if (!s_Pads[p].Connected)
                {
                    r.SentLow = r.SentHigh = -1.0f; // resend when it reconnects
                    continue;
                }
                float low = s_GameInput ? r.Low : 0.0f, high = s_GameInput ? r.High : 0.0f;
                if (low == r.SentLow && high == r.SentHigh) continue;
                SendRumble(p, low, high);
                r.SentLow = low;
                r.SentHigh = high;
            }
        }
    }

    void Input::SetGamepadVibration(float low, float high, float duration, int pad)
    {
        if (pad < 0 || pad >= kMaxPads) return;
        RumbleState& r = s_Rumble[pad];
        r.Low = std::clamp(low, 0.0f, 1.0f);
        r.High = std::clamp(high, 0.0f, 1.0f);
        r.EndTime = duration > 0.0f && (r.Low > 0.0f || r.High > 0.0f) ? glfwGetTime() + duration : 0.0;
        if (s_Window) UpdateRumble(); // start now rather than next frame
    }

    void Input::StopAllVibration()
    {
        for (int p = 0; p < kMaxPads; ++p)
        {
            s_Rumble[p].Low = s_Rumble[p].High = 0.0f;
            s_Rumble[p].EndTime = 0.0;
        }
        if (s_Window) UpdateRumble();
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

        // The cursor mode first: locking moves the cursor, which must not count as mouse movement.
        CursorMode cursor = GetEffectiveCursorMode();
        bool cursorChanged = cursor != s_CursorApplied;
        if (cursorChanged)
        {
            glfwSetInputMode(s_Window, GLFW_CURSOR, cursor == CursorMode::Locked ? GLFW_CURSOR_DISABLED
                                                    : cursor == CursorMode::Hidden ? GLFW_CURSOR_HIDDEN : GLFW_CURSOR_NORMAL);
            if (glfwRawMouseMotionSupported())
                glfwSetInputMode(s_Window, GLFW_RAW_MOUSE_MOTION, cursor == CursorMode::Locked ? GLFW_TRUE : GLFW_FALSE);
            s_CursorApplied = cursor;
        }
        double x, y;
        glfwGetCursorPos(s_Window, &x, &y);
        s_PrevMousePos = s_MousePos;
        s_MousePos = { (float)x, (float)y };
        if (cursorChanged) s_PrevMousePos = s_MousePos;
        s_Scroll = s_ScrollAccum;
        s_ScrollAccum = 0.0f;
        s_ScrollX = s_ScrollXAccum;
        s_ScrollXAccum = 0.0f;
        s_Typed = std::move(s_TypedAccum);
        s_TypedAccum.clear();
        s_InputString = std::move(s_InputStringAccum);
        s_InputStringAccum.clear();
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
        UpdateRumble();
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
    bool Input::IsGamepadButtonReleased(GamepadButton b, int pad)
    {
        return s_GameInput && IsGamepadConnected(pad) && !s_Pads[pad].Buttons[(int)b] && s_Pads[pad].PrevButtons[(int)b];
    }
    float Input::GetGamepadAxis(GamepadAxis a, int pad) { return s_GameInput && IsGamepadConnected(pad) ? s_Pads[pad].Axes[(int)a] : 0.0f; }

    void Input::AddScroll(float dy, float dx)
    {
        s_ScrollAccum += dy;
        s_ScrollXAccum += dx;
    }

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
        if (action == GLFW_PRESS || action == GLFW_REPEAT)
        {
            if (key == GLFW_KEY_BACKSPACE) s_InputStringAccum += '\b';
            else if (key == GLFW_KEY_ENTER || key == GLFW_KEY_KP_ENTER) s_InputStringAccum += '\n';
        }
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
        size_t from = s_TypedAccum.size();
        if (cp < 0x80) s_TypedAccum += (char)cp;
        else if (cp < 0x800) { s_TypedAccum += (char)(0xC0 | (cp >> 6)); s_TypedAccum += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { s_TypedAccum += (char)(0xE0 | (cp >> 12)); s_TypedAccum += (char)(0x80 | ((cp >> 6) & 0x3F)); s_TypedAccum += (char)(0x80 | (cp & 0x3F)); }
        else { s_TypedAccum += (char)(0xF0 | (cp >> 18)); s_TypedAccum += (char)(0x80 | ((cp >> 12) & 0x3F)); s_TypedAccum += (char)(0x80 | ((cp >> 6) & 0x3F)); s_TypedAccum += (char)(0x80 | (cp & 0x3F)); }
        s_InputStringAccum.append(s_TypedAccum, from, std::string::npos);
    }

    const std::string& Input::GetTypedText() { return KeyboardOpen() ? s_Typed : s_Empty; }
    const std::string& Input::GetInputString() { return KeyboardOpen() ? s_InputString : s_Empty; }
    void Input::SetKeyboardCapturedByUI(bool captured) { s_UICapture = captured; }
    bool Input::IsKeyboardCapturedByUI() { return s_UICapture; }
    void Input::SetGameInputEnabled(bool enabled)
    {
        if (s_GameInput == enabled) return;
        s_GameInput = enabled;
        if (s_Window) UpdateRumble(); // pause or resume rumble with the game
    }
    bool Input::IsGameInputEnabled() { return s_GameInput; }

    void Input::SetCursorMode(CursorMode mode) { s_CursorRequested = mode; }
    CursorMode Input::GetCursorMode() { return s_CursorRequested; }
    void Input::SetCursorReleased(bool released) { s_CursorReleased = released; }
    bool Input::IsCursorReleased() { return s_CursorReleased; }
    CursorMode Input::GetEffectiveCursorMode() { return s_GameInput && !s_CursorReleased ? s_CursorRequested : CursorMode::Normal; }

    bool Input::RawKeyDown(Key key) { return s_Keys[(int)key]; }
    bool Input::RawMouseDown(MouseButton b) { return s_Mouse[(int)b]; }

    bool Input::IsKeyDown(Key key) { return KeyboardOpen() && s_Keys[(int)key]; }
    bool Input::IsKeyPressed(Key key) { return KeyboardOpen() && s_Keys[(int)key] && !s_PrevKeys[(int)key]; }
    bool Input::IsKeyReleased(Key key) { return KeyboardOpen() && !s_Keys[(int)key] && s_PrevKeys[(int)key]; }
    bool Input::IsAnyKeyDown()
    {
        if (!s_GameInput) return false;
        if (KeyboardOpen() && std::any_of(s_Keys.begin(), s_Keys.end(), [](bool k) { return k; })) return true;
        if (std::any_of(s_Mouse.begin(), s_Mouse.end(), [](bool m) { return m; })) return true;
        for (const PadState& p : s_Pads)
            if (p.Connected && std::any_of(p.Buttons.begin(), p.Buttons.end(), [](bool b) { return b; })) return true;
        return false;
    }
    bool Input::IsAnyKeyPressed()
    {
        if (!s_GameInput) return false;
        if (KeyboardOpen())
            for (size_t k = 0; k < s_Keys.size(); ++k)
                if (s_Keys[k] && !s_PrevKeys[k]) return true;
        for (size_t b = 0; b < s_Mouse.size(); ++b)
            if (s_Mouse[b] && !s_PrevMouse[b]) return true;
        for (const PadState& p : s_Pads)
            for (size_t b = 0; p.Connected && b < p.Buttons.size(); ++b)
                if (p.Buttons[b] && !p.PrevButtons[b]) return true;
        return false;
    }
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
    bool Input::IsMouseReleased(MouseButton b) { return s_GameInput && !s_Mouse[(int)b] && s_PrevMouse[(int)b]; }
    void Input::SimulateMouseButton(MouseButton b, bool down) { s_Mouse[(int)b] = down; }
    void Input::SimulateKey(Key key, bool down) { s_Keys[(int)key] = down; }
    glm::vec2 Input::GetMousePosition() { return s_MousePos; }
    glm::vec2 Input::GetMouseDelta() { return s_MousePos - s_PrevMousePos; }
    float Input::GetScrollDelta() { return s_Scroll; }
    glm::vec2 Input::GetScroll2D() { return { s_ScrollX, s_Scroll }; }
}
