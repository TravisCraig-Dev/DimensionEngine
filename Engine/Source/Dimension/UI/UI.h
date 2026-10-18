#pragma once
#include "Dimension/ECS/Entity.h"
#include <glm/glm.hpp>
#include <vector>

namespace Dimension
{
    class Scene;

    // Screen-space UI: Canvas -> UI Rect hierarchy -> Image / Text / Button.
    namespace UI
    {
        struct PointerState
        {
            glm::vec2 Position{ 0.0f }; // render-target pixels, top-left origin
            bool Down = false, Pressed = false, Released = false;
        };

        // A textured, tinted screen-space quad (pixels, top-left origin). Colors are display-space (sRGB).
        struct DrawCommand
        {
            glm::vec2 Min, Max;
            glm::vec2 UVMin, UVMax;
            glm::vec4 Color;
            uint32_t Texture; // bindless index
        };

        // Resolves every UI Rect under a Canvas to screen pixels. Returns entities in draw order.
        std::vector<Entity> ComputeLayout(Scene& scene, const glm::vec2& screenSize);

        // Hover/press/click handling for buttons. Returns the topmost UI element under the pointer.
        Entity Update(Scene& scene, const PointerState& pointer, const glm::vec2& screenSize);

        void BuildDrawList(Scene& scene, const glm::vec2& screenSize, std::vector<DrawCommand>& out);
    }
}
