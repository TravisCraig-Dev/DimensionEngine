#pragma once
#include "Dimension/Renderer/Mesh.h"

namespace Dimension
{
    struct ClothComponent;

    namespace Cloth
    {
        // The cloth's rest shape: a (Resolution + 1)^2 grid of Size in the object's XY plane (centered; the top edge at
        // +Y), facing +Z, with UVs over the whole sheet.
        MeshData BuildGrid(const ClothComponent& cloth);
        // Indices of the vertices the Pin setting fixes (they follow the object).
        std::vector<uint32_t> PinnedVertices(const ClothComponent& cloth);
        // The GPU mesh for a cloth's grid, and its CPU vertices (shared by cloths of the same size and resolution).
        Ref<Mesh> GridMesh(const ClothComponent& cloth);
        const std::vector<Vertex>& GridVertices(const ClothComponent& cloth);
        void ReleaseMeshes();
        // Smooth normals (and tangents along +U) for moved vertices.
        void RecomputeNormals(std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices);
    }
}
