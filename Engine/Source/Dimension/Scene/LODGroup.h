#pragma once
#include "Dimension/ECS/Entity.h"
#include "Dimension/Core/Guid.h"
#include <glm/glm.hpp>
#include <unordered_map>
#include <vector>

namespace Dimension
{
    class Scene;

    // LOD Groups (see LODGroupComponent): which level each group shows for a camera.
    namespace LODGroups
    {
        // The size measured against the screen: the group's Size, or the largest side of its meshes' bounds.
        float WorldSize(Scene& scene, Entity group, glm::vec3* center = nullptr);
        // How many levels the group has: 1 + Generated Levels, or its children (up to 4), and the impostor after them.
        int LevelCount(Scene& scene, Entity group);
        // The impostor's level (-1 = none).
        int ImpostorLevel(Scene& scene, Entity group);
        // The world bounds of an object's meshes and all below it (false: none).
        bool Bounds(Scene& scene, Entity e, glm::vec3& min, glm::vec3& max);

        // The level shown (0 = most detailed; -1 = none, culled). Within Fade Width above a threshold it shares the
        // pixels with the next level (or with nothing, before culling): Level covers Coverage of them, Next the rest.
        struct Pick
        {
            int Level = 0;
            int Next = -1;
            float Coverage = 1.0f;
        };
        // `screenHeight` gets the fraction of the view's height the group covers (times its Bias).
        Pick Evaluate(Scene& scene, Entity group, const glm::vec3& cameraPosition, const glm::mat4& projection, bool orthographic,
                      float* screenHeight = nullptr);
        int Choose(Scene& scene, Entity group, const glm::vec3& cameraPosition, const glm::mat4& projection, bool orthographic,
                   float* screenHeight = nullptr);

        // Every group's choice for one camera, as the renderer uses it. Fades are (coverage 1-254) | (side << 8): side 0
        // keeps the pixels below the coverage in a dither pattern, side 1 the others.
        struct Frame
        {
            std::vector<uint8_t> Hidden;                  // by entity index: a level not shown (with all below it)
            std::unordered_map<uint32_t, uint32_t> Fade;  // by entity index: objects of a fading level
            struct Own
            {
                int Level, Next;
                uint32_t Fade, NextFade;
            };
            std::unordered_map<uint32_t, Own> Generated;  // by entity index: groups drawing generated levels of their own mesh
            std::vector<std::pair<Entity, uint32_t>> Impostors; // groups drawing their impostor, with its fade
        };
        void Evaluate(Scene& scene, const glm::vec3& cameraPosition, const glm::mat4& projection, bool orthographic, Frame& frame);
        // Marks (by entity index) every object a LOD group hides for this camera.
        void Hidden(Scene& scene, const glm::vec3& cameraPosition, const glm::mat4& projection, bool orthographic, std::vector<uint8_t>& hidden);

        // A mesh for generated level `level` (0 = the mesh itself): simplified to Reduction^level of its triangles,
        // once (kept until the mesh is reloaded). Without `wait`, a level not made yet is simplified on a worker thread
        // and the nearest finer level that is ready comes back meanwhile (the renderer: no frame waits on it).
        AssetHandle GeneratedMesh(AssetHandle mesh, int level, float reduction, bool wait = true);
    }
}
