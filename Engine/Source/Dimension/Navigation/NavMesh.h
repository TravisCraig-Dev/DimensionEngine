#pragma once
#include "Dimension/ECS/Entity.h"
#include "Dimension/Scene/Components.h"
#include <glm/glm.hpp>
#include <memory>
#include <vector>

namespace Dimension
{
    class Scene;

    // A baked navigation mesh: a grid of columns Cell Size apart, each holding the walkable spots (nodes) at their
    // heights (several where floors stack), linked to the neighbouring columns' nodes an agent can step to (8 ways;
    // diagonals only where both sides are open). Nodes linked together share an Island.
    struct NavMeshData
    {
        glm::vec2 Origin{ 0.0f }; // x/z of the grid's corner
        float CellSize = 0.25f;
        int Width = 0, Depth = 0; // columns along x and z
        float AgentRadius = 0.5f, AgentHeight = 2.0f, StepHeight = 0.4f;
        std::vector<uint32_t> ColumnStart; // Width * Depth + 1: the nodes of column c are [ColumnStart[c], ColumnStart[c + 1])
        std::vector<float> Heights;        // per node
        std::vector<int32_t> Columns;      // per node: its column
        std::vector<int32_t> Links;        // per node, 8 each (Navigation::kDirX/kDirZ order): the linked node, -1 = none
        std::vector<int32_t> Islands;      // per node

        int NodeCount() const { return (int)Heights.size(); }
        int Column(int x, int z) const { return x >= 0 && z >= 0 && x < Width && z < Depth ? z * Width + x : -1; }
        // The node's spot: its column's center at its height.
        glm::vec3 NodePosition(int node) const;
    };

    namespace Navigation
    {
        // Link directions: +x, +z, -x, -z, then the diagonals.
        inline constexpr int kDirX[8] = { 1, 0, -1, 0, 1, -1, -1, 1 };
        inline constexpr int kDirZ[8] = { 0, 1, 0, -1, 1, 1, -1, -1 };

        // Bakes a NavMesh Surface from the scene's colliders (it needs physics: a playing scene's, or else a temporary
        // one is built). Stores and returns the result (nullptr when the object has no surface).
        std::shared_ptr<const NavMeshData> Bake(Scene& scene, Entity surface);
        // Bakes every surface (all, or only those not baked yet). Agents find their mesh again.
        void BakeAll(Scene& scene, bool onlyMissing = false);

        // The surface mesh nearest a point within maxDistance (nullptr = none).
        std::shared_ptr<const NavMeshData> MeshNear(Scene& scene, const glm::vec3& point, float maxDistance, int* node = nullptr);
        // The node nearest a point (-1 = none within maxDistance).
        int Nearest(const NavMeshData& mesh, const glm::vec3& point, float maxDistance);
        // Ground height at x/z around a node (blended with its neighbours, so slopes are smooth).
        float HeightAt(const NavMeshData& mesh, int node, float x, float z);
        // Walks straight on the mesh from a node's column to `to` (x/z): true if nothing is in the way. Otherwise
        // `reached` is where it stopped. `endNode` is the node it ends on.
        bool Walk(const NavMeshData& mesh, int startNode, glm::vec2 from, glm::vec2 to, int* endNode = nullptr, glm::vec2* reached = nullptr);
        // The shortest way from one point to another: its corners, from `from` (on the mesh) to `to` (or, when `to`
        // cannot be reached, to the nearest reachable spot: Partial).
        NavPathStatus FindPath(const NavMeshData& mesh, const glm::vec3& from, const glm::vec3& to, std::vector<glm::vec3>& corners);
        // The nearest point on any surface's mesh to `point` within maxDistance.
        bool SamplePosition(Scene& scene, const glm::vec3& point, float maxDistance, glm::vec3& result);
        NavPathStatus CalculatePath(Scene& scene, const glm::vec3& from, const glm::vec3& to, std::vector<glm::vec3>& corners);

        // Agents: steer along their paths and around each other (a playing scene, every frame).
        void Update(Scene& scene, float dt);
        // Sends an agent to a point: false when there is no way at all (no mesh near it or the point).
        bool SetDestination(Scene& scene, Entity agent, const glm::vec3& destination);
        void ResetPath(Scene& scene, Entity agent);
        // Moves an agent straight to a point on the mesh (false: none near it).
        bool Warp(Scene& scene, Entity agent, const glm::vec3& position);
    }
}
