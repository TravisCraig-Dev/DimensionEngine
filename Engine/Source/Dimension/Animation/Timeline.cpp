#include "Dimension/Animation/Timeline.h"
#include "Dimension/Scene/Scene.h"
#include "Dimension/Scene/ComponentRegistry.h"
#include "Dimension/Asset/AssetManager.h"
#include "Dimension/Audio/AudioEngine.h"
#include "Dimension/Core/Log.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <fstream>

namespace Dimension
{
    using json = nlohmann::json;

    Ref<TimelineAsset> TimelineAsset::Load(const fs::path& path)
    {
        auto timeline = MakeRef<TimelineAsset>();
        std::ifstream in(path);
        if (!in) return timeline;
        try
        {
            json j = json::parse(in);
            timeline->Duration = std::max(j.value("duration", 5.0f), 0.01f);
            for (auto& t : j.value("tracks", json::array()))
            {
                TimelineTrack track;
                track.Path = t.value("path", std::string());
                track.Component = t.value("component", std::string());
                track.Field = t.value("field", std::string());
                track.Interpolation = (TimelineInterpolation)t.value("interpolation", 0);
                track.Muted = t.value("muted", false);
                track.Kind = (TimelineTrackKind)std::clamp(t.value("kind", 0), 0, 4);
                for (auto& c : t.value("clips", json::array()))
                {
                    TimelineClip clip;
                    clip.Start = c.value("start", 0.0f);
                    clip.Duration = std::max(c.value("duration", 1.0f), 0.01f);
                    clip.Audio = AssetHandle(c.value("audio", (uint64_t)0));
                    clip.Animation = c.value("animation", std::string());
                    clip.Timeline = AssetHandle(c.value("timeline", (uint64_t)0));
                    clip.Speed = c.value("speed", 1.0f);
                    clip.ClipIn = c.value("clipIn", 0.0f);
                    clip.Loop = c.value("loop", true);
                    track.Clips.push_back(clip);
                }
                Timeline::SortClips(track);
                for (auto& k : t.value("keys", json::array()))
                {
                    TimelineKey key;
                    key.Time = k.value("t", 0.0f);
                    auto v = k.value("v", json::array());
                    for (size_t i = 0; i < v.size() && i < 4; ++i) key.Value[i] = v[i].get<float>();
                    track.Keys.push_back(key);
                }
                std::stable_sort(track.Keys.begin(), track.Keys.end(), [](const TimelineKey& a, const TimelineKey& b) { return a.Time < b.Time; });
                timeline->Tracks.push_back(std::move(track));
            }
            for (auto& e : j.value("events", json::array())) timeline->Events.push_back({ e.value("t", 0.0f), e.value("function", std::string()) });
        }
        catch (const std::exception& e)
        {
            Log::Error("Invalid timeline '{}': {}", path.string(), e.what());
        }
        return timeline;
    }

    bool TimelineAsset::Save(const fs::path& path) const
    {
        json j{ { "duration", Duration }, { "tracks", json::array() }, { "events", json::array() } };
        for (auto& t : Tracks)
        {
            json keys = json::array();
            for (auto& k : t.Keys) keys.push_back({ { "t", k.Time }, { "v", { k.Value[0], k.Value[1], k.Value[2], k.Value[3] } } });
            json track{ { "path", t.Path }, { "muted", t.Muted } };
            if (t.IsClipTrack())
            {
                json clips = json::array();
                for (auto& c : t.Clips)
                {
                    json clip{ { "start", c.Start }, { "duration", c.Duration }, { "clipIn", c.ClipIn } };
                    if (t.Kind == TimelineTrackKind::Audio) clip["audio"] = (uint64_t)c.Audio;
                    if (t.Kind == TimelineTrackKind::Animation || t.Kind == TimelineTrackKind::Control)
                    {
                        if (t.Kind == TimelineTrackKind::Animation) clip["animation"] = c.Animation;
                        else clip["timeline"] = (uint64_t)c.Timeline;
                        clip["speed"] = c.Speed;
                        clip["loop"] = c.Loop;
                    }
                    clips.push_back(clip);
                }
                track["kind"] = (int)t.Kind;
                track["clips"] = clips;
            }
            else
            {
                track["component"] = t.Component;
                track["field"] = t.Field;
                track["interpolation"] = (int)t.Interpolation;
                track["keys"] = keys;
            }
            j["tracks"].push_back(track);
        }
        for (auto& e : Events) j["events"].push_back({ { "t", e.Time }, { "function", e.Function } });
        std::ofstream out(path);
        if (!out) return false;
        out << j.dump(2);
        return (bool)out;
    }

