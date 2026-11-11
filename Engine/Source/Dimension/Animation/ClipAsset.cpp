#include "Dimension/Animation/ClipAsset.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <fstream>

namespace Dimension
{
    using json = nlohmann::json;

    namespace
    {
        const char* PathName(AnimationPath p) { return p == AnimationPath::Translation ? "position" : p == AnimationPath::Scale ? "scale" : "rotation"; }
        AnimationPath PathFrom(const std::string& s)
        {
            return s == "position" ? AnimationPath::Translation : s == "scale" ? AnimationPath::Scale : AnimationPath::Rotation;
        }
    }

    Ref<AnimationClipAsset> AnimationClipAsset::Load(const fs::path& path)
    {
        auto clip = MakeRef<AnimationClipAsset>();
        clip->Name = path.stem().string();
        std::ifstream in(path);
        if (!in) return clip;
        try
        {
            json j = json::parse(in);
            clip->Model = AssetHandle(j.value("model", (uint64_t)0));
            clip->Name = j.value("name", clip->Name);
            clip->Duration = std::max(j.value("duration", 1.0f), 0.01f);
            clip->Loop = j.value("loop", true);
            for (auto& c : j.value("channels", json::array()))
            {
                Channel ch;
                ch.Joint = c.value("joint", std::string());
                ch.Path = PathFrom(c.value("path", std::string("rotation")));
                std::string mode = c.value("interpolation", std::string());
                ch.Interpolation = mode == "step" ? AnimationInterpolation::Step : mode == "cubic" ? AnimationInterpolation::CubicSpline : AnimationInterpolation::Linear;
                ch.Smooth = mode == "smooth";
                auto vectors = [](const json& list, std::vector<glm::vec4>& out) {
                    for (auto& v : list)
                        if (v.is_array() && v.size() >= 3) out.push_back({ v[0].get<float>(), v[1].get<float>(), v[2].get<float>(), v.size() > 3 ? v[3].get<float>() : 0.0f });
                };
                vectors(c.value("in", json::array()), ch.InTangents);
                vectors(c.value("out", json::array()), ch.OutTangents);
                for (auto& k : c.value("keys", json::array()))
                {
                    if (!k.is_array() || k.size() < 4) continue;
                    ch.Times.push_back(k[0].get<float>());
                    ch.Values.push_back({ k[1].get<float>(), k[2].get<float>(), k[3].get<float>(), k.size() > 4 ? k[4].get<float>() : 0.0f });
                }
                if (ch.Interpolation == AnimationInterpolation::CubicSpline && (ch.InTangents.size() != ch.Times.size() || ch.OutTangents.size() != ch.Times.size()))
                    ch.Smooth = true; // tangents missing: automatic ones
                if (!ch.Joint.empty() && !ch.Times.empty()) clip->Channels.push_back(std::move(ch));
            }
        }
        catch (const std::exception&)
        {
        }
        return clip;
    }

    bool AnimationClipAsset::Save(const fs::path& path) const
    {
        json channels = json::array();
        for (const Channel& ch : Channels)
        {
            json keys = json::array();
            bool rotation = ch.Path == AnimationPath::Rotation;
            for (size_t k = 0; k < ch.Times.size() && k < ch.Values.size(); ++k)
            {
                const glm::vec4& v = ch.Values[k];
                if (rotation) keys.push_back({ ch.Times[k], v.x, v.y, v.z, v.w });
                else keys.push_back({ ch.Times[k], v.x, v.y, v.z });
            }
            bool cubic = !ch.Smooth && ch.Interpolation == AnimationInterpolation::CubicSpline;
            json entry{ { "joint", ch.Joint }, { "path", PathName(ch.Path) },
                        { "interpolation", ch.Smooth ? "smooth" : cubic ? "cubic" : ch.Interpolation == AnimationInterpolation::Step ? "step" : "linear" },
                        { "keys", keys } };
            if (cubic)
            {
                json in = json::array(), out = json::array();
                for (const glm::vec4& v : ch.InTangents) in.push_back({ v.x, v.y, v.z, v.w });
                for (const glm::vec4& v : ch.OutTangents) out.push_back({ v.x, v.y, v.z, v.w });
                entry["in"] = in;
                entry["out"] = out;
            }
            channels.push_back(entry);
        }
        json j{ { "model", (uint64_t)Model }, { "name", Name }, { "duration", Duration }, { "loop", Loop }, { "channels", channels } };
        std::ofstream out(path);
        if (!out) return false;
        out << j.dump(1);
        return (bool)out;
    }

