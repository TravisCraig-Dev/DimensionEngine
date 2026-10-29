#include "Dimension/Renderer/SceneGrid.h"
#include <algorithm>
#include <cmath>

namespace Dimension::SceneGrid
{
    int NormalAxis(GridPlane plane) { return plane == GridPlane::XZ ? 1 : plane == GridPlane::XY ? 2 : 0; }

    void PlaneAxes(GridPlane plane, int& a, int& b)
    {
        switch (plane)
        {
        case GridPlane::XY: a = 0, b = 1; break;
        case GridPlane::YZ: a = 2, b = 1; break;
        default: a = 0, b = 2; break;
        }
    }

    float Spacing(const GridSettings& settings, const glm::vec3& cameraPosition, bool orthographic, float orthoHalfHeight)
    {
        if (settings.CellSize > 0.0f) return settings.CellSize;
        float height = orthographic ? orthoHalfHeight : std::abs(cameraPosition[NormalAxis(settings.Plane)] - settings.Offset);
        return std::max(1.0f, std::pow(10.0f, std::floor(std::log10(std::max(height, 1.0f) * 0.5f))));
    }

    std::vector<GridLine> Build(const GridSettings& settings, const glm::vec3& cameraPosition, bool orthographic, float orthoHalfHeight)
    {
        std::vector<GridLine> out;
        float opacity = std::clamp(settings.Opacity, 0.0f, 1.0f);
        if (opacity <= 0.0f) return out;
        float spacing = Spacing(settings, cameraPosition, orthographic, orthoHalfHeight);
        int a = 0, b = 2, n = NormalAxis(settings.Plane);
        PlaneAxes(settings.Plane, a, b);
        const int half = 60;
        // Lines on whole cells, around the point under the camera.
        float ca = std::floor(cameraPosition[a] / spacing), cb = std::floor(cameraPosition[b] / spacing);
        const glm::vec4 minor{ 0.5f, 0.5f, 0.5f, 0.25f }, major{ 0.6f, 0.6f, 0.6f, 0.5f };
        const glm::vec4 axisColor[3] = { { 0.9f, 0.3f, 0.3f, 0.8f }, { 0.3f, 0.85f, 0.3f, 0.8f }, { 0.3f, 0.45f, 0.9f, 0.8f } };
        auto point = [&](float u, float v) {
            glm::vec3 p(0.0f);
            p[a] = u;
            p[b] = v;
            p[n] = settings.Offset;
            return p;
        };
        auto colorOf = [&](double cell, int along) {
            glm::vec4 c = cell == 0.0 ? axisColor[along] : std::fmod(std::abs(cell), 10.0) == 0.0 ? major : minor;
            c.a *= opacity;
            return c;
        };
        float lo = -half * spacing, hi = half * spacing;
        for (int i = -half; i <= half; ++i)
        {
            double cellA = ca + i, cellB = cb + i;
            float u = (float)(cellA * spacing), v = (float)(cellB * spacing);
            // At u along a: the line runs along b (and the other way round).
            out.push_back({ point(u, (float)(cb * spacing) + lo), point(u, (float)(cb * spacing) + hi), colorOf(cellA, b) });
            out.push_back({ point((float)(ca * spacing) + lo, v), point((float)(ca * spacing) + hi, v), colorOf(cellB, a) });
        }
        return out;
    }
}
