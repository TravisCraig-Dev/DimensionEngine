#include "Dimension/Music/SongPlayer.h"
#include "Dimension/Asset/AssetManager.h"
#include "Dimension/Core/Log.h"
#define MA_NO_ENCODING // as where the implementation is built (AudioEngine.cpp)
#include <miniaudio.h>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>

namespace Dimension::Music
{
    namespace
    {
        constexpr double kPi = 3.14159265358979323846;

        // ---- Samples ----

        std::mutex s_SampleMutex;
        std::map<std::string, std::pair<fs::file_time_type, std::shared_ptr<const SampleData>>> s_Samples;
    }

    std::shared_ptr<const SampleData> LoadSample(const fs::path& file)
    {
        std::error_code ec;
        if (file.empty() || !fs::is_regular_file(file, ec)) return nullptr;
        auto stamp = fs::last_write_time(file, ec);
        std::string key = file.lexically_normal().string();
        {
            std::lock_guard lock(s_SampleMutex);
            if (auto it = s_Samples.find(key); it != s_Samples.end() && it->second.first == stamp) return it->second.second;
        }
        ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 2, 0); // the file's own rate
        ma_decoder decoder;
        if (ma_decoder_init_file(file.string().c_str(), &config, &decoder) != MA_SUCCESS)
        {
            Log::Warn("Sampler: cannot read '{}'", file.filename().string());
            return nullptr;
        }
        auto data = std::make_shared<SampleData>();
        data->SampleRate = decoder.outputSampleRate;
        float buffer[4096 * 2];
        for (;;)
        {
            ma_uint64 read = 0;
            if (ma_decoder_read_pcm_frames(&decoder, buffer, 4096, &read) != MA_SUCCESS && read == 0) break;
            if (read == 0) break;
            data->Frames.insert(data->Frames.end(), buffer, buffer + read * 2);
        }
        ma_decoder_uninit(&decoder);
        std::lock_guard lock(s_SampleMutex);
        s_Samples[key] = { stamp, data };
        return data;
    }

    fs::path ResolveSamplePath(const SamplerParams& s)
    {
        if (s.Sample && AssetManager::IsInitialized())
        {
            fs::path p = AssetManager::GetAbsolutePath(AssetHandle(s.Sample));
            if (!p.empty()) return p;
        }
        if (s.SamplePath.empty()) return {};
        fs::path p = s.SamplePath;
        if (p.is_relative() && AssetManager::IsInitialized()) p = AssetManager::GetAssetsDirectory() / p;
        return p;
    }

    // ---- Voices ----

    namespace
    {
        struct EnvState
        {
            enum Stage : uint8_t { Attack, Decay, Sustain, Release, Off } Stage = Attack;
            float Level = 0.0f, ReleaseFrom = 0.0f;

            float Next(const Envelope& e, float sr)
            {
                switch (Stage)
                {
                case Attack:
                    Level += 1.0f / std::max(1.0f, e.Attack * sr);
                    if (Level >= 1.0f) Level = 1.0f, Stage = Decay;
                    break;
                case Decay:
                    Level -= (1.0f - e.Sustain) / std::max(1.0f, e.Decay * sr);
                    if (Level <= e.Sustain) Level = e.Sustain, Stage = Sustain;
                    break;
                case Sustain: Level = e.Sustain; break;
                case Release:
                    Level -= ReleaseFrom / std::max(1.0f, e.Release * sr);
                    if (Level <= 0.0f) Level = 0.0f, Stage = Off;
                    break;
                case Off: Level = 0.0f; break;
                }
                return Level;
            }
            void StartRelease()
            {
                if (Stage == Off || Stage == Release) return;
                Stage = Release;
                ReleaseFrom = std::max(Level, 1e-4f);
            }
        };

        // Andrew Simper's state-variable filter (trapezoidal): low, band and high pass at once.
        struct Svf
        {
            float Ic1 = 0.0f, Ic2 = 0.0f;
            void Process(float x, float cutoff, float k, float sr, float& low, float& band, float& high)
            {
                float g = (float)std::tan(kPi * std::clamp(cutoff, 10.0f, sr * 0.45f) / sr);
                float a1 = 1.0f / (1.0f + g * (g + k)), a2 = g * a1, a3 = g * a2;
                float v3 = x - Ic2;
                float v1 = a1 * Ic1 + a2 * v3;
                float v2 = Ic2 + a2 * Ic1 + a3 * v3;
                Ic1 = 2.0f * v1 - Ic1;
                Ic2 = 2.0f * v2 - Ic2;
                low = v2;
                band = v1;
                high = x - k * v1 - v2;
            }
        };

        float PolyBlep(double t, double dt)
        {
            if (t < dt)
            {
                t /= dt;
                return (float)(t + t - t * t - 1.0);
            }
            if (t > 1.0 - dt)
            {
                t = (t - 1.0) / dt;
                return (float)(t * t + t + t + 1.0);
            }
            return 0.0f;
        }

        float Oscillator(Waveform w, double phase, double dt, float pulseWidth, uint32_t& seed)
        {
            switch (w)
            {
            case Waveform::Saw: return (float)(2.0 * phase - 1.0) - PolyBlep(phase, dt);
            case Waveform::Square:
            {
                double pw = std::clamp((double)pulseWidth, 0.05, 0.95);
                float v = phase < pw ? 1.0f : -1.0f;
                v += PolyBlep(phase, dt);
                v -= PolyBlep(std::fmod(phase + (1.0 - pw), 1.0), dt);
                return v;
            }
            case Waveform::Triangle: return (float)(phase < 0.5 ? 4.0 * phase - 1.0 : 3.0 - 4.0 * phase);
            case Waveform::Sine: return (float)std::sin(2.0 * kPi * phase);
            case Waveform::Noise:
            default:
                seed = seed * 1664525u + 1013904223u;
                return (float)((seed >> 8) & 0xFFFF) / 32768.0f - 1.0f;
            }
        }

        float Noise(uint32_t& seed)
        {
            seed = seed * 1664525u + 1013904223u;
            return (float)((seed >> 8) & 0xFFFF) / 32768.0f - 1.0f;
        }

        float KeyToHz(float key) { return 440.0f * std::pow(2.0f, (key - 69.0f) / 12.0f); }
    }

    struct SongPlayer::Voice
    {
        bool Active = false;
        int Channel = 0, Key = 60;
        float Velocity = 0.8f;
        uint64_t Age = 0;
        double GateFrames = -1.0; // frames until the note is released (-1: until NoteOff)
        bool Held = false;        // started by NoteOn (released by NoteOff)
        EnvState Amp, FilterEnv;
        // Synth
        double Phase1 = 0.0, Phase2 = 0.0;
        float Freq = 440.0f, TargetFreq = 440.0f;
        Svf Filter;
        // Drum
        double Time = 0.0;
        double DrumPhase = 0.0;
        std::array<double, 6> MetalPhase{};
        Svf DrumFilter;
        // Sampler
        double Pos = 0.0, Step = 1.0;
        uint32_t Seed = 1;
    };

    SongPlayer::SongPlayer(uint32_t sampleRate) : m_SampleRate(std::max(8000u, sampleRate)), m_Voices(new Voice[kMaxVoices]) {}
    SongPlayer::~SongPlayer() = default;

    void SongPlayer::SetRack(std::shared_ptr<PluginRack> rack)
    {
        std::lock_guard lock(m_Mutex);
        m_Rack = std::move(rack);
    }

    void SongPlayer::SetSong(const Song& song)
    {
        std::vector<std::shared_ptr<const SampleData>> samples;
        for (const Channel& c : song.Channels)
            samples.push_back(c.Type == InstrumentType::Sampler ? LoadSample(ResolveSamplePath(c.Sampler)) : nullptr);
        std::shared_ptr<PluginRack> rack;
        {
            std::lock_guard lock(m_Mutex);
            rack = m_Rack;
        }
        std::vector<std::shared_ptr<PluginInstance>> channelPlugins(song.Channels.size());
        std::vector<std::vector<std::shared_ptr<PluginInstance>>> effectPlugins(song.Mixer.size());
        if (rack)
        {
            for (size_t i = 0; i < song.Channels.size(); ++i)
                if (song.Channels[i].Type == InstrumentType::Plugin) channelPlugins[i] = rack->Get(song.Channels[i].Plugin.Id);
            for (size_t t = 0; t < song.Mixer.size(); ++t)
            {
                effectPlugins[t].resize(song.Mixer[t].Effects.size());
                for (size_t e = 0; e < song.Mixer[t].Effects.size(); ++e)
                    if (song.Mixer[t].Effects[e].Type == EffectType::Plugin) effectPlugins[t][e] = rack->Get(song.Mixer[t].Effects[e].Plugin.Id);
            }
        }
        std::lock_guard lock(m_Mutex);
        // Plug-in notes of instruments that went away (or moved) end.
        for (size_t i = 0; i < m_ChannelPlugins.size(); ++i)
            if (m_ChannelPlugins[i] && (i >= channelPlugins.size() || channelPlugins[i] != m_ChannelPlugins[i])) m_ChannelPlugins[i]->AllNotesOff();
        m_PluginNoteOffs.clear();
        m_ChannelPlugins.swap(channelPlugins);
        m_EffectPlugins.swap(effectPlugins);
        if (m_PluginBuffer.size() < kChunk * 2) m_PluginBuffer.assign(kChunk * 2, 0.0f), m_PluginDry.assign(kChunk * 2, 0.0f);
        // Voices of channels that went away stop.
        for (int i = 0; i < kMaxVoices; ++i)
            if (m_Voices[i].Active && (m_Voices[i].Channel >= (int)song.Channels.size() ||
                                       (m_Voices[i].Channel < (int)m_Song.Channels.size() && song.Channels[m_Voices[i].Channel].Type != m_Song.Channels[m_Voices[i].Channel].Type)))
                m_Voices[i].Active = false;
        m_Song = song;
        m_Samples = std::move(samples);
        int tracks = std::min((int)m_Song.Mixer.size(), kMaxMixer);
        m_Fx.resize(tracks);
        for (int t = 0; t < tracks; ++t)
        {
            const auto& effects = m_Song.Mixer[t].Effects;
            auto& fx = m_Fx[t];
            fx.resize(std::min((int)effects.size(), kMaxEffects));
            for (int e = 0; e < (int)fx.size(); ++e)
                if (effects[e].Type == EffectType::Plugin) fx[e].reset();
                else if (!fx[e] || fx[e]->GetType() != effects[e].Type) fx[e] = EffectProcessor::Create(effects[e].Type, m_SampleRate);
        }
        if (m_Bus.size() < (size_t)tracks * kChunk * 2) m_Bus.assign((size_t)tracks * kChunk * 2, 0.0f);
        m_Pattern = std::clamp(m_Pattern, 0, std::max(0, (int)m_Song.Patterns.size() - 1));
    }

    void SongPlayer::SetMode(Mode mode)
    {
        std::lock_guard lock(m_Mutex);
        if (m_Mode == mode) return;
        m_Mode = mode;
        m_Tick = 0.0;
    }

    void SongPlayer::SetPattern(int pattern)
    {
        std::lock_guard lock(m_Mutex);
        m_Pattern = std::clamp(pattern, 0, std::max(0, (int)m_Song.Patterns.size() - 1));
        if (m_Tick >= LoopEnd()) m_Tick = 0.0;
    }

    void SongPlayer::Play() { m_Playing = true; }
    void SongPlayer::Pause() { m_Playing = false; }

    void SongPlayer::Stop()
    {
        std::lock_guard lock(m_Mutex);
        m_Playing = false;
        m_Tick = 0.0;
        for (auto& p : m_ChannelPlugins)
            if (p) p->AllNotesOff();
        m_PluginNoteOffs.clear();
        for (int i = 0; i < kMaxVoices; ++i)
            if (m_Voices[i].Active && !m_Voices[i].Held)
            {
                m_Voices[i].Amp.StartRelease();
                m_Voices[i].FilterEnv.StartRelease();
                m_Voices[i].GateFrames = -1.0;
            }
    }

    void SongPlayer::SetTick(double tick)
    {
        std::lock_guard lock(m_Mutex);
        m_Tick = std::clamp(tick, 0.0, std::max(0.0, LoopEnd() - 1.0));
    }

    double SongPlayer::LoopEnd() const
    {
        if (m_Mode == Mode::Song)
        {
            int end = m_Song.SongTicks();
            return end > 0 ? end : m_Song.BarTicks();
        }
        return m_Song.PatternTicks(m_Pattern);
    }

    void SongPlayer::NoteOn(int channel, int key, float velocity)
    {
        std::lock_guard lock(m_Mutex);
        if (channel < 0 || channel >= (int)m_Song.Channels.size()) return;
        if (m_Song.Channels[channel].Type == InstrumentType::Plugin) return PluginNote(channel, key, velocity, true, 0);
        Trigger(channel, key, velocity, -1.0);
    }

    void SongPlayer::NoteOff(int channel, int key)
    {
        std::lock_guard lock(m_Mutex);
        if (channel >= 0 && channel < (int)m_Song.Channels.size() && m_Song.Channels[channel].Type == InstrumentType::Plugin)
            return PluginNote(channel, key, 0.0f, false, 0);
        for (int i = 0; i < kMaxVoices; ++i)
        {
            Voice& v = m_Voices[i];
            if (v.Active && v.Held && v.Channel == channel && v.Key == key)
            {
                v.Held = false;
                v.Amp.StartRelease();
                v.FilterEnv.StartRelease();
            }
        }
    }

    void SongPlayer::PluginNote(int channel, int key, float velocity, bool on, int frame)
    {
        if (channel >= 0 && channel < (int)m_ChannelPlugins.size() && m_ChannelPlugins[channel])
            m_ChannelPlugins[channel]->QueueNote(key, velocity, on, frame);
    }

    void SongPlayer::AllNotesOff()
    {
        std::lock_guard lock(m_Mutex);
        for (auto& p : m_ChannelPlugins)
            if (p) p->AllNotesOff();
        m_PluginNoteOffs.clear();
        for (int i = 0; i < kMaxVoices; ++i) m_Voices[i].Active = false;
    }

    int SongPlayer::ActiveVoices() const
    {
        std::lock_guard lock(m_Mutex);
        int n = 0;
        for (int i = 0; i < kMaxVoices; ++i) n += m_Voices[i].Active;
        return n;
    }

    float SongPlayer::TakeChannelPeak(int channel)
    {
        if (channel < 0 || channel >= kMaxMeters) return 0.0f;
        return m_ChannelPeaks[channel].exchange(0.0f);
    }

    std::array<float, 2> SongPlayer::TakeMasterPeak() { return { m_PeakL.exchange(0.0f), m_PeakR.exchange(0.0f) }; }

    void SongPlayer::Trigger(int channel, int key, float velocity, double lengthFrames)
    {
        const Channel& c = m_Song.Channels[channel];
        if (c.Type == InstrumentType::Plugin) return; // (plug-ins get their notes as events)
        bool held = lengthFrames < 0.0;
        // A gliding synth is monophonic: the playing note slides to the new one.
        if (c.Type == InstrumentType::Synth && c.Synth.Glide > 0.0f)
            for (int i = 0; i < kMaxVoices; ++i)
            {
                Voice& v = m_Voices[i];
                if (v.Active && v.Channel == channel && v.Amp.Stage != EnvState::Release && v.Amp.Stage != EnvState::Off)
                {
                    v.Key = key;
                    v.TargetFreq = KeyToHz((float)key);
                    v.Velocity = velocity;
                    v.GateFrames = lengthFrames;
                    v.Held = held;
                    return;
                }
            }
        // A drum or one-shot sample on the same key cuts its previous hit short (as a choke), others stack.
        Voice* slot = nullptr;
        for (int i = 0; i < kMaxVoices && !slot; ++i)
            if (!m_Voices[i].Active) slot = &m_Voices[i];
        if (!slot)
        {
            slot = &m_Voices[0];
            for (int i = 1; i < kMaxVoices; ++i)
                if (m_Voices[i].Age < slot->Age) slot = &m_Voices[i];
        }
        if (c.Type == InstrumentType::Drum && (c.Drum.Kind == DrumKind::ClosedHat || c.Drum.Kind == DrumKind::OpenHat))
            for (int i = 0; i < kMaxVoices; ++i)
            {
                Voice& v = m_Voices[i];
                // Hats choke each other (a closed hat stops the open one).
                if (v.Active && &v != slot && v.Channel < (int)m_Song.Channels.size() && m_Song.Channels[v.Channel].Type == InstrumentType::Drum &&
                    (m_Song.Channels[v.Channel].Drum.Kind == DrumKind::OpenHat))
                    v.Amp.StartRelease();
            }
        Voice v;
        v.Active = true;
        v.Channel = channel;
        v.Key = key;
        v.Velocity = velocity;
        v.Age = ++m_VoiceAge;
        v.GateFrames = lengthFrames;
        v.Held = held;
        v.Freq = v.TargetFreq = KeyToHz((float)key);
        m_NoiseSeed = m_NoiseSeed * 22695477u + 1u;
        v.Seed = m_NoiseSeed | 1u;
        if (c.Type == InstrumentType::Sampler)
        {
            const SampleData* s = channel < (int)m_Samples.size() ? m_Samples[channel].get() : nullptr;
            if (!s || s->FrameCount() == 0) return;
            v.Step = (double)s->SampleRate / m_SampleRate * std::pow(2.0, (key - c.Sampler.RootKey) / 12.0);
            v.Pos = c.Sampler.Reverse ? (double)s->FrameCount() - 1.0 : 0.0;
            if (c.Sampler.Reverse) v.Step = -v.Step;
        }
        *slot = v;
    }

    float SongPlayer::RenderVoice(Voice& v, float& left, float& right)
    {
        const float sr = (float)m_SampleRate;
        const Channel& c = m_Song.Channels[v.Channel];
        float out = 0.0f;
        // The note's end (sequenced notes).
        if (v.GateFrames >= 0.0)
        {
            v.GateFrames -= 1.0;
            if (v.GateFrames < 0.0)
            {
                v.GateFrames = -1.0;
                bool oneShot = c.Type == InstrumentType::Drum || (c.Type == InstrumentType::Sampler && c.Sampler.OneShot);
                if (!oneShot)
                {
                    v.Amp.StartRelease();
                    v.FilterEnv.StartRelease();
                }
            }
        }
        switch (c.Type)
        {
        case InstrumentType::Plugin:
            v.Active = false;
            break;
        case InstrumentType::Synth:
        {
            const SynthParams& p = c.Synth;
            if (p.Glide > 0.0f) v.Freq += (v.TargetFreq - v.Freq) * (1.0f - std::exp(-1.0f / (p.Glide * sr)));
            else v.Freq = v.TargetFreq;
            double f1 = v.Freq, f2 = v.Freq * std::pow(2.0, p.Octave2 + p.Detune / 12.0);
            double dt1 = f1 / sr, dt2 = f2 / sr;
            float o1 = Oscillator(p.Wave1, v.Phase1, dt1, p.PulseWidth, v.Seed);
            float o2 = Oscillator(p.Wave2, v.Phase2, dt2, p.PulseWidth, v.Seed);
            v.Phase1 = std::fmod(v.Phase1 + dt1, 1.0);
            v.Phase2 = std::fmod(v.Phase2 + dt2, 1.0);
            float x = o1 * (1.0f - p.Mix) + o2 * p.Mix;
            float fenv = v.FilterEnv.Next(p.Filter, sr);
            float cutoff = p.Cutoff * std::pow(2.0f, p.FilterEnv * fenv * 5.0f);
            float low, band, high;
            v.Filter.Process(x, cutoff, 2.0f - 1.9f * std::clamp(p.Resonance, 0.0f, 1.0f), sr, low, band, high);
            out = low * v.Amp.Next(p.Amp, sr) * 0.5f;
            if (v.Amp.Stage == EnvState::Off) v.Active = false;
            break;
        }
        case InstrumentType::Sampler:
        {
            const SampleData* s = v.Channel < (int)m_Samples.size() ? m_Samples[v.Channel].get() : nullptr;
            size_t n = s ? s->FrameCount() : 0;
            if (!s || v.Pos < 0.0 || v.Pos >= (double)n)
            {
                v.Active = false;
                return 0.0f;
            }
            size_t i = (size_t)v.Pos;
            float t = (float)(v.Pos - (double)i);
            size_t j = std::min(i + 1, n - 1);
            float env = v.Amp.Next(c.Sampler.Amp, sr) * v.Velocity;
            float l = (s->Frames[i * 2] * (1 - t) + s->Frames[j * 2] * t) * env;
            float r = (s->Frames[i * 2 + 1] * (1 - t) + s->Frames[j * 2 + 1] * t) * env;
            v.Pos += v.Step;
            if (v.Amp.Stage == EnvState::Off) v.Active = false;
            // Stereo samples keep their image: the channel's pan and level apply in the mix.
            left = l;
            right = r;
            return -1e30f; // (stereo already written)
        }
        case InstrumentType::Drum:
        {
            const DrumParams& d = c.Drum;
            double t = v.Time;
            v.Time += 1.0 / sr;
            float tune = std::pow(2.0f, (d.Tune + (v.Key - 60)) / 12.0f);
            float decay = std::max(0.05f, d.Decay);
            float tone = std::clamp(d.Tone, 0.0f, 1.0f);
            float length = 1.0f;
            float low, band, high;
            switch (d.Kind)
            {
            case DrumKind::Kick:
            {
                double f = (45.0 + 120.0 * std::exp(-t * 28.0)) * tune;
                v.DrumPhase += f / sr;
                out = (float)std::sin(2.0 * kPi * v.DrumPhase) * (float)std::exp(-t * 5.0 / decay);
                out += Noise(v.Seed) * (float)std::exp(-t * 400.0) * 0.35f * tone;
                out = std::tanh(out * (1.2f + tone));
                length = 1.2f * decay;
                break;
            }
            case DrumKind::Snare:
            {
                v.DrumPhase += 185.0 * tune / sr;
                float body = (float)std::sin(2.0 * kPi * v.DrumPhase) * (float)std::exp(-t * 22.0 / decay);
                v.DrumFilter.Process(Noise(v.Seed), 1800.0f * tune, 1.2f, sr, low, band, high);
                float snares = high * (float)std::exp(-t * 13.0 / decay);
                out = body * (0.7f - 0.4f * tone) + snares * (0.5f + 0.6f * tone);
                length = 0.6f * decay;
                break;
            }
            case DrumKind::Clap:
            {
                v.DrumFilter.Process(Noise(v.Seed), 1150.0f * tune, 0.9f, sr, low, band, high);
                float env = 0.0f;
                for (double at : { 0.0, 0.011, 0.022 })
                    if (t >= at && t < at + 0.011) env = std::max(env, (float)std::exp(-(t - at) * 260.0));
                if (t >= 0.033) env = std::max(env, (float)std::exp(-(t - 0.033) * 14.0 / decay));
                out = band * env * (1.6f + tone);
                length = 0.6f * decay;
                break;
            }
            case DrumKind::ClosedHat:
            case DrumKind::OpenHat:
            {
                // An 808-style metallic tone (six square waves) mixed with noise, high passed.
                static const double ratios[6] = { 205.3, 304.4, 369.6, 522.7, 540.0, 800.0 };
                float metal = 0.0f;
                for (int i = 0; i < 6; ++i)
                {
                    v.MetalPhase[i] = std::fmod(v.MetalPhase[i] + ratios[i] * 1.7 * tune / sr, 1.0);
                    metal += v.MetalPhase[i] < 0.5 ? 1.0f : -1.0f;
                }
                float x = metal / 6.0f * (1.0f - tone) + Noise(v.Seed) * tone;
                v.DrumFilter.Process(x, 7500.0f * tune, 1.4f, sr, low, band, high);
                bool open = d.Kind == DrumKind::OpenHat;
                out = high * (float)std::exp(-t * (open ? 7.0 : 55.0) / decay) * 1.2f;
                length = (open ? 0.9f : 0.18f) * decay;
                break;
            }
            case DrumKind::Tom:
            {
                double f = (95.0 + 55.0 * std::exp(-t * 14.0)) * tune;
                v.DrumPhase += f / sr;
                out = (float)std::sin(2.0 * kPi * v.DrumPhase) * (float)std::exp(-t * 8.0 / decay);
                out += Noise(v.Seed) * (float)std::exp(-t * 200.0) * 0.15f * tone;
                length = 0.8f * decay;
                break;
            }
            case DrumKind::Rim:
            default:
            {
                v.DrumPhase += 820.0 * tune / sr;
                v.DrumFilter.Process(Noise(v.Seed), 2500.0f * tune, 1.0f, sr, low, band, high);
                out = ((float)std::sin(2.0 * kPi * v.DrumPhase) * 0.7f + band * 0.6f * (0.5f + tone)) * (float)std::exp(-t * 110.0 / decay);
                length = 0.12f * decay;
                break;
            }
            }
            // A short fade at its natural end, or when choked (released).
            if (v.Amp.Stage == EnvState::Release)
            {
                v.Amp.Next({ 0.0f, 0.0f, 1.0f, 0.03f }, sr);
                out *= v.Amp.Level;
                if (v.Amp.Stage == EnvState::Off) v.Active = false;
            }
            float remain = length - (float)t;
            if (remain < 0.01f) out *= std::max(0.0f, remain / 0.01f);
            if (remain <= 0.0f) v.Active = false;
            out *= 0.9f;
            break;
        }
        }
        return out;
    }

    void SongPlayer::Collect(double from, uint32_t frameBase, uint32_t count, double framesPerTick, std::vector<Event>& events) const
    {
        // Notes starting in the `count` frames from tick `from` (each on the first frame at or after its start, so a
        // note falls in exactly one block), with swing on every second step.
        const double to = from + (count + 1) / framesPerTick;
        const double swing = std::clamp(m_Song.Swing, 0.0f, 1.0f) * m_Song.StepTicks() * 0.5;
        const int step = m_Song.StepTicks();
        auto add = [&](const Note& n, double start) {
            if (n.Start % (2 * step) == step) start += swing;
            double frame = std::ceil((start - from) * framesPerTick - 1e-6);
            if (frame < 0.0 || frame >= count) return;
            events.push_back({ frameBase + (uint32_t)frame, n.Channel, n.Key, n.Velocity, n.Length * framesPerTick });
        };
        if (m_Mode == Mode::Pattern)
        {
            if (m_Pattern < 0 || m_Pattern >= (int)m_Song.Patterns.size()) return;
            int length = m_Song.PatternTicks(m_Pattern);
            for (const Note& n : m_Song.Patterns[m_Pattern].Notes)
                if (n.Start < length) add(n, n.Start);
            return;
        }
        for (const PlaylistClip& clip : m_Song.Playlist)
        {
            if (clip.Pattern < 0 || clip.Pattern >= (int)m_Song.Patterns.size() || m_Song.TrackMuted(clip.Track)) continue;
            int length = m_Song.ClipTicks(clip);
            if (clip.Start >= to || clip.Start + length + swing <= from) continue;
            for (const Note& n : m_Song.Patterns[clip.Pattern].Notes)
                if (n.Start < length) add(n, (double)clip.Start + n.Start);
        }
    }

    void SongPlayer::Render(float* out, uint32_t frames)
    {
        std::lock_guard lock(m_Mutex);
        // In short chunks, so automation moves settings smoothly.
        for (uint32_t done = 0; done < frames;)
        {
            uint32_t n = std::min<uint32_t>(kChunk, frames - done);
            RenderChunk(out + (size_t)done * 2, n);
            done += n;
        }
    }

    void SongPlayer::ApplyAutomation(double tick)
    {
        for (const AutomationClip& a : m_Song.Automation)
        {
            if (m_Song.TrackMuted(a.Track) || tick < a.Start || tick >= a.Start + a.Length) continue;
            float* p = m_Song.Param(a.Target);
            if (!p) continue;
            *p = m_Song.FromNormalized(a.Target, a.ValueAt(tick - a.Start));
            if (a.Target.Kind == ParamKind::PluginParam)
            {
                PluginInstance* instance = nullptr;
                if (a.Target.Slot < 0 && a.Target.Index < (int)m_ChannelPlugins.size()) instance = m_ChannelPlugins[a.Target.Index].get();
                if (a.Target.Slot >= 0 && a.Target.Index < (int)m_EffectPlugins.size() && a.Target.Slot < (int)m_EffectPlugins[a.Target.Index].size())
                    instance = m_EffectPlugins[a.Target.Index][a.Target.Slot].get();
                if (instance) instance->QueueParameter((uint32_t)a.Target.Param, *p, 0);
            }
        }
    }

    void SongPlayer::RenderChunk(float* out, uint32_t frames)
    {
        std::fill(out, out + (size_t)frames * 2, 0.0f);
        const float sr = (float)m_SampleRate;
        const double ticksPerFrame = m_Song.TicksPerSecond() / sr;
        const double framesPerTick = 1.0 / ticksPerFrame;
        if (m_Playing && m_Mode == Mode::Song) ApplyAutomation(m_Tick);

        // This chunk's notes (wrapping at the loop's end).
        m_Events.clear();
        if (m_Playing)
        {
            double from = m_Tick, end = LoopEnd();
            double to = from + frames * ticksPerFrame;
            // The loop's end: the first frame at or after it starts the loop again.
            double wrap = std::ceil((end - from) * framesPerTick - 1e-6);
            if (wrap >= frames) Collect(from, 0, frames, framesPerTick, m_Events);
            else
            {
                uint32_t wrapFrame = (uint32_t)std::max(0.0, wrap);
                Collect(from, 0, wrapFrame, framesPerTick, m_Events);
                Collect(0.0, wrapFrame, frames - wrapFrame, framesPerTick, m_Events);
                to = (frames - wrapFrame) * ticksPerFrame;
            }
            m_Tick = to;
            std::stable_sort(m_Events.begin(), m_Events.end(), [](const Event& a, const Event& b) { return a.Frame < b.Frame; });
        }

        bool anySolo = false;
        for (const Channel& c : m_Song.Channels) anySolo |= c.Solo;
        // Per channel: gain and pan (equal power), muted by Mute or another channel's Solo; and its mixer track.
        const size_t channelCount = m_Song.Channels.size();
        const int tracks = std::min((int)m_Song.Mixer.size(), kMaxMixer);
        float gainL[kMaxMeters], gainR[kMaxMeters], peaks[kMaxMeters] = {};
        int insert[kMaxMeters];
        bool audibleChannel[kMaxMeters];
        for (size_t i = 0; i < channelCount && i < kMaxMeters; ++i)
        {
            const Channel& c = m_Song.Channels[i];
            bool audible = !c.Mute && (!anySolo || c.Solo);
            audibleChannel[i] = audible;
            float g = audible ? c.Volume : 0.0f;
            float angle = (std::clamp(c.Pan, -1.0f, 1.0f) + 1.0f) * 0.25f * (float)kPi;
            gainL[i] = g * std::cos(angle) * 1.41421356f;
            gainR[i] = g * std::sin(angle) * 1.41421356f;
            insert[i] = c.Insert >= 0 && c.Insert < tracks ? c.Insert : 0;
        }
        std::fill(m_Bus.begin(), m_Bus.begin() + (size_t)tracks * kChunk * 2, 0.0f);

        // Voices into their channels' mixer tracks.
        size_t next = 0;
        for (uint32_t f = 0; f < frames; ++f)
        {
            while (next < m_Events.size() && m_Events[next].Frame <= f)
            {
                const Event& e = m_Events[next++];
                // (muted channels start no notes; a channel turned all the way down does, so automation can bring it up)
                if (e.Channel >= 0 && e.Channel < (int)channelCount && e.Channel < kMaxMeters && audibleChannel[e.Channel])
                {
                    if (m_Song.Channels[e.Channel].Type == InstrumentType::Plugin)
                    {
                        PluginNote(e.Channel, e.Key, e.Velocity, true, (int)f);
                        m_PluginNoteOffs.push_back({ e.Channel, e.Key, f + std::max(1.0, e.LengthFrames) });
                    }
                    else Trigger(e.Channel, e.Key, e.Velocity, std::max(1.0, e.LengthFrames));
                }
            }
            for (int i = 0; i < kMaxVoices; ++i)
            {
                Voice& v = m_Voices[i];
                if (!v.Active) continue;
                if (v.Channel >= (int)channelCount || v.Channel >= kMaxMeters)
                {
                    v.Active = false;
                    continue;
                }
                float sl = 0.0f, sr2 = 0.0f;
                float mono = RenderVoice(v, sl, sr2);
                if (mono > -1e29f)
                {
                    mono *= v.Velocity;
                    sl = sr2 = mono;
                }
                sl *= gainL[v.Channel];
                sr2 *= gainR[v.Channel];
                peaks[v.Channel] = std::max(peaks[v.Channel], std::max(std::fabs(sl), std::fabs(sr2)));
                float* bus = m_Bus.data() + ((size_t)insert[v.Channel] * kChunk + f) * 2;
                bus[0] += sl;
                bus[1] += sr2;
            }
        }

        // Plug-in instruments: their notes' ends in this chunk, then their sound into their mixer tracks.
        for (size_t i = 0; i < m_PluginNoteOffs.size();)
        {
            PluginNoteOff& n = m_PluginNoteOffs[i];
            if (n.Frames < frames)
            {
                PluginNote(n.Channel, n.Key, 0.0f, false, (int)n.Frames);
                n = m_PluginNoteOffs.back();
                m_PluginNoteOffs.pop_back();
            }
            else
            {
                n.Frames -= frames;
                ++i;
            }
        }
        PluginTransport transport;
        transport.Tempo = m_Song.Tempo;
        transport.Playing = m_Playing;
        transport.Quarters = m_Tick / kTicksPerBeat;
        transport.Samples = (int64_t)(m_Tick / m_Song.TicksPerSecond() * sr);
        transport.BeatsPerBar = m_Song.BeatsPerBar;
        for (size_t c = 0; c < channelCount && c < m_ChannelPlugins.size() && c < kMaxMeters; ++c)
        {
            PluginInstance* plugin = m_ChannelPlugins[c].get();
            if (!plugin) continue;
            float* buf = m_PluginBuffer.data();
            std::fill(buf, buf + frames * 2, 0.0f);
            plugin->Process(buf, frames, transport);
            float* bus = m_Bus.data() + (size_t)insert[c] * kChunk * 2;
            for (uint32_t f = 0; f < frames; ++f)
            {
                float l = buf[f * 2] * gainL[c], r = buf[f * 2 + 1] * gainR[c]; // (the pan law is unity at the centre)
                peaks[c] = std::max(peaks[c], std::max(std::fabs(l), std::fabs(r)));
                bus[f * 2] += l;
                bus[f * 2 + 1] += r;
            }
        }

        // The Mixer: each insert's effects, level and pan, into its output (a later track or the Master); the Master last.
        auto runTrack = [&](int t) {
            const MixerTrack& m = m_Song.Mixer[t];
            float* bus = m_Bus.data() + (size_t)t * kChunk * 2;
            for (int slot = 0; slot < (int)m.Effects.size() && slot < kMaxEffects; ++slot)
            {
                const Effect& e = m.Effects[slot];
                if (e.Type == EffectType::Plugin)
                {
                    PluginInstance* plugin = t < (int)m_EffectPlugins.size() && slot < (int)m_EffectPlugins[t].size() ? m_EffectPlugins[t][slot].get() : nullptr;
                    if (!e.Enabled || !plugin) continue;
                    float mix = std::clamp(e.Mix, 0.0f, 1.0f);
                    std::copy(bus, bus + frames * 2, m_PluginDry.begin());
                    plugin->Process(bus, frames, transport);
                    if (mix < 1.0f)
                        for (uint32_t i = 0; i < frames * 2; ++i) bus[i] = m_PluginDry[i] + (bus[i] - m_PluginDry[i]) * mix;
                    continue;
                }
                EffectProcessor* fx = t < (int)m_Fx.size() && slot < (int)m_Fx[t].size() ? m_Fx[t][slot].get() : nullptr;
                if (e.Enabled && fx && fx->GetType() == e.Type) fx->Process(e, bus, frames, m_Song.Tempo, m_Song.StepsPerBeat);
            }
            float g = m.Mute ? 0.0f : std::max(0.0f, m.Volume);
            float pan = std::clamp(m.Pan, -1.0f, 1.0f);
            float gl = g * std::min(1.0f, 1.0f - pan), gr = g * std::min(1.0f, 1.0f + pan);
            float peak = 0.0f;
            for (uint32_t f = 0; f < frames; ++f)
            {
                bus[f * 2] *= gl;
                bus[f * 2 + 1] *= gr;
                peak = std::max(peak, std::max(std::fabs(bus[f * 2]), std::fabs(bus[f * 2 + 1])));
            }
            if (peak > m_MixerPeaks[t].load()) m_MixerPeaks[t] = peak;
            return bus;
        };
        for (int t = 1; t < tracks; ++t)
        {
            float* bus = runTrack(t);
            int target = m_Song.Mixer[t].Output;
            if (target <= t || target >= tracks) target = 0;
            float* into = m_Bus.data() + (size_t)target * kChunk * 2;
            for (uint32_t i = 0; i < frames * 2; ++i) into[i] += bus[i];
        }
        float* master = runTrack(0);
        float peakL = 0.0f, peakR = 0.0f;
        for (uint32_t f = 0; f < frames; ++f)
        {
            out[f * 2] = master[f * 2];
            out[f * 2 + 1] = master[f * 2 + 1];
            peakL = std::max(peakL, std::fabs(out[f * 2]));
            peakR = std::max(peakR, std::fabs(out[f * 2 + 1]));
        }
        for (size_t i = 0; i < channelCount && i < kMaxMeters; ++i)
            if (peaks[i] > m_ChannelPeaks[i].load()) m_ChannelPeaks[i] = peaks[i];
        if (peakL > m_PeakL.load()) m_PeakL = peakL;
        if (peakR > m_PeakR.load()) m_PeakR = peakR;
    }

    float SongPlayer::TakeMixerPeak(int track)
    {
        if (track < 0 || track >= kMaxMixer) return 0.0f;
        return m_MixerPeaks[track].exchange(0.0f);
    }

    // ---- Offline rendering ----

    std::vector<float> RenderSong(const Song& song, uint32_t sampleRate, SongPlayer::Mode mode, int pattern, int loops, float tail)
    {
        SongPlayer player(sampleRate);
        if (!song.Plugins().empty())
        {
            auto rack = std::make_shared<PluginRack>((double)sampleRate, (int)SongPlayer::kChunk);
            rack->Sync(song);
            player.SetRack(rack);
        }
        player.SetSong(song);
        player.SetMode(mode);
        player.SetPattern(pattern);
        double ticks = mode == SongPlayer::Mode::Song ? song.SongTicks() : (double)song.PatternTicks(pattern) * std::max(1, loops);
        if (ticks <= 0.0) ticks = song.BarTicks();
        size_t playFrames = (size_t)std::ceil(ticks / song.TicksPerSecond() * sampleRate);
        size_t total = playFrames + (size_t)(std::max(0.0f, tail) * sampleRate);
        std::vector<float> out(total * 2, 0.0f);
        player.Play();
        const size_t block = 256;
        size_t f = 0;
        while (f < total)
        {
            if (f == playFrames) player.Pause(); // the tail: no more notes, the playing ones ring out
            size_t limit = f < playFrames ? playFrames : total; // (blocks stop exactly at the song's end)
            uint32_t n = (uint32_t)std::min(block, limit - f);
            player.Render(out.data() + f * 2, n);
            f += n;
        }
        return out;
    }

    bool WriteWav(const fs::path& path, const std::vector<float>& stereo, uint32_t sampleRate)
    {
        std::ofstream file(path, std::ios::binary);
        if (!file) return false;
        uint32_t dataBytes = (uint32_t)(stereo.size() * 2);
        auto u32 = [&](uint32_t v) { file.write((const char*)&v, 4); };
        auto u16 = [&](uint16_t v) { file.write((const char*)&v, 2); };
        file.write("RIFF", 4);
        u32(36 + dataBytes);
        file.write("WAVEfmt ", 8);
        u32(16);
        u16(1); // PCM
        u16(2);
        u32(sampleRate);
        u32(sampleRate * 4);
        u16(4);
        u16(16);
        file.write("data", 4);
        u32(dataBytes);
        std::vector<int16_t> pcm(stereo.size());
        for (size_t i = 0; i < stereo.size(); ++i) pcm[i] = (int16_t)std::lround(std::clamp(stereo[i], -1.0f, 1.0f) * 32767.0f);
        file.write((const char*)pcm.data(), (std::streamsize)(pcm.size() * 2));
        return (bool)file;
    }

    // ---- Output device ----

    namespace
    {
        struct Output
        {
            ma_device Device;
            std::shared_ptr<SongPlayer> Player;
        };
        std::unique_ptr<Output> s_Output;
        std::mutex s_OutputMutex;

        void OnPlayback(ma_device* device, void* out, const void*, ma_uint32 frames)
        {
            auto* o = (Output*)device->pUserData;
            float* samples = (float*)out;
            o->Player->Render(samples, frames);
            for (ma_uint32 i = 0; i < frames * 2; ++i) samples[i] = std::clamp(samples[i], -1.0f, 1.0f);
        }
    }

    bool MusicOutput::Start(std::shared_ptr<SongPlayer> player)
    {
        Stop();
        if (!player) return false;
        std::lock_guard lock(s_OutputMutex);
        auto o = std::make_unique<Output>();
        o->Player = std::move(player);
        ma_device_config config = ma_device_config_init(ma_device_type_playback);
        config.playback.format = ma_format_f32;
        config.playback.channels = 2;
        config.sampleRate = o->Player->GetSampleRate();
        config.periodSizeInMilliseconds = 10; // low latency for playing notes live
        config.dataCallback = OnPlayback;
        config.pUserData = o.get();
        if (ma_device_init(nullptr, &config, &o->Device) != MA_SUCCESS)
        {
            Log::Warn("Music: no audio output device");
            return false;
        }
        if (ma_device_start(&o->Device) != MA_SUCCESS)
        {
            ma_device_uninit(&o->Device);
            Log::Warn("Music: the audio output would not start");
            return false;
        }
        s_Output = std::move(o);
        return true;
    }

    void MusicOutput::Stop()
    {
        std::lock_guard lock(s_OutputMutex);
        if (!s_Output) return;
        ma_device_uninit(&s_Output->Device); // stops and waits for the callback
        s_Output.reset();
    }

    bool MusicOutput::IsRunning()
    {
        std::lock_guard lock(s_OutputMutex);
        return s_Output != nullptr;
    }
}
