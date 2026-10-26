#pragma once
#include "Dimension/Renderer/Mesh.h"
#include <glm/glm.hpp>
#include <vector>

namespace Dimension
{
    // Breaking a mesh into pieces (a Breakable component's debris): Voronoi cells around random seed points inside the
    // mesh, each cut out of the mesh with the planes halfway to its neighbours' seeds. Every cut is closed with cap
    // faces (a separate sub-mesh, so they can take an inside material). Caps are filled per loop of cut edges from
    // its center, which is exact for convex cuts (crates, walls, rocks, vases cut across) and close for others.
    namespace Fracture
    {
        struct Piece
        {
            MeshData Mesh;      // positions relative to Center; the source's sub-meshes, then the cap sub-mesh
            glm::vec3 Center;   // the piece's centroid in the source mesh's space
            float Volume = 0.0f;
        };
        // Up to `count` pieces (cells with nothing of the mesh in them are left out); the same seed gives the same
        // pieces. The cap sub-mesh uses material slot `capSlot`.
        std::vector<Piece> Shatter(const MeshData& mesh, int count, uint32_t seed, int32_t capSlot);
        // Volume enclosed by a closed triangle mesh (signed tetrahedra from the origin).
        float Volume(const MeshData& mesh);
    }
}
