#pragma once
#include "Dimension/Modeling/EditableMesh.h"
#include <array>
#include <unordered_map>

// Sculpting (Blender's Sculpt Mode): brushes that push a mesh's surface around, with symmetry, a mask, dynamic topology
// (edges split and collapsed under the brush to keep a chosen detail size) and a voxel remesher. Everything works in the
// model's own space.
namespace Dimension::Modeling::Sculpt
{
    enum class Brush
    {
        Draw = 0,  // pushes out along the brush's normal (in with Invert)
        Clay,      // builds up towards a plane above the surface, filling dips first
        Inflate,   // each vertex along its own normal (deflates with Invert)
        Smooth,    // evens out bumps
        Flatten,   // pulls towards the plane under the brush
        Scrape,    // cuts what stands above that plane (fills dips below it with Invert)
        Pinch,     // pulls towards the brush's center (pushes apart with Invert)
        Crease,    // a sharp groove (a ridge with Invert): draw in plus pinch
        Grab,      // drags the surface caught at the stroke's start with the mouse
        SnakeHook, // drags the surface along the stroke (pulls out long shapes; dynamic topology adds the detail)
        Mask,      // paints the mask (masked parts don't move; Invert erases it)
        Pose,      // turns the part under the brush about a pivot at the brush's edge, as a joint bends (drag)
        Boundary,  // drags the open edges (borders) under the brush, the surface behind them following (drag)
        Cloth,     // drags the surface like cloth: edges keep their length, so it folds and wrinkles (drag)
        Count
    };
    const char* BrushName(Brush b);
    // Brushes that follow the mouse (given the motion) instead of dabbing on the surface under it.
    inline bool IsDragBrush(Brush b)
    {
        return b == Brush::Grab || b == Brush::SnakeHook || b == Brush::Pose || b == Brush::Boundary || b == Brush::Cloth;
    }
    // ...and of those, the ones given the motion since the stroke began (the others: since the last dab).
    inline bool IsAnchoredBrush(Brush b) { return b == Brush::Grab || b == Brush::Pose || b == Brush::Boundary; }

    // A grey image that varies a brush's strength across it.
    struct BrushTexture
    {
        std::vector<float> Values; // 0..1 per pixel, rows from the top
        uint32_t Width = 0, Height = 0;
        int Mapping = 0;      // 0: one copy over each dab (facing the view); 1: tiled over the view plane every Scale units
        float Scale = 1.0f;   // tiled: the model units one copy spans
        float Angle = 0.0f;   // degrees
        // From RGBA8 pixels (their brightness times alpha).
        static BrushTexture FromRGBA8(const uint32_t* pixels, uint32_t width, uint32_t height);
        float Sample(glm::vec2 uv) const; // bilinear, repeating
        // The value at `p` for a dab at `center` of `radius` seen along `view`.
        float At(const glm::vec3& p, const glm::vec3& center, float radius, const glm::vec3& view) const;
    };

    struct BrushSettings
    {
        Brush Type = Brush::Draw;
        float Radius = 0.1f;   // model units
        float Strength = 0.5f; // 0..1
        bool Invert = false;
        bool FrontFacesOnly = true;      // vertices facing away from the view are left alone
        glm::vec3 ViewDir{ 0, 0, -1 };   // the camera's looking direction (model space)
        uint8_t Symmetry = 0;            // bits: 1 X, 2 Y, 4 Z (mirrored across the model's own planes)
        const BrushTexture* Texture = nullptr; // varies the strength (not the drag brushes')
    };

    // A triangle mesh being sculpted: vertices know their triangles, so brushes and dynamic topology work locally.
    class SculptMesh
    {
    public:
        std::vector<glm::vec3> Positions, Normals;
        std::vector<glm::vec2> UVs;        // per vertex (for the preview)
        std::vector<std::array<glm::vec2, 3>> TriUVs; // per triangle corner (kept through dynamic topology)
        std::vector<float> Mask;           // per vertex, 0..1
        std::vector<glm::uvec3> Tris;      // counter-clockwise; dead ones are all ~0
        std::vector<int32_t> TriMaterial;  // per triangle
        std::vector<std::vector<uint32_t>> VertTris;

        static SculptMesh FromEditable(const EditableMesh& m);
        // Back into `m`: positions and the mask when the topology is as it came, else a new mesh of the triangles
        // (smooth shaded, UVs carried through the splits and collapses; `m`'s name, material slots, modifiers and
        // smoothing settings kept).
        void WriteTo(EditableMesh& m) const;
        // A quick render mesh of the current state (one sub-mesh per material slot).
        MeshData ToMeshData(size_t materialSlots) const;
        bool TopologyChanged() const { return m_TopologyChanged; }
        size_t LiveVertexCount() const;
        size_t LiveTriangleCount() const;
        bool IsDead(uint32_t tri) const { return Tris[tri].x == ~0u; }

