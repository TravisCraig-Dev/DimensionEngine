#pragma once
#include "Dimension/Modeling/EditableMesh.h"

// Rigging (Blender's armatures, vertex groups, weight painting, shape keys and actions) on an EditableMesh. The
// armature becomes the render mesh's skeleton and skin, shape keys its blend shapes and actions its clips, so the
// Animator, blend shape weights and the rest of the animation system work on models made in the editor.
namespace Dimension::Modeling::Rig
{
    // ---- Bones

    // A bone's rest transform in the model's space: origin at the head, +Y towards the tail, rolled about Y.
    glm::mat4 RestMatrix(const Bone& b);
    // Relative to its parent's rest transform.
    glm::mat4 RestLocal(const EditableMesh& m, int bone);
    // Every bone's posed transform in the model's space (rest local x the bone's pose, down the hierarchy).
    std::vector<glm::mat4> PoseMatrices(const EditableMesh& m);
    // Per bone: posed x inverse rest (moves rest-pose vertices to the pose).
    std::vector<glm::mat4> SkinMatrices(const EditableMesh& m);
    Ref<Skeleton> BuildSkeleton(const EditableMesh& m);
    // Moves a render mesh made from `m` (its Skin) into m's current pose on the CPU (a preview: the result is static).
    void PoseMeshData(MeshData& data, const EditableMesh& m);
    // Whether the pose is the rest pose.
    bool AtRest(const EditableMesh& m);
    void ClearPose(EditableMesh& m, bool selectedOnly = false);

    std::string UniqueBoneName(const EditableMesh& m, const std::string& base);
    // "Arm.L" <-> "Arm.R" (also _L/_R, .l/.r, Left/Right, L_/R_ prefixes); the same name when it has no side.
    std::string FlipSideName(const std::string& name);
    int FindBone(const EditableMesh& m, const std::string& name);
    int AddBone(EditableMesh& m, const std::string& name, const glm::vec3& head, const glm::vec3& tail, int parent = -1);
    // Removes bones (their children go to the removed bone's parent; weights and action keys follow).
    void DeleteBones(EditableMesh& m, const std::vector<int>& bones);
    // Whether making `parent` the parent of `bone` would loop.
    bool WouldLoop(const EditableMesh& m, int bone, int parent);
    // Bones on the +X side mirrored to the -X side with flipped names (Blender's Symmetrize); returns how many made.
    int Symmetrize(EditableMesh& m);

    // ---- Weights

    float GetWeight(const EditableMesh& m, uint32_t vertex, int bone);
    void SetWeight(EditableMesh& m, uint32_t vertex, int bone, float weight); // (0 removes it)
    // Each vertex's four strongest deform bones, scaled to add up to one (vertices with none keep none).
    void NormalizeWeights(EditableMesh& m);
    // Automatic weights (like Blender's bone heat): each vertex weighted by its distance to the bones it can see
    // from inside the surface, then smoothed along the mesh. Only `selected` vertices when true.
    void AutomaticWeights(EditableMesh& m, bool selectedOnly = false);
    // Weights for another mesh made from this one (by modifiers): each vertex blends the nearest source vertices'
    // weights; Mirror modifiers' mirrored halves take the mirrored side's bones (by name).
    std::vector<std::vector<VertexWeight>> TransferWeights(const EditableMesh& from, const EditableMesh& to);

    enum class WeightMode { Add = 0, Subtract, Mix, Smooth };
    struct WeightBrush
    {
        int Bone = 0;
        WeightMode Mode = WeightMode::Mix;
        float Value = 1.0f;    // Add/Subtract: amount, Mix: the weight painted towards
        float Radius = 0.1f;   // model units
        float Strength = 0.5f;
        bool FrontFacesOnly = true;
        glm::vec3 ViewDir{ 0, 0, -1 };
        uint8_t Symmetry = 0;  // mirrored strokes paint the mirrored bone (by name)
    };
    size_t PaintWeights(EditableMesh& m, const glm::vec3& center, const WeightBrush& brush);
    // Blue (0) through green to red (1).
    glm::vec4 WeightColor(float weight);

    // ---- Shape keys

    // A new key holding the basis (or the current mix with `fromMix`). Returns its index.
    int AddShapeKey(EditableMesh& m, const std::string& name, bool fromMix = false);
    void RemoveShapeKey(EditableMesh& m, int key);
    // Edit a key's positions in Positions (-1: the basis).
    void SetEditedShape(EditableMesh& m, int key);
    // The positions with every key's Value mixed in (relative to the basis).
    std::vector<glm::vec3> MixedPositions(const EditableMesh& m);

    // ---- Actions

    int AddAction(EditableMesh& m, const std::string& name);
    // Keys the bones' current poses at `time` (only the selected ones with `selectedOnly`; into a key already there),
    // and the shape keys' values with `shapes`.
    void InsertKey(EditableMesh& m, int action, float time, bool selectedOnly = false, bool shapes = false);
    // Moves the key at `from` to `to` (merging into one there). Returns false when there's none at `from`.
    bool MoveKey(EditableMesh& m, int action, float from, float to);
    void DeleteKey(EditableMesh& m, int action, float time);
    // Sets the bones' poses to the action at `time` (between keys: interpolated).
    void EvaluateAction(EditableMesh& m, int action, float time);
    AnimationClip ToClip(const EditableMesh& m, const Action& action);

    // ---- IK

    // Turns `bone` and `chain - 1` of its parents (cyclic coordinate descent) so its tail reaches `target` (model
    // space). Returns how far the tail ends up from it.
    float SolveIK(EditableMesh& m, int bone, const glm::vec3& target, int chain, int iterations = 256);
    // The bones' IK constraints (each reaching for its target bone's posed head).
    void ApplyConstraints(EditableMesh& m);
}
