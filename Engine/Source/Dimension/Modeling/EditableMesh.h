#pragma once
#include "Dimension/Core/Base.h"
#include "Dimension/Renderer/Mesh.h"
#include "Dimension/Modeling/Modifier.h"
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <unordered_map>

namespace Dimension::Modeling
{
    // Element flags (vertices, edges, faces).
    enum ElementFlag : uint8_t
    {
        Selected = 1,
        Hidden = 2,
    };

    // An edge by its two vertices, order-free: (min << 32) | max.
    using EdgeKey = uint64_t;
    inline EdgeKey MakeEdgeKey(uint32_t a, uint32_t b)
    {
        if (a > b) std::swap(a, b);
        return ((uint64_t)a << 32) | b;
    }
    inline uint32_t EdgeA(EdgeKey k) { return (uint32_t)(k >> 32); }
    inline uint32_t EdgeB(EdgeKey k) { return (uint32_t)(k & 0xFFFFFFFFu); }

    // A face corner's UV state in the UV Editor (Face::UVFlags).
    enum UVFlag : uint8_t
    {
        UVSelected = 1,
        UVPinned = 2, // unwrapping keeps it where it is
    };

    // A polygon: its corners' vertices in counter-clockwise order seen from the front, with a UV per corner.
    struct Face
    {
        std::vector<uint32_t> Verts;
        std::vector<glm::vec2> UVs; // one per corner (same count as Verts)
        int32_t Material = 0;       // material slot
        bool Smooth = false;        // smooth shading (else flat)
        uint8_t Flags = 0;
        std::vector<uint8_t> UVFlags; // per corner UVFlag bits (ignored unless there is one per corner)

        uint8_t CornerFlags(size_t c) const { return UVFlags.size() == Verts.size() ? UVFlags[c] : 0; }
        void SetCornerFlag(size_t c, uint8_t flag, bool on)
        {
            if (UVFlags.size() != Verts.size()) UVFlags.assign(Verts.size(), 0);
            if (on) UVFlags[c] |= flag;
            else UVFlags[c] &= (uint8_t)~flag;
        }
        // The corners in the opposite order (the face turned over), UVs and UV flags with them.
        void Reverse()
        {
            std::reverse(Verts.begin(), Verts.end());
            std::reverse(UVs.begin(), UVs.end());
            std::reverse(UVFlags.begin(), UVFlags.end());
        }
    };

    // What an edge carries beyond its vertices (only stored for edges that carry something).
    struct EdgeData
    {
        uint8_t Flags = 0;
        bool Sharp = false;   // splits smooth shading
        bool Seam = false;    // UV unwrap seam
        float Crease = 0.0f;  // subdivision surface crease (0..1)
        bool IsDefault() const { return Flags == 0 && !Sharp && !Seam && Crease == 0.0f; }
    };

    // Adjacency derived from the faces (built on demand, dropped by EditableMesh::Changed()).
    struct Topology
    {
        std::vector<EdgeKey> Edges;                      // every edge: faces' edges first (in face order), then loose ones
        std::unordered_map<EdgeKey, uint32_t> EdgeIndex; // edge key -> index in Edges
        std::vector<std::vector<uint32_t>> EdgeFaces;    // faces using each edge
        std::vector<std::vector<uint32_t>> VertFaces;    // faces using each vertex
        std::vector<std::vector<uint32_t>> VertEdges;    // edges at each vertex
        std::vector<std::vector<uint32_t>> FaceEdges;    // per face, the edge from corner i to corner i + 1
        int32_t FindEdge(uint32_t a, uint32_t b) const
        {
            auto it = EdgeIndex.find(MakeEdgeKey(a, b));
            return it == EdgeIndex.end() ? -1 : (int32_t)it->second;
        }
    };

    enum class SelectMode { Vertex = 0, Edge = 1, Face = 2 };

    // A polygon mesh for modeling (like Blender's edit mesh): n-gons with per-corner UVs, wire edges, edge attributes
    // (sharp, seam, crease), per-element selection and hiding, and material slots. Positions are in the model's own
    // space. ToMeshData triangulates it for rendering; Save/Load read and write .dmodel files.
    class EditableMesh
    {
    public:
        std::string Name;
        std::vector<glm::vec3> Positions;
        std::vector<uint8_t> VertFlags; // one per vertex
        std::vector<Face> Faces;
        std::vector<EdgeKey> LooseEdges;                // wire edges (not part of any face)
        std::unordered_map<EdgeKey, EdgeData> EdgeAttrs; // per-edge data (missing = default)
        std::vector<std::string> MaterialSlots;          // empty: one slot drawn with the renderer's material
        bool AutoSmooth = false;                          // smooth faces split where they meet at more than this
        float AutoSmoothAngle = 30.0f;                    // degrees
        std::vector<Modifier> Modifiers;                  // applied in order by ToMeshData (Modifiers.h)
        std::vector<float> Mask;                          // per vertex sculpt mask 0..1 (ignored unless one per vertex)
        bool HasActiveModifiers() const
        {
            for (const Modifier& m : Modifiers)
                if (m.Enabled) return true;
            return false;
        }

