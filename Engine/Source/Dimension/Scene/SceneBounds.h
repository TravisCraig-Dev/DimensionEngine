#pragma once
#include "Dimension/Scene/Scene.h"
#include "Dimension/Renderer/Mesh.h"
#include <functional>
#include <optional>

namespace Dimension
{
    namespace SceneBounds
    {
        // The world-space box around what the scene draws: active objects' models (their mesh data's bounds through
        // their transforms), sprites (a unit quad) and terrains (size and height). Objects for which `skip` returns
        // true (hidden in the Scene view) are left out. With nothing drawn, the box around the active objects'
        // positions; none for an empty scene. (Frame All in the Scene view.)
        std::optional<AABB> World(Scene& scene, const std::function<bool(Entity)>& skip = {});
    }
}
