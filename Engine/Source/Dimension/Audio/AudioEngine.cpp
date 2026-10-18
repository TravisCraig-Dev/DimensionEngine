#include "Dimension/Audio/AudioEngine.h"
#include "Dimension/Scene/Scene.h"
#include "Dimension/Asset/AssetManager.h"
#include "Dimension/Core/Log.h"
#include "Dimension/Project/Project.h"
#include <algorithm>
#include <cmath>

#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_ENCODING
#include <miniaudio.h>

namespace Dimension
{
    namespace
    {
        ma_engine* s_Engine = nullptr;

        ma_sound* SoundOf(AudioSourceComponent& src) { return static_cast<ma_sound*>(src.Runtime.get()); }

        // Mixer graph: per group a sound group (sounds attach here) feeding a low-pass filter that feeds the parent.
        struct GroupNodes
        {
            ma_sound_group Group;
            ma_lpf_node LowPass;
            bool Valid = false;
        };
        std::vector<std::unique_ptr<GroupNodes>> s_Groups;
        std::vector<int> s_BuildOrder;
        bool s_StructureDirty = true;

        constexpr float kLowPassOffHz = 20000.0f;
        float CutoffOf(const AudioMixerGroup& g, ma_uint32 sampleRate)
        {
            float hz = g.LowPassHz > 0.0f ? g.LowPassHz : kLowPassOffHz;
            return std::clamp(hz, 20.0f, sampleRate * 0.45f);
        }

        void DestroyGraph()
        {
            for (auto it = s_BuildOrder.rbegin(); it != s_BuildOrder.rend(); ++it)
            {
                auto& g = s_Groups[*it];
                if (!g || !g->Valid) continue;
                ma_sound_group_uninit(&g->Group);
                ma_lpf_node_uninit(&g->LowPass, nullptr);
                g->Valid = false;
            }
            s_Groups.clear();
            s_BuildOrder.clear();
        }

        // Valid parent of group i (Master for invalid or cyclic links).
        int ParentOf(const std::vector<AudioMixerGroup>& groups, int i)
        {
            if (i == 0) return -1;
            int p = groups[i].Parent;
            if (p < 0 || p >= (int)groups.size() || p == i) return 0;
            for (int hop = p, steps = 0; hop > 0 && steps <= (int)groups.size(); ++steps) // reject cycles
            {
                hop = groups[hop].Parent;
                if (hop == i || hop < 0 || hop >= (int)groups.size()) return 0;
            }
            return p;
        }
    }

    std::vector<AudioMixerGroup> AudioMixer::DefaultGroups()
    {
        AudioMixerGroup master, music, sfx;
        master.Name = "Master";
        master.Parent = -1;
        music.Name = "Music";
        sfx.Name = "SFX";
        return { master, music, sfx };
    }

    std::vector<AudioMixerGroup>& AudioMixer::Groups()
    {
        auto& groups = Project::GetConfig().MixerGroups;
        if (groups.empty()) groups = DefaultGroups();
        return groups;
    }

    int AudioMixer::FindGroup(const std::string& name)
    {
        auto& groups = Groups();
        for (size_t i = 0; i < groups.size(); ++i)
            if (groups[i].Name == name) return (int)i;
        return -1;
    }

    float AudioMixer::DbToLinear(float db) { return db <= kSilentDb ? 0.0f : std::pow(10.0f, db / 20.0f); }
    float AudioMixer::LinearToDb(float linear) { return linear <= 1e-4f ? kSilentDb : std::max(kSilentDb, 20.0f * std::log10(linear)); }

