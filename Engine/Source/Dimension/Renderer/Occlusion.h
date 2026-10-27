#pragma once
#include <glm/glm.hpp>
#include <vector>

namespace Dimension
{
    // Occlusion culling from an earlier frame's depth: the depth buffer reduced on the GPU to a grid of cells, each
    // holding the farthest depth drawn in it, read back a frame or two later with the view it was drawn from. A box
    // lies behind what was drawn when, in every cell it covers (and the ring around them, for the small shift since),
    // something nearer than the box's nearest point was drawn.
    struct OccluderDepth
    {
        uint32_t Width = 0, Height = 0;
        std::vector<float> Farthest; // per cell, row 0 at the top of the view: depth 0 (near) - 1 (far, nothing drawn)
        glm::mat4 ViewProj{ 1.0f };

        bool IsValid() const { return Width > 0 && Height > 0 && Farthest.size() == (size_t)Width * Height; }
        // A world box (center, half extents) wholly hidden. Boxes reaching behind the camera or over half the view
        // are never hidden.
        bool Hides(const glm::vec3& center, const glm::vec3& extents) const;
    };
}
