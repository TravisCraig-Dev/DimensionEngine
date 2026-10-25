#pragma once
#include "Dimension/Core/Base.h"
#include <glm/glm.hpp>
#include <memory>

namespace Dimension
{
    struct MeshData;

    // One convex piece of a decomposed mesh, in the mesh's space.
    struct ConvexPart
    {
        std::vector<glm::vec3> Points;
        std::vector<uint32_t> Triangles; // hull faces (3 indices each), for gizmos
    };

    namespace ConvexDecomposition
    {
        // Splits a concave mesh into at most `maxParts` convex pieces with V-HACD (voxelized at about `resolution`
        // voxels), so dynamic bodies can collide like the real shape: a ring around a pole, a cup that holds things.
        // Results are cached per mesh and settings (decomposing takes a moment); empty when the mesh has no triangles.
        std::shared_ptr<const std::vector<ConvexPart>> Decompose(const std::shared_ptr<const MeshData>& mesh, int maxParts, int resolution = 20000);
        void ClearCache();
    }
}
