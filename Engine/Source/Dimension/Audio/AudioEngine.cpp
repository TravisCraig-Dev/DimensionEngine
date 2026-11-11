#include "Dimension/Audio/AudioEngine.h"
#include "Dimension/Audio/Convolver.h"
#include "Dimension/Audio/VoiceChat.h"
#include "Dimension/Physics/Physics3D.h"
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
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4701) // (third-party: a variable MSVC cannot prove set)
#endif
#include <extras/stb_vorbis.c>
#ifdef _MSC_VER
#pragma warning(pop)
#endif
// Opus (.opus, Ogg Opus) through libopus + opusfile, as a custom miniaudio decoding backend.
#include <extras/decoders/libopus/miniaudio_libopus.c>

namespace Dimension
{
    namespace
    {
        ma_engine* s_Engine = nullptr;
        ma_resource_manager* s_ResourceManager = nullptr;
        ma_decoding_backend_vtable* s_CustomBackends[] = { ma_decoding_backend_libopus };

        // The editor's clip preview.
        ma_sound* s_Preview = nullptr;
        fs::path s_PreviewFile;

        // A source's sound, and with Occlusion the low-pass between it and its mixer group.
        struct SourceVoice
        {
            ma_sound Sound; // first: SoundOf casts the runtime pointer
            ma_lpf_node Filter;
            bool HasFilter = false;
            float Cutoff = 0.0f; // current filter cutoff (Hz)
        };

        ma_sound* SoundOf(AudioSourceComponent& src) { return static_cast<ma_sound*>(src.Runtime.get()); }
        SourceVoice* VoiceOf(AudioSourceComponent& src) { return static_cast<SourceVoice*>(src.Runtime.get()); }

        constexpr float kOpenCutoff = 20000.0f;   // filter wide open: no audible effect
        constexpr float kOcclusionFadeSeconds = 0.15f;

        float DbToLinear(float db) { return std::pow(10.0f, db / 20.0f); }

        // Volume and cutoff for the source's current occlusion amount.
        float OccludedVolume(const AudioSourceComponent& src)
        {
            float base = src.Mute ? 0.0f : src.Volume;
            return src.Occlusion ? base * (1.0f + (DbToLinear(src.OcclusionVolume) - 1.0f) * src.OcclusionAmount) : base;
        }
        float OccludedCutoff(const AudioSourceComponent& src)
        {
            float closed = std::clamp(src.OcclusionCutoff, 20.0f, kOpenCutoff);
            return std::exp(std::log(kOpenCutoff) + (std::log(closed) - std::log(kOpenCutoff)) * std::clamp(src.OcclusionAmount, 0.0f, 1.0f));
        }

        void SetCutoff(SourceVoice& v, float cutoff)
        {
            if (!v.HasFilter || std::abs(cutoff - v.Cutoff) < v.Cutoff * 0.01f) return;
            ma_lpf_config cfg = ma_lpf_config_init(ma_format_f32, ma_engine_get_channels(s_Engine), ma_engine_get_sample_rate(s_Engine),
                                                   std::min(cutoff, ma_engine_get_sample_rate(s_Engine) * 0.45f), 2);
            ma_lpf_node_reinit(&cfg, &v.Filter);
            v.Cutoff = cutoff;
        }

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

        // Gain stage of a send (between the group's splitter and the target group).
        struct SendNode
        {
            ma_node_base Base; // must be first
            std::atomic<float> Gain{ 1.0f };
            ma_uint32 Channels = 2;
        };

        void SendProcess(ma_node* node, const float** framesIn, ma_uint32* frameCountIn, float** framesOut, ma_uint32* frameCountOut)
        {
            auto* s = reinterpret_cast<SendNode*>(node);
            ma_uint32 frames = std::min(*frameCountIn, *frameCountOut);
            *frameCountIn = *frameCountOut = frames;
            float gain = s->Gain.load(std::memory_order_relaxed);
            size_t n = (size_t)frames * s->Channels;
            for (size_t i = 0; i < n; ++i) framesOut[0][i] = framesIn[0][i] * gain;
        }

        ma_node_vtable s_SendVtable = { SendProcess, nullptr, 1, 1, 0 };

        // Convolution reverb insert (see Convolver): dry signal plus the impulse-response tail at Wet.
        struct ConvolutionNode
        {
            ma_node_base Base;
            Convolver Conv;
            std::atomic<float> Wet{ 0.4f };
        };

        void ConvolutionProcess(ma_node* node, const float** framesIn, ma_uint32* frameCountIn, float** framesOut, ma_uint32* frameCountOut)
        {
            auto* n = reinterpret_cast<ConvolutionNode*>(node);
            ma_uint32 frames = std::min(*frameCountIn, *frameCountOut);
            *frameCountIn = *frameCountOut = frames;
            n->Conv.Process(framesIn[0], framesOut[0], frames, n->Wet.load(std::memory_order_relaxed));
        }

        ma_node_vtable s_ConvolutionVtable = { ConvolutionProcess, nullptr, 1, 1, 0 };