    namespace Timeline
    {
        namespace
        {
            const FieldInfo* FindField(const ComponentInfo* info, const std::string& name)
            {
                if (!info) return nullptr;
                for (const FieldInfo& f : info->Fields)
                    if (f.Name == name) return &f;
                return nullptr;
            }

            // Nested timelines deeper than this are ignored (a timeline that plays itself would never end).
            constexpr int kMaxNesting = 8;
        }

        float ControlTime(const TimelineClip& clip, float nestedDuration, float time)
        {
            float d = std::max(nestedDuration, 0.01f);
            float local = clip.ClipIn + std::clamp(time - clip.Start, 0.0f, clip.Duration) * clip.Speed;
            if (clip.Loop)
            {
                local = std::fmod(local, d);
                return local < 0.0f ? local + d : local;
            }
            return std::clamp(local, 0.0f, d);
        }

        int Sample(const TimelineTrack& track, float time, float out[4])
        {
            const auto& k = track.Keys;
            if (k.empty()) return 0;
            if (time <= k.front().Time || k.size() == 1)
            {
                for (int c = 0; c < 4; ++c) out[c] = k.front().Value[c];
                return 4;
            }
            if (time >= k.back().Time)
            {
                for (int c = 0; c < 4; ++c) out[c] = k.back().Value[c];
                return 4;
            }
            size_t i = 0;
            while (i + 2 < k.size() && time >= k[i + 1].Time) ++i;
            const TimelineKey &a = k[i], &b = k[i + 1];
            float span = std::max(b.Time - a.Time, 1e-6f);
            float u = std::clamp((time - a.Time) / span, 0.0f, 1.0f);
            for (int c = 0; c < 4; ++c)
            {
                if (track.Interpolation == TimelineInterpolation::Step) out[c] = a.Value[c];
                else if (track.Interpolation == TimelineInterpolation::Linear) out[c] = a.Value[c] + (b.Value[c] - a.Value[c]) * u;
                else
                {
                    // Cubic Hermite with Catmull-Rom slopes from the neighbours (flat at the first and last key, so
                    // motion eases in and out there).
                    float ma = i > 0 ? (b.Value[c] - k[i - 1].Value[c]) / std::max(b.Time - k[i - 1].Time, 1e-6f) : 0.0f;
                    float mb = i + 2 < k.size() ? (k[i + 2].Value[c] - a.Value[c]) / std::max(k[i + 2].Time - a.Time, 1e-6f) : 0.0f;
                    float u2 = u * u, u3 = u2 * u;
                    out[c] = (2 * u3 - 3 * u2 + 1) * a.Value[c] + (u3 - 2 * u2 + u) * span * ma + (-2 * u3 + 3 * u2) * b.Value[c] + (u3 - u2) * span * mb;
                }
            }
            return 4;
        }

        void SetKey(TimelineTrack& track, float time, const float* value, int channels)
        {
            TimelineKey key;
            key.Time = time;
            for (int c = 0; c < std::min(channels, 4); ++c) key.Value[c] = value[c];
            for (auto& existing : track.Keys)
                if (std::abs(existing.Time - time) < 1e-3f)
                {
                    existing = key;
                    return;
                }
            track.Keys.insert(std::upper_bound(track.Keys.begin(), track.Keys.end(), key,
                                               [](const TimelineKey& a, const TimelineKey& b) { return a.Time < b.Time; }),
                              key);
        }

        Entity ResolvePath(Scene& scene, Entity root, const std::string& path)
        {
            Entity cur = root;
            size_t at = 0;
            while (!cur.IsNull() && at < path.size())
            {
                size_t slash = path.find('/', at);
                std::string name = path.substr(at, slash == std::string::npos ? std::string::npos : slash - at);
                Entity next = Entity::Null();
                for (Entity c : scene.GetChildren(cur))
                    if (scene.GetRegistry().Get<TagComponent>(c).Name == name)
                    {
                        next = c;
                        break;
                    }
                cur = next;
                at = slash == std::string::npos ? path.size() : slash + 1;
            }
            return cur;
        }