    float AudioMixer::EffectiveGain(int i)
    {
        auto& groups = Groups();
        if (i < 0 || i >= (int)groups.size()) return 1.0f;
        if (groups[i].Mute) return 0.0f;
        bool anySolo = std::any_of(groups.begin(), groups.end(), [](const AudioMixerGroup& g) { return g.Solo; });
        if (anySolo)
        {
            // Audible: soloed groups, their children (inherit) and their ancestors (the path to Master).
            bool audible = false;
            for (int g = i; g >= 0 && !audible; g = ParentOf(groups, g)) audible = groups[g].Solo;
            for (size_t k = 0; k < groups.size() && !audible; ++k)
                if (groups[k].Solo)
                    for (int g = (int)k; g >= 0; g = ParentOf(groups, g))
                        if (g == i) { audible = true; break; }
            if (!audible) return 0.0f;
        }
        return DbToLinear(groups[i].VolumeDb);
    }

    void AudioMixer::SetVolumeDb(const std::string& group, float db)
    {
        int i = FindGroup(group);
        if (i < 0) { Log::Warn("Audio Mixer has no group '{}'", group); return; }
        Groups()[i].VolumeDb = std::clamp(db, kSilentDb, 20.0f);
        Apply();
    }

    float AudioMixer::GetVolumeDb(const std::string& group)
    {
        int i = FindGroup(group);
        return i >= 0 ? Groups()[i].VolumeDb : 0.0f;
    }

    void AudioMixer::MarkStructureDirty() { s_StructureDirty = true; }

    void AudioMixer::Rebuild()
    {
        DestroyGraph();
        s_StructureDirty = false;
        if (!s_Engine) return;
        auto& groups = Groups();
        // Parents before children.
        std::vector<int> depth(groups.size(), 0);
        for (int i = 0; i < (int)groups.size(); ++i)
            for (int p = ParentOf(groups, i); p >= 0; p = ParentOf(groups, p)) ++depth[i];
        s_BuildOrder.resize(groups.size());
        for (int i = 0; i < (int)groups.size(); ++i) s_BuildOrder[i] = i;
        std::stable_sort(s_BuildOrder.begin(), s_BuildOrder.end(), [&](int a, int b) { return depth[a] < depth[b]; });

        s_Groups.resize(groups.size());
        ma_node_graph* graph = ma_engine_get_node_graph(s_Engine);
        ma_uint32 channels = ma_engine_get_channels(s_Engine), rate = ma_engine_get_sample_rate(s_Engine);
        for (int i : s_BuildOrder)
        {
            auto nodes = std::make_unique<GroupNodes>();
            if (ma_sound_group_init(s_Engine, 0, nullptr, &nodes->Group) != MA_SUCCESS) continue;
            ma_lpf_node_config cfg = ma_lpf_node_config_init(channels, rate, CutoffOf(groups[i], rate), 2);
            if (ma_lpf_node_init(graph, &cfg, nullptr, &nodes->LowPass) != MA_SUCCESS)
            {
                ma_sound_group_uninit(&nodes->Group);
                continue;
            }
            int parent = ParentOf(groups, i);
            ma_node* destination = parent >= 0 && s_Groups[parent] ? (ma_node*)&s_Groups[parent]->Group : ma_engine_get_endpoint(s_Engine);
            ma_node_attach_output_bus(&nodes->Group, 0, &nodes->LowPass, 0);
            ma_node_attach_output_bus(&nodes->LowPass, 0, destination, 0);
            nodes->Valid = true;
            s_Groups[i] = std::move(nodes);
        }
        Apply();
        Log::Trace("Audio mixer: {} group(s)", groups.size());
    }

    void AudioMixer::Apply()
    {
        if (!s_Engine) return;
        auto& groups = Groups();
        ma_uint32 channels = ma_engine_get_channels(s_Engine), rate = ma_engine_get_sample_rate(s_Engine);
        for (size_t i = 0; i < s_Groups.size() && i < groups.size(); ++i)
        {
            auto& g = s_Groups[i];
            if (!g || !g->Valid) continue;
            ma_sound_group_set_volume(&g->Group, EffectiveGain((int)i));
            ma_lpf_config cfg = ma_lpf_config_init(ma_format_f32, channels, rate, CutoffOf(groups[i], rate), 2);
            ma_lpf_node_reinit(&cfg, &g->LowPass);
        }
    }

