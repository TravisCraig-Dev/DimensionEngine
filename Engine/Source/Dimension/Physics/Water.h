#pragma once
#include "Dimension/Renderer/Mesh.h"
#include "Dimension/ECS/Entity.h"
#include <glm/glm.hpp>

namespace Dimension
{
    class Scene;
    struct WaterVolumeComponent;

    // Water Volume waves: a sum of three Gerstner waves in the volume's space (the surface is the top face, y =
    // Size.y / 2, centered on the object). Points on the surface move in circles: up and down by the wave height and
    // back and forth along each wave's direction (by Steepness), which sharpens the crests.
    namespace Water
    {
        struct SurfacePoint
        {
            float Height = 0.0f;              // surface height in the volume's space
            glm::vec3 Normal{ 0, 1, 0 };
            glm::vec3 Velocity{ 0.0f };       // of the water at the surface (the waves' orbital motion)
        };
        bool HasWaves(const WaterVolumeComponent& water);
        // Where the surface point resting at `rest` (x, z in the volume's space) is at `time`, and its normal.
        glm::vec3 Displace(const WaterVolumeComponent& water, glm::vec2 rest, float time, glm::vec3* normal = nullptr);
        // The surface above (x, z) at `time` (inverts the horizontal motion, so it is the height at that spot).
        SurfacePoint Sample(const WaterVolumeComponent& water, glm::vec2 xz, float time);

        // The surface grid (Surface Resolution quads per side over Size.x by Size.z, facing +Y) and its CPU vertices.
        Ref<Mesh> SurfaceMesh(const WaterVolumeComponent& water);
        const std::vector<Vertex>& SurfaceVertices(const WaterVolumeComponent& water);
        // The water's box in the world: the volume's Size around the object, or for an Ocean as wide as can be.
        void Bounds(const WaterVolumeComponent& water, const glm::vec3& center, const glm::vec3& scale, glm::vec3& lo, glm::vec3& hi);
        // An Ocean's surface: rings of vertices around a center (x, z in the volume's space) out to `radius`, closer
        // together near the middle. OceanMesh is its triangles; OceanVertices puts the vertices where the waves are at
        // `time`, the waves fading out far away (where they would only flicker).
        Ref<Mesh> OceanMesh();
        // The normals' length carries the crest foam (1 + foam).
        void OceanVertices(const WaterVolumeComponent& water, glm::vec2 center, float radius, float time, std::vector<Vertex>& out);
        // Foam on the crests at a rest point (0-1, before the volume's Foam): where the waves bunch the surface up.
        float CrestFoam(const WaterVolumeComponent& water, glm::vec2 rest, float time);
        // Fills every Water Volume's SimulatedPositions/Normals for `time` (flat water and oceans leave them empty).
        void Update(Scene& scene, float time);
        void ReleaseMeshes();
    }
}
