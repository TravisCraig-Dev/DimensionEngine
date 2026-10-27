#pragma once
#include "Dimension/Renderer/Mesh.h"

namespace Dimension
{
    namespace MeshSimplify
    {
        // Simpler versions of a mesh for levels of detail: edges are collapsed one at a time, cheapest first, the cost
        // being how far the shape moves (quadric error metrics; open borders, sub-mesh borders and UV seams cost more
        // to move), until about `ratio` of the triangles are left or the next collapse would move the surface more
        // than `maxError` (a fraction of the mesh's size). A collapse moves a corner onto a neighbouring one, so the
        // vertices (UVs, tangents, skinning) stay as they were; only the triangles change. Sub-meshes keep their
        // order and materials.
        MeshData Simplify(const MeshData& source, float ratio, float maxError = 0.05f);
        size_t TriangleCount(const MeshData& data);
    }
}
