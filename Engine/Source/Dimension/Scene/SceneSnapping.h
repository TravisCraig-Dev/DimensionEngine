#pragma once
#include "Dimension/Scene/Scene.h"
#include <glm/glm.hpp>
#include <vector>

namespace Dimension
{
    // Scene view snapping (like Unity's): to the grid, to other objects' vertices (hold V) and onto surfaces (Ctrl +
    // Shift while moving). Works on the scene's mesh renderers (their imported geometry, bind pose for skinned ones)
    // and terrains, so objects without colliders count too. `skip` objects and their children are left out (the
    // objects being moved).
    namespace SceneSnapping
    {
        // Rounds the chosen axes to multiples of `step` (step <= 0 leaves them).
        glm::vec3 ToGrid(const glm::vec3& p, float step, const glm::bvec3& axes = glm::bvec3(true));

        struct SurfaceHit
        {
            bool Hit = false;
            glm::vec3 Point{ 0.0f }, Normal{ 0.0f, 1.0f, 0.0f };
            float Distance = 0.0f;
            Entity Object = Entity::Null();
        };
        // The first mesh triangle or terrain a world-space ray meets.
        SurfaceHit Raycast(Scene& scene, const glm::vec3& origin, const glm::vec3& direction, const std::vector<Entity>& skip = {});

        // The mesh vertex (world space) nearest a point on screen, within `maxPixels`. `viewProj` maps world space to
        // clip space (NDC y up, as the editor camera's); `screen` is in pixels from the top left of a viewport
        // `viewport` pixels big. With `only` given, only those objects' vertices count (their children's too).
        struct VertexHit
        {
            bool Hit = false;
            glm::vec3 Position{ 0.0f };
            float Pixels = 0.0f;
            Entity Object = Entity::Null();
        };
        VertexHit NearestVertex(Scene& scene, const glm::mat4& viewProj, const glm::vec2& viewport, const glm::vec2& screen,
                                float maxPixels, const std::vector<Entity>& only = {}, const std::vector<Entity>& skip = {});

        // How far an object's meshes (with its children's) reach from its pivot against `normal`: the distance to lift
        // the pivot off a surface with that normal so the object rests on it. 0 without meshes.
        float RestOffset(Scene& scene, Entity e, const glm::vec3& normal);
    }
}