    AnimationClipAsset AnimationClipAsset::FromClip(const Skeleton& skeleton, const AnimationClip& clip)
    {
        AnimationClipAsset a;
        a.Name = clip.Name;
        a.Duration = std::max(clip.Duration, 0.01f);
        for (const AnimationChannel& ch : clip.Channels)
        {
            if (ch.Joint < 0 || ch.Joint >= (int)skeleton.Joints.size()) continue;
            Channel c;
            c.Joint = skeleton.Joints[ch.Joint].Name;
            c.Path = ch.Path;
            c.Interpolation = ch.Interpolation;
            c.Times = ch.Times;
            c.Values = ch.Values;
            c.InTangents = ch.InTangents;
            c.OutTangents = ch.OutTangents;
            a.Channels.push_back(std::move(c));
        }
        return a;
    }

    AnimationClip AnimationClipAsset::ToClip(const Skeleton& skeleton) const
    {
        AnimationClip clip;
        clip.Name = Name;
        clip.Duration = Duration;
        for (const Channel& c : Channels)
        {
            int joint = Animation::MatchJoint(skeleton, c.Joint);
            if (joint < 0 || c.Times.empty() || c.Times.size() != c.Values.size()) continue;
            AnimationChannel ch;
            ch.Joint = joint;
            ch.Path = c.Path;
            ch.Interpolation = c.Interpolation;
            ch.Times = c.Times;
            ch.Values = c.Values;
            ch.InTangents = c.InTangents;
            ch.OutTangents = c.OutTangents;
            if (c.Smooth) Animation::AutoTangents(ch);
            clip.Channels.push_back(std::move(ch));
        }
        return clip;
    }

    AnimationClipAsset::Channel* AnimationClipAsset::FindChannel(const std::string& joint, AnimationPath path)
    {
        for (Channel& c : Channels)
            if (c.Joint == joint && c.Path == path) return &c;
        return nullptr;
    }

    AnimationClipAsset::Channel& AnimationClipAsset::GetOrAddChannel(const std::string& joint, AnimationPath path)
    {
        if (Channel* c = FindChannel(joint, path)) return *c;
        Channel c;
        c.Joint = joint;
        c.Path = path;
        Channels.push_back(std::move(c));
        return Channels.back();
    }

    namespace
    {
        // A channel whose keys change: explicit tangents (imported or set by hand) stay with their keys, so their size
        // must match; a channel whose tangents are incomplete becomes smooth.
        void KeysChanged(AnimationClipAsset::Channel& c)
        {
            if (AnimationClipAsset::HasExplicitTangents(c)) return;
            if (c.Interpolation == AnimationInterpolation::CubicSpline && !c.Smooth) c.Smooth = true;
            c.InTangents.clear();
            c.OutTangents.clear();
        }

        // The automatic tangent of key k (its slope from its neighbours, flat at the ends).
        glm::vec4 AutoSlope(const AnimationClipAsset::Channel& c, size_t k)
        {
            if (k == 0 || k + 1 >= c.Times.size()) return glm::vec4(0.0f);
            glm::vec4 before = c.Values[k - 1], after = c.Values[k + 1];
            if (c.Path == AnimationPath::Rotation)
            {
                if (glm::dot(before, c.Values[k]) < 0.0f) before = -before;
                if (glm::dot(after, c.Values[k]) < 0.0f) after = -after;
            }
            float span = c.Times[k + 1] - c.Times[k - 1];
            return span > 1e-6f ? (after - before) / span : glm::vec4(0.0f);
        }
    }

    bool AnimationClipAsset::HasExplicitTangents(const Channel& c)
    {
        return !c.Smooth && c.Interpolation == AnimationInterpolation::CubicSpline && c.InTangents.size() == c.Times.size() &&
               c.OutTangents.size() == c.Times.size() && c.Values.size() == c.Times.size();
    }

    void AnimationClipAsset::MakeTangentsExplicit(Channel& c)
    {
        if (HasExplicitTangents(c)) return;
        AnimationChannel automatic;
        automatic.Path = c.Path;
        automatic.Times = c.Times;
        automatic.Values = c.Values;
        Animation::AutoTangents(automatic); // (rotations kept on the short way round between keys)
        c.Values = automatic.Values;
        c.InTangents = automatic.InTangents;
        c.OutTangents = automatic.OutTangents;
        c.Interpolation = AnimationInterpolation::CubicSpline;
        c.Smooth = false;
    }

    bool AnimationClipAsset::SetTangents(const std::string& joint, AnimationPath path, size_t key, const glm::vec4* in, const glm::vec4* out)
    {
        Channel* c = FindChannel(joint, path);
        if (!c || key >= c->Times.size()) return false;
        MakeTangentsExplicit(*c);
        if (in) c->InTangents[key] = *in;
        if (out) c->OutTangents[key] = *out;
        ++Version;
        return true;
    }

    bool AnimationClipAsset::AutoTangent(const std::string& joint, AnimationPath path, size_t key)
    {
        Channel* c = FindChannel(joint, path);
        if (!c || key >= c->Times.size()) return false;
        MakeTangentsExplicit(*c);
        c->InTangents[key] = c->OutTangents[key] = AutoSlope(*c, key);
        ++Version;
        return true;
    }