        uint32_t VertexCount() const { return (uint32_t)Positions.size(); }
        uint32_t FaceCount() const { return (uint32_t)Faces.size(); }
        uint32_t AddVertex(const glm::vec3& p, uint8_t flags = 0);
        // Adds a face (UVs empty: all zero). Returns its index.
        uint32_t AddFace(std::vector<uint32_t> verts, std::vector<glm::vec2> uvs = {}, int32_t material = 0, bool smooth = false);
        void AddLooseEdge(uint32_t a, uint32_t b);
        // Appends another mesh (its vertices renumbered after this one's). Returns the first new vertex's index.
        uint32_t Append(const EditableMesh& other);

        // Call after changing faces or edges (drops the cached topology).
        void Changed() { m_Topology.reset(); ++m_Version; }
        const Topology& Topo() const;
        uint64_t Version() const { return m_Version; }

        // Edge data and flags.
        EdgeData GetEdge(EdgeKey k) const;
        EdgeData& EditEdge(EdgeKey k) { return EdgeAttrs[k]; }
        bool IsEdgeSelected(EdgeKey k) const { return GetEdge(k).Flags & Selected; }
        bool IsEdgeHidden(EdgeKey k) const { return GetEdge(k).Flags & Hidden; }
        void SetEdgeFlag(EdgeKey k, ElementFlag flag, bool on);
        void PruneEdgeAttrs(); // drops default entries and entries for edges that no longer exist

        bool IsVertSelected(uint32_t v) const { return VertFlags[v] & Selected; }
        bool IsVertHidden(uint32_t v) const { return VertFlags[v] & Hidden; }

        // Geometry.
        glm::vec3 FaceNormal(uint32_t f) const; // Newell's normal (unit; zero for degenerate faces)
        glm::vec3 FaceCenter(uint32_t f) const;
        float FaceArea(uint32_t f) const;
        AABB Bounds() const;
        // Splits a face into triangles (ear clipping in its plane, so concave polygons work): corner indices (into the
        // face's Verts), three per triangle.
        void TriangulateFace(uint32_t f, std::vector<uint32_t>& corners) const;

        // A render mesh: faces triangulated, normals from the shading (flat faces get their own normal; smooth faces
        // share vertex normals, split at sharp edges and, with AutoSmooth, at edges sharper than its angle), one
        // sub-mesh per used material slot, tangents generated. Hidden faces are drawn too. The enabled modifiers apply
        // first (`applyModifiers` false: the mesh as it is).
        MeshData ToMeshData(bool applyModifiers = true) const;
        // An editable mesh from a render mesh: corners at the same position are welded into one vertex (UVs kept per
        // corner), then pairs of triangles that make flat, convex quads are joined (`joinQuads`). Faces whose normals
        // were smooth come out smooth.
        static EditableMesh FromMeshData(const MeshData& data, bool joinQuads = true, float weldDistance = 1e-5f);

        // .dmodel files (JSON).
        nlohmann::json ToJson() const;
        static bool FromJson(const nlohmann::json& j, EditableMesh& out);
        bool Save(const fs::path& path) const;
        static bool Load(const fs::path& path, EditableMesh& out);

    private:
        mutable std::shared_ptr<Topology> m_Topology;
        uint64_t m_Version = 0;
    };

    // Primitives (Blender's Add > Mesh), centered on the origin, Y up, every element selected, with UVs.
    namespace Primitives
    {
        EditableMesh Plane(float size = 2.0f);
        EditableMesh Cube(float size = 2.0f);
        EditableMesh Circle(uint32_t vertices = 32, float radius = 1.0f, bool fill = false);
        EditableMesh Grid(uint32_t xSubdivisions = 10, uint32_t ySubdivisions = 10, float size = 2.0f);
        EditableMesh UVSphere(uint32_t segments = 32, uint32_t rings = 16, float radius = 1.0f);
        EditableMesh IcoSphere(uint32_t subdivisions = 2, float radius = 1.0f);
        EditableMesh Cylinder(uint32_t vertices = 32, float radius = 1.0f, float depth = 2.0f, bool caps = true);
        EditableMesh Cone(uint32_t vertices = 32, float radius1 = 1.0f, float radius2 = 0.0f, float depth = 2.0f, bool caps = true);
        EditableMesh Torus(uint32_t majorSegments = 48, uint32_t minorSegments = 12, float majorRadius = 1.0f, float minorRadius = 0.25f);
    }
}