        // The nearest triangle hit by the ray (false: none). `normal` is the triangle's (unit).
        bool Raycast(const glm::vec3& from, const glm::vec3& dir, float& t, glm::vec3& normal) const;

        // A stroke: Begin at the first point (Grab catches the vertices there), Dab at each point along it. `move` is the
        // mouse's motion carried to the surface since the stroke began (Grab) or since the last dab (Snake Hook).
        void BeginStroke(const glm::vec3& center, const BrushSettings& brush);
        void Dab(const glm::vec3& center, const BrushSettings& brush, const glm::vec3& move = glm::vec3(0.0f));
        void EndStroke();

        // Dynamic topology: under each dab, edges longer than `detail` are split and ones under 40% of it collapsed.
        float DetailSize = 0.0f; // 0: off
        // The whole mesh to the detail size (Detail Flood Fill). Returns false when it gave up before settling.
        bool FloodFillDetail(float detail, int maxPasses = 8);

        void ClearMask();
        void InvertMask();
        void SmoothMask(int iterations = 1);
        void SmoothAll(int iterations, float factor = 0.5f, bool respectMask = true);

        void UpdateNormals(); // all of them

    private:
        struct Caught
        {
            uint32_t Vertex;
            float Weight;
            glm::vec3 Start;
            glm::vec3 Mirror; // the move's mirroring for this vertex's copy of the brush
        };
        std::vector<Caught> m_Caught; // Grab, Pose, Boundary
        struct PoseFrame
        {
            glm::vec3 Mirror, Pivot, Center;
        };
        std::vector<PoseFrame> m_Pose;
        std::unordered_map<uint64_t, float> m_ClothRest; // edge -> its length when the stroke first reached it
        bool m_TopologyChanged = false;
        void CatchPose(const glm::vec3& center, const BrushSettings& brush, const glm::vec3& mirror);
        void CatchBoundary(const glm::vec3& center, const BrushSettings& brush, const glm::vec3& mirror);
        void ClothDab(const glm::vec3& center, const BrushSettings& brush, const glm::vec3& move);

        // A grid of the vertices (cells at least as big as the longest edge, so a triangle crossing a cell has its
        // corners in that cell or the ones around it): brushes and rays only look near where they are. Built when first
        // needed, kept up as vertices move, rebuilt when an edge outgrows the cells.
        mutable bool m_GridOk = false;
        mutable float m_Cell = 1.0f, m_MaxEdge = 0.0f;
        mutable glm::vec3 m_Lo{ 0.0f }, m_Hi{ 0.0f }; // the vertices' bounds (grown as they move, never shrunk)
        mutable std::unordered_map<uint64_t, std::vector<uint32_t>> m_Grid;
        mutable std::vector<uint64_t> m_VertKey;
        mutable std::vector<uint32_t> m_TriStamp;
        mutable uint32_t m_StampId = 0;
        void EnsureGrid() const;
        uint64_t CellKey(const glm::ivec3& c) const;
        glm::ivec3 CellOf(const glm::vec3& p) const;
        void GridMoved(uint32_t v); // after Positions[v] changed
        void NoteEdges(uint32_t v); // its edges' lengths (the grid is rebuilt when one is longer than a cell)
        void VertsNear(const glm::vec3& c, float r, std::vector<uint32_t>& out) const; // live vertices within r
        uint32_t NextStamp() const;

        void ApplyBrush(const glm::vec3& center, const BrushSettings& brush, const glm::vec3& move, const glm::vec3& mirror);
        void Retopologize(const glm::vec3& center, float radius);
        bool Split(uint32_t a, uint32_t b);
        bool Collapse(uint32_t a, uint32_t b, float maxLen);
        uint32_t AddVertex(const glm::vec3& p, const glm::vec3& n, const glm::vec2& uv, float mask);
        uint32_t AddTri(uint32_t a, uint32_t b, uint32_t c, int32_t material, const std::array<glm::vec2, 3>& uv = {});
        void KillTri(uint32_t t);
        void UpdateNormals(const std::vector<uint32_t>& verts);
        bool IsBorder(uint32_t v) const;
        std::vector<uint32_t> Neighbors(uint32_t v) const;
    };

    // Voxel Remesh: a new, even, all-quad surface over the mesh's volume, made from a grid of `voxelSize` cells (its
    // inside found by counting crossings, so the mesh should be closed). `smooth` relaxing passes follow. `sharp` keeps
    // corners and creases (dual contouring: each vertex where the surface's planes in its cell meet, and left out of the
    // relaxing) instead of rounding them off. Material slots, modifiers and the name are kept; faces come out smooth;
    // UVs are lost.
    EditableMesh VoxelRemesh(const EditableMesh& m, float voxelSize, int smooth = 2, bool sharp = false);
}