    bool AnimationClipAsset::FlatTangent(const std::string& joint, AnimationPath path, size_t key)
    {
        glm::vec4 zero(0.0f);
        return SetTangents(joint, path, key, &zero, &zero);
    }

    void AnimationClipAsset::SetKey(const std::string& joint, AnimationPath path, float time, const glm::vec4& value)
    {
        Channel& c = GetOrAddChannel(joint, path);
        KeysChanged(c);
        bool tangents = HasExplicitTangents(c);
        time = std::clamp(time, 0.0f, Duration);
        glm::vec4 v = value;
        for (size_t k = 0; k < c.Times.size(); ++k)
            if (std::abs(c.Times[k] - time) <= kKeyTolerance * 0.5f)
            {
                // (on the side of the key it replaces, which its tangents were made for)
                c.Values[k] = path == AnimationPath::Rotation && glm::dot(c.Values[k], v) < 0.0f ? -v : v;
                ++Version;
                return;
            }
        size_t at = std::upper_bound(c.Times.begin(), c.Times.end(), time) - c.Times.begin();
        // Rotations stay on the short way round from the key before.
        if (path == AnimationPath::Rotation && at > 0 && glm::dot(c.Values[at - 1], v) < 0.0f) v = -v;
        c.Times.insert(c.Times.begin() + at, time);
        c.Values.insert(c.Values.begin() + at, v);
        if (tangents) // the new key's automatic
        {
            glm::vec4 slope = AutoSlope(c, at);
            c.InTangents.insert(c.InTangents.begin() + at, slope);
            c.OutTangents.insert(c.OutTangents.begin() + at, slope);
        }
        ++Version;
    }

    bool AnimationClipAsset::RemoveKeys(const std::string& joint, float time)
    {
        bool removed = false;
        for (Channel& c : Channels)
        {
            if (c.Joint != joint) continue;
            for (size_t k = c.Times.size(); k-- > 0;)
                if (std::abs(c.Times[k] - time) <= kKeyTolerance * 0.5f)
                {
                    KeysChanged(c);
                    if (HasExplicitTangents(c))
                    {
                        c.InTangents.erase(c.InTangents.begin() + k);
                        c.OutTangents.erase(c.OutTangents.begin() + k);
                    }
                    c.Times.erase(c.Times.begin() + k);
                    c.Values.erase(c.Values.begin() + k);
                    removed = true;
                }
        }
        std::erase_if(Channels, [](const Channel& c) { return c.Times.empty(); });
        if (removed) ++Version;
        return removed;
    }

    bool AnimationClipAsset::MoveKeys(const std::string& joint, float from, float to)
    {
        to = std::clamp(to, 0.0f, Duration);
        bool moved = false;
        for (Channel& c : Channels)
        {
            if (c.Joint != joint) continue;
            for (size_t k = 0; k < c.Times.size(); ++k)
                if (std::abs(c.Times[k] - from) <= kKeyTolerance * 0.5f)
                {
                    KeysChanged(c);
                    bool tangents = HasExplicitTangents(c); // they move with their keys
                    auto erase = [&](size_t i) {
                        c.Times.erase(c.Times.begin() + i);
                        c.Values.erase(c.Values.begin() + i);
                        if (tangents)
                        {
                            c.InTangents.erase(c.InTangents.begin() + i);
                            c.OutTangents.erase(c.OutTangents.begin() + i);
                        }
                    };
                    glm::vec4 v = c.Values[k], in = tangents ? c.InTangents[k] : glm::vec4(0.0f), out = tangents ? c.OutTangents[k] : glm::vec4(0.0f);
                    erase(k);
                    // A key already at the new time gives way.
                    for (size_t o = c.Times.size(); o-- > 0;)
                        if (std::abs(c.Times[o] - to) <= kKeyTolerance * 0.5f) erase(o);
                    size_t at = std::upper_bound(c.Times.begin(), c.Times.end(), to) - c.Times.begin();
                    c.Times.insert(c.Times.begin() + at, to);
                    c.Values.insert(c.Values.begin() + at, v);
                    if (tangents)
                    {
                        c.InTangents.insert(c.InTangents.begin() + at, in);
                        c.OutTangents.insert(c.OutTangents.begin() + at, out);
                    }
                    moved = true;
                    break;
                }
        }
        if (moved) ++Version;
        return moved;
    }

    bool AnimationClipAsset::HasKey(const std::string& joint, float time) const
    {
        for (const Channel& c : Channels)
            if (c.Joint == joint)
                for (float t : c.Times)
                    if (std::abs(t - time) <= kKeyTolerance * 0.5f) return true;
        return false;
    }
}
