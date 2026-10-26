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

        // Editing (the editor's brushes). Sculpt modes change the heights; paint modes add a layer's weight.
        // Detail modes paint where trees and grass tufts grow (Erase removes both).
        enum class BrushMode : int32_t { Raise = 0, Lower, Smooth, Flatten, PaintGrass, PaintRock, PaintSnow, PlaceTrees, PlaceGrass, EraseDetails, Count };
        inline bool IsDetailMode(BrushMode m) { return m >= BrushMode::PlaceTrees && m <= BrushMode::EraseDetails; }
        // Starts editing: the current heights and layers (from the edits, or else the heightmap/noise and automatic
        // layers) become the editable arrays.
        void BeginEdit(TerrainComponent& terrain);
        // One brush step at a point in the object's space (XZ), radius in meters, strength 0-1, over dt seconds.
        // Flatten moves toward `flattenHeight` (meters). Returns false when the point is off the terrain.
        bool ApplyBrush(TerrainComponent& terrain, BrushMode mode, const glm::vec2& at, float radius, float strength, float dt, float flattenHeight);
        // Writes the editable arrays into the serialized fields (call when a stroke ends).
        void CommitEdit(TerrainComponent& terrain);
        // Drops the edits: back to the heightmap/noise and automatic layers (details stay).
        void ClearEdits(TerrainComponent& terrain);
        void ClearDetails(TerrainComponent& terrain);
        // Painted tree and grass density per sample (2 bytes each), or empty when nothing is painted.
        std::vector<uint8_t> DetailMap(const TerrainComponent& terrain);
        // Where the details stand, in the object's space (each a transform with its turn and size), from the painted
        // density: a jittered grid per kind (cells of 100 / Density m2), each cell taken with the painted probability.
        struct DetailInstances
        {
            std::vector<glm::mat4> Trees, Grass;
        };
        DetailInstances BuildDetails(const TerrainComponent& terrain, const std::vector<float>& heights);
        // Cached for the current settings and edits (rebuilt when they change).
        const DetailInstances& GetDetails(const TerrainComponent& terrain);
        // Where a ray (world space) first meets the terrain (`world` is the object's transform).
        bool Raycast(const TerrainComponent& terrain, const glm::mat4& world, const glm::vec3& origin, const glm::vec3& direction, glm::vec3& hit);

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
