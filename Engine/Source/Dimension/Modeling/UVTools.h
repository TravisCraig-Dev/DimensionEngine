#pragma once
#include "Dimension/Modeling/EditableMesh.h"

// UV editing on an EditableMesh's per-corner UVs (Blender's UV menu and UV Editor). UVs follow the engine's texture
// convention: (0, 0) is the image's top-left corner and v grows downwards. Each operation takes the faces to work on
// (usually SelectedFaces); corners' UV selection and pins are the faces' UVFlags.
namespace Dimension::Modeling::UV
{
    // A face corner: face index and corner index (into the face's Verts).
    struct Corner
    {
        uint32_t Face = 0, Index = 0;
        bool operator==(const Corner& o) const { return Face == o.Face && Index == o.Index; }
    };

    // The selected faces that are not hidden (what the UV Editor shows and the UV menu works on).
    std::vector<uint32_t> SelectedFaces(const EditableMesh& m);
    // UV islands among `faces`: groups of faces joined across edges whose two faces have the same UVs at both ends.
    std::vector<std::vector<uint32_t>> Islands(const EditableMesh& m, const std::vector<uint32_t>& faces);

    // ---- Unwrapping and projections

    enum class UnwrapMethod
    {
        AngleBased = 0,     // ABF++: the flat triangles' angles solved as close to the surface's as can be, then laid out
                            // from them (LSCM on the solved shapes); closed charts fall back to MinimumStretch
        Conformal = 1,      // least squares conformal maps (LSCM): angles kept, areas may grow apart
        MinimumStretch = 2, // conformal start refined towards rigid (as-rigid-as-possible): angles and areas both kept
    };
    struct UnwrapSettings
    {
        UnwrapMethod Method = UnwrapMethod::AngleBased;
        bool UsePins = true; // pinned UVs stay where they are; their charts are flattened around them and not packed
        bool Pack = true;    // the charts are packed into the 0..1 square afterwards
        float Margin = 0.01f; // the gap between packed charts (UV units)
        int Iterations = 12;  // angle based: Newton steps (plus 8); minimum stretch: refinement steps
    };
    // Flattens `faces`, cut along seams (and where the faces end) into charts, each with as little distortion as can be.
    // Returns the number of charts.
    int Unwrap(EditableMesh& m, const std::vector<uint32_t>& faces, const UnwrapSettings& settings = {});
    // Smart UV Project: faces grouped by the direction they face (within `angleLimit` degrees), each group's connected
    // parts projected flat along it, then packed. Returns the number of charts.
    int SmartProject(EditableMesh& m, const std::vector<uint32_t>& faces, float angleLimit = 66.0f, float margin = 0.01f);
    // Each face projected along the box side it faces most; `cubeSize` model units span the whole texture, centered on
    // the faces' bounds.
    void CubeProject(EditableMesh& m, const std::vector<uint32_t>& faces, float cubeSize = 1.0f);
    // Wrapped around an axis (0 X, 1 Y, 2 Z of the model) through the faces' center: u around it, v along it (cylinder)
    // or from pole to pole (sphere).
    void CylinderProject(EditableMesh& m, const std::vector<uint32_t>& faces, int axis = 1);
    void SphereProject(EditableMesh& m, const std::vector<uint32_t>& faces, int axis = 1);
    // As seen through `modelToClip` (the camera's projection x view x the object's transform), with the view's width
    // over height `aspect`. `scaleToBounds`: stretched to fill 0..1, else the view's own square around its center.
    void ProjectFromView(EditableMesh& m, const std::vector<uint32_t>& faces, const glm::mat4& modelToClip, float aspect,
                         bool scaleToBounds = true);
    // Each face fills the whole texture (quads the square; other faces a regular polygon inside it).
    void Reset(EditableMesh& m, const std::vector<uint32_t>& faces);

    // ---- Islands

    // Packs the islands of `faces` into the 0..1 square: each turned to its smallest bounding box (`rotate`), sizes
    // kept relative to each other, `margin` apart.
    void PackIslands(EditableMesh& m, const std::vector<uint32_t>& faces, float margin = 0.01f, bool rotate = true);
    // Scales each island about its center so they all have the same texel density (UV area per surface area).
    void AverageIslandScale(EditableMesh& m, const std::vector<uint32_t>& faces);
    // Marks seams on the edges between faces of `faces` whose UVs don't meet (the islands' borders). Returns the count.
    size_t SeamsFromIslands(EditableMesh& m, const std::vector<uint32_t>& faces);
    // Minimize Stretch: the islands with selected UVs (all when none is selected) relaxed towards their surface's true
    // shape, keeping their place and size (pinned UVs stay).
    void Relax(EditableMesh& m, const std::vector<uint32_t>& faces, int iterations = 10);

