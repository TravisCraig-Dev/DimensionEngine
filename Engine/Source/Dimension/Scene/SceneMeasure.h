#pragma once
#include "Dimension/Scene/Scene.h"
#include <glm/glm.hpp>
#include <string>

namespace Dimension
{
    // The Scene view's measuring tool: the points a click picks, and how lengths read.
    namespace SceneMeasure
    {
        enum class PickKind { None, Vertex, Surface, Ground };
        struct Pick
        {
            PickKind Kind = PickKind::None;
            glm::vec3 Point{ 0.0f };
            Entity Object = Entity::Null();
            bool Hit() const { return Kind != PickKind::None; }
        };
        // The point under the mouse: a mesh vertex within `vertexPixels` of it (0: no vertex snapping), else the surface
        // the ray meets (meshes and terrains), else where it crosses the ground plane (y = 0). `viewProj`, `viewport` and
        // `screen` are as SceneSnapping::NearestVertex takes them; the ray is the mouse's, in world space.
        Pick PickPoint(Scene& scene, const glm::mat4& viewProj, const glm::vec2& viewport, const glm::vec2& screen, const glm::vec3& rayOrigin,
                       const glm::vec3& rayDirection, float vertexPixels = 12.0f);
        // "4.2 mm", "35.0 cm", "1.25 m", "12.5 m", "1.23 km" (meters in, sign kept).
        std::string FormatLength(float meters);
    }
}
