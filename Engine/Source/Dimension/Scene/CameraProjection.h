#pragma once
#include "Dimension/Scene/Scene.h"
#include <glm/glm.hpp>

namespace Dimension
{
    // Converting between a camera's view and the world (like Unity's Camera.WorldToScreenPoint, ScreenToWorldPoint and
    // ScreenPointToRay). Screen points are in pixels of the view the scene is shown in (Scene::GetViewSize), with the
    // origin at the top-left like the mouse position; the third coordinate is the distance in front of the camera
    // (along its view direction) in world units.
    namespace CameraProjection
    {
        // The world-to-camera matrix (the camera's transform without scale, inverted), as the renderer uses.
        glm::mat4 View(const Scene& scene, Entity camera);
        glm::mat4 Projection(const Scene& scene, Entity camera, const glm::vec2& viewSize);

        // z < 0 for points behind the camera.
        glm::vec3 WorldToScreen(const Scene& scene, Entity camera, const glm::vec3& world, const glm::vec2& viewSize);
        glm::vec3 ScreenToWorld(const Scene& scene, Entity camera, const glm::vec3& screen, const glm::vec2& viewSize);
        // The ray from the camera's near plane through a screen point (direction normalized).
        void ScreenPointToRay(const Scene& scene, Entity camera, const glm::vec2& screen, const glm::vec2& viewSize,
                              glm::vec3& origin, glm::vec3& direction);
    }
}
