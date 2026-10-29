#pragma once
#include "Dimension/Scene/Scene.h"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <vector>

namespace Dimension
{
    // Lining objects up with the Scene view (like Unity's GameObject > Align With View, Align View to Selected and
    // Move To View).
    namespace ViewAlign
    {
        // Puts an object at a world position and rotation, whatever its parent (its world scale kept).
        void SetWorldPose(Scene& scene, Entity e, const glm::vec3& position, const glm::quat& rotation);
        // Each object takes the camera's place and facing (`cameraWorld`: the inverse of the view matrix); a Camera
        // object then sees what the view sees.
        void AlignWithView(Scene& scene, const std::vector<Entity>& objects, const glm::mat4& cameraWorld);
        // The objects moved together (keeping their spacing) so the center of their positions lands on `point`.
        void MoveTo(Scene& scene, const std::vector<Entity>& objects, const glm::vec3& point);
        // The view from an object: at its position looking along its forward (-Z), its up kept unless it looks
        // straight up or down (Align View to Selected).
        glm::mat4 ViewFrom(Scene& scene, Entity e);
    }
}
