#pragma once
#include <glm/glm.hpp>
#include <vector>

namespace Dimension
{
    // Lines drawn by game code while playing (like Unity's Debug.DrawLine / Debug.DrawRay; C#'s Debug.DrawLine): shown
    // in the editor's Scene and Game views with the gizmos, not in builds. A line lasts until the next game update, or
    // for `duration` seconds of game time; paused, the last ones stay. Stopping play clears them.
    namespace DebugDraw
    {
        struct DebugLine
        {
            glm::vec3 A, B;
            glm::vec4 Color;
            float Remaining = 0.0f; // seconds of game time left (0: this update only)
        };
        void Line(const glm::vec3& a, const glm::vec3& b, const glm::vec4& color = glm::vec4(1.0f), float duration = 0.0f);
        // From `origin` along `direction` (its length is the ray's).
        void Ray(const glm::vec3& origin, const glm::vec3& direction, const glm::vec4& color = glm::vec4(1.0f), float duration = 0.0f);
        // A game update begins (Scene::OnUpdateRuntime): the lines of the last one go, timed ones count `dt` down.
        void Tick(float dt);
        void Clear();
        const std::vector<DebugDraw::DebugLine>& Lines();
    }
}