    enum class StretchMode { Angle = 0, Area = 1 };
    // Per face of `faces`: how distorted its UVs are, 0 (true to the surface) .. 1 (badly; flipped faces count as 1).
    std::vector<float> FaceStretch(const EditableMesh& m, const std::vector<uint32_t>& faces, StretchMode mode);

    // ---- Selection (per corner)

    enum class Sticky
    {
        Location = 0, // corners of the same vertex with the same UV select together (Blender's Shared Location)
        Disabled = 1, // corners select alone
        Vertex = 2,   // every corner of the same vertex
    };
    bool IsSelected(const EditableMesh& m, Corner c);
    std::vector<Corner> SelectedCorners(const EditableMesh& m, const std::vector<uint32_t>& faces);
    void SelectAll(EditableMesh& m, const std::vector<uint32_t>& faces, bool select = true);
    void InvertSelection(EditableMesh& m, const std::vector<uint32_t>& faces);
    // Selects or deselects a corner and the corners that stick to it.
    void SelectCorner(EditableMesh& m, const std::vector<uint32_t>& faces, Corner c, bool select, Sticky sticky);
    // Adds the corners sticking to selected ones (after selecting corners one by one).
    void ExpandSticky(EditableMesh& m, const std::vector<uint32_t>& faces, Sticky sticky);
    // Whole islands of the selection (or the island of `seed` when given).
    void SelectLinked(EditableMesh& m, const std::vector<uint32_t>& faces, const Corner* seed = nullptr, bool select = true);
    // Split Selection: only the corners of fully selected faces stay selected (so moving them tears those faces off).
    void SplitSelection(EditableMesh& m, const std::vector<uint32_t>& faces);
    void SetPinned(EditableMesh& m, const std::vector<uint32_t>& faces, bool pinned); // the selected corners
    bool IsPinned(const EditableMesh& m, Corner c);

    // ---- Changing the selected UVs

    // The selected UVs' bounds center (false when none is selected).
    bool SelectionCenter(const EditableMesh& m, const std::vector<uint32_t>& faces, glm::vec2& center);
    // Moves the selected UVs by an affine 2D transform (column-major 3x3, last row 0 0 1).
    void TransformSelected(EditableMesh& m, const std::vector<uint32_t>& faces, const glm::mat3& transform);
    // Mirrors the selected UVs across their center: axis 0 flips u, 1 flips v.
    void MirrorSelected(EditableMesh& m, const std::vector<uint32_t>& faces, int axis);
    // Merge at center (all selected UVs to one point).
    void WeldSelected(EditableMesh& m, const std::vector<uint32_t>& faces);
    // Merges selected UVs closer than `distance` (each cluster to its center). Returns how many UV points went.
    size_t MergeByDistance(EditableMesh& m, const std::vector<uint32_t>& faces, float distance);
    // Lines the selected UVs up: axis 0 gives them all the same u (a vertical line), 1 the same v (horizontal).
    void AlignSelected(EditableMesh& m, const std::vector<uint32_t>& faces, int axis);
    // Rounds the selected UVs to the nearest pixel corner of a `size` texture.
    void SnapToPixels(EditableMesh& m, const std::vector<uint32_t>& faces, glm::ivec2 size);

    // Follow Active Quads: the quads among `faces` connected to `active` (a quad; -1: the first fully UV-selected quad,
    // else the first quad) laid out from its UVs as a grid, each step across an edge continuing the one before.
    // `evenLength` gives every step the same length; else steps follow the edges' lengths on the surface. Returns how
    // many faces were laid out.
    size_t FollowActiveQuads(EditableMesh& m, const std::vector<uint32_t>& faces, int64_t active = -1, bool evenLength = false);
    // Stitch: corners of the same vertex selected at different places are joined. `snapIslands` first moves (and turns)
    // each island onto the one it is stitched to (the biggest stays), so only the seam closes; else they meet halfway.
    // Returns how many vertices were stitched.
    size_t Stitch(EditableMesh& m, const std::vector<uint32_t>& faces, bool snapIslands = true);

    // UV Sync Selection: the UV Editor's selection as the mesh's own. FromMesh selects the corners of selected vertices;
    // ToMesh selects the vertices with a selected corner (edges and faces following).
    void SyncSelectionFromMesh(EditableMesh& m, const std::vector<uint32_t>& faces);
    void SyncSelectionToMesh(EditableMesh& m, const std::vector<uint32_t>& faces);
    // The faces that are not hidden (what the UV Editor shows with sync selection).
    std::vector<uint32_t> VisibleFaces(const EditableMesh& m);
}