        // An audio file as interleaved floats at `rate` (its own channel count), at most `maxSeconds` long.
        bool DecodeFile(const fs::path& file, ma_uint32 rate, double maxSeconds, std::vector<float>& out, ma_uint32& channels)
        {
            ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 0, rate);
            cfg.ppCustomBackendVTables = s_CustomBackends;
            cfg.customBackendCount = 1;
            ma_decoder decoder;
            if (ma_decoder_init_file_w(file.wstring().c_str(), &cfg, &decoder) != MA_SUCCESS) return false;
            ma_format format;
            ma_uint32 sampleRate = 0;
            ma_decoder_get_data_format(&decoder, &format, &channels, &sampleRate, nullptr, 0);
            ma_uint64 limit = (ma_uint64)(maxSeconds * rate);
            out.clear();
            std::vector<float> chunk(4096 * (size_t)std::max(channels, 1u));
            ma_uint64 total = 0;
            while (total < limit)
            {
                ma_uint64 read = 0;
                if (ma_decoder_read_pcm_frames(&decoder, chunk.data(), std::min<ma_uint64>(4096, limit - total), &read) != MA_SUCCESS || read == 0) break;
                out.insert(out.end(), chunk.begin(), chunk.begin() + (size_t)(read * channels));
                total += read;
            }
            ma_decoder_uninit(&decoder);
            return total > 0 && channels > 0;
        }

