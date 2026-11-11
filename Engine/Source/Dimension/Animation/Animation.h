#pragma once
#include "Dimension/Core/Base.h"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <array>
#include <functional>

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
        // Blend shape weights (glTF morph target animations): one channel per shape, Joint = the shape's index in the
        // model's blend shapes, Values[k].x its weight (0..1), Path Translation (plain values). Kept apart from the
        // joint channels.
        std::vector<AnimationChannel> WeightChannels;
        // Object properties (like Unity's clips keying any component field): a component's numeric field on the
        // Animator's object, or on one under it by name path ("Lamp/Bulb"), as up to four floats (Curve.Values).
        struct PropertyChannel
        {
            std::string Path, Component, Field;
            AnimationChannel Curve; // (Path Translation: plain values)
        };
        std::vector<PropertyChannel> PropertyChannels;
        // Animation events (like Unity's on a clip): at a time (seconds), the function scripts get in OnAnimationEvent.
        struct Event
        {
            float Time = 0.0f;
            std::string Function;
        };
        std::vector<Event> Events;
        // The events a playhead passes going from `from` to `to` (seconds; with `loop`, across the wrap when `to` is
        // below `from`), in order.
        void EventsBetween(float from, float to, bool loop, std::vector<std::string>& out) const;
    };

    // Local joint transforms.
    struct Pose
    {
        std::vector<glm::vec3> Translations;
        std::vector<glm::quat> Rotations;
        std::vector<glm::vec3> Scales;
        // Blend shape weights (0..1) by shape index; NaN = not animated (the renderer's own weight stays).
        std::vector<float> Weights;

        void SetBindPose(const Skeleton& skeleton);
    };

    // Scripts' IK goals for a humanoid (Unity's SetIKPosition / SetIKRotation / SetLookAtPosition): its feet and hands
    // (AvatarIKGoal order: left foot, right foot, left hand, right hand) and where the head looks.
    struct HumanoidIKGoals
    {
        struct Limb
        {
            float PositionWeight = 0.0f, RotationWeight = 0.0f;
            glm::vec3 Position{ 0.0f };
            glm::quat Rotation{ 1.0f, 0.0f, 0.0f, 0.0f };
            // Its hint (Unity's AvatarIKHint: the knee or elbow points toward it), by weight from where it bends now.
            float HintWeight = 0.0f;
            glm::vec3 Hint{ 0.0f };
        };
        std::array<Limb, 4> Limbs;
        float LookAtWeight = 0.0f;
        glm::vec3 LookAt{ 0.0f };
        bool Any() const
        {
            for (const Limb& l : Limbs)
                if (l.PositionWeight > 0.0f || l.RotationWeight > 0.0f) return true;
            return LookAtWeight > 0.0f;
        }
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
        // Keyframe reduction (like Unity's animation compression): drops the keys of a linear channel that the line
        // between the keys kept passes within a tolerance (rotations: `rotationDegrees` of turn; others: `distance`),
        // Ramer-Douglas-Peucker style, so every original key stays within it. The first and last keys stay; step and
        // spline channels are left alone. Returns how many keys went.
        int ReduceKeys(AnimationChannel& ch, float rotationDegrees, float distance);
        int ReduceKeys(AnimationClip& clip, float rotationDegrees, float distance);
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
        // A humanoid's IK goals (model space) on a pose: each limb's upper and lower bone turned so its hand or foot reaches
        // the goal (by its position weight; the knee or elbow bends toward its hint, else keeps its bend plane), then the hand or foot turned to the
        // goal's rotation (by its rotation weight), then the head (with neck and chest) turned toward the look-at point.
        // Returns false when the skeleton is not humanoid.
        bool ApplyHumanoidIK(const Skeleton& skeleton, const HumanoidMap& map, Pose& pose, const HumanoidIKGoals& goals);
        // Foot IK on uneven ground (Unity's Foot IK with ground alignment): each foot's ground found by `ground` (a ray
        // straight down from `reach` above the foot, world space: true with the hit point and normal), the foot moved to
        // stand on it as the animation stood on the object's own level (its lift above that kept), the hips lowered when
        // a foot must go below where the leg reaches, and each foot tilted to the ground's slope (up to 40 degrees).
        // `world` is the object's transform. Returns false when the skeleton is not humanoid or no ground was found.
        using GroundQuery = std::function<bool(const glm::vec3& origin, float distance, glm::vec3& point, glm::vec3& normal)>;
        bool PlantFeet(const Skeleton& skeleton, const HumanoidMap& map, Pose& pose, const glm::mat4& world, float reach, const GroundQuery& ground);
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
