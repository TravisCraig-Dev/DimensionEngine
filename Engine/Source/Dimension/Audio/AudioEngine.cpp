#include "Dimension/Audio/AudioEngine.h"
#include "Dimension/Scene/Scene.h"
#include "Dimension/Asset/AssetManager.h"
#include "Dimension/Core/Log.h"
#include "Dimension/Project/Project.h"
#include <algorithm>
#include <cmath>
#include <cstring>

// Ogg Vorbis decoding through stb_vorbis (bundled with miniaudio): header first so miniaudio enables its Vorbis
// decoder, implementation after.
#define STB_VORBIS_HEADER_ONLY
#include <extras/stb_vorbis.c>
#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_ENCODING
#include <miniaudio.h>
#include <extras/nodes/ma_reverb_node/ma_reverb_node.c>
#undef STB_VORBIS_HEADER_ONLY
#include <extras/stb_vorbis.c>

namespace Dimension
{
    namespace
    {
        ma_engine* s_Engine = nullptr;

        ma_sound* SoundOf(AudioSourceComponent& src) { return static_cast<ma_sound*>(src.Runtime.get()); }

        // Pass-through node that meters the signal (the group's level meter).
        struct MeterNode
        {
            ma_node_base Base; // must be first
            AudioMeter Meter;
            ma_uint32 Channels = 2, SampleRate = 48000;
        };

        void MeterProcess(ma_node* node, const float** framesIn, ma_uint32* frameCountIn, float** framesOut, ma_uint32* frameCountOut)
        {
            auto* m = reinterpret_cast<MeterNode*>(node);
            // A passthrough node: miniaudio has already read the input into the output buffer (framesIn == framesOut).
            ma_uint32 frames = std::min(*frameCountIn, *frameCountOut);
            if (framesIn[0] != framesOut[0]) std::memmove(framesOut[0], framesIn[0], (size_t)frames * m->Channels * sizeof(float));
            *frameCountIn = *frameCountOut = frames;
            m->Meter.Process(framesOut[0], frames, m->Channels, m->SampleRate);
        }

        ma_node_vtable s_MeterVtable = { MeterProcess, nullptr, 1, 1, MA_NODE_FLAG_PASSTHROUGH };

        // Mixer graph: per group a sound group (sounds attach here) feeding a low-pass filter, the optional echo and
        // reverb effects, then a meter, which feeds the parent. Effects that are off are bypassed (unlinked).
        struct GroupNodes
        {
            ma_sound_group Group;
            ma_lpf_node LowPass;
            ma_delay_node Echo;
            ma_reverb_node Reverb;
            MeterNode Meter;
            bool HasEcho = false, HasReverb = false; // initialized
            bool EchoLinked = false, ReverbLinked = false;
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
                ma_node_uninit(&g->Meter.Base, nullptr);
                if (g->HasReverb) ma_reverb_node_uninit(&g->Reverb, nullptr);
                if (g->HasEcho) ma_delay_node_uninit(&g->Echo, nullptr);
                ma_lpf_node_uninit(&g->LowPass, nullptr);
                g->Valid = false;
            }
            s_Groups.clear();
            s_BuildOrder.clear();
        }

        // Chains filter -> [echo] -> [reverb] -> meter for the effects that are on. miniaudio allows re-linking while
        // the device runs, so effects switch on and off live.
        void LinkEffects(GroupNodes& n, const AudioMixerGroup& g)
        {
            bool echo = n.HasEcho && g.Echo, reverb = n.HasReverb && g.Reverb;
            if (n.Valid && echo == n.EchoLinked && reverb == n.ReverbLinked) return;
            ma_node* chain[4];
            int count = 0;
            chain[count++] = &n.LowPass;
            if (echo) chain[count++] = &n.Echo;
            if (reverb) chain[count++] = &n.Reverb;
            chain[count++] = &n.Meter.Base;
            if (!echo && n.HasEcho) ma_node_detach_output_bus(&n.Echo, 0);
            if (!reverb && n.HasReverb) ma_node_detach_output_bus(&n.Reverb, 0);
            for (int k = 0; k + 1 < count; ++k) ma_node_attach_output_bus(chain[k], 0, chain[k + 1], 0);
            n.EchoLinked = echo;
            n.ReverbLinked = reverb;
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

    std::optional<AudioClipInfo> AudioEngine::GetClipInfo(const fs::path& file)
    {
        ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 0, 0);
        ma_decoder decoder;
        if (ma_decoder_init_file_w(file.wstring().c_str(), &cfg, &decoder) != MA_SUCCESS) return std::nullopt;
        AudioClipInfo info;
        ma_format format;
        ma_decoder_get_data_format(&decoder, &format, &info.Channels, &info.SampleRate, nullptr, 0);
        ma_uint64 frames = 0;
        if (ma_decoder_get_length_in_pcm_frames(&decoder, &frames) == MA_SUCCESS) info.Frames = frames;
        if (info.Frames == 0 && info.Channels > 0)
        {
            // Some decoders (Vorbis through miniaudio) do not report a length: count the frames instead.
            std::vector<float> chunk(4096 * (size_t)info.Channels);
            ma_uint64 read = 0;
            while (ma_decoder_read_pcm_frames(&decoder, chunk.data(), 4096, &read) == MA_SUCCESS && read > 0) info.Frames += read;
        }
        info.Seconds = info.SampleRate ? (float)((double)info.Frames / info.SampleRate) : 0.0f;
        ma_decoder_uninit(&decoder);
        std::string ext = file.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        info.Format = ext == ".ogg" ? "Ogg Vorbis" : ext == ".mp3" ? "MP3" : ext == ".flac" ? "FLAC" : "WAV";
        return info;
    }

