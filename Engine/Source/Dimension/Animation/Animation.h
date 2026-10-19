#pragma once
#include "Dimension/Core/Base.h"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

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
    enum class AnimationInterpolation : uint8_t { Linear, Step };

    struct AnimationChannel
    {
        int Joint = 0;
        AnimationPath Path = AnimationPath::Rotation;
        AnimationInterpolation Interpolation = AnimationInterpolation::Linear;
        std::vector<float> Times;
        std::vector<glm::vec4> Values; // xyz for translation/scale, quaternion (x, y, z, w) for rotation
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

    namespace Animation
    {
        // Samples `clip` at `time` (seconds) on top of the bind pose.
        void Sample(const Skeleton& skeleton, const AnimationClip& clip, float time, Pose& out);
        // out = a * (1 - t) + b * t (rotations slerped).
        void Blend(const Pose& a, const Pose& b, float t, Pose& out);
        // Skinning matrices: jointWorld * inverseBind, in mesh space.
        void ComputeSkinMatrices(const Skeleton& skeleton, const Pose& pose, std::vector<glm::mat4>& out);
        const AnimationClip* FindClip(const std::vector<AnimationClip>& clips, const std::string& name);

        // Animator layers. A mask gives each joint a weight: 1 for the listed joints and all their descendants,
        // 0 elsewhere (an empty list covers the whole skeleton).
        void BuildMask(const Skeleton& skeleton, const std::vector<std::string>& joints, std::vector<float>& weights);
        // Override: base moves toward the layer pose by weight * mask.
        void LayerOverride(Pose& base, const Pose& layer, const std::vector<float>& mask, float weight);
        // Additive: adds the layer's difference from `reference` (its motion's first frame) to base.
        void LayerAdditive(Pose& base, const Pose& layer, const Pose& reference, const std::vector<float>& mask, float weight);
    }
}
