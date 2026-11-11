#pragma once
#include "Dimension/Animation/Animation.h"
#include "Dimension/Core/Guid.h"
#include <filesystem>
#include <string>
#include <vector>

namespace Dimension
{
    namespace fs = std::filesystem;

    // An editable skeletal animation clip (.anim, like Unity's AnimationClip asset): keys for the joints of a model's
    // skeleton, named by joint (so the clip survives the model being re-imported). Animators on that model (or using it
    // as their Animation Source) play it by its name next to the model's own clips. Made empty, or extracted from one
    // of the model's clips to edit (Window > Animation keys it).
    struct AnimationClipAsset
    {
        AssetHandle Model = AssetHandle::None(); // the model whose skeleton the joints belong to
        std::string Name;                        // the clip's name, as Animators and controllers refer to it
        float Duration = 1.0f;
        bool Loop = true;                        // informational: the state or Animator decides looping
        struct Channel
        {
            std::string Joint;
            AnimationPath Path = AnimationPath::Rotation;
            AnimationInterpolation Interpolation = AnimationInterpolation::Linear;
            std::vector<float> Times;
            std::vector<glm::vec4> Values; // xyz, or a quaternion (x, y, z, w)
            // Smooth: curves through the keys with automatic tangents (Interpolation is then ignored). Imported cubic
            // splines keep their own tangents (Interpolation CubicSpline) until a key is edited, which makes them Smooth.
            bool Smooth = false;
            std::vector<glm::vec4> InTangents, OutTangents;
        };
        std::vector<Channel> Channels;
        uint64_t Version = 0; // bumped on every edit (clips made from it are made again)

        static Ref<AnimationClipAsset> Load(const fs::path& path);
        bool Save(const fs::path& path) const;

        // From one of a model's clips (its joints named), and back as a clip for a skeleton (joints matched by name;
        // channels of joints it lacks are left out).
        static AnimationClipAsset FromClip(const Skeleton& skeleton, const AnimationClip& clip);
        AnimationClip ToClip(const Skeleton& skeleton) const;

        // Editing: the channel of a joint's path (null when none / created), and keys set (replacing one within half
        // a frame of `time`, kept in time order) or removed. Keys at a time: whether any channel of the joint has one.
        Channel* FindChannel(const std::string& joint, AnimationPath path);
        Channel& GetOrAddChannel(const std::string& joint, AnimationPath path);
        void SetKey(const std::string& joint, AnimationPath path, float time, const glm::vec4& value);
        bool RemoveKeys(const std::string& joint, float time);   // every channel of the joint at that time
        bool MoveKeys(const std::string& joint, float from, float to);
        bool HasKey(const std::string& joint, float time) const;
        static constexpr float kKeyTolerance = 1.0f / 120.0f;
    };
}
