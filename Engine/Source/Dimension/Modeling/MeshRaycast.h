#pragma once
#include "Dimension/Modeling/EditableMesh.h"

namespace Dimension::Modeling
{
    // Ray casts against an editable mesh's faces (triangulated, hidden faces left out) through a bounding volume
    // hierarchy: picking faces under the mouse and testing whether elements are behind the surface.
    class MeshRaycaster
    {
    public:
        struct RayHit
        {
            bool Hit = false;
            float Distance = 0.0f; // along the (normalized) direction
            uint32_t Face = 0;
            glm::vec3 Point{ 0.0f };
        };

        void Build(const EditableMesh& mesh);
        bool Empty() const { return m_Tris.empty(); }
        // The nearest face hit from `origin` along `dir` within `maxDistance`.
        RayHit Raycast(const glm::vec3& origin, const glm::vec3& dir, float maxDistance = 1e30f) const;
        // True when a face lies between the points (a bit short of `to`, so the surface at `to` does not count).
        bool Occluded(const glm::vec3& from, const glm::vec3& to, float epsilon = 1e-4f) const;

    private:
        struct Tri
        {
            glm::vec3 A, B, C;
            uint32_t Face;
        };
        struct Node
        {
            glm::vec3 Min, Max;
            uint32_t Left = 0, Count = 0; // leaf: first triangle and count; inner: left child (right = Left + 1)
        };
        void BuildNode(uint32_t node, uint32_t first, uint32_t count);
        std::vector<Tri> m_Tris;
        std::vector<Node> m_Nodes;
    };
}
