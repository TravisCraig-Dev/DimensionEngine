#pragma once
#include "Dimension/Renderer/Mesh.h"
#include <glm/glm.hpp>
#include <vector>

namespace Dimension
{
    struct TerrainComponent;
    struct TerrainPathComponent;
    class Scene;
    class Texture2D;

    // Terrain (with a Mesh Renderer for its smoothness and the like): a height field of Resolution x Resolution
    // samples over Size (meters, centered on the object, in its XZ plane), from a grayscale heightmap or generated
    // noise, scaled to Height. It renders as one mesh whose surface blends its layers (the first three by slope and
    // height, any of them painted), and collides as a Jolt height field, with holes cut out of both.
    namespace Terrain
    {
        constexpr int kMaxLayers = 16;
        // The terrain's layer count (1 - kMaxLayers), and its splat maps: RGBA8, 4 layers each.
        int LayerCount(const TerrainComponent& terrain);
        int SplatMapCount(const TerrainComponent& terrain);
        // A layer's weight (0-1) at a sample of the edit arrays (BeginEdit first).
        float LayerWeight(const TerrainComponent& terrain, size_t sample, int layer);
        // Moves a sample's weights (edit arrays) toward a layer by `amount` (0-1).
        void PaintSample(TerrainComponent& terrain, size_t sample, int layer, float amount);
        // Sample count per side (an even number >= 4, as Jolt's height fields need).
        int SampleCount(const TerrainComponent& terrain);
        // Heights in meters, row-major with rows along +Z and columns along +X.
        std::vector<float> BuildHeights(const TerrainComponent& terrain);
        // The height at a point in the object's space (bilinear between samples; clamped to the edges).
        float HeightAt(const TerrainComponent& terrain, const std::vector<float>& heights, float x, float z);
        // The mesh leaves out every triangle that touches a hole sample (as the height field does).
        MeshData BuildMesh(const TerrainComponent& terrain, const std::vector<float>& heights);
        // Layer weights per sample as SplatMapCount RGBA8 maps, one after the other: layers 0-3 for the first n * n
        // samples, then 4-7, ...
        std::vector<uint32_t> BuildSplat(const TerrainComponent& terrain, const MeshData& mesh);
        // Hole samples (1 = hole) per sample, or empty when there are none.
        std::vector<uint8_t> HoleMap(const TerrainComponent& terrain);
        // Whether the ground at a point (object space) is cut away: any triangle there touches a hole sample.
        bool IsHoleAt(const TerrainComponent& terrain, const std::vector<uint8_t>& holes, float x, float z);