    void* AudioMixer::GetGroupNode(int group)
    {
        if (!s_Engine) return nullptr;
        if (s_StructureDirty || s_Groups.empty()) return nullptr; // not built yet: route to the endpoint
        if (group < 0 || group >= (int)s_Groups.size() || !s_Groups[group] || !s_Groups[group]->Valid) group = 0;
        return s_Groups.empty() || !s_Groups[group] ? nullptr : &s_Groups[group]->Group;
    }

    void AudioMixer::Shutdown()
    {
        DestroyGraph();
        s_StructureDirty = true;
    }

    void AudioEngine::Init()
    {
        if (s_Engine) return;
        s_Engine = new ma_engine();
        if (ma_engine_init(nullptr, s_Engine) != MA_SUCCESS)
        {
            Log::Warn("No audio device available; audio disabled");
            delete s_Engine;
            s_Engine = nullptr;
            return;
        }
        Log::Info("Audio engine initialized ({} Hz)", ma_engine_get_sample_rate(s_Engine));
    }

    void AudioEngine::Shutdown()
    {
        AudioMixer::Shutdown();
        if (!s_Engine) return;
        ma_engine_uninit(s_Engine);
        delete s_Engine;
        s_Engine = nullptr;
    }

    bool AudioEngine::IsAvailable() { return s_Engine != nullptr; }
    void* AudioEngine::GetNative() { return s_Engine; }
    void AudioEngine::SetMasterVolume(float v)
    {
        if (s_Engine) ma_engine_set_volume(s_Engine, v);
    }

    AudioWorld::AudioWorld(Scene& scene) : m_Scene(scene) {}

    AudioWorld::~AudioWorld()
    {
        for (auto [e, src] : m_Scene.GetRegistry().ViewOf<AudioSourceComponent>()) src.Runtime.reset();
        m_OneShots.clear();
    }

    namespace
    {
        // Sound for `clip` routed into the source's mixer group, with the source's settings.
        std::shared_ptr<void> MakeSound(const AudioSourceComponent& src, AssetHandle clip, bool stream)
        {
            fs::path path = AssetManager::GetAbsolutePath(clip);
            auto* sound = new ma_sound();
            auto* group = static_cast<ma_sound_group*>(AudioMixer::GetGroupNode(src.Output.empty() ? 0 : AudioMixer::FindGroup(src.Output)));
            ma_uint32 flags = stream ? MA_SOUND_FLAG_STREAM : MA_SOUND_FLAG_DECODE;
            if (ma_sound_init_from_file_w(s_Engine, path.c_str(), flags, group, nullptr, sound) != MA_SUCCESS)
            {
                Log::Error("Failed to load audio clip '{}'", path.string());
                delete sound;
                return nullptr;
            }
            ma_sound_set_volume(sound, src.Mute ? 0.0f : src.Volume);
            ma_sound_set_pitch(sound, src.Pitch);
            ma_sound_set_spatialization_enabled(sound, src.Spatial);
            ma_sound_set_min_distance(sound, src.MinDistance);
            ma_sound_set_max_distance(sound, src.MaxDistance);
            return std::shared_ptr<void>(sound, [](void* p) {
                ma_sound_uninit(static_cast<ma_sound*>(p));
                delete static_cast<ma_sound*>(p);
            });
        }
    }

    void AudioWorld::CreateSource(Entity e, bool autoplay)
    {
        auto& src = m_Scene.GetRegistry().Get<AudioSourceComponent>(e);
        if (!s_Engine || src.Runtime || !src.Clip.IsValid()) return;
        src.Runtime = MakeSound(src, src.Clip, src.Stream);
        auto* sound = SoundOf(src);
        if (!sound) return;
        ma_sound_set_looping(sound, src.Loop);
        if (autoplay && src.PlayOnAwake && m_Scene.GetRegistry().Get<TransformComponent>(e).ActiveInHierarchy)
            ma_sound_start(sound);
    }