        std::string PathOf(Scene& scene, Entity root, Entity target, bool* ok)
        {
            std::string path;
            Entity cur = target;
            while (!cur.IsNull() && cur != root)
            {
                const std::string& name = scene.GetRegistry().Get<TagComponent>(cur).Name;
                path = path.empty() ? name : name + "/" + path;
                cur = scene.GetParent(cur);
            }
            if (ok) *ok = cur == root;
            return cur == root ? path : std::string();
        }

        void SortClips(TimelineTrack& track)
        {
            std::stable_sort(track.Clips.begin(), track.Clips.end(), [](const TimelineClip& a, const TimelineClip& b) { return a.Start < b.Start; });
        }

        namespace
        {
            // Activation: active while `time` is inside any clip.
            bool ApplyActivation(Scene& scene, Entity target, const TimelineTrack& track, float time)
            {
                bool active = false;
                for (const TimelineClip& c : track.Clips) active |= time >= c.Start && time < c.End();
                scene.GetRegistry().Get<TagComponent>(target).Active = active;
                return true;
            }

            // Animation: the latest clip containing `time`, crossfaded from an earlier one it overlaps; after the last
            // clip its final frame holds; before the first clip and in gaps the Animator plays on its own.
            bool ApplyAnimation(Scene& scene, Entity target, const TimelineTrack& track, float time)
            {
                auto* anim = scene.GetRegistry().TryGet<AnimatorComponent>(target);
                if (!anim) return false;
                const TimelineClip *cur = nullptr, *prev = nullptr;
                for (const TimelineClip& c : track.Clips)
                    if (time >= c.Start && time < c.End())
                    {
                        prev = cur;
                        cur = &c;
                    }
                float local = 0.0f;
                if (!cur && !track.Clips.empty())
                {
                    bool afterAll = true;
                    for (const TimelineClip& c : track.Clips) afterAll &= time >= c.End();
                    if (afterAll)
                        for (const TimelineClip& c : track.Clips)
                            if (!cur || c.End() >= cur->End()) cur = &c;
                    if (cur) local = cur->Duration;
                }
                else if (cur)
                    local = time - cur->Start;
                if (!cur)
                {
                    anim->TimelineDriven = false;
                    return false;
                }
                anim->TimelineDriven = true;
                anim->TimelineClip = cur->Animation;
                anim->TimelineTime = cur->ClipIn + local * cur->Speed;
                anim->TimelineLoop = cur->Loop;
                anim->TimelinePrevClip.clear();
                anim->TimelineWeight = 1.0f;
                if (prev && prev->End() > cur->Start)
                {
                    float overlap = std::max(prev->End() - cur->Start, 1e-4f);
                    anim->TimelinePrevClip = prev->Animation;
                    anim->TimelinePrevTime = prev->ClipIn + (time - prev->Start) * prev->Speed;
                    anim->TimelinePrevLoop = prev->Loop;
                    anim->TimelineWeight = std::clamp((time - cur->Start) / overlap, 0.0f, 1.0f);
                }
                return true;
            }
        }

        namespace
        {
            int ApplyNested(Scene& scene, Entity root, const TimelineAsset& timeline, float time, int depth);

            // Control: the latest clip containing `time` plays its timeline on the target; after the last clip that
            // timeline holds where it ended; before the first clip and in gaps nothing is applied.
            int ApplyControl(Scene& scene, Entity target, const TimelineTrack& track, float time, int depth)
            {
                const TimelineClip* cur = nullptr;
                for (const TimelineClip& c : track.Clips)
                    if (time >= c.Start && time < c.End()) cur = &c;
                if (!cur)
                {
                    bool afterAll = !track.Clips.empty();
                    for (const TimelineClip& c : track.Clips) afterAll &= time >= c.End();
                    if (afterAll)
                        for (const TimelineClip& c : track.Clips)
                            if (!cur || c.End() >= cur->End()) cur = &c;
                }
                auto nested = cur ? AssetManager::GetTimeline(cur->Timeline) : nullptr;
                if (!nested) return 0;
                ApplyNested(scene, target, *nested, ControlTime(*cur, nested->Duration, time), depth + 1);
                return 1;
            }

