#pragma once
#include "Dimension/Modeling/MeshOps.h"

// Modeling tools that change a mesh's topology (Blender's Edit Mode operators): extrude, inset, bevel, loop cut,
// knife, bridge, fill, dissolve, subdivide, spin, mirror, separate and join. Each works on the selection, skips hidden
// elements and leaves what it made selected (call FlushSelection for the select mode afterwards).
namespace Dimension::Modeling::Ops
{
    // ---- Extrude and duplicate

    // Extrudes the selection: selected faces as connected regions (each face on its own with `individual`), selected
    // edges that no selected face uses into quads, and selected vertices on no selected edge into wire edges. The new
    // geometry lies on top of the old, selected (the old is not): move it afterwards. Returns the regions' average
    // normal (zero when no face was extruded).
    glm::vec3 Extrude(EditableMesh& m, bool individual = false);
    // Copies the selected faces, edges and vertices; the copy is selected, the original not.
    void Duplicate(EditableMesh& m);
    // Each selected vertex's direction for Shrink/Fatten (Alt+S): the average normal of its selected faces (of all
    // its faces when none is selected), unit length; zero for vertices not selected.
    std::vector<glm::vec3> ShrinkFattenDirections(const EditableMesh& m);

    // ---- Inset, bevel

    // Insets the selected faces: a border of `thickness` inside each region (each face with `individual`), the inner
    // faces raised by `depth` along their normal. The inner faces stay selected.
    void Inset(EditableMesh& m, float thickness, float depth = 0.0f, bool individual = false);
    // Bevels the selected edges between two faces (the selected vertices with `vertices`): `width` is the distance
    // from the old edge to the new ones across each face, `segments` rounds the bevel. The new faces are selected.
    // `clamp` (Blender's Clamp Overlap): the width limited so the new edges can't pass each other (half the shortest
    // edge next to the bevel).
    void Bevel(EditableMesh& m, float width, uint32_t segments = 1, bool vertices = false, bool clamp = true);

    // ---- Cutting

    // Cuts `cuts` evenly spaced loops across the ring of quads through `edge` (the faces at either end of the ring
    // gain corners). `slide` (-1..1) shifts the cuts along the ring's edges. The new loops are selected.
    bool LoopCut(EditableMesh& m, uint32_t edge, uint32_t cuts = 1, float slide = 0.0f);
    // Where each selected vertex goes when an edge slide reaches -1 or +1: along the edges beside the selected edges
    // (a loop slides between its two neighbouring loops).
    struct SlideRails
    {
        std::vector<uint32_t> Verts;
        std::vector<glm::vec3> Start, Minus, Plus; // positions at 0, -1 and +1
    };
    SlideRails EdgeSlideRails(const EditableMesh& m);
    void ApplySlide(EditableMesh& m, const SlideRails& rails, float factor);

    // A knife point: on vertex A (A == B), on edge A-B at T (0 at A, 1 at B), or inside face Face at Position.
    struct CutPoint
    {
        uint32_t A = 0, B = 0;
        float T = 0.0f;
        int32_t Face = -1;          // >= 0: a point inside that face
        glm::vec3 Position{ 0.0f }; // (model space)
    };
    // Cuts along a path of points: each point on an edge splits it, consecutive points that share a face split that
    // face, and points inside a face between two points on its border split it along the zigzag through them (inside
    // points with no border point on both sides are left out). The path's vertices and new edges are selected.
    // Returns the number of faces split.
    size_t Knife(EditableMesh& m, const std::vector<CutPoint>& path);
    // Connect Vertex Path (J): splits faces between their selected vertices. Returns the number of faces split.
    size_t ConnectVertices(EditableMesh& m);

    // ---- Making faces

    // Make Edge/Face (F): closed loops of selected border or wire edges become faces; otherwise two selected vertices
    // make an edge and three or more make one face around them. Returns whether anything was made.
    bool Fill(EditableMesh& m);
    // Joins two selected edge loops (open or closed) with faces; selected faces inside them are removed first.
    bool BridgeEdgeLoops(EditableMesh& m);

    // ---- Dissolve

    // Removes the selected vertices, joining the faces around each into one.
    void DissolveVertices(EditableMesh& m);
    // Removes the selected edges, joining the faces on either side (wire edges go).
    void DissolveEdges(EditableMesh& m);
    // Joins each connected group of selected faces into one face.
    void DissolveFaces(EditableMesh& m);
    // Joins neighbouring faces that meet at less than `angleDegrees` (only selected edges when any is selected) and
    // removes vertices left between two nearly straight edges.
    void LimitedDissolve(EditableMesh& m, float angleDegrees = 5.0f);

    // ---- Subdivide, spin, mirror

    // Splits each selected edge into `cuts` + 1; fully selected quads and triangles become grids, other fully selected
    // faces fans of quads around their center (one cut), and quads with two opposite selected edges strips.
    void Subdivide(EditableMesh& m, uint32_t cuts = 1);
    // Extrudes the selection `steps` times around `axis` through `center` (model space), `angle` radians in all. A full
    // turn welds the last copy to the start.
    void Spin(EditableMesh& m, const glm::vec3& center, const glm::vec3& axis, float angle, uint32_t steps);
    // Mirrors the selected vertices across the plane through `pivot` with normal `axis` (model space); fully selected
    // faces turn over so they keep facing out.
    void Mirror(EditableMesh& m, const glm::vec3& pivot, const glm::vec3& axis);

    // ---- Separate and join

    // Takes the selected faces (and selected wire edges and lone vertices) out of `m` into a new mesh.
    EditableMesh Separate(EditableMesh& m);
    // Splits the mesh into its connected parts: `m` keeps the first, the others are returned.
    std::vector<EditableMesh> SeparateLooseParts(EditableMesh& m);
    // Appends `other` moved by `transform` (faces turn over when it mirrors). Returns the first new vertex's index.
    uint32_t Join(EditableMesh& m, const EditableMesh& other, const glm::mat4& transform);

    // ---- Helpers

    // Selects faces with their vertices and edges (for any select mode's flush).
    void SelectFacesFully(EditableMesh& m, const std::vector<uint32_t>& faces);
    // The UV a face would have at a point of its plane (affine from its corners).
    glm::vec2 FaceUVAt(const EditableMesh& m, uint32_t f, const glm::vec3& p);
}
