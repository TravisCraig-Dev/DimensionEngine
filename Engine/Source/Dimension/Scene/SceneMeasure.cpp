#include "Dimension/Scene/SceneMeasure.h"
#include "Dimension/Scene/SceneSnapping.h"
#include <cmath>
#include <cstdio>

namespace Dimension::SceneMeasure
{
    Pick PickPoint(Scene& scene, const glm::mat4& viewProj, const glm::vec2& viewport, const glm::vec2& screen, const glm::vec3& rayOrigin,
                   const glm::vec3& rayDirection, float vertexPixels)
    {
        Pick out;
        // Over an object, only its own vertices (not ones hidden behind it); off every object, any vertex near the mouse
        // (silhouette corners).
        auto surface = SceneSnapping::Raycast(scene, rayOrigin, rayDirection);
        if (vertexPixels > 0.0f)
        {
            std::vector<Entity> only;
            if (surface.Hit && !surface.Object.IsNull()) only.push_back(surface.Object);
            if (auto v = SceneSnapping::NearestVertex(scene, viewProj, viewport, screen, vertexPixels, only); v.Hit)
                return { PickKind::Vertex, v.Position, v.Object };
        }
        if (surface.Hit) return { PickKind::Surface, surface.Point, surface.Object };
        if (std::abs(rayDirection.y) > 1e-6f)
        {
            float t = -rayOrigin.y / rayDirection.y;
            if (t > 0.0f) return { PickKind::Ground, rayOrigin + rayDirection * t, Entity::Null() };
        }
        return out;
    }

    std::string FormatLength(float meters)
    {
        float a = std::abs(meters);
        char buf[32];
        if (a < 0.01f) std::snprintf(buf, sizeof(buf), "%.1f mm", meters * 1000.0f);
        else if (a < 1.0f) std::snprintf(buf, sizeof(buf), "%.1f cm", meters * 100.0f);
        else if (a < 10.0f) std::snprintf(buf, sizeof(buf), "%.2f m", meters);
        else if (a < 1000.0f) std::snprintf(buf, sizeof(buf), "%.1f m", meters);
        else std::snprintf(buf, sizeof(buf), "%.2f km", meters / 1000.0f);
        return buf;
    }
}
