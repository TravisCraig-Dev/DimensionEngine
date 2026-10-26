#pragma once
#include "Dimension/Renderer/Mesh.h"
#include <glm/glm.hpp>
#include <vector>

namespace Dimension
{
    struct TerrainComponent;
    class Texture2D;

    // Terrain (with a Mesh Renderer for its smoothness and the like): a height field of Resolution x Resolution
    // samples over Size (meters, centered on the object, in its XZ plane), from a grayscale heightmap or generated
    // noise, scaled to Height. It renders as one mesh whose surface blends grass, rock and snow layers by slope and
    // height, and collides as a Jolt height field.
    namespace Terrain
    {
        // Sample count per side (an even number >= 4, as Jolt's height fields need).
        int SampleCount(const TerrainComponent& terrain);
        // Heights in meters, row-major with rows along +Z and columns along +X.
        std::vector<float> BuildHeights(const TerrainComponent& terrain);
        // The height at a point in the object's space (bilinear between samples; clamped to the edges).
        float HeightAt(const TerrainComponent& terrain, const std::vector<float>& heights, float x, float z);
        MeshData BuildMesh(const TerrainComponent& terrain, const std::vector<float>& heights);
        // Layer weights per sample (r grass, g rock, b snow) as RGBA8.
        std::vector<uint32_t> BuildSplat(const TerrainComponent& terrain, const MeshData& mesh);

        // The GPU mesh and splat texture for a terrain's current settings (cached; rebuilt when they change).
        struct Runtime
        {
            Ref<Mesh> Surface;
            Ref<Texture2D> Splat;
            uint64_t Key = 0;
        };
        const Runtime* GetRuntime(const TerrainComponent& terrain);
        uint64_t SettingsKey(const TerrainComponent& terrain);
        void ReleaseRuntimes();
    }
}