    void AudioWorld::Start()
    {
        AudioMixer::Rebuild(); // no sounds exist yet: safe to (re)create the mixer graph
        std::vector<Entity> sources;
        for (auto [e, src] : m_Scene.GetRegistry().ViewOf<AudioSourceComponent>()) sources.push_back(e);
        for (Entity e : sources) CreateSource(e, true);
        Update();
    }

    void AudioWorld::CreateSourcesFor(Entity root)
    {
        if (m_Scene.GetRegistry().Has<AudioSourceComponent>(root)) CreateSource(root, true);
        for (Entity c : m_Scene.GetChildren(root)) CreateSourcesFor(c);
    }

    void AudioWorld::OnEntityDestroyed(Entity e)
    {
        if (auto* src = m_Scene.GetRegistry().TryGet<AudioSourceComponent>(e)) src->Runtime.reset();
    }

    void AudioWorld::Update()
    {
        if (!s_Engine) return;
        auto& reg = m_Scene.GetRegistry();
        Entity cam = m_Scene.GetPrimaryCamera();
        if (!cam.IsNull())
        {
            auto& t = reg.Get<TransformComponent>(cam);
            glm::vec3 p = t.GetWorldPosition(), f = t.Forward(), u = t.Up();
            ma_engine_listener_set_position(s_Engine, 0, p.x, p.y, p.z);
            ma_engine_listener_set_direction(s_Engine, 0, f.x, f.y, f.z);
            ma_engine_listener_set_world_up(s_Engine, 0, u.x, u.y, u.z);
        }
        for (auto [e, src, t] : reg.ViewOf<AudioSourceComponent, TransformComponent>())
        {
            ma_sound* s = SoundOf(src);
            if (!s) continue;
            glm::vec3 p = t.GetWorldPosition();
            ma_sound_set_position(s, p.x, p.y, p.z);
            ma_sound_set_volume(s, src.Mute ? 0.0f : src.Volume);
            ma_sound_set_pitch(s, src.Pitch);
        }
        // Release finished one-shots.
        std::erase_if(m_OneShots, [](const std::shared_ptr<void>& p) { return ma_sound_at_end(static_cast<ma_sound*>(p.get())); });
    }

    void AudioWorld::Play(Scene& scene, Entity e)
    {
        auto* src = scene.GetRegistry().TryGet<AudioSourceComponent>(e);
        if (!src || !SoundOf(*src)) return;
        ma_sound_seek_to_pcm_frame(SoundOf(*src), 0);
        ma_sound_start(SoundOf(*src));
    }

    void AudioWorld::Stop(Scene& scene, Entity e)
    {
        auto* src = scene.GetRegistry().TryGet<AudioSourceComponent>(e);
        if (src && SoundOf(*src)) ma_sound_stop(SoundOf(*src));
    }

    bool AudioWorld::IsPlaying(Scene& scene, Entity e)
    {
        auto* src = scene.GetRegistry().TryGet<AudioSourceComponent>(e);
        return src && SoundOf(*src) && ma_sound_is_playing(SoundOf(*src));
    }

    void AudioWorld::PlayOneShot(Scene& scene, Entity e, AssetHandle clip, float volumeScale)
    {
        AudioWorld* world = scene.GetAudio();
        auto* src = scene.GetRegistry().TryGet<AudioSourceComponent>(e);
        if (!world || !src || !s_Engine) return;
        auto sound = MakeSound(*src, clip.IsValid() ? clip : src->Clip, false);
        if (!sound) return;
        auto* s = static_cast<ma_sound*>(sound.get());
        ma_sound_set_volume(s, (src->Mute ? 0.0f : src->Volume) * volumeScale);
        glm::vec3 p = scene.GetRegistry().Get<TransformComponent>(e).GetWorldPosition();
        ma_sound_set_position(s, p.x, p.y, p.z);
        ma_sound_start(s);
        world->m_OneShots.push_back(std::move(sound));
    }
}
