#pragma once
#include "Dimension/ECS/Entity.h"
#include <glm/glm.hpp>
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
        };

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
