#pragma once
#include "Dimension/ECS/Entity.h"
#include <glm/glm.hpp>
#include <vector>

namespace Dimension
{
    class Scene;

    // LOD Groups (see LODGroupComponent): which child each group shows for a camera.
    namespace LODGroups
    {
        // The size measured against the screen: the group's Size, or the largest side of its children's mesh bounds.
        float WorldSize(Scene& scene, Entity group, glm::vec3* center = nullptr);
        // The child shown (0 = most detailed; -1 = none, culled) for a camera at `cameraPosition` with `projection`;
        // `screenHeight` gets the fraction of the view's height the group covers (times its Bias).
        int Choose(Scene& scene, Entity group, const glm::vec3& cameraPosition, const glm::mat4& projection, bool orthographic,
                   float* screenHeight = nullptr);
        // Marks (by entity index) every object a LOD group hides for this camera: its other children and all below them.
        void Hidden(Scene& scene, const glm::vec3& cameraPosition, const glm::mat4& projection, bool orthographic, std::vector<uint8_t>& hidden);
    }
}
