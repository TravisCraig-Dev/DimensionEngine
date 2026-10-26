#pragma once
#include "Dimension/Renderer/Mesh.h"
#include <glm/glm.hpp>
#include <vector>

namespace Dimension
{
    struct RopeComponent;

    // Ropes, chains of lamps, swings, bridges: a Rope component is a line of Segments + 1 points simulated as a Jolt
    // soft body (stretch and bend constraints, colliding by its Radius), its start held by the object and with Attach
    // To its end by another object, drawn as a tube with the object's Mesh Renderer material.
    namespace Rope
    {
        constexpr int kSides = 8; // around the tube
        // The rope's starting shape in world space: hanging straight down from the object (its -Y) or, attached, from
        // the object to the attachment point, sagging so it is Length long (straight, stretched, when they are farther
        // apart than that).
        std::vector<glm::vec3> RestPoints(const RopeComponent& rope, const glm::vec3& start, const glm::vec3& down, const glm::vec3* end);
        // The tube around points (any space): (points) rings of kSides + 1 vertices (the seam repeats for the UVs),
        // U around, V along the rope (one repeat per meter of rope).
        void BuildTube(const RopeComponent& rope, const std::vector<glm::vec3>& points, std::vector<Vertex>& out);
        // The tube's GPU mesh (indices; its vertices are rebuilt every frame), shared by ropes with the same segments.
        Ref<Mesh> TubeMesh(const RopeComponent& rope);
        void ReleaseMeshes();
        int PointCount(const RopeComponent& rope);
    }
}