        // Editing (the editor's brushes). Sculpt modes change the heights; paint modes add a layer's weight.
        // Detail modes paint where trees and grass tufts grow (Erase removes both). Dig and Fill cut and close holes.
        enum class BrushMode : int32_t
        {
            Raise = 0, Lower, Smooth, Flatten,
            Paint, // the layer chosen (ApplyBrush's paintLayer)
            PlaceTrees, PlaceGrass, EraseDetails,
            DigHole, FillHole,
            Stamp,
            Erode,
            Count
        };
        inline bool IsPaintMode(BrushMode m) { return m == BrushMode::Paint; }
        inline bool IsDetailMode(BrushMode m) { return m >= BrushMode::PlaceTrees && m <= BrushMode::EraseDetails; }
        inline bool IsHoleMode(BrushMode m) { return m == BrushMode::DigHole || m == BrushMode::FillHole; }
        // Starts editing: the current heights and layers (from the edits, or else the heightmap/noise and automatic
        // layers) become the editable arrays.
        void BeginEdit(TerrainComponent& terrain);
        // Stamp brush: a grayscale image (a mountain, a crater, a mesa) laid over a square of twice the brush radius,
        // turned by Rotation (degrees). The ground rises toward the stroke's starting height plus the image times
        // Height, or with Carve sinks toward it minus that (only ever moving one way, so stamps can overlap).
        struct StampSettings
        {
            AssetHandle Image = AssetHandle::None();
            float Height = 5.0f; // meters at white
            float Rotation = 0.0f;
            bool Carve = false;
        };
        // One brush step at a point in the object's space (XZ), radius in meters, strength 0-1, over dt seconds.
        // Flatten moves toward `flattenHeight` (meters), which is also the stamp's base. Returns false when the point
        // is off the terrain (or a stamp has no readable image).
        bool ApplyBrush(TerrainComponent& terrain, BrushMode mode, const glm::vec2& at, float radius, float strength, float dt, float flattenHeight,
                        const StampSettings* stamp = nullptr, int paintLayer = 0);
        // The stamp's value (0-1) at a point relative to the brush center, or -1 outside its square.
        float StampValue(const StampSettings& stamp, const glm::vec2& offset, float radius);
        // A Terrain Path's curve: Catmull-Rom through its points (each segment split into `perSegment` pieces).
        std::vector<glm::vec3> PathCurve(const std::vector<glm::vec3>& points, int perSegment = 16);
        // Shapes the terrain along a Terrain Path (its curve in world space via pathWorld; terrainWorld is the terrain
        // object's transform): levels a road or carves a river, and paints its layer. Works on the edit arrays (BeginEdit
        // first, CommitEdit after). Returns how many samples changed.
        int ApplyPath(TerrainComponent& terrain, const glm::mat4& terrainWorld, const TerrainPathComponent& path, const glm::mat4& pathWorld);
        // A crater (an explosion's, a meteor's) at a world point (`world` is the terrain object's transform): a bowl Radius
        // across and Depth deep in the middle, a rim of thrown-up earth around it, its ground painted with `paintLayer`
        // (its place in the Layers list, 3 = the default dirt; -1 = none) and cleared of trees and grass. Works on the edit arrays (BeginEdit
        // first, CommitEdit after). Returns how many samples changed.
        int Crater(TerrainComponent& terrain, const glm::mat4& world, const glm::vec3& point, float radius, float depth, int paintLayer = 3);
        // Digs a crater into every terrain under the point, at any time (also while playing: the terrain redraws and its
        // collider is rebuilt). Returns how many samples changed.
        int DigCrater(Scene& scene, const glm::vec3& point, float radius, float depth, int paintLayer = 3);
        // Hydraulic erosion (the Erode brush): `droplets` raindrops start at random inside the circle (the same `seed`
        // gives the same drops) and run downhill over the heights, picking up sediment where they speed up and the
        // ground falls away, and dropping it where they slow down or climb, which carves gullies and fills basins.
        // Material only moves (except where it would leave 0 - Height). Works on the edit arrays (BeginEdit first).
        void Erode(TerrainComponent& terrain, const glm::vec2& at, float radius, int droplets, uint32_t seed);
        // Writes the editable arrays into the serialized fields (call when a stroke ends).
        void CommitEdit(TerrainComponent& terrain);
        // Drops the edits: back to the heightmap/noise and automatic layers, without holes (details stay).
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
        // The heights (as BuildHeights, or the edit arrays while editing), cached for the current settings and edits.
        std::vector<float> GetHeights(const TerrainComponent& terrain);
        // Where a ray (world space) first meets the terrain (`world` is the object's transform).
        bool Raycast(const TerrainComponent& terrain, const glm::mat4& world, const glm::vec3& origin, const glm::vec3& direction, glm::vec3& hit);

        // The GPU mesh and splat textures for a terrain's current settings (cached; rebuilt when they change).
        struct Runtime
        {
            Ref<Mesh> Surface;
            std::vector<Ref<Texture2D>> Splats; // a map per 4 layers
            uint64_t Key = 0;
        };
        const Runtime* GetRuntime(const TerrainComponent& terrain);
        uint64_t SettingsKey(const TerrainComponent& terrain);
        void ReleaseRuntimes();
    }
}
