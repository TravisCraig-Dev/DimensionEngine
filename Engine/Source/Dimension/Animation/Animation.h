#pragma once
#include "Dimension/Core/Base.h"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <array>

namespace Dimension
{
    struct Joint
    {
        std::string Name;
        int Parent = -1;                    // index into Skeleton::Joints, -1 for roots
        glm::vec3 Translation{ 0.0f };      // bind-pose local transform
        glm::quat Rotation{ 1, 0, 0, 0 };
        glm::vec3 Scale{ 1.0f };
        glm::mat4 InverseBind{ 1.0f };      // mesh space -> joint space at bind time
    };

    struct Skeleton
    {
        std::vector<Joint> Joints;
        std::vector<int> Order;             // joint indices, parents before children
        glm::mat4 RootTransform{ 1.0f };    // transform of non-joint ancestors above the root joints

        void ComputeOrder();
        int FindJoint(const std::string& name) const;
    };

    enum class AnimationPath : uint8_t { Translation, Rotation, Scale };
    // CubicSpline: Hermite curves through the keys with in and out tangents per key (glTF's CUBICSPLINE).
    enum class AnimationInterpolation : uint8_t { Linear, Step, CubicSpline };

    struct AnimationChannel
    {
        int Joint = 0;
        AnimationPath Path = AnimationPath::Rotation;
        AnimationInterpolation Interpolation = AnimationInterpolation::Linear;
        std::vector<float> Times;
        std::vector<glm::vec4> Values; // xyz for translation/scale, quaternion (x, y, z, w) for rotation
        std::vector<glm::vec4> InTangents, OutTangents; // CubicSpline: per key, in value units per second
    };

    struct AnimationClip
    {
        std::string Name;
        float Duration = 0.0f;
        std::vector<AnimationChannel> Channels;
    };

    // Local joint transforms.
    struct Pose
    {
        std::vector<glm::vec3> Translations;
        std::vector<glm::quat> Rotations;
        std::vector<glm::vec3> Scales;

        void SetBindPose(const Skeleton& skeleton);
    };

    // Per-Animator state for root motion between frames.
    struct RootMotionState
    {
        bool Primed = false;
        glm::vec3 LastRoot{ 0.0f }; // root joint translation last frame (pose space)
        float LastTime = 0.0f;      // base motion time last frame (detects loop wrap-around)
        std::string LastMotion;     // base clip / blend-tree key last frame (a switch restarts tracking)
        float LastYaw = 0.0f;       // the root's heading last frame (radians, model space), when turning is taken too
    };

    // The standard human bones (as Unity's HumanBodyBones, without fingers, eyes and jaw).
    enum class HumanBone : int32_t
    {
        Hips, Spine, Chest, UpperChest, Neck, Head,
        LeftShoulder, LeftUpperArm, LeftLowerArm, LeftHand, RightShoulder, RightUpperArm, RightLowerArm, RightHand,
        LeftUpperLeg, LeftLowerLeg, LeftFoot, LeftToes, RightUpperLeg, RightLowerLeg, RightFoot, RightToes,
        Count
    };

    // A skeleton's joint for each human bone (-1 = none): its avatar.
    struct HumanoidMap
    {
        std::array<int, (size_t)HumanBone::Count> Joints;
        HumanoidMap() { Joints.fill(-1); }
        int operator[](HumanBone bone) const { return Joints[(size_t)bone]; }
        int Mapped() const;      // how many human bones it has
        bool IsHumanoid() const; // hips, spine, head, arms and legs (upper, lower, hand / foot) all found
    };

    namespace Animation
    {
        // Root motion: takes the horizontal (model XZ) movement of the root joint out of the pose, pinning it at the
        // motion's start, and returns that movement since last frame in model space (y = 0; vertical motion stays
        // in the pose). `start`/`end` are the root's translations at the motion's first and last frames, so a loop
        // wrap adds a whole cycle. `motionKey` identifies the playing motion; when it changes, tracking restarts.
        glm::vec3 ExtractRootMotion(const Skeleton& skeleton, int rootJoint, Pose& pose, RootMotionState& state, const glm::vec3& start,
                                    const glm::vec3& end, float time, bool loop, const std::string& motionKey);
        // As above, also taking the root's turning about the model's vertical axis out of the pose (it keeps the start
        // frame's heading): `yawDelta` gets this frame's turn (radians, counter-clockwise seen from above), and the
        // travel is returned in the object's frame (turned as the object has). `startRotation`/`endRotation` are the
        // root's rotations at the motion's first and last frames.
        glm::vec3 ExtractRootMotion(const Skeleton& skeleton, int rootJoint, Pose& pose, RootMotionState& state, const glm::vec3& start,
                                    const glm::vec3& end, const glm::quat& startRotation, const glm::quat& endRotation, float time,
                                    bool loop, const std::string& motionKey, float& yawDelta);
        // Samples `clip` at `time` (seconds) on top of the bind pose.
        void Sample(const Skeleton& skeleton, const AnimationClip& clip, float time, Pose& out);
        // A channel's value at `time` (xyz, or a quaternion as x, y, z, w).
        glm::vec4 SampleChannel(const AnimationChannel& channel, float time);
        // Smooth keys: a CubicSpline channel through its keys with automatic tangents (each key's slope from its
        // neighbours, as Catmull-Rom; flat at the ends), rotations kept on the short way round.
        void AutoTangents(AnimationChannel& channel);
        // out = a * (1 - t) + b * t (rotations slerped).
        void Blend(const Pose& a, const Pose& b, float t, Pose& out);
        // Skinning matrices: jointWorld * inverseBind, in mesh space.
        void ComputeSkinMatrices(const Skeleton& skeleton, const Pose& pose, std::vector<glm::mat4>& out);
        // Model-space transform of every joint (RootTransform * parents * local), the space skinning works in.
        void JointGlobals(const Skeleton& skeleton, const Pose& pose, std::vector<glm::mat4>& out);
        // Inverse kinematics: turns the `chainLength` ancestors of `tip` so that the tip reaches `target` (model space).
        // Two joints use an analytic solve that bends toward `hint` (a pole position; null keeps the current bend
        // plane); other lengths use cyclic coordinate descent. `weight` blends from the input pose. Returns false when
        // there is nothing to solve.
        bool SolveIK(const Skeleton& skeleton, Pose& pose, int tip, int chainLength, const glm::vec3& target, const glm::vec3* hint, float weight);
        // Look-at: turns `joint` so its `aimAxis` (in the joint's own space, e.g. +Z for a head) points at `target`
        // (model space), shared over `chainLength` joints (the joint and its ancestors, e.g. neck and spine; each takes an
        // even part of the turn). The turn is limited to `maxAngle` degrees. Returns false when there is nothing to do.
        bool SolveLookAt(const Skeleton& skeleton, Pose& pose, int joint, const glm::vec3& aimAxis, int chainLength, const glm::vec3& target,
                         float maxAngle, float weight);
        const AnimationClip* FindClip(const std::vector<AnimationClip>& clips, const std::string& name);

