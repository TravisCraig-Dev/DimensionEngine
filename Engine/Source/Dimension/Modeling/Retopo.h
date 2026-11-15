#pragma once
#include "Dimension/Modeling/EditableMesh.h"

// Retopology helpers: finding another surface (rays and nearest points) and wrapping a mesh onto it.
namespace Dimension::Modeling
{
    // A bounding volume hierarchy over triangles: ray hits and nearest points.
    class TriangleTree
    {
    public:
        // Triangles from positions, three indices each.
        void Build(std::vector<glm::vec3> positions, const std::vector<uint32_t>& indices);
        // A mesh's faces (triangulated), moved by `transform`.
        void Build(const EditableMesh& mesh, const glm::mat4& transform = glm::mat4(1.0f));
        // Appends another mesh's triangles (call Finish after the last).
        void Add(const std::vector<glm::vec3>& positions, const std::vector<uint32_t>& indices, const glm::mat4& transform);
        void Finish();
        bool Empty() const { return m_Tris.empty(); }
        size_t TriangleCount() const { return m_Tris.size(); }

        struct Hit
        {
            bool Found = false;
            float Distance = 0.0f;
            glm::vec3 Point{ 0.0f }, Normal{ 0.0f }; // the triangle's (unit)
        };
        // The nearest hit along the ray within maxDistance (`dir` unit length).
        Hit Raycast(const glm::vec3& origin, const glm::vec3& dir, float maxDistance = 1e30f) const;
        // The surface point nearest `p` (Distance: how far).
        Hit Closest(const glm::vec3& p) const;

    private:
        struct Tri
        {
            glm::vec3 A, B, C;
        };
        struct Node
        {
            glm::vec3 Min, Max;
            uint32_t Left = 0, Count = 0;
        };
        void BuildNode(uint32_t node, uint32_t first, uint32_t count);
        std::vector<Tri> m_Tris;
        std::vector<Node> m_Nodes;
    };

    namespace Retopo
    {
        enum class WrapMode
        {
            Nearest = 0, // to the nearest point of the surface
            Project = 1, // along each vertex's normal (forwards or backwards, whichever is nearer)
        };
        // Moves vertices onto the target surface, `offset` out along its normal. `only`: a flag per vertex (null: all).
        // Returns how many moved.
        size_t Shrinkwrap(EditableMesh& m, const TriangleTree& target, float offset, WrapMode mode, const std::vector<uint8_t>* only = nullptr);
    }
}
