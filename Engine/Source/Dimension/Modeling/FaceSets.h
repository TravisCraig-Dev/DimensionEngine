#pragma once
#include "Dimension/Modeling/EditableMesh.h"

// Face sets (Blender's Sculpt Mode face sets): faces grouped by an id (Face::FaceSet, 0 none) to show, hide, isolate
// and mask parts of a model by, and to keep brushes inside one part (Sculpt::BrushSettings::AutoMaskFaceSet).
namespace Dimension::Modeling::FaceSets
{
    // An id no face uses yet (one more than the largest, at least 1).
    int32_t Next(const EditableMesh& m);
    // The ids in use (not 0), in order.
    std::vector<int32_t> Used(const EditableMesh& m);
    // The overlay color of a face set (0: none, white). `seed` shuffles them (Randomize Colors).
    glm::vec3 Color(int32_t set, uint32_t seed = 0);

    // The faces given (hidden ones left out) become a new face set. Returns its id (0: no faces).
    int32_t Assign(EditableMesh& m, const std::vector<uint32_t>& faces);
    // A new face set of the visible faces whose vertices are all masked at least `threshold` (Face Set from Masked).
    int32_t FromMasked(EditableMesh& m, float threshold = 0.5f);
    int32_t FromVisible(EditableMesh& m);   // all visible faces
    int32_t FromSelection(EditableMesh& m); // the faces selected in Edit Mode

    // Init Face Sets: every face gets a set, each set the faces reached from one another across edges that are not a
    // break: none (loose parts), a change of material, a bend sharper than `angle` degrees (normals), a UV seam, or an
    // edge marked sharp. Returns how many sets were made.
    enum class InitMode { LooseParts = 0, Materials, Normals, UVSeams, SharpEdges };
    int InitFrom(EditableMesh& m, InitMode mode, float angle = 30.0f);

    // Grow: visible faces sharing a vertex with the set join it. Shrink: the set's faces at its border take the set
    // beside them.
    void Grow(EditableMesh& m, int32_t set);
    void Shrink(EditableMesh& m, int32_t set);

    // Visibility (the faces' Hidden flags; vertices and edges follow: hidden when all their faces are).
    void Isolate(EditableMesh& m, int32_t set); // only that set shows
    // H: isolates the set; when it is all that shows already, everything shows again.
    void ToggleIsolate(EditableMesh& m, int32_t set);
    void Hide(EditableMesh& m, int32_t set);
    void RevealAll(EditableMesh& m);
    // Hides exactly the faces flagged (one per face), deselecting them; vertices and edges follow.
    void SetFacesHidden(EditableMesh& m, const std::vector<uint8_t>& hidden);
    bool AnyHidden(const EditableMesh& m);

    // The mask from a face set: every vertex outside it masked (`invert`: the set's own vertices masked instead).
    void MaskFromSet(EditableMesh& m, int32_t set, bool invert = false);
}
