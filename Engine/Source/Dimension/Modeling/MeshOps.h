#pragma once
#include "Dimension/Modeling/EditableMesh.h"

// Editing operations on an EditableMesh (Blender's Edit Mode tools). Each works on the selection, skips hidden
// elements, and leaves the mesh's topology marked changed.
namespace Dimension::Modeling::Ops
{
    // ---- Selection

    // Makes the selection consistent for a mode: the mode's own elements are the truth, and the others follow them
    // (vertex mode: edges and faces whose vertices are all selected; edge mode: vertices of selected edges, faces whose
    // edges are all selected; face mode: the vertices and edges of selected faces). Hidden elements are deselected.
    void FlushSelection(EditableMesh& m, SelectMode mode);
    // Switching modes: from vertices to edges or faces, elements fully selected stay selected; going down, elements
    // of selected ones are selected.
    void ConvertSelection(EditableMesh& m, SelectMode from, SelectMode to);
    void SelectAll(EditableMesh& m, SelectMode mode);
    void DeselectAll(EditableMesh& m);
    void InvertSelection(EditableMesh& m, SelectMode mode);
    bool AnySelected(const EditableMesh& m);
    void SelectVertex(EditableMesh& m, uint32_t v, bool on);
    void SelectEdge(EditableMesh& m, EdgeKey e, bool on);
    void SelectFace(EditableMesh& m, uint32_t f, bool on);
    // Everything connected to the selection (or to `seed` vertex when given), across edges.
    void SelectLinked(EditableMesh& m, SelectMode mode, int64_t seedVertex = -1, bool deselect = false);
    // Grows or shrinks the selection by one step (across edges in vertex/edge mode, across shared vertices in face mode).
    void SelectMore(EditableMesh& m, SelectMode mode);
    void SelectLess(EditableMesh& m, SelectMode mode);
    // The edges of an edge loop through `edge` (across 4-way vertices of quad meshes; along borders on open edges).
    std::vector<uint32_t> EdgeLoop(const EditableMesh& m, uint32_t edge);
    // The edges of the ring through `edge` (opposite edges across quads), and the faces between them.
    std::vector<uint32_t> EdgeRing(const EditableMesh& m, uint32_t edge, std::vector<uint32_t>* faces = nullptr);
    // Loop selection as Alt+click does it: vertex/edge mode select the edge loop, face mode the face loop (the faces
    // of the edge ring). `extend` adds to the selection (else replaces it); clicking a fully selected loop with extend
    // deselects it.
    void SelectLoop(EditableMesh& m, SelectMode mode, uint32_t edge, bool extend, bool ring = false);

    // Which elements are selected, for transforms: selected vertices (not hidden).
    std::vector<uint32_t> SelectedVertices(const EditableMesh& m);
    // The selection's center (median of selected vertices), or the bounds center when nothing is selected.
    glm::vec3 SelectionCenter(const EditableMesh& m);

    // ---- Hiding

    void HideSelected(EditableMesh& m, SelectMode mode, bool unselected = false);
    void RevealAll(EditableMesh& m, bool select = true);

    // ---- Proportional editing

    enum class Falloff { Smooth = 0, Sphere, Root, Linear, Sharp, Constant, InverseSquare };
    float FalloffWeight(Falloff f, float t); // t = distance / radius, 0..1 -> 1..0
    // Weights for every vertex: 1 on the selection, the falloff of the distance to the nearest selected vertex within
    // `radius`, 0 beyond. `connected` measures along edges (only vertices linked to the selection move).
    std::vector<float> ProportionalWeights(const EditableMesh& m, float radius, Falloff falloff, bool connected);

    // ---- Topology edits

    enum class DeleteKind { Vertices, Edges, Faces, OnlyEdgesAndFaces, OnlyFaces };
    void Delete(EditableMesh& m, DeleteKind kind);
    // Merges the selected vertices: all into one at their center (`collapse` false), or each connected group at its
    // own center (collapse). Returns the number of vertices removed.
    size_t MergeAtCenter(EditableMesh& m, bool collapse);
    size_t MergeAt(EditableMesh& m, const glm::vec3& point);
    // Welds selected vertices closer than `distance` (all vertices when `selectedOnly` is false). Returns removed count.
    size_t MergeByDistance(EditableMesh& m, float distance, bool selectedOnly = true);
    // Drops vertices no face or edge uses (only `onlyThese` when given: a flag per vertex), renumbering the rest.
    void RemoveUnusedVertices(EditableMesh& m, const std::vector<uint8_t>* onlyThese = nullptr);
    // Replaces vertex indices through `remap` (several may map to one), dropping repeated corners, faces with fewer
    // than three corners and duplicated loose edges; then removes the vertices nothing maps to.
    void RemapVertices(EditableMesh& m, const std::vector<uint32_t>& remap);

    // ---- Normals and shading

    void FlipNormals(EditableMesh& m, bool selectedOnly = true);
    // Makes neighbouring faces agree and points each connected part outward (`inside`: inward). Selected faces only
    // (all when none is selected).
    void RecalculateNormals(EditableMesh& m, bool inside = false);
    void SetSmooth(EditableMesh& m, bool smooth, bool selectedOnly = true);
    void MarkSharp(EditableMesh& m, bool sharp);
    void MarkSeam(EditableMesh& m, bool seam);
    void SetCrease(EditableMesh& m, float crease);

    // ---- Transform

    // Moves vertices by `transform` (in the mesh's space), each blended by its weight (no weights: the selection).
    void TransformVertices(EditableMesh& m, const glm::mat4& transform, const std::vector<float>* weights = nullptr);
}
