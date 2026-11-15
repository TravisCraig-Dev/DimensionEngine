#pragma once
#include "Dimension/Modeling/EditableMesh.h"
#include <functional>

namespace Dimension::Modeling
{
    // The mesh after its enabled modifiers, in order (the result has none).
    EditableMesh EvaluateModifiers(const EditableMesh& mesh);
    // One modifier's result on a mesh (disabled ones apply too).
    EditableMesh ApplyModifier(const EditableMesh& mesh, const Modifier& modifier);

    // How a Boolean modifier finds the other model by its asset id (set by the asset manager; without one, Booleans
    // change nothing). The mesh given back should have its own modifiers applied.
    using MeshResolver = std::function<bool(uint64_t asset, EditableMesh& out)>;
    void SetMeshResolver(MeshResolver resolver);

    // Building blocks (also usable on their own).
    namespace ModifierOps
    {
        // One Catmull-Clark step: every face becomes quads around its middle (n-gons too); creased edges stay sharp
        // as far as their crease, open borders keep their outline.
        EditableMesh SubdivideSmooth(const EditableMesh& mesh, bool useCreases = true);
        // Solid shapes: 0 difference (a minus b), 1 union, 2 intersection. b is in a's space; the result keeps both
        // sides' UVs and materials (b's slots joined to a's by name).
        EditableMesh BooleanOp(const EditableMesh& a, const EditableMesh& b, int operation);
        // Per-vertex normals: the average of the faces around each vertex.
        std::vector<glm::vec3> VertexNormals(const EditableMesh& mesh);
        // Smooth 3D value noise, 0..1.
        float Noise(const glm::vec3& p);
    }
}