        // Mixer graph: per group a sound group (sounds attach here) feeding a low-pass filter, the optional echo and
        // reverb effects, then a meter and a splitter: output 0 feeds the parent, output 1 the send target (if any).
        // Effects that are off are bypassed (unlinked).
        struct GroupNodes
        {
            ma_sound_group Group;
            ma_lpf_node LowPass;
            ma_delay_node Echo;
            ma_reverb_node Reverb;
            ConvolutionNode Convolution;
            MeterNode Meter;
            ma_splitter_node Splitter;
            SendNode Send;
            bool HasEcho = false, HasReverb = false, HasConvolution = false; // initialized
            bool EchoLinked = false, ReverbLinked = false, ConvolutionLinked = false;
            int LinkedSend = -1;
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
                ma_node_uninit(&g->Send.Base, nullptr);
                ma_splitter_node_uninit(&g->Splitter, nullptr);
                ma_node_uninit(&g->Meter.Base, nullptr);
                if (g->HasConvolution) ma_node_uninit(&g->Convolution.Base, nullptr);
                if (g->HasReverb) ma_reverb_node_uninit(&g->Reverb, nullptr);
                if (g->HasEcho) ma_delay_node_uninit(&g->Echo, nullptr);
                ma_lpf_node_uninit(&g->LowPass, nullptr);
                g->Valid = false;
            }
            s_Groups.clear();
            s_BuildOrder.clear();
        }

        // Reverb zones' current influence, per group.
        std::vector<AudioMixer::ZoneReverb> s_ZoneReverbs;
        AudioMixer::ZoneReverb ZoneOf(int group)
        {
            return group >= 0 && group < (int)s_ZoneReverbs.size() ? s_ZoneReverbs[group] : AudioMixer::ZoneReverb{};
        }

        // Chains filter -> [echo] -> [reverb] -> meter for the effects that are on. miniaudio allows re-linking while
        // the device runs, so effects switch on and off live.
        void LinkEffects(GroupNodes& n, const AudioMixerGroup& g, bool reverbOn)
        {
            bool echo = n.HasEcho && g.Echo, reverb = n.HasReverb && reverbOn, conv = n.HasConvolution && g.Convolution;
            if (n.Valid && echo == n.EchoLinked && reverb == n.ReverbLinked && conv == n.ConvolutionLinked) return;
            ma_node* chain[5];
            int count = 0;
            chain[count++] = &n.LowPass;
            if (echo) chain[count++] = &n.Echo;
            if (reverb) chain[count++] = &n.Reverb;
            if (conv) chain[count++] = &n.Convolution.Base;
            chain[count++] = &n.Meter.Base;
            if (!echo && n.HasEcho) ma_node_detach_output_bus(&n.Echo, 0);
            if (!reverb && n.HasReverb) ma_node_detach_output_bus(&n.Reverb, 0);
            if (!conv && n.HasConvolution) ma_node_detach_output_bus(&n.Convolution.Base, 0);
            for (int k = 0; k + 1 < count; ++k) ma_node_attach_output_bus(chain[k], 0, chain[k + 1], 0);
            n.EchoLinked = echo;
            n.ReverbLinked = reverb;
            n.ConvolutionLinked = conv;
        }

        // Points the group's send (splitter output 1 -> send gain) at its target group (live).
        void LinkSend(GroupNodes& n, int target)
        {
            if (target >= 0 && (target >= (int)s_Groups.size() || !s_Groups[target] || !s_Groups[target]->Valid)) target = -1;
            if (target == n.LinkedSend) return;
            if (n.LinkedSend >= 0) ma_node_detach_output_bus(&n.Send.Base, 0);
            if (target >= 0) ma_node_attach_output_bus(&n.Send.Base, 0, &s_Groups[target]->Group, 0);
            n.LinkedSend = target;
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

    std::vector<int> AudioMixer::ValidSends(const std::vector<AudioMixerGroup>& groups)
    {
        int n = (int)groups.size();
        std::vector<int> sends(n, -1);
        // Does `from` lead to `to` along parents and the sends accepted so far?
        auto reaches = [&](int from, int to) {
            std::vector<uint8_t> seen(n, 0);
            std::vector<int> stack{ from };
            while (!stack.empty())
            {
                int g = stack.back();
                stack.pop_back();
                if (g == to) return true;
                if (g < 0 || g >= n || seen[g]) continue;
                seen[g] = 1;
                stack.push_back(ParentOf(groups, g));
                if (sends[g] >= 0) stack.push_back(sends[g]);
            }
            return false;
        };
        for (int i = 0; i < n; ++i)
        {
            int t = groups[i].SendTarget;
            if (t >= 0 && t < n && t != i && !reaches(t, i)) sends[i] = t;
        }
        return sends;
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
        cfg.ppCustomBackendVTables = s_CustomBackends;
        cfg.customBackendCount = 1;
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
        info.Format = ext == ".opus" ? "Opus" : ext == ".ogg" ? "Ogg Vorbis" : ext == ".mp3" ? "MP3" : ext == ".flac" ? "FLAC" : "WAV";
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
            // Convolution: the impulse response is decoded at the device rate (up to 6 s).
            if (groups[i].ConvolutionImpulse)
            {
                std::vector<float> impulse;
                ma_uint32 irChannels = 0;
                fs::path irPath = AssetManager::GetAbsolutePath(AssetHandle(groups[i].ConvolutionImpulse));
                if (!irPath.empty() && DecodeFile(irPath, rate, 6.0, impulse, irChannels))
                {
                    nodes->Convolution.Conv.Init(impulse.data(), impulse.size() / irChannels, (int)irChannels, (int)channels, 1024);
                    ma_node_config convCfg = ma_node_config_init();
                    convCfg.vtable = &s_ConvolutionVtable;
                    convCfg.pInputChannels = &channels;
                    convCfg.pOutputChannels = &channels;
                    nodes->HasConvolution = nodes->Convolution.Conv.IsReady() &&
                                            ma_node_init(graph, &convCfg, nullptr, &nodes->Convolution.Base) == MA_SUCCESS;
                }
                else
                    Log::Warn("Audio mixer: cannot read the impulse response of group '{}'", groups[i].Name);
            }
            ma_splitter_node_config splitCfg = ma_splitter_node_config_init(channels);
            ma_node_config sendCfg = ma_node_config_init();
            sendCfg.vtable = &s_SendVtable;
            sendCfg.pInputChannels = &channels;
            sendCfg.pOutputChannels = &channels;
            nodes->Send.Channels = channels;
            bool splitOk = ma_splitter_node_init(graph, &splitCfg, nullptr, &nodes->Splitter) == MA_SUCCESS;
            if (splitOk && ma_node_init(graph, &sendCfg, nullptr, &nodes->Send.Base) != MA_SUCCESS)
            {
                ma_splitter_node_uninit(&nodes->Splitter, nullptr);
                splitOk = false;
            }
            if (!splitOk)
            {
                if (nodes->HasConvolution) ma_node_uninit(&nodes->Convolution.Base, nullptr);
                if (nodes->HasReverb) ma_reverb_node_uninit(&nodes->Reverb, nullptr);
                if (nodes->HasEcho) ma_delay_node_uninit(&nodes->Echo, nullptr);
                ma_node_uninit(&nodes->Meter.Base, nullptr);
                ma_lpf_node_uninit(&nodes->LowPass, nullptr);
                ma_sound_group_uninit(&nodes->Group);
                continue;
            }
            int parent = ParentOf(groups, i);
            ma_node* destination = parent >= 0 && s_Groups[parent] ? (ma_node*)&s_Groups[parent]->Group : ma_engine_get_endpoint(s_Engine);
            ma_node_attach_output_bus(&nodes->Group, 0, &nodes->LowPass, 0);
            ma_node_attach_output_bus(&nodes->Meter.Base, 0, &nodes->Splitter, 0);
            ma_node_attach_output_bus(&nodes->Splitter, 0, destination, 0);
            ma_node_attach_output_bus(&nodes->Splitter, 1, &nodes->Send.Base, 0);
            LinkEffects(*nodes, groups[i], AudioMixer::EffectiveReverb(groups[i], ZoneOf(i)).On);
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
        std::vector<int> sends = ValidSends(groups);
        for (size_t i = 0; i < s_Groups.size() && i < groups.size(); ++i)
        {
            auto& g = s_Groups[i];
            if (!g || !g->Valid) continue;
            ma_sound_group_set_volume(&g->Group, EffectiveGain((int)i));
            const AudioMixerGroup& mg = groups[i];
            LinkSend(*g, sends[i]);
            g->Send.Gain.store(DbToLinear(mg.SendLevelDb), std::memory_order_relaxed);
            if (g->HasEcho)
            {
                ma_delay_node_set_wet(&g->Echo, std::clamp(mg.EchoWet, 0.0f, 1.0f));
                ma_delay_node_set_dry(&g->Echo, 1.0f);
                ma_delay_node_set_decay(&g->Echo, std::clamp(mg.EchoDecay, 0.0f, 0.95f)); // a new delay time applies at the next rebuild (play start)
            }
            g->Convolution.Wet.store(std::clamp(mg.ConvolutionWet, 0.0f, 2.0f), std::memory_order_relaxed);
            ReverbParams reverb = EffectiveReverb(mg, ZoneOf((int)i));
            if (g->HasReverb && reverb.On)
            {
                verblib_set_room_size(&g->Reverb.reverb, reverb.RoomSize);
                verblib_set_damping(&g->Reverb.reverb, reverb.Damping);
                verblib_set_wet(&g->Reverb.reverb, reverb.Wet * 3.0f); // verblib's wet scale is 0-3
                verblib_set_dry(&g->Reverb.reverb, 1.0f);
            }
            LinkEffects(*g, mg, reverb.On);
            ma_lpf_config cfg = ma_lpf_config_init(ma_format_f32, channels, rate, CutoffOf(groups[i], rate), 2);
            ma_lpf_node_reinit(&cfg, &g->LowPass);
        }
    }

    AudioMixer::ReverbParams AudioMixer::EffectiveReverb(const AudioMixerGroup& g, const ZoneReverb& zone)
    {
        ReverbParams own{ g.Reverb, std::clamp(g.ReverbRoomSize, 0.0f, 1.0f), std::clamp(g.ReverbDamping, 0.0f, 1.0f), std::clamp(g.ReverbWet, 0.0f, 1.0f) };
        float w = std::clamp(zone.Weight, 0.0f, 1.0f);
        if (w <= 0.0f) return own;
        // Without its own reverb the group fades from dry (no wet) in the zone's room.
        if (!own.On) own = { false, zone.RoomSize, zone.Damping, 0.0f };
        auto mix = [&](float a, float b) { return std::clamp(a + (b - a) * w, 0.0f, 1.0f); };
        return { true, mix(own.RoomSize, zone.RoomSize), mix(own.Damping, zone.Damping), mix(own.Wet, zone.Wet) };
    }

    void AudioMixer::SetZoneReverb(int group, const ZoneReverb& zone)
    {
        if (group < 0) return;
        if ((int)s_ZoneReverbs.size() <= group) s_ZoneReverbs.resize(group + 1);
        if (s_ZoneReverbs[group] == zone) return;
        s_ZoneReverbs[group] = zone;
        if (!s_Engine || group >= (int)s_Groups.size() || group >= (int)Groups().size()) return;
        auto& g = s_Groups[group];
        if (!g || !g->Valid) return;
        ReverbParams reverb = EffectiveReverb(Groups()[group], zone);
        if (g->HasReverb && reverb.On)
        {
            verblib_set_room_size(&g->Reverb.reverb, reverb.RoomSize);
            verblib_set_damping(&g->Reverb.reverb, reverb.Damping);
            verblib_set_wet(&g->Reverb.reverb, reverb.Wet * 3.0f);
            verblib_set_dry(&g->Reverb.reverb, 1.0f);
        }
        LinkEffects(*g, Groups()[group], reverb.On);
    }

    AudioMixer::ZoneReverb AudioMixer::GetZoneReverb(int group) { return ZoneOf(group); }

    void AudioMixer::ClearZoneReverbs()
    {
        for (int i = 0; i < (int)s_ZoneReverbs.size(); ++i) SetZoneReverb(i, {});
        s_ZoneReverbs.clear();
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
        // Our own resource manager, so sounds can decode Opus too.
        s_ResourceManager = new ma_resource_manager();
        ma_resource_manager_config rm = ma_resource_manager_config_init();
        rm.decodedFormat = ma_format_f32;
        rm.ppCustomDecodingBackendVTables = s_CustomBackends;
        rm.customDecodingBackendCount = 1;
        if (ma_resource_manager_init(&rm, s_ResourceManager) != MA_SUCCESS)
        {
            delete s_ResourceManager;
            s_ResourceManager = nullptr;
        }
        ma_engine_config config = ma_engine_config_init();
        config.pResourceManager = s_ResourceManager;
        s_Engine = new ma_engine();
        if (ma_engine_init(&config, s_Engine) != MA_SUCCESS)
        {
            Log::Warn("No audio device available; audio disabled");
            delete s_Engine;
            s_Engine = nullptr;
            if (s_ResourceManager)
            {
                ma_resource_manager_uninit(s_ResourceManager);
                delete s_ResourceManager;
                s_ResourceManager = nullptr;
            }
            return;
        }
        Log::Info("Audio engine initialized ({} Hz)", ma_engine_get_sample_rate(s_Engine));
    }

    void AudioEngine::Shutdown()
    {
        VoiceChat::Default().Stop(); // its sound and thread, before the engine goes
        PreviewStop();
        AudioMixer::Shutdown();
        if (!s_Engine) return;
        ma_engine_uninit(s_Engine);
        delete s_Engine;
        s_Engine = nullptr;
        if (s_ResourceManager)
        {
            ma_resource_manager_uninit(s_ResourceManager);
            delete s_ResourceManager;
            s_ResourceManager = nullptr;
        }
    }

    bool AudioEngine::IsAvailable() { return s_Engine != nullptr; }

    float AudioWaveform::Peak() const
    {
        float peak = 0.0f;
        for (float v : Min) peak = std::max(peak, -v);
        for (float v : Max) peak = std::max(peak, v);
        return std::min(peak, 1.0f);
    }

    std::optional<AudioWaveform> AudioEngine::GetWaveform(const fs::path& file, size_t buckets)
    {
        if (buckets == 0) return std::nullopt;
        auto info = GetClipInfo(file); // the length (counted for decoders that do not know it)
        if (!info || info->Channels == 0 || info->Frames == 0) return std::nullopt;
        ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 0, 0);
        cfg.ppCustomBackendVTables = s_CustomBackends;
        cfg.customBackendCount = 1;
        ma_decoder decoder;
        if (ma_decoder_init_file_w(file.wstring().c_str(), &cfg, &decoder) != MA_SUCCESS) return std::nullopt;
        AudioWaveform w;
        w.Channels = info->Channels;
        w.Buckets = buckets;
        w.Seconds = info->Seconds;
        w.Min.assign(buckets * w.Channels, 0.0f);
        w.Max.assign(buckets * w.Channels, 0.0f);
        std::vector<bool> touched(buckets, false);
        std::vector<float> chunk(4096 * (size_t)w.Channels);
        uint64_t frame = 0;
        ma_uint64 read = 0;
        while (ma_decoder_read_pcm_frames(&decoder, chunk.data(), 4096, &read) == MA_SUCCESS && read > 0)
        {
            for (ma_uint64 f = 0; f < read; ++f, ++frame)
            {
                size_t b = std::min<size_t>((size_t)(frame * buckets / info->Frames), buckets - 1);
                for (uint32_t c = 0; c < w.Channels; ++c)
                {
                    float v = std::clamp(chunk[f * w.Channels + c], -1.0f, 1.0f);
                    float& lo = w.Min[c * buckets + b];
                    float& hi = w.Max[c * buckets + b];
                    if (!touched[b]) lo = hi = v;
                    else lo = std::min(lo, v), hi = std::max(hi, v);
                }
                touched[b] = true;
            }
        }
        ma_decoder_uninit(&decoder);
        return w;
    }

    bool AudioEngine::PreviewPlay(const fs::path& file, float offsetSeconds, bool loop)
    {
        PreviewStop();
        if (!s_Engine) return false;
        auto* sound = new ma_sound();
        if (ma_sound_init_from_file_w(s_Engine, file.wstring().c_str(), MA_SOUND_FLAG_STREAM, nullptr, nullptr, sound) != MA_SUCCESS)
        {
            delete sound;
            return false;
        }
        ma_sound_set_spatialization_enabled(sound, MA_FALSE);
        ma_sound_set_looping(sound, loop ? MA_TRUE : MA_FALSE);
        if (offsetSeconds > 0.0f)
        {
            ma_uint32 rate = 0;
            ma_sound_get_data_format(sound, nullptr, nullptr, &rate, nullptr, 0);
            ma_sound_seek_to_pcm_frame(sound, (ma_uint64)((double)offsetSeconds * rate));
        }
        ma_sound_start(sound);
        s_Preview = sound;
        s_PreviewFile = file;
        return true;
    }

    void AudioEngine::PreviewStop()
    {
        if (s_Preview)
        {
            ma_sound_uninit(s_Preview);
            delete s_Preview;
            s_Preview = nullptr;
        }
        s_PreviewFile.clear();
    }

    bool AudioEngine::PreviewIsPlaying() { return s_Preview && ma_sound_is_playing(s_Preview) && !ma_sound_at_end(s_Preview); }

    const fs::path& AudioEngine::PreviewFile() { return s_PreviewFile; }

    float AudioEngine::PreviewPosition()
    {
        if (!s_Preview) return 0.0f;
        float seconds = 0.0f;
        ma_sound_get_cursor_in_seconds(s_Preview, &seconds);
        return seconds;
    }
    void* AudioEngine::GetNative() { return s_Engine; }
    namespace
    {
        float s_MasterVolume = 1.0f;
        bool s_Paused = false;
    }
    void AudioEngine::SetMasterVolume(float v)
    {
        s_MasterVolume = std::max(v, 0.0f);
        if (s_Engine) ma_engine_set_volume(s_Engine, s_MasterVolume);
    }
    float AudioEngine::GetMasterVolume() { return s_MasterVolume; }
    void AudioEngine::SetPaused(bool paused)
    {
        if (paused == s_Paused) return;
        s_Paused = paused;
        if (!s_Engine) return;
        if (paused) ma_engine_stop(s_Engine);
        else ma_engine_start(s_Engine);
    }
    bool AudioEngine::IsPaused() { return s_Paused; }
    uint32_t AudioEngine::GetSampleRate() { return s_Engine ? ma_engine_get_sample_rate(s_Engine) : 0; }
    double AudioEngine::GetDspTime()
    {
        if (!s_Engine) return 0.0;
        return (double)ma_engine_get_time_in_pcm_frames(s_Engine) / std::max<double>(ma_engine_get_sample_rate(s_Engine), 1.0);
    }

    AudioWorld::AudioWorld(Scene& scene) : m_Scene(scene) { m_VolumeAtStart = AudioEngine::GetMasterVolume(); }

    AudioWorld::~AudioWorld()
    {
        // Play stopping: sound goes on again at the volume it had.
        AudioEngine::SetPaused(false);
        AudioEngine::SetMasterVolume(m_VolumeAtStart);
        for (auto [e, src] : m_Scene.GetRegistry().ViewOf<AudioSourceComponent>()) src.Runtime.reset();
        m_OneShots.clear();
        AudioMixer::ClearZoneReverbs();
    }

    AudioMixer::ZoneReverb AudioWorld::ReverbZoneAt(Scene& scene, const glm::vec3& listener, const std::string& group)
    {
        // Strength 1 inside Min Distance, falling linearly to 0 at Max Distance; overlapping zones blend their
        // settings by strength, and the strongest sets the weight.
        AudioMixer::ZoneReverb z;
        float total = 0.0f;
        for (auto [e, zone, t] : scene.GetRegistry().ViewOf<AudioReverbZoneComponent, TransformComponent>())
        {
            if (!t.ActiveInHierarchy || (zone.Group.empty() ? std::string("Master") : zone.Group) != group) continue;
            float d = glm::distance(listener, t.GetWorldPosition());
            float lo = std::max(zone.MinDistance, 0.0f), hi = std::max(zone.MaxDistance, lo + 1e-3f);
            float w = d <= lo ? 1.0f : d >= hi ? 0.0f : 1.0f - (d - lo) / (hi - lo);
            if (w <= 0.0f) continue;
            z.RoomSize += std::clamp(zone.RoomSize, 0.0f, 1.0f) * w;
            z.Damping += std::clamp(zone.Damping, 0.0f, 1.0f) * w;
            z.Wet += std::clamp(zone.Wet, 0.0f, 1.0f) * w;
            total += w;
            z.Weight = std::max(z.Weight, w);
        }
        if (total > 0.0f)
        {
            z.RoomSize /= total;
            z.Damping /= total;
            z.Wet /= total;
        }
        return z;
    }

    namespace
    {
        // Sound for `clip` routed into the source's mixer group, with the source's settings. Occluding sources get a
        // low-pass filter between the sound and the group.
        std::shared_ptr<void> MakeSound(const AudioSourceComponent& src, AssetHandle clip, bool stream)
        {
            fs::path path = AssetManager::GetAbsolutePath(clip);
            auto* voice = new SourceVoice();
            auto* group = static_cast<ma_sound_group*>(AudioMixer::GetGroupNode(src.Output.empty() ? 0 : AudioMixer::FindGroup(src.Output)));
            ma_uint32 flags = stream ? MA_SOUND_FLAG_STREAM : MA_SOUND_FLAG_DECODE;
            if (ma_sound_init_from_file_w(s_Engine, path.c_str(), flags, group, nullptr, &voice->Sound) != MA_SUCCESS)
            {
                Log::Error("Failed to load audio clip '{}'", path.string());
                delete voice;
                return nullptr;
            }
            if (src.Occlusion && src.Spatial)
            {
                ma_uint32 channels = ma_engine_get_channels(s_Engine), rate = ma_engine_get_sample_rate(s_Engine);
                ma_lpf_node_config cfg = ma_lpf_node_config_init(channels, rate, std::min(kOpenCutoff, rate * 0.45f), 2);
                if (ma_lpf_node_init(ma_engine_get_node_graph(s_Engine), &cfg, nullptr, &voice->Filter) == MA_SUCCESS)
                {
                    voice->HasFilter = true;
                    voice->Cutoff = kOpenCutoff;
                    ma_node* destination = group ? (ma_node*)group : ma_engine_get_endpoint(s_Engine);
                    ma_node_attach_output_bus(&voice->Filter, 0, destination, 0);
                    ma_node_attach_output_bus(&voice->Sound, 0, &voice->Filter, 0);
                    SetCutoff(*voice, OccludedCutoff(src));
                }
            }
            ma_sound* sound = &voice->Sound;
            ma_sound_set_volume(sound, OccludedVolume(src));
            ma_sound_set_pitch(sound, src.Pitch);
            ma_sound_set_spatialization_enabled(sound, src.Spatial);
            ma_sound_set_min_distance(sound, src.MinDistance);
            ma_sound_set_max_distance(sound, src.MaxDistance);
            ma_sound_set_attenuation_model(sound, src.Rolloff == AudioRolloff::Linear ? ma_attenuation_model_linear : ma_attenuation_model_inverse);
            ma_sound_set_doppler_factor(sound, std::max(src.DopplerLevel, 0.0f));
            return std::shared_ptr<void>(voice, [](void* p) {
                auto* v = static_cast<SourceVoice*>(p);
                ma_sound_uninit(&v->Sound);
                if (v->HasFilter) ma_lpf_node_uninit(&v->Filter, nullptr);
                delete v;
            });
        }

        bool IsTriggerCollider(Registry& reg, Entity e)
        {
            if (auto* c = reg.TryGet<BoxColliderComponent>(e); c && c->IsTrigger) return true;
            if (auto* c = reg.TryGet<SphereColliderComponent>(e); c && c->IsTrigger) return true;
            if (auto* c = reg.TryGet<CapsuleColliderComponent>(e); c && c->IsTrigger) return true;
            if (auto* c = reg.TryGet<MeshColliderComponent>(e); c && c->IsTrigger) return true;
            return false;
        }
    }

    void AudioWorld::CreateSource(Entity e, bool autoplay)
    {
        auto& src = m_Scene.GetRegistry().Get<AudioSourceComponent>(e);
        if (!s_Engine || src.Runtime || !src.Clip.IsValid()) return;
        src.Runtime = MakeSound(src, src.Clip, src.Stream);
        auto* sound = SoundOf(src);
        if (!sound) return;
        src.RoutedOutput = src.Output;
        src.HasLastPosition = false;
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

    bool AudioWorld::IsOccluded(Scene& scene, const glm::vec3& listener, Entity listenerEntity, Entity source)
    {
        return Occlusion(scene, listener, listenerEntity, source) > 0.0f;
    }

    float AudioWorld::Occlusion(Scene& scene, const glm::vec3& listener, Entity listenerEntity, Entity source)
    {
        auto& reg = scene.GetRegistry();
        if (!scene.GetPhysics3D() || !reg.Has<TransformComponent>(source)) return 0.0f;
        auto related = [&](Entity hit, Entity to) { // same entity, or one is an ancestor of the other
            if (to.IsNull()) return false;
            for (Entity p = hit; !p.IsNull(); p = scene.GetParent(p))
                if (p == to) return true;
            for (Entity p = to; !p.IsNull(); p = scene.GetParent(p))
                if (p == hit) return true;
            return false;
        };
        glm::vec3 d = reg.Get<TransformComponent>(source).GetWorldPosition() - listener;
        float len = glm::length(d);
        if (len < 1e-3f) return 0.0f;
        // Colliders along the line block it, except triggers and the source's and the listener's own. Each adds its
        // Audio Occluder amount (1 without one).
        float total = 0.0f;
        for (const RaycastHit& hit : Physics3DWorld::RaycastAll(scene, listener, d, len))
            if (!IsTriggerCollider(reg, hit.Hit) && !related(hit.Hit, source) && !related(hit.Hit, listenerEntity))
            {
                auto* occluder = reg.TryGet<AudioOccluderComponent>(hit.Hit);
                total += occluder ? std::max(occluder->Occlusion, 0.0f) : 1.0f;
            }
        return std::min(total, 1.0f);
    }

    void AudioWorld::Update(float dt)
    {
        auto& reg = m_Scene.GetRegistry();
        Entity cam = m_Scene.GetPrimaryCamera();
        // Occlusion amounts ease toward blocked (1) or clear (0); a first update snaps.
        if (!cam.IsNull())
        {
            glm::vec3 listener = reg.Get<TransformComponent>(cam).GetWorldPosition();
            float step = dt > 0.0f ? dt / kOcclusionFadeSeconds : 1.0f;
            for (auto [e, src, t] : reg.ViewOf<AudioSourceComponent, TransformComponent>())
            {
                if (!src.Occlusion || !src.Spatial || !t.ActiveInHierarchy) { src.OcclusionAmount = 0.0f; continue; }
                float target = Occlusion(m_Scene, listener, cam, e);
                src.OcclusionAmount += std::clamp(target - src.OcclusionAmount, -step, step);
            }
            // Reverb zones drive their groups' reverb from where the listener is.
            auto& groups = AudioMixer::Groups();
            for (int g = 0; g < (int)groups.size(); ++g) AudioMixer::SetZoneReverb(g, ReverbZoneAt(m_Scene, listener, groups[g].Name));
        }
        if (!s_Engine) return;
        if (!cam.IsNull())
        {
            auto& t = reg.Get<TransformComponent>(cam);
            glm::vec3 p = t.GetWorldPosition(), f = t.Forward(), u = t.Up();
            ma_engine_listener_set_position(s_Engine, 0, p.x, p.y, p.z);
            ma_engine_listener_set_direction(s_Engine, 0, f.x, f.y, f.z);
            ma_engine_listener_set_world_up(s_Engine, 0, u.x, u.y, u.z);
            // Its velocity (for the Doppler shift), from how far it moved.
            glm::vec3 v = m_HasListener && dt > 0.0f ? (p - m_ListenerLast) / dt : glm::vec3(0.0f);
            ma_engine_listener_set_velocity(s_Engine, 0, v.x, v.y, v.z);
            m_ListenerLast = p;
            m_HasListener = true;
        }
        for (auto [e, src, t] : reg.ViewOf<AudioSourceComponent, TransformComponent>())
        {
            ma_sound* s = SoundOf(src);
            if (!s) continue;
            glm::vec3 p = t.GetWorldPosition();
            ma_sound_set_position(s, p.x, p.y, p.z);
            ma_sound_set_volume(s, OccludedVolume(src));
            ma_sound_set_pitch(s, src.Pitch);
            SetCutoff(*VoiceOf(src), OccludedCutoff(src));
            // Its 3D settings as they are now (scripts change them while playing), and its velocity.
            ma_sound_set_spatialization_enabled(s, src.Spatial);
            ma_sound_set_min_distance(s, std::max(src.MinDistance, 0.01f));
            ma_sound_set_max_distance(s, std::max(src.MaxDistance, src.MinDistance));
            ma_sound_set_attenuation_model(s, src.Rolloff == AudioRolloff::Linear ? ma_attenuation_model_linear : ma_attenuation_model_inverse);
            ma_sound_set_doppler_factor(s, std::max(src.DopplerLevel, 0.0f));
            glm::vec3 v = src.HasLastPosition && dt > 0.0f ? (p - src.LastPosition) / dt : glm::vec3(0.0f);
            ma_sound_set_velocity(s, v.x, v.y, v.z);
            src.LastPosition = p;
            src.HasLastPosition = true;
            // Another mixer group: the sound (or its occlusion filter) goes into that one now.
            if (src.Output != src.RoutedOutput)
            {
                auto* group = static_cast<ma_node*>(AudioMixer::GetGroupNode(src.Output.empty() ? 0 : AudioMixer::FindGroup(src.Output)));
                ma_node* destination = group ? group : ma_engine_get_endpoint(s_Engine);
                SourceVoice* voice = VoiceOf(src);
                ma_node_attach_output_bus(voice->HasFilter ? (ma_node*)&voice->Filter : (ma_node*)&voice->Sound, 0, destination, 0);
                src.RoutedOutput = src.Output;
            }
        }
        // Release finished one-shots.
        std::erase_if(m_OneShots, [](const std::shared_ptr<void>& p) { return ma_sound_at_end(static_cast<ma_sound*>(p.get())); });
    }

    void AudioWorld::Play(Scene& scene, Entity e)
    {
        auto* src = scene.GetRegistry().TryGet<AudioSourceComponent>(e);
        if (!src || !SoundOf(*src)) return;
        src->Paused = false;
        ma_sound* sound = SoundOf(*src);
        ma_sound_set_start_time_in_pcm_frames(sound, 0); // now (a schedule is cancelled)
        ma_sound_set_stop_time_in_pcm_frames(sound, ~(ma_uint64)0);
        ma_sound_seek_to_pcm_frame(sound, 0);
        ma_sound_start(sound);
    }

    void AudioWorld::PlayScheduled(Scene& scene, Entity e, double dspTime)
    {
        auto* src = scene.GetRegistry().TryGet<AudioSourceComponent>(e);
        if (!src || !SoundOf(*src) || !s_Engine) return;
        src->Paused = false;
        ma_sound* sound = SoundOf(*src);
        ma_sound_stop(sound);
        ma_sound_seek_to_pcm_frame(sound, 0);
        ma_sound_set_stop_time_in_pcm_frames(sound, ~(ma_uint64)0);
        ma_sound_set_start_time_in_pcm_frames(sound, (ma_uint64)std::llround(std::max(dspTime, 0.0) * ma_engine_get_sample_rate(s_Engine)));
        ma_sound_start(sound);
    }

    void AudioWorld::SetScheduledEndTime(Scene& scene, Entity e, double dspTime)
    {
        auto* src = scene.GetRegistry().TryGet<AudioSourceComponent>(e);
        if (!src || !SoundOf(*src) || !s_Engine) return;
        ma_sound_set_stop_time_in_pcm_frames(SoundOf(*src), (ma_uint64)std::llround(std::max(dspTime, 0.0) * ma_engine_get_sample_rate(s_Engine)));
    }

    void AudioWorld::Stop(Scene& scene, Entity e)
    {
        auto* src = scene.GetRegistry().TryGet<AudioSourceComponent>(e);
        if (!src) return;
        src->Paused = false;
        if (SoundOf(*src)) ma_sound_stop(SoundOf(*src));
    }

    void AudioWorld::Pause(Scene& scene, Entity e)
    {
        auto* src = scene.GetRegistry().TryGet<AudioSourceComponent>(e);
        if (!src || !SoundOf(*src) || !ma_sound_is_playing(SoundOf(*src))) return;
        ma_sound_stop(SoundOf(*src)); // miniaudio keeps the cursor
        src->Paused = true;
    }

    void AudioWorld::UnPause(Scene& scene, Entity e)
    {
        auto* src = scene.GetRegistry().TryGet<AudioSourceComponent>(e);
        if (!src || !src->Paused) return;
        src->Paused = false;
        if (SoundOf(*src)) ma_sound_start(SoundOf(*src));
    }

    bool AudioWorld::IsPaused(Scene& scene, Entity e)
    {
        auto* src = scene.GetRegistry().TryGet<AudioSourceComponent>(e);
        return src && src->Paused;
    }

    float AudioWorld::GetTime(Scene& scene, Entity e)
    {
        auto* src = scene.GetRegistry().TryGet<AudioSourceComponent>(e);
        float t = 0.0f;
        if (src && SoundOf(*src)) ma_sound_get_cursor_in_seconds(SoundOf(*src), &t);
        return t;
    }

    void AudioWorld::SetTime(Scene& scene, Entity e, float seconds)
    {
        auto* src = scene.GetRegistry().TryGet<AudioSourceComponent>(e);
        ma_sound* sound = src ? SoundOf(*src) : nullptr;
        if (!sound) return;
        ma_uint32 rate = 0;
        ma_sound_get_data_format(sound, nullptr, nullptr, &rate, nullptr, 0);
        ma_uint64 frames = 0;
        ma_sound_get_length_in_pcm_frames(sound, &frames);
        ma_uint64 frame = (ma_uint64)(std::max(seconds, 0.0f) * (float)rate);
        ma_sound_seek_to_pcm_frame(sound, frames > 0 ? std::min(frame, frames - 1) : frame);
    }

    float AudioWorld::GetLength(Scene& scene, Entity e)
    {
        auto* src = scene.GetRegistry().TryGet<AudioSourceComponent>(e);
        float length = 0.0f;
        if (src && SoundOf(*src)) ma_sound_get_length_in_seconds(SoundOf(*src), &length);
        return length;
    }

    void AudioWorld::SetLoop(Scene& scene, Entity e, bool loop)
    {
        auto* src = scene.GetRegistry().TryGet<AudioSourceComponent>(e);
        if (!src) return;
        src->Loop = loop;
        if (SoundOf(*src)) ma_sound_set_looping(SoundOf(*src), loop ? MA_TRUE : MA_FALSE);
    }

    void AudioWorld::SetClip(Scene& scene, Entity e, AssetHandle clip)
    {
        auto* src = scene.GetRegistry().TryGet<AudioSourceComponent>(e);
        if (!src || src->Clip == clip) return;
        src->Runtime.reset(); // stops it
        src->Paused = false;
        src->Clip = clip;
        if (AudioWorld* world = scene.GetAudio()) world->CreateSource(e, false);
    }

    void AudioWorld::PlayClipAtPoint(Scene& scene, AssetHandle clip, const glm::vec3& position, float volume)
    {
        AudioWorld* world = scene.GetAudio();
        if (!world || !s_Engine || !clip.IsValid()) return;
        AudioSourceComponent settings; // a spatial source with the defaults
        settings.Clip = clip;
        settings.Volume = volume;
        settings.Spatial = true;
        auto sound = MakeSound(settings, clip, false);
        if (!sound) return;
        auto* s = static_cast<ma_sound*>(sound.get());
        ma_sound_set_position(s, position.x, position.y, position.z);
        ma_sound_start(s);
        world->m_OneShots.push_back(std::move(sound));
    }

    bool AudioWorld::IsPlaying(Scene& scene, Entity e)
    {
        auto* src = scene.GetRegistry().TryGet<AudioSourceComponent>(e);
        return src && SoundOf(*src) && ma_sound_is_playing(SoundOf(*src));
    }

    void AudioWorld::PlayClip(Scene& scene, Entity e, AssetHandle clip, float offset)
    {
        AudioWorld* world = scene.GetAudio();
        auto* src = scene.GetRegistry().TryGet<AudioSourceComponent>(e);
        if (!world || !src || !s_Engine || !clip.IsValid()) return;
        if (src->Clip != clip || !SoundOf(*src))
        {
            src->Runtime.reset();
            src->Clip = clip;
            world->CreateSource(e, false);
        }
        ma_sound* sound = SoundOf(*src);
        if (!sound) return;
        ma_uint32 rate = 0;
        ma_sound_get_data_format(sound, nullptr, nullptr, &rate, nullptr, 0);
        ma_sound_seek_to_pcm_frame(sound, (ma_uint64)(std::max(offset, 0.0f) * (float)rate));
        ma_sound_start(sound);
    }

    void AudioWorld::PlayOneShot(Scene& scene, Entity e, AssetHandle clip, float volumeScale)
    {
        AudioWorld* world = scene.GetAudio();
        auto* src = scene.GetRegistry().TryGet<AudioSourceComponent>(e);
        if (!world || !src || !s_Engine) return;
        auto sound = MakeSound(*src, clip.IsValid() ? clip : src->Clip, false);
        if (!sound) return;
        auto* s = static_cast<ma_sound*>(sound.get());
        ma_sound_set_volume(s, OccludedVolume(*src) * volumeScale); // keeps the occlusion it starts with
        glm::vec3 p = scene.GetRegistry().Get<TransformComponent>(e).GetWorldPosition();
        ma_sound_set_position(s, p.x, p.y, p.z);
        ma_sound_start(s);
        world->m_OneShots.push_back(std::move(sound));
    }
}