            int ApplyNested(Scene& scene, Entity root, const TimelineAsset& timeline, float time, int depth)
            {
                if (depth > kMaxNesting) return 0;
                int written = 0;
                for (const TimelineTrack& track : timeline.Tracks)
                {
                    if (track.Muted) continue;
                    if (track.IsClipTrack())
                    {
                        Entity target = ResolvePath(scene, root, track.Path);
                        if (target.IsNull()) continue;
                        if (track.Kind == TimelineTrackKind::Activation) written += ApplyActivation(scene, target, track, time);
                        else if (track.Kind == TimelineTrackKind::Animation) written += ApplyAnimation(scene, target, track, time);
                        else if (track.Kind == TimelineTrackKind::Control) written += ApplyControl(scene, target, track, time, depth);
                        continue;
                    }
                    if (track.Keys.empty()) continue;
                    Entity target = ResolvePath(scene, root, track.Path);
                    const ComponentInfo* info = ComponentRegistry::Find(track.Component);
                    if (target.IsNull() || !info || !info->Has(scene, target)) continue;
                    const FieldInfo* field = FindField(info, track.Field);
                    if (!field) continue;
                    float value[4];
                    int n = Sample(track, time, value);
                    if (n && FieldFromFloats(*field, field->Access(info->Get(scene, target)), value, 4)) ++written;
                }
                return written;
            }
        }

        int Apply(Scene& scene, Entity root, const TimelineAsset& timeline, float time)
        {
            return ApplyNested(scene, root, timeline, time, 0);
        }

        int ReadCurrent(Scene& scene, Entity root, const TimelineTrack& track, float out[4])
        {
            Entity target = ResolvePath(scene, root, track.Path);
            const ComponentInfo* info = ComponentRegistry::Find(track.Component);
            if (target.IsNull() || !info || !info->Has(scene, target)) return 0;
            const FieldInfo* field = FindField(info, track.Field);
            if (!field) return 0;
            for (int c = 0; c < 4; ++c) out[c] = 0.0f;
            return FieldToFloats(*field, field->Access(info->Get(scene, target)), out);
        }

        void PlayAudio(Scene& scene, Entity root, const TimelineAsset& timeline, float from, float to)
        {
            for (const TimelineTrack& track : timeline.Tracks)
            {
                if (track.Muted || track.Kind != TimelineTrackKind::Audio) continue;
                Entity target = ResolvePath(scene, root, track.Path);
                if (target.IsNull() || !scene.GetRegistry().Has<AudioSourceComponent>(target)) continue;
                for (const TimelineClip& c : track.Clips)
                {
                    if (c.End() > from && c.End() <= to) AudioWorld::Stop(scene, target);
                    if (c.Start > from && c.Start <= to && c.End() > to) AudioWorld::PlayClip(scene, target, c.Audio, c.ClipIn + (to - c.Start));
                }
            }
        }

        namespace
        {
            void ReleaseNested(Scene& scene, Entity root, const TimelineAsset& timeline, int depth)
            {
                if (depth > kMaxNesting) return;
                for (const TimelineTrack& track : timeline.Tracks)
                {
                    if (track.Kind != TimelineTrackKind::Animation && track.Kind != TimelineTrackKind::Control) continue;
                    Entity target = ResolvePath(scene, root, track.Path);
                    if (target.IsNull()) continue;
                    if (track.Kind == TimelineTrackKind::Control)
                    {
                        for (const TimelineClip& c : track.Clips)
                            if (auto nested = AssetManager::GetTimeline(c.Timeline)) ReleaseNested(scene, target, *nested, depth + 1);
                    }
                    else if (auto* anim = scene.GetRegistry().TryGet<AnimatorComponent>(target))
                        anim->TimelineDriven = false;
                }
            }

