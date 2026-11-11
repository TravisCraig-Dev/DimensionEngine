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
        std::vector<AnimationClip::Event> Events; // in time order (OnAnimationEvent(function) when playback passes one)
        uint64_t Version = 0; // bumped on every edit (clips made from it are made again)

        static Ref<AnimationClipAsset> Load(const fs::path& path);
        bool Save(const fs::path& path) const;

        // From one of a model's clips (its joints named), and back as a clip for a skeleton (joints matched by name;
        // channels of joints it lacks are left out). Blend shape weights are channels named kBlendShape + the shape's
        // name (weight in x, 0..1), matched to the model's shapes by name (`shapes`: the model's shape names).
        static AnimationClipAsset FromClip(const Skeleton& skeleton, const AnimationClip& clip, const std::vector<std::string>* shapes = nullptr);
        AnimationClip ToClip(const Skeleton& skeleton, const std::vector<std::string>* shapes = nullptr) const;
        static constexpr const char* kBlendShape = "blendShape:";
        static bool IsBlendShape(const std::string& channel) { return channel.rfind(kBlendShape, 0) == 0; }
        // Object properties are channels named kProperty + "path|Component|Field" (path from the Animator's object,
        // empty for itself), their values up to four floats.
        static constexpr const char* kProperty = "property:";
        static bool IsProperty(const std::string& channel) { return channel.rfind(kProperty, 0) == 0; }
        static std::string PropertyChannel(const std::string& path, const std::string& component, const std::string& field)
        {
            return kProperty + path + "|" + component + "|" + field;
        }
        static bool ParseProperty(const std::string& channel, std::string& path, std::string& component, std::string& field);

        // Editing: the channel of a joint's path (null when none / created), and keys set (replacing one within half
        // a frame of `time`, kept in time order) or removed. Keys at a time: whether any channel of the joint has one.
        Channel* FindChannel(const std::string& joint, AnimationPath path);
        Channel& GetOrAddChannel(const std::string& joint, AnimationPath path);
        void SetKey(const std::string& joint, AnimationPath path, float time, const glm::vec4& value);
        bool RemoveKeys(const std::string& joint, float time);   // every channel of the joint at that time
        bool MoveKeys(const std::string& joint, float from, float to);
        bool HasKey(const std::string& joint, float time) const;
        // Events: added (kept in time order; returns its index), moved (its new index).
        int AddEvent(float time, const std::string& function);
        int MoveEvent(int index, float time);
        static constexpr float kKeyTolerance = 1.0f / 120.0f;

        // Tangents set by hand (the curve view's handles): the channel keeps explicit spline tangents (a Smooth or
        // linear channel gets its automatic ones written out first), and key `key`'s in / out tangents (value units
        // per second; null leaves that side) are set. Explicit tangents survive later key edits: a new key gets an
        // automatic tangent, moved and removed keys take theirs along. AutoTangent puts one key's back to automatic
        // (its slope from its neighbours), FlatTangent levels it. False when there is no such channel or key.
        static bool HasExplicitTangents(const Channel& c);
        static void MakeTangentsExplicit(Channel& c);
        bool SetTangents(const std::string& joint, AnimationPath path, size_t key, const glm::vec4* in, const glm::vec4* out);
        bool AutoTangent(const std::string& joint, AnimationPath path, size_t key);
        bool FlatTangent(const std::string& joint, AnimationPath path, size_t key);
    };
}