    namespace
    {
        // A running snapshot transition: start and target values per group index.
        struct MixerTransition
        {
            struct Track { int Group; float FromDb, ToDb, FromHz, ToHz; };
            std::vector<Track> Tracks;
            float Duration = 0.0f, Elapsed = 0.0f;
            bool Active = false;
        };
        MixerTransition s_Transition;

        constexpr float kLowPassOpenHz = 22000.0f; // "off" while blending: above hearing, so the blend is smooth
        float CutoffForBlend(float hz) { return hz <= 0.0f ? kLowPassOpenHz : std::clamp(hz, 10.0f, kLowPassOpenHz); }
    }

    std::vector<AudioMixerSnapshot>& AudioMixer::Snapshots() { return Project::GetConfig().MixerSnapshots; }

    int AudioMixer::FindSnapshot(const std::string& name)
    {
        auto& snaps = Snapshots();
        for (size_t i = 0; i < snaps.size(); ++i)
            if (snaps[i].Name == name) return (int)i;
        return -1;
    }

    void AudioMixer::CaptureSnapshot(const std::string& name)
    {
        AudioMixerSnapshot snap;
        snap.Name = name;
        for (auto& g : Groups()) snap.Values.push_back({ g.Name, g.VolumeDb, g.LowPassHz });
        int i = FindSnapshot(name);
        if (i >= 0) Snapshots()[i] = std::move(snap);
        else Snapshots().push_back(std::move(snap));
    }

    bool AudioMixer::TransitionToSnapshot(const std::string& name, float seconds)
    {
        int s = FindSnapshot(name);
        if (s < 0)
        {
            Log::Warn("Audio Mixer has no snapshot '{}'", name);
            return false;
        }
        auto& groups = Groups();
        s_Transition = {};
        for (auto& v : Snapshots()[s].Values)
        {
            int g = FindGroup(v.Group);
            if (g < 0) continue;
            s_Transition.Tracks.push_back({ g, groups[g].VolumeDb, v.VolumeDb, CutoffForBlend(groups[g].LowPassHz), CutoffForBlend(v.LowPassHz) });
        }
        s_Transition.Duration = std::max(seconds, 0.0f);
        s_Transition.Active = true;
        Update(0.0f); // applies at once when the duration is 0
        return true;
    }

    bool AudioMixer::IsTransitioning() { return s_Transition.Active; }
    void AudioMixer::CancelTransition() { s_Transition.Active = false; }