            void AdvanceNested(Scene& scene, Entity root, const TimelineAsset& timeline, float from, float to,
                               std::vector<std::pair<Entity, std::string>>& events, int depth)
            {
                if (depth > kMaxNesting || to <= from) return;
                for (const TimelineEvent& ev : timeline.Events)
                    if (ev.Time > from && ev.Time <= to) events.push_back({ root, ev.Function });
                PlayAudio(scene, root, timeline, from, to);
                for (const TimelineTrack& track : timeline.Tracks)
                {
                    if (track.Muted || track.Kind != TimelineTrackKind::Control) continue;
                    Entity target = ResolvePath(scene, root, track.Path);
                    if (target.IsNull()) continue;
                    for (const TimelineClip& c : track.Clips)
                    {
                        if (to < c.Start || from >= c.End()) continue;
                        auto nested = AssetManager::GetTimeline(c.Timeline);
                        if (!nested) continue;
                        // The part of (from, to] inside the clip, in the nested timeline's own (unwrapped) time; entering
                        // the clip includes things at its Clip In.
                        float speed = std::max(c.Speed, 0.0f);
                        float a = from < c.Start ? c.ClipIn - 1e-4f : c.ClipIn + (from - c.Start) * speed;
                        float b = c.ClipIn + (std::min(to, c.End()) - c.Start) * speed;
                        float d = std::max(nested->Duration, 0.01f);
                        if (!c.Loop)
                        {
                            AdvanceNested(scene, target, *nested, a, std::min(b, d), events, depth + 1);
                            continue;
                        }
                        // Looping: one pass per cycle the range crosses (each new cycle includes things at 0).
                        for (int guard = 0; a < b && guard < 64; ++guard)
                        {
                            float cycle = std::floor(std::max(a, 0.0f) / d) * d;
                            float end = std::min(b, cycle + d);
                            AdvanceNested(scene, target, *nested, a - cycle <= 0.0f ? -1e-4f : a - cycle, end - cycle, events, depth + 1);
                            a = end;
                        }
                    }
                }
            }
        }

        void Release(Scene& scene, Entity root, const TimelineAsset& timeline)
        {
            ReleaseNested(scene, root, timeline, 0);
        }

        void Advance(Scene& scene, Entity root, const TimelineAsset& timeline, float from, float to, std::vector<std::pair<Entity, std::string>>& events)
        {
            AdvanceNested(scene, root, timeline, from, to, events, 0);
        }

        void Update(Scene& scene, float dt)
        {
            for (auto [e, tl, t] : scene.GetRegistry().ViewOf<TimelineComponent, TransformComponent>())
            {
                if (!t.ActiveInHierarchy) continue;
                bool starting = false;
                if (!tl.Started)
                {
                    tl.Started = true;
                    tl.Playing = tl.PlayOnAwake;
                    tl.Time = 0.0f;
                    starting = tl.Playing;
                }
                if (!tl.Playing) continue;
                auto asset = AssetManager::GetTimeline(tl.Timeline);
                if (!asset) continue;
                // Things at exactly 0 s fire on the first frame (Play() restarts from there too).
                float before = starting || tl.Time <= 0.0f ? -1.0f : tl.Time;
                tl.Time += dt * tl.Speed;
                float duration = std::max(asset->Duration, 0.01f);
                // Events and audio passed this frame, nested timelines' too (a loop wrap fires the ones after `before`
                // and before the new time).
                std::vector<std::pair<Entity, std::string>> fired;
                if (tl.Time >= duration)
                {
                    Advance(scene, e, *asset, before, duration, fired);
                    if (tl.Loop)
                    {
                        tl.Time = std::fmod(tl.Time, duration);
                        Advance(scene, e, *asset, -1.0f, tl.Time, fired);
                    }
                    else
                    {
                        tl.Time = duration;
                        tl.Playing = false;
                    }
                }
                else
                    Advance(scene, e, *asset, before, tl.Time, fired);
                Apply(scene, e, *asset, tl.Time);
                for (const auto& [target, fn] : fired) scene.DispatchAnimationEvent(target, fn);
            }
        }

        void Restart(Scene& scene)
        {
            for (auto [e, tl] : scene.GetRegistry().ViewOf<TimelineComponent>())
            {
                tl.Started = false;
                tl.Time = 0.0f;
            }
        }
    }
}
