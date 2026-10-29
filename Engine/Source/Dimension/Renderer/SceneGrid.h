#pragma once
#include <glm/glm.hpp>
#include <vector>

namespace Dimension
{
    // The Scene view's grid: which plane it lies in (the ground, or a wall facing Z or X), its cells and how strong it is.
    enum class GridPlane : int { XZ, XY, YZ };
    struct GridSettings
    {
        GridPlane Plane = GridPlane::XZ;
        float CellSize = 0.0f; // 0: adapts to the view (powers of ten, finer up close)
        float Opacity = 1.0f;
        float Offset = 0.0f;   // along the plane's normal (Y for XZ, Z for XY, X for YZ), e.g. the selection's height
    };

    struct GridLine
    {
        glm::vec3 A, B;
        glm::vec4 Color;
    };

    namespace SceneGrid
    {
        // The world axis across the plane (1 for XZ, 2 for XY, 0 for YZ) and the two along it.
        int NormalAxis(GridPlane plane);
        void PlaneAxes(GridPlane plane, int& a, int& b);
        // The cell size used: CellSize, or with 0 one adapting to how far the camera is from the plane (orthographic
        // views: to `orthoHalfHeight`, half the view's height in world units).
        float Spacing(const GridSettings& settings, const glm::vec3& cameraPosition, bool orthographic, float orthoHalfHeight);
        // The lines around the camera (60 cells each way): every 10th brighter, those through the origin in the color of
        // the axis they run along (X red, Y green, Z blue); alpha scaled by Opacity.
        std::vector<GridLine> Build(const GridSettings& settings, const glm::vec3& cameraPosition, bool orthographic, float orthoHalfHeight);
    }
}