    void AudioMixer::Update(float dt)
    {
        if (!s_Transition.Active) return;
        s_Transition.Elapsed += dt;
        float t = s_Transition.Duration > 0.0f ? std::clamp(s_Transition.Elapsed / s_Transition.Duration, 0.0f, 1.0f) : 1.0f;
        auto& groups = Groups();
        for (auto& tr : s_Transition.Tracks)
        {
            if (tr.Group >= (int)groups.size()) continue;
            auto& g = groups[tr.Group];
            g.VolumeDb = tr.FromDb + (tr.ToDb - tr.FromDb) * t;
            float hz = std::exp2(std::log2(tr.FromHz) + (std::log2(tr.ToHz) - std::log2(tr.FromHz)) * t);
            g.LowPassHz = hz >= kLowPassOpenHz * 0.999f ? 0.0f : hz;
        }
        if (t >= 1.0f) s_Transition.Active = false;
        Apply();
    }

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
            ma_node_config meterCfg = ma_node_config_init();
            meterCfg.vtable = &s_MeterVtable;
            meterCfg.pInputChannels = &channels;
            meterCfg.pOutputChannels = &channels;
            nodes->Meter.Channels = channels;
            nodes->Meter.SampleRate = rate;
            if (ma_node_init(graph, &meterCfg, nullptr, &nodes->Meter.Base) != MA_SUCCESS)
            {
                ma_lpf_node_uninit(&nodes->LowPass, nullptr);
                ma_sound_group_uninit(&nodes->Group);
                continue;
            }
            // Effects: the echo's buffer length is its delay, so a new delay time needs a rebuild.
            ma_uint32 delayFrames = std::max<ma_uint32>(1, (ma_uint32)(std::clamp(groups[i].EchoDelayMs, 10.0f, 5000.0f) * 0.001f * rate));
            ma_delay_node_config echoCfg = ma_delay_node_config_init(channels, rate, delayFrames, std::clamp(groups[i].EchoDecay, 0.0f, 0.95f));
            nodes->HasEcho = ma_delay_node_init(graph, &echoCfg, nullptr, &nodes->Echo) == MA_SUCCESS;
            if (channels <= 2)
            {
                ma_reverb_node_config reverbCfg = ma_reverb_node_config_init(channels, rate);
                nodes->HasReverb = ma_reverb_node_init(graph, &reverbCfg, nullptr, &nodes->Reverb) == MA_SUCCESS;
            }
            int parent = ParentOf(groups, i);
            ma_node* destination = parent >= 0 && s_Groups[parent] ? (ma_node*)&s_Groups[parent]->Group : ma_engine_get_endpoint(s_Engine);
            ma_node_attach_output_bus(&nodes->Group, 0, &nodes->LowPass, 0);
            ma_node_attach_output_bus(&nodes->Meter.Base, 0, destination, 0);
            LinkEffects(*nodes, groups[i]);
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
            const AudioMixerGroup& mg = groups[i];
            if (g->HasEcho)
            {
                ma_delay_node_set_wet(&g->Echo, std::clamp(mg.EchoWet, 0.0f, 1.0f));
                ma_delay_node_set_dry(&g->Echo, 1.0f);
                ma_delay_node_set_decay(&g->Echo, std::clamp(mg.EchoDecay, 0.0f, 0.95f)); // a new delay time applies at the next rebuild (play start)
            }
            if (g->HasReverb)
            {
                verblib_set_room_size(&g->Reverb.reverb, std::clamp(mg.ReverbRoomSize, 0.0f, 1.0f));
                verblib_set_damping(&g->Reverb.reverb, std::clamp(mg.ReverbDamping, 0.0f, 1.0f));
                verblib_set_wet(&g->Reverb.reverb, std::clamp(mg.ReverbWet, 0.0f, 1.0f) * 3.0f); // verblib's wet scale is 0-3
                verblib_set_dry(&g->Reverb.reverb, 1.0f);
            }
            LinkEffects(*g, mg);
            ma_lpf_config cfg = ma_lpf_config_init(ma_format_f32, channels, rate, CutoffOf(groups[i], rate), 2);
            ma_lpf_node_reinit(&cfg, &g->LowPass);
        }
    }

    void AudioMeter::Process(const float* samples, uint32_t frames, uint32_t channels, uint32_t sampleRate)
    {
        size_t n = (size_t)frames * channels;
        if (n == 0) return;
        float peak = 0.0f;
        double sum = 0.0;
        for (size_t i = 0; i < n; ++i)
        {
            float v = samples[i];
            peak = std::max(peak, std::abs(v));
            sum += (double)v * v;
        }
        float prev = m_Peak.load(std::memory_order_relaxed);
        while (peak > prev && !m_Peak.compare_exchange_weak(prev, peak, std::memory_order_relaxed)) {}
        // One-pole smoothing of the mean square, frame-rate independent.
        float a = 1.0f - std::exp(-(float)frames / (kRmsSeconds * (float)std::max(sampleRate, 1u)));
        float ms = m_MeanSquare.load(std::memory_order_relaxed);
        m_MeanSquare.store(ms + ((float)(sum / (double)n) - ms) * a, std::memory_order_relaxed);
    }

    AudioLevel AudioMeter::Read()
    {
        AudioLevel level;
        level.Peak = m_Peak.exchange(0.0f, std::memory_order_relaxed);
        level.Rms = std::sqrt(std::max(0.0f, m_MeanSquare.load(std::memory_order_relaxed)));
        return level;
    }

    AudioLevel AudioMixer::GetLevel(int group)
    {
        if (!s_Engine || s_StructureDirty || group < 0 || group >= (int)s_Groups.size() || !s_Groups[group] || !s_Groups[group]->Valid)
            return {};
        return s_Groups[group]->Meter.Meter.Read();
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