        // Retargeting: plays a clip made for one skeleton on another whose joints have the same names (ignoring case
        // and prefixes such as "mixamorig:" or "Armature|"). Rotations carry over as changes from each skeleton's bind
        // pose; translations only for root joints, scaled by the ratio of the skeletons' root heights (so a tall and a
        // short character walk at their own stride); the other joints keep the target's proportions. Joints the
        // target lacks are skipped.
        int MatchJoint(const Skeleton& skeleton, const std::string& name);
        AnimationClip RetargetClip(const Skeleton& source, const Skeleton& target, const AnimationClip& clip);

        // Humanoid avatars: a skeleton's joints matched to the standard human bones by their names, whatever the rig's
        // naming convention (Mixamo "LeftUpLeg", Unreal "thigh_l", Blender "thigh.L", 3ds Max "Bip01 L Thigh", ...):
        // twist, roll, end and finger joints are left out, and spine joints are taken in order (Spine, Chest, Upper
        // Chest). A skeleton is humanoid when it has hips, a spine, a head and both arms and legs (shoulders, chest,
        // upper chest, neck and toes are optional).
        HumanoidMap MapHumanoid(const Skeleton& skeleton);
        const char* HumanBoneName(HumanBone bone);
        // Retargeting between humanoids built differently (other names, joint axes, proportions, extra joints): each
        // human bone turns in the model's space as the source's turns from its bind pose, the hips travel scaled by the
        // ratio of the hips' heights, and the clip is resampled at 30 frames a second. Joints that are no human bone
        // (fingers, tails) carry over by name as RetargetClip does. Falls back to RetargetClip when either skeleton is
        // not humanoid. With muscle limits each human bone stays within a human range of motion (HumanMuscleLimit)
        // from its T-pose, turned relative to its parent bone; a clamped bone carries its children along.
        AnimationClip RetargetHumanoid(const Skeleton& source, const Skeleton& target, const AnimationClip& clip, bool muscleLimits = true);

        // A human bone's range of motion (degrees, from the T-pose, relative to its parent human bone), as Unity's
        // muscles: Front, its tip swinging forward (+) or back (for feet and toes: up (+) or down); Side, its tip swinging
        // up (+) or down for shoulders and arms, out to its own side (+) or in for legs and feet, left (+) or right for the
        // spine, neck and head; Twist, about the bone itself.
        // Bend limits the whole swing instead (elbows and wrists, whose bending way depends on how the rig was bound).
        struct MuscleLimit
        {
            glm::vec2 Front{ -180.0f, 180.0f }, Side{ -180.0f, 180.0f }, Twist{ -180.0f, 180.0f };
            float Bend = 180.0f;
        };
        MuscleLimit HumanMuscleLimit(HumanBone bone);
        // A turn of a bone relative to its parent (in the T-pose's frame: left +X, up +Y, forward +Z) kept within the
        // bone's limit (unchanged when already inside it).
        glm::quat ClampMuscle(HumanBone bone, const glm::quat& turn);

        // Animator layers. A mask gives each joint a weight: 1 for the listed joints and all their descendants,
        // 0 elsewhere (an empty list covers the whole skeleton).
        void BuildMask(const Skeleton& skeleton, const std::vector<std::string>& joints, std::vector<float>& weights);
        // Override: base moves toward the layer pose by weight * mask.
        void LayerOverride(Pose& base, const Pose& layer, const std::vector<float>& mask, float weight);
        // Additive: adds the layer's difference from `reference` (its motion's first frame) to base.
        void LayerAdditive(Pose& base, const Pose& layer, const Pose& reference, const std::vector<float>& mask, float weight);
    }
}
