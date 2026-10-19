#pragma once
#include "Dimension/ECS/Entity.h"
#include <glm/glm.hpp>
#include <functional>
#include <string>
#include <vector>

namespace Dimension
{
    class Scene;

    // Screen-space UI: Canvas -> UI Rect hierarchy -> Image / Text / Button / Slider / Toggle / Input Field,
    // with Layout Groups and clipped Scroll Views.
    namespace UI
    {
        struct PointerState
        {
            glm::vec2 Position{ 0.0f }; // render-target pixels, top-left origin
            bool Down = false, Pressed = false, Released = false;
            float Scroll = 0.0f;         // wheel notches this frame (positive = up)
            std::string Text;            // UTF-8 characters typed this frame
            bool Backspace = false, Enter = false;
            // Text editing (presses including key repeat), modifiers and clipboard.
            bool Delete = false, Left = false, Right = false, Home = false, End = false, Tab = false;
            bool Shift = false, Ctrl = false;
            bool SelectAll = false, Copy = false, Cut = false, Paste = false;
            std::string PasteText;                              // clipboard contents when Paste is set
            std::function<void(const std::string&)> SetClipboard; // receives copied/cut text
        };

        // Width in pixels of a single line of text at a pixel size. Defaults to the built-in font; tests can swap in
        // a fixed-width measure (fonts need the GPU).
        using TextMeasure = std::function<float(const std::string& text, float pixelSize)>;
        void SetTextMeasure(TextMeasure measure);
        float MeasureText(const std::string& text, float pixelSize);

        // A textured, tinted screen-space quad (pixels, top-left origin), already clipped. Colors are display-space (sRGB).
        struct DrawCommand
        {
            glm::vec2 Min, Max;
            glm::vec2 UVMin, UVMax;
            glm::vec4 Color;
            uint32_t Texture; // bindless index
        };

        // Resolves every UI Rect under a Canvas to screen pixels (layout groups, scroll offsets, clip rects).
        // Returns entities in draw order.
        std::vector<Entity> ComputeLayout(Scene& scene, const glm::vec2& screenSize);

        // Pointer/keyboard handling for all widgets; dispatches script events. Returns the topmost UI element under the pointer.
        Entity Update(Scene& scene, const PointerState& pointer, const glm::vec2& screenSize, float dt = 0.0f);

        void BuildDrawList(Scene& scene, const glm::vec2& screenSize, std::vector<DrawCommand>& out);

        // Number of UTF-8 codepoints in a string.
        size_t CodepointCount(const std::string& s);
    }
}
