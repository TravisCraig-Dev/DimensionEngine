#include "Dimension/Core/Input.h"
#include <GLFW/glfw3.h>
#include <array>

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
        std::string s_TypedAccum, s_Typed;
        const std::string s_Empty;
    }

    void Input::Init(GLFWwindow* window) { s_Window = window; }

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
    }

    void Input::AddScroll(float dy) { s_ScrollAccum += dy; }

    void Input::AddChar(unsigned int cp)
    {
        // UTF-8 encode.
        if (cp < 0x80) s_TypedAccum += (char)cp;
        else if (cp < 0x800) { s_TypedAccum += (char)(0xC0 | (cp >> 6)); s_TypedAccum += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { s_TypedAccum += (char)(0xE0 | (cp >> 12)); s_TypedAccum += (char)(0x80 | ((cp >> 6) & 0x3F)); s_TypedAccum += (char)(0x80 | (cp & 0x3F)); }
        else { s_TypedAccum += (char)(0xF0 | (cp >> 18)); s_TypedAccum += (char)(0x80 | ((cp >> 12) & 0x3F)); s_TypedAccum += (char)(0x80 | ((cp >> 6) & 0x3F)); s_TypedAccum += (char)(0x80 | (cp & 0x3F)); }
    }

    const std::string& Input::GetTypedText() { return s_GameInput ? s_Typed : s_Empty; }
    void Input::SetGameInputEnabled(bool enabled) { s_GameInput = enabled; }
    bool Input::IsGameInputEnabled() { return s_GameInput; }

    bool Input::RawKeyDown(Key key) { return s_Keys[(int)key]; }
    bool Input::RawMouseDown(MouseButton b) { return s_Mouse[(int)b]; }

    bool Input::IsKeyDown(Key key) { return s_GameInput && s_Keys[(int)key]; }
    bool Input::IsKeyPressed(Key key) { return s_GameInput && s_Keys[(int)key] && !s_PrevKeys[(int)key]; }
    bool Input::IsMouseDown(MouseButton b) { return s_GameInput && s_Mouse[(int)b]; }
    bool Input::IsMousePressed(MouseButton b) { return s_GameInput && s_Mouse[(int)b] && !s_PrevMouse[(int)b]; }
    glm::vec2 Input::GetMousePosition() { return s_MousePos; }
    glm::vec2 Input::GetMouseDelta() { return s_MousePos - s_PrevMousePos; }
    float Input::GetScrollDelta() { return s_Scroll; }
}
