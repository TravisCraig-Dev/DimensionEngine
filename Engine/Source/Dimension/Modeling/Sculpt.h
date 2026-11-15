#pragma once
#include "Dimension/Modeling/EditableMesh.h"

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
        Count
    };
    const char* BrushName(Brush b);

    struct BrushSettings
    {
        Brush Type = Brush::Draw;
        float Radius = 0.1f;   // model units
        float Strength = 0.5f; // 0..1
        bool Invert = false;
        bool FrontFacesOnly = true;      // vertices facing away from the view are left alone
        glm::vec3 ViewDir{ 0, 0, -1 };   // the camera's looking direction (model space)
        uint8_t Symmetry = 0;            // bits: 1 X, 2 Y, 4 Z (mirrored across the model's own planes)
    };

    // A triangle mesh being sculpted: vertices know their triangles, so brushes and dynamic topology work locally.
    class SculptMesh
    {
    public:
        std::vector<glm::vec3> Positions, Normals;
        std::vector<glm::vec2> UVs;        // per vertex (for the preview; the real UVs stay in the edited mesh)
        std::vector<float> Mask;           // per vertex, 0..1
        std::vector<glm::uvec3> Tris;      // counter-clockwise; dead ones are all ~0
        std::vector<int32_t> TriMaterial;  // per triangle
        std::vector<std::vector<uint32_t>> VertTris;

        static SculptMesh FromEditable(const EditableMesh& m);
        // Back into `m`: positions and the mask when the topology is as it came, else a new mesh of the triangles
        // (smooth shaded, no UVs; `m`'s name, material slots, modifiers and smoothing settings kept).
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
        std::vector<Caught> m_Caught; // Grab
        bool m_TopologyChanged = false;

        void ApplyBrush(const glm::vec3& center, const BrushSettings& brush, const glm::vec3& move, const glm::vec3& mirror);
        void Retopologize(const glm::vec3& center, float radius);
        bool Split(uint32_t a, uint32_t b);
        bool Collapse(uint32_t a, uint32_t b, float maxLen);
        uint32_t AddVertex(const glm::vec3& p, const glm::vec3& n, const glm::vec2& uv, float mask);
        uint32_t AddTri(uint32_t a, uint32_t b, uint32_t c, int32_t material);
        void KillTri(uint32_t t);
        void UpdateNormals(const std::vector<uint32_t>& verts);
        bool IsBorder(uint32_t v) const;
        std::vector<uint32_t> Neighbors(uint32_t v) const;
    };

    // Voxel Remesh: a new, even, all-quad surface over the mesh's volume, made from a grid of `voxelSize` cells (its
    // inside found by counting crossings, so the mesh should be closed). `smooth` relaxing passes follow. Material slots,
    // modifiers and the name are kept; faces come out smooth; UVs are lost.
    EditableMesh VoxelRemesh(const EditableMesh& m, float voxelSize, int smooth = 2);
}
