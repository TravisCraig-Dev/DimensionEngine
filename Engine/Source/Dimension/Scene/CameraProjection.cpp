#include "Dimension/Scene/CameraProjection.h"
#include <algorithm>
#include <cmath>

namespace Dimension
{
    namespace CameraProjection
    {
        namespace
        {
            glm::vec3 Forward(const Scene& scene, Entity camera)
            {
                return glm::normalize(-glm::vec3(scene.GetRegistry().Get<TransformComponent>(camera).World[2])); // cameras look down -Z
            }
            glm::vec3 Position(const Scene& scene, Entity camera)
            {
                return glm::vec3(scene.GetRegistry().Get<TransformComponent>(camera).World[3]);
            }
            // Screen pixels (top-left origin) to normalized device coordinates; scene views draw with a negative-height
            // viewport, so ndc y = +1 is the top row.
            glm::vec2 ToNdc(const glm::vec2& screen, const glm::vec2& viewSize)
            {
                return { screen.x / viewSize.x * 2.0f - 1.0f, 1.0f - screen.y / viewSize.y * 2.0f };
            }
        }

        glm::mat4 View(const Scene& scene, Entity camera)
        {
            glm::mat4 m = scene.GetRegistry().Get<TransformComponent>(camera).World;
            for (int i = 0; i < 3; ++i) m[i] = glm::vec4(glm::normalize(glm::vec3(m[i])), 0.0f); // strip scale
            return glm::inverse(m);
        }

        glm::mat4 Projection(const Scene& scene, Entity camera, const glm::vec2& viewSize)
        {
            return scene.GetRegistry().Get<CameraComponent>(camera).GetProjection(viewSize.x / std::max(viewSize.y, 1.0f));
        }

        glm::vec3 WorldToScreen(const Scene& scene, Entity camera, const glm::vec3& world, const glm::vec2& viewSize)
        {
            glm::vec4 clip = Projection(scene, camera, viewSize) * View(scene, camera) * glm::vec4(world, 1.0f);
            float depth = glm::dot(world - Position(scene, camera), Forward(scene, camera));
            glm::vec2 ndc = std::abs(clip.w) > 1e-8f ? glm::vec2(clip) / clip.w : glm::vec2(0.0f);
            return { (ndc.x + 1.0f) * 0.5f * viewSize.x, (1.0f - ndc.y) * 0.5f * viewSize.y, depth };
        }

        void ScreenPointToRay(const Scene& scene, Entity camera, const glm::vec2& screen, const glm::vec2& viewSize,
                              glm::vec3& origin, glm::vec3& direction)
        {
            glm::mat4 inv = glm::inverse(Projection(scene, camera, viewSize) * View(scene, camera));
            glm::vec2 ndc = ToNdc(screen, viewSize);
            glm::vec4 nearPoint = inv * glm::vec4(ndc, 0.0f, 1.0f), farPoint = inv * glm::vec4(ndc, 1.0f, 1.0f);
            origin = glm::vec3(nearPoint) / nearPoint.w;
            direction = glm::normalize(glm::vec3(farPoint) / farPoint.w - origin);
        }

        glm::vec3 ScreenToWorld(const Scene& scene, Entity camera, const glm::vec3& screen, const glm::vec2& viewSize)
        {
            glm::vec3 origin, direction;
            ScreenPointToRay(scene, camera, glm::vec2(screen), viewSize, origin, direction);
            // Along the ray to the requested distance in front of the camera.
            glm::vec3 forward = Forward(scene, camera), eye = Position(scene, camera);
            float along = glm::dot(direction, forward);
            float t = std::abs(along) > 1e-8f ? (screen.z - glm::dot(origin - eye, forward)) / along : 0.0f;
            return origin + direction * t;
        }
    }
}
