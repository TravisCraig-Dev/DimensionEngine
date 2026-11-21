#include "Dimension/Music/Song.h"
#include "Dimension/Core/Log.h"
#include <cmath>
#include <fstream>

namespace Dimension::Music
{
    using json = nlohmann::json;

    namespace
    {
        bool TargetsChannel(const ParamRef& r)
        {
            return r.Kind == ParamKind::ChannelVolume || r.Kind == ParamKind::ChannelPan || r.Kind == ParamKind::SynthCutoff ||
                   r.Kind == ParamKind::SynthResonance || (r.Kind == ParamKind::PluginParam && r.Slot < 0);
        }
        bool TargetsMixer(const ParamRef& r) { return !TargetsChannel(r); }
        bool TargetsEffect(const ParamRef& r)
        {
            return r.Kind == ParamKind::EffectParam || r.Kind == ParamKind::EffectMix || (r.Kind == ParamKind::PluginParam && r.Slot >= 0);
        }

        nlohmann::json PluginToJson(const PluginRef& p)
        {
            nlohmann::json values = nlohmann::json::object(), names = nlohmann::json::object();
            for (auto& [id, v] : p.Values) values[std::to_string(id)] = v;
            for (auto& [id, n] : p.ParamNames) names[std::to_string(id)] = n;
            return { { "name", p.Name }, { "module", p.Module }, { "classId", p.ClassId }, { "id", p.Id }, { "state", p.State },
                     { "controllerState", p.ControllerState }, { "values", values }, { "paramNames", names } };
        }

        PluginRef PluginFromJson(const nlohmann::json& j)
        {
            PluginRef p;
            if (!j.is_object()) return p;
            p.Name = j.value("name", "");
            p.Module = j.value("module", "");
            p.ClassId = j.value("classId", "");
            p.Id = j.value("id", (uint64_t)0);
            p.State = j.value("state", "");
            p.ControllerState = j.value("controllerState", "");
            // (bound to locals: iterating items() of a temporary is undefined)
            const nlohmann::json values = j.value("values", nlohmann::json::object()), names = j.value("paramNames", nlohmann::json::object());
            for (auto& [k, v] : values.items())
                if (v.is_number()) p.Values[std::atoi(k.c_str())] = std::clamp(v.get<float>(), 0.0f, 1.0f);
            for (auto& [k, v] : names.items())
                if (v.is_string()) p.ParamNames[std::atoi(k.c_str())] = v.get<std::string>();
            return p;
        }
    }


    const char* InstrumentTypeName(InstrumentType t)
    {
        switch (t)
        {
        case InstrumentType::Drum: return "Drum";
        case InstrumentType::Synth: return "Synth";
        case InstrumentType::Sampler: return "Sampler";
        case InstrumentType::Plugin: return "Plugin";
        }
        return "Drum";
    }

    const char* DrumKindName(DrumKind k)
    {
        static const char* names[] = { "Kick", "Snare", "Clap", "Closed Hat", "Open Hat", "Tom", "Rim" };
        return names[std::clamp((int)k, 0, (int)DrumKind::Count - 1)];
    }

    const char* WaveformName(Waveform w)
    {
        static const char* names[] = { "Saw", "Square", "Triangle", "Sine", "Noise" };
        return names[std::clamp((int)w, 0, (int)Waveform::Count - 1)];
    }

    uint32_t ChannelColor(const Channel& c)
    {
        if (c.Color) return c.Color;
        // A pleasant hue from the name (stable between runs).
        uint32_t h = 2166136261u;
        for (char ch : c.Name) h = (h ^ (uint8_t)ch) * 16777619u;
        float hue = (h % 360) / 360.0f, s = 0.55f, v = 0.85f;
        float r, g, b;
        int i = (int)(hue * 6.0f);
        float f = hue * 6.0f - i, p = v * (1 - s), q = v * (1 - f * s), t = v * (1 - (1 - f) * s);
        switch (i % 6)
        {
        case 0: r = v, g = t, b = p; break;
        case 1: r = q, g = v, b = p; break;
        case 2: r = p, g = v, b = t; break;
        case 3: r = p, g = q, b = v; break;
        case 4: r = t, g = p, b = v; break;
        default: r = v, g = p, b = q; break;
        }
        return (uint32_t)(r * 255) | ((uint32_t)(g * 255) << 8) | ((uint32_t)(b * 255) << 16) | 0xFF000000u;
    }

    int Song::PatternTicks(int pattern) const
    {
        if (pattern < 0 || pattern >= (int)Patterns.size()) return BarTicks();
        return std::max(1, Patterns[pattern].Bars) * BarTicks();
    }

    int Song::SongTicks() const
    {
        int end = 0;
        for (const auto& c : Playlist) end = std::max(end, c.Start + ClipTicks(c));
        for (const auto& a : Automation) end = std::max(end, a.Start + a.Length);
        return end;
    }

    MixerTrack MasterTrack()
    {
        MixerTrack m;
        m.Name = "Master";
        m.Volume = 0.8f;
        return m;
    }

    std::string Song::MixerTrackName(int track) const
    {
        if (track == 0) return "Master";
        if (track > 0 && track < (int)Mixer.size() && !Mixer[track].Name.empty()) return Mixer[track].Name;
        return "Insert " + std::to_string(track);
    }

    void Song::RemoveMixerTrack(int track)
    {
        if (track <= 0 || track >= (int)Mixer.size()) return;
        Mixer.erase(Mixer.begin() + track);
        for (Channel& c : Channels)
            if (c.Insert == track) c.Insert = 0;
            else if (c.Insert > track) --c.Insert;
        for (MixerTrack& m : Mixer)
            if (m.Output == track) m.Output = 0;
            else if (m.Output > track) --m.Output;
        std::erase_if(Automation, [&](const AutomationClip& a) { return TargetsMixer(a.Target) && a.Target.Index == track; });
        for (AutomationClip& a : Automation)
            if (TargetsMixer(a.Target) && a.Target.Index > track) --a.Target.Index;
    }

    void Song::RemoveEffect(int track, int slot)
    {
        if (track < 0 || track >= (int)Mixer.size() || slot < 0 || slot >= (int)Mixer[track].Effects.size()) return;
        Mixer[track].Effects.erase(Mixer[track].Effects.begin() + slot);
        std::erase_if(Automation, [&](const AutomationClip& a) { return TargetsEffect(a.Target) && a.Target.Index == track && a.Target.Slot == slot; });
        for (AutomationClip& a : Automation)
            if (TargetsEffect(a.Target) && a.Target.Index == track && a.Target.Slot > slot) --a.Target.Slot;
    }

    PluginRef* Song::PluginOf(const ParamRef& r)
    {
        if (r.Kind != ParamKind::PluginParam) return nullptr;
        if (r.Slot < 0)
            return r.Index >= 0 && r.Index < (int)Channels.size() && Channels[r.Index].Type == InstrumentType::Plugin ? &Channels[r.Index].Plugin : nullptr;
        if (r.Index < 0 || r.Index >= (int)Mixer.size() || r.Slot >= (int)Mixer[r.Index].Effects.size()) return nullptr;
        Effect& e = Mixer[r.Index].Effects[r.Slot];
        return e.Type == EffectType::Plugin ? &e.Plugin : nullptr;
    }

    std::vector<PluginRef*> Song::Plugins()
    {
        std::vector<PluginRef*> out;
        for (Channel& c : Channels)
            if (c.Type == InstrumentType::Plugin && c.Plugin.Valid()) out.push_back(&c.Plugin);
        for (MixerTrack& m : Mixer)
            for (Effect& e : m.Effects)
                if (e.Type == EffectType::Plugin && e.Plugin.Valid()) out.push_back(&e.Plugin);
        return out;
    }

    std::vector<const PluginRef*> Song::Plugins() const
    {
        std::vector<const PluginRef*> out;
        for (PluginRef* p : const_cast<Song*>(this)->Plugins()) out.push_back(p);
        return out;
    }

    float* Song::Param(const ParamRef& r)
    {
        if (r.Kind == ParamKind::PluginParam)
        {
            PluginRef* p = PluginOf(r);
            if (!p) return nullptr;
            auto it = p->Values.find(r.Param);
            if (it == p->Values.end()) it = p->Values.emplace(r.Param, 0.5f).first; // (made by the automation clip that sets it)
            return &it->second;
        }
        if (TargetsChannel(r))
        {
            if (r.Index < 0 || r.Index >= (int)Channels.size()) return nullptr;
            Channel& c = Channels[r.Index];
            switch (r.Kind)
            {
            case ParamKind::ChannelVolume: return &c.Volume;
            case ParamKind::ChannelPan: return &c.Pan;
            case ParamKind::SynthCutoff: return &c.Synth.Cutoff;
            case ParamKind::SynthResonance: return &c.Synth.Resonance;
            default: return nullptr;
            }
        }
        if (r.Index < 0 || r.Index >= (int)Mixer.size()) return nullptr;
        MixerTrack& m = Mixer[r.Index];
        if (r.Kind == ParamKind::MixerVolume) return &m.Volume;
        if (r.Kind == ParamKind::MixerPan) return &m.Pan;
        if (r.Slot < 0 || r.Slot >= (int)m.Effects.size()) return nullptr;
        Effect& e = m.Effects[r.Slot];
        if (r.Kind == ParamKind::EffectMix) return &e.Mix;
        if (r.Kind != ParamKind::EffectParam || r.Param < 0 || r.Param >= GetEffectInfo(e.Type).ParamCount) return nullptr;
        return &e.P[r.Param];
    }

    bool Song::ParamRange(const ParamRef& r, float& min, float& max, bool& log) const
    {
        if (!const_cast<Song*>(this)->Param(r)) return false;
        log = false;
        switch (r.Kind)
        {
        case ParamKind::ChannelVolume:
        case ParamKind::MixerVolume: min = 0.0f, max = 1.25f; break;
        case ParamKind::ChannelPan:
        case ParamKind::MixerPan: min = -1.0f, max = 1.0f; break;
        case ParamKind::SynthCutoff: min = 20.0f, max = 20000.0f, log = true; break;
        case ParamKind::SynthResonance:
        case ParamKind::EffectMix:
        case ParamKind::PluginParam: min = 0.0f, max = 1.0f; break;
        case ParamKind::EffectParam:
        {
            const EffectParamInfo& info = GetEffectInfo(Mixer[r.Index].Effects[r.Slot].Type).Params[r.Param];
            min = info.Min, max = info.Max, log = info.Log;
            break;
        }
        }
        return true;
    }

    std::string Song::ParamName(const ParamRef& r) const
    {
        if (!const_cast<Song*>(this)->Param(r)) return "(missing)";
        switch (r.Kind)
        {
        case ParamKind::ChannelVolume: return Channels[r.Index].Name + " Volume";
        case ParamKind::ChannelPan: return Channels[r.Index].Name + " Pan";
        case ParamKind::SynthCutoff: return Channels[r.Index].Name + " Cutoff";
        case ParamKind::SynthResonance: return Channels[r.Index].Name + " Resonance";
        case ParamKind::MixerVolume: return MixerTrackName(r.Index) + " Volume";
        case ParamKind::MixerPan: return MixerTrackName(r.Index) + " Pan";
        case ParamKind::EffectMix: return MixerTrackName(r.Index) + " " + EffectTypeName(Mixer[r.Index].Effects[r.Slot].Type) + " Mix";
        case ParamKind::EffectParam:
        {
            EffectType t = Mixer[r.Index].Effects[r.Slot].Type;
            return MixerTrackName(r.Index) + " " + EffectTypeName(t) + " " + GetEffectInfo(t).Params[r.Param].Name;
        }
        case ParamKind::PluginParam:
        {
            const PluginRef* p = const_cast<Song*>(this)->PluginOf(r);
            std::string owner = r.Slot < 0 ? Channels[r.Index].Name : MixerTrackName(r.Index) + " " + p->Name;
            auto it = p->ParamNames.find(r.Param);
            return owner + " " + (it != p->ParamNames.end() ? it->second : "Parameter " + std::to_string(r.Param));
        }
        }
        return "?";
    }

    float Song::ToNormalized(const ParamRef& r, float value) const
    {
        float min, max;
        bool log;
        if (!ParamRange(r, min, max, log) || max <= min) return 0.0f;
        value = std::clamp(value, min, max);
        if (log && min > 0.0f) return std::log(value / min) / std::log(max / min);
        return (value - min) / (max - min);
    }

    float Song::FromNormalized(const ParamRef& r, float n) const
    {
        float min, max;
        bool log;
        if (!ParamRange(r, min, max, log)) return 0.0f;
        n = std::clamp(n, 0.0f, 1.0f);
        if (log && min > 0.0f) return min * std::pow(max / min, n);
        return min + (max - min) * n;
    }

    std::vector<ParamRef> Song::AutomatableParams() const
    {
        std::vector<ParamRef> out;
        for (int c = 0; c < (int)Channels.size(); ++c)
        {
            out.push_back({ ParamKind::ChannelVolume, c });
            out.push_back({ ParamKind::ChannelPan, c });
            if (Channels[c].Type == InstrumentType::Synth)
            {
                out.push_back({ ParamKind::SynthCutoff, c });
                out.push_back({ ParamKind::SynthResonance, c });
            }
        }
        for (int m = 0; m < (int)Mixer.size(); ++m)
        {
            out.push_back({ ParamKind::MixerVolume, m });
            out.push_back({ ParamKind::MixerPan, m });
            for (int e = 0; e < (int)Mixer[m].Effects.size(); ++e)
            {
                for (int p = 0; p < GetEffectInfo(Mixer[m].Effects[e].Type).ParamCount; ++p) out.push_back({ ParamKind::EffectParam, m, e, p });
                out.push_back({ ParamKind::EffectMix, m, e });
            }
        }
        return out;
    }

    void Song::InsertChannel(int at, Channel channel)
    {
        at = std::clamp(at, 0, (int)Channels.size());
        Channels.insert(Channels.begin() + at, std::move(channel));
        for (auto& p : Patterns)
            for (auto& n : p.Notes)
                if (n.Channel >= at) ++n.Channel;
        for (auto& a : Automation)
            if (TargetsChannel(a.Target) && a.Target.Index >= at) ++a.Target.Index;
    }

    void Song::SwapChannels(int a, int b)
    {
        if (a < 0 || b < 0 || a >= (int)Channels.size() || b >= (int)Channels.size() || a == b) return;
        std::swap(Channels[a], Channels[b]);
        auto swapIndex = [&](int& i) {
            if (i == a) i = b;
            else if (i == b) i = a;
        };
        for (auto& p : Patterns)
            for (auto& n : p.Notes) swapIndex(n.Channel);
        for (auto& c : Automation)
            if (TargetsChannel(c.Target)) swapIndex(c.Target.Index);
    }

    std::string Song::TrackName(int track) const
    {
        if (track >= 0 && track < (int)Tracks.size() && !Tracks[track].Name.empty()) return Tracks[track].Name;
        return "Track " + std::to_string(track + 1);
    }

    PlaylistTrack& Song::Track(int track)
    {
        track = std::max(0, track);
        if (track >= (int)Tracks.size()) Tracks.resize(track + 1);
        return Tracks[track];
    }

    int Song::FitPatternBars(int pattern)
    {
        if (pattern < 0 || pattern >= (int)Patterns.size()) return 1;
        Pattern& p = Patterns[pattern];
        int end = 0;
        for (const Note& n : p.Notes) end = std::max(end, n.Start + n.Length);
        p.Bars = std::clamp(std::max(p.Bars, (end + BarTicks() - 1) / BarTicks()), 1, 16);
        return p.Bars;
    }

    bool Song::HasStep(int pattern, int channel, int step) const
    {
        if (pattern < 0 || pattern >= (int)Patterns.size()) return false;
        int t = step * StepTicks();
        for (const Note& n : Patterns[pattern].Notes)
            if (n.Channel == channel && n.Start == t) return true;
        return false;
    }

    void Song::ToggleStep(int pattern, int channel, int step)
    {
        if (pattern < 0 || pattern >= (int)Patterns.size() || channel < 0 || channel >= (int)Channels.size()) return;
        auto& notes = Patterns[pattern].Notes;
        int t = step * StepTicks();
        size_t before = notes.size();
        std::erase_if(notes, [&](const Note& n) { return n.Channel == channel && n.Start == t; });
        if (notes.size() != before) return;
        notes.push_back({ channel, t, StepTicks(), Channels[channel].Key, 0.8f });
    }

    void Song::RemoveChannel(int channel)
    {
        if (channel < 0 || channel >= (int)Channels.size()) return;
        Channels.erase(Channels.begin() + channel);
        for (auto& p : Patterns)
        {
            std::erase_if(p.Notes, [&](const Note& n) { return n.Channel == channel; });
            for (auto& n : p.Notes)
                if (n.Channel > channel) --n.Channel;
        }
        std::erase_if(Automation, [&](const AutomationClip& a) { return TargetsChannel(a.Target) && a.Target.Index == channel; });
        for (auto& a : Automation)
            if (TargetsChannel(a.Target) && a.Target.Index > channel) --a.Target.Index;
    }

    namespace
    {
        json EnvToJson(const Envelope& e) { return json::array({ e.Attack, e.Decay, e.Sustain, e.Release }); }
        Envelope EnvFromJson(const json& j, Envelope def)
        {
            if (j.is_array() && j.size() == 4) return { j[0].get<float>(), j[1].get<float>(), j[2].get<float>(), j[3].get<float>() };
            return def;
        }
    }

    json Song::ToJson() const
    {
        json j;
        j["format"] = "dsong";
        j["version"] = 1;
        j["name"] = Name;
        j["tempo"] = Tempo;
        j["beatsPerBar"] = BeatsPerBar;
        j["stepsPerBeat"] = StepsPerBeat;
        j["swing"] = Swing;
        json channels = json::array();
        for (const Channel& c : Channels)
        {
            json cj{ { "name", c.Name }, { "type", InstrumentTypeName(c.Type) }, { "volume", c.Volume }, { "pan", c.Pan },
                     { "mute", c.Mute }, { "solo", c.Solo }, { "key", c.Key }, { "color", c.Color }, { "insert", c.Insert } };
            if (c.Type == InstrumentType::Drum)
                cj["drum"] = { { "kind", (int)c.Drum.Kind }, { "tune", c.Drum.Tune }, { "decay", c.Drum.Decay }, { "tone", c.Drum.Tone } };
            if (c.Type == InstrumentType::Synth)
            {
                const SynthParams& s = c.Synth;
                cj["synth"] = { { "wave1", (int)s.Wave1 }, { "wave2", (int)s.Wave2 }, { "mix", s.Mix }, { "octave2", s.Octave2 }, { "detune", s.Detune },
                                { "pulseWidth", s.PulseWidth }, { "cutoff", s.Cutoff }, { "resonance", s.Resonance }, { "filterEnv", s.FilterEnv },
                                { "glide", s.Glide }, { "amp", EnvToJson(s.Amp) }, { "filter", EnvToJson(s.Filter) } };
            }
            if (c.Type == InstrumentType::Plugin) cj["plugin"] = PluginToJson(c.Plugin);
            if (c.Type == InstrumentType::Sampler)
                cj["sampler"] = { { "sample", c.Sampler.Sample }, { "samplePath", c.Sampler.SamplePath }, { "rootKey", c.Sampler.RootKey },
                                  { "oneShot", c.Sampler.OneShot }, { "reverse", c.Sampler.Reverse }, { "amp", EnvToJson(c.Sampler.Amp) } };
            channels.push_back(std::move(cj));
        }
        j["channels"] = std::move(channels);
        json patterns = json::array();
        for (const Pattern& p : Patterns)
        {
            // Notes as flat rows [channel, start, length, key, velocity] (compact files).
            json notes = json::array();
            for (const Note& n : p.Notes) notes.push_back(json::array({ n.Channel, n.Start, n.Length, n.Key, n.Velocity }));
            patterns.push_back({ { "name", p.Name }, { "bars", p.Bars }, { "notes", std::move(notes) } });
        }
        j["patterns"] = std::move(patterns);
        json playlist = json::array();
        for (const PlaylistClip& c : Playlist) playlist.push_back(json::array({ c.Pattern, c.Track, c.Start, c.Length }));
        j["playlist"] = std::move(playlist);
        json tracks = json::array();
        for (const PlaylistTrack& t : Tracks) tracks.push_back({ { "name", t.Name }, { "mute", t.Mute } });
        j["tracks"] = std::move(tracks);
        json mixer = json::array();
        for (const MixerTrack& m : Mixer)
        {
            json effects = json::array();
            for (const Effect& e : m.Effects)
            {
                json params = json::array();
                for (int i = 0; i < GetEffectInfo(e.Type).ParamCount; ++i) params.push_back(e.P[i]);
                json ej = { { "type", EffectTypeName(e.Type) }, { "enabled", e.Enabled }, { "mix", e.Mix }, { "params", std::move(params) } };
                if (e.Type == EffectType::Plugin) ej["plugin"] = PluginToJson(e.Plugin);
                effects.push_back(std::move(ej));
            }
            mixer.push_back({ { "name", m.Name }, { "volume", m.Volume }, { "pan", m.Pan }, { "mute", m.Mute }, { "output", m.Output },
                              { "effects", std::move(effects) } });
        }
        j["mixer"] = std::move(mixer);
        json automation = json::array();
        for (const AutomationClip& a : Automation)
        {
            json points = json::array();
            for (const AutomationPoint& p : a.Points) points.push_back(json::array({ p.Tick, p.Value }));
            automation.push_back({ { "target", json::array({ (int)a.Target.Kind, a.Target.Index, a.Target.Slot, a.Target.Param }) },
                                   { "track", a.Track }, { "start", a.Start }, { "length", a.Length }, { "points", std::move(points) } });
        }
        j["automation"] = std::move(automation);
        return j;
    }

    bool Song::FromJson(const json& j, Song& out)
    {
        out = Song();
        if (!j.is_object() || j.value("format", "") != "dsong") return false;
        try
        {
            out.Name = j.value("name", "Song");
            out.Tempo = std::clamp(j.value("tempo", 130.0f), 10.0f, 999.0f);
            out.BeatsPerBar = std::clamp(j.value("beatsPerBar", 4), 1, 16);
            out.StepsPerBeat = std::clamp(j.value("stepsPerBeat", 4), 1, 8);
            out.Swing = j.value("swing", 0.0f);
            for (const json& cj : j.value("channels", json::array()))
            {
                Channel c;
                c.Name = cj.value("name", "Channel");
                std::string type = cj.value("type", "Drum");
                c.Type = type == "Synth" ? InstrumentType::Synth : type == "Sampler" ? InstrumentType::Sampler : type == "Plugin" ? InstrumentType::Plugin : InstrumentType::Drum;
                if (cj.contains("plugin")) c.Plugin = PluginFromJson(cj["plugin"]);
                c.Volume = cj.value("volume", 0.78f);
                c.Pan = cj.value("pan", 0.0f);
                c.Mute = cj.value("mute", false);
                c.Solo = cj.value("solo", false);
                c.Key = std::clamp(cj.value("key", 60), 0, 127);
                c.Color = cj.value("color", 0u);
                c.Insert = std::max(0, cj.value("insert", 0));
                if (cj.contains("drum"))
                {
                    const json& d = cj["drum"];
                    c.Drum.Kind = (DrumKind)std::clamp(d.value("kind", 0), 0, (int)DrumKind::Count - 1);
                    c.Drum.Tune = d.value("tune", 0.0f);
                    c.Drum.Decay = d.value("decay", 1.0f);
                    c.Drum.Tone = d.value("tone", 0.5f);
                }
                if (cj.contains("synth"))
                {
                    const json& s = cj["synth"];
                    SynthParams& p = c.Synth;
                    p.Wave1 = (Waveform)std::clamp(s.value("wave1", 0), 0, (int)Waveform::Count - 1);
                    p.Wave2 = (Waveform)std::clamp(s.value("wave2", 1), 0, (int)Waveform::Count - 1);
                    p.Mix = s.value("mix", p.Mix);
                    p.Octave2 = s.value("octave2", 0);
                    p.Detune = s.value("detune", p.Detune);
                    p.PulseWidth = s.value("pulseWidth", 0.5f);
                    p.Cutoff = s.value("cutoff", p.Cutoff);
                    p.Resonance = s.value("resonance", p.Resonance);
                    p.FilterEnv = s.value("filterEnv", p.FilterEnv);
                    p.Glide = s.value("glide", 0.0f);
                    p.Amp = EnvFromJson(s.value("amp", json()), p.Amp);
                    p.Filter = EnvFromJson(s.value("filter", json()), p.Filter);
                }
                if (cj.contains("sampler"))
                {
                    const json& s = cj["sampler"];
                    c.Sampler.Sample = s.value("sample", (uint64_t)0);
                    c.Sampler.SamplePath = s.value("samplePath", "");
                    c.Sampler.RootKey = s.value("rootKey", 60);
                    c.Sampler.OneShot = s.value("oneShot", true);
                    c.Sampler.Reverse = s.value("reverse", false);
                    c.Sampler.Amp = EnvFromJson(s.value("amp", json()), c.Sampler.Amp);
                }
                out.Channels.push_back(std::move(c));
            }
            int channelCount = (int)out.Channels.size();
            for (const json& pj : j.value("patterns", json::array()))
            {
                Pattern p;
                p.Name = pj.value("name", "Pattern");
                p.Bars = std::clamp(pj.value("bars", 1), 1, 64);
                for (const json& n : pj.value("notes", json::array()))
                {
                    if (!n.is_array() || n.size() < 5) continue;
                    Note note{ n[0].get<int>(), n[1].get<int>(), n[2].get<int>(), n[3].get<int>(), n[4].get<float>() };
                    if (note.Channel < 0 || note.Channel >= channelCount || note.Start < 0) continue;
                    note.Length = std::max(1, note.Length);
                    note.Key = std::clamp(note.Key, 0, 127);
                    p.Notes.push_back(note);
                }
                out.Patterns.push_back(std::move(p));
            }
            for (const json& c : j.value("playlist", json::array()))
                if (c.is_array() && c.size() >= 4)
                {
                    PlaylistClip clip{ c[0].get<int>(), c[1].get<int>(), c[2].get<int>(), c[3].get<int>() };
                    if (clip.Pattern >= 0 && clip.Pattern < (int)out.Patterns.size() && clip.Start >= 0 && clip.Track >= 0) out.Playlist.push_back(clip);
                }
            for (const json& t : j.value("tracks", json::array())) out.Tracks.push_back({ t.value("name", ""), t.value("mute", false) });
            // The Mixer (older songs: a Master with their master volume).
            out.Mixer.clear();
            for (const json& mj : j.value("mixer", json::array()))
            {
                MixerTrack m;
                m.Name = mj.value("name", "");
                m.Volume = mj.value("volume", 1.0f);
                m.Pan = mj.value("pan", 0.0f);
                m.Mute = mj.value("mute", false);
                m.Output = mj.value("output", 0);
                for (const json& ej : mj.value("effects", json::array()))
                {
                    if ((int)m.Effects.size() >= kMaxEffects) break;
                    std::string type = ej.value("type", "EQ");
                    EffectType t = EffectType::EQ;
                    for (int i = 0; i < (int)EffectType::Count; ++i)
                        if (type == EffectTypeName((EffectType)i)) t = (EffectType)i;
                    Effect e = Effect::Make(t);
                    e.Enabled = ej.value("enabled", true);
                    e.Mix = ej.value("mix", e.Mix);
                    const json params = ej.value("params", json::array());
                    for (int i = 0; i < GetEffectInfo(t).ParamCount && i < (int)params.size(); ++i) e.P[i] = params[i].get<float>();
                    if (ej.contains("plugin")) e.Plugin = PluginFromJson(ej["plugin"]);
                    m.Effects.push_back(e);
                }
                out.Mixer.push_back(std::move(m));
            }
            if (out.Mixer.empty())
            {
                out.Mixer.push_back(MasterTrack());
                out.Mixer[0].Volume = j.value("masterVolume", 0.8f);
            }
            out.Mixer[0].Name = "Master";
            out.Mixer[0].Output = 0;
            for (int i = 1; i < (int)out.Mixer.size(); ++i)
                if (out.Mixer[i].Output != 0 && (out.Mixer[i].Output <= i || out.Mixer[i].Output >= (int)out.Mixer.size())) out.Mixer[i].Output = 0;
            for (Channel& c : out.Channels)
                if (c.Insert >= (int)out.Mixer.size()) c.Insert = 0;
            for (const json& aj : j.value("automation", json::array()))
            {
                AutomationClip a;
                const json t = aj.value("target", json::array());
                if (t.size() >= 4) a.Target = { (ParamKind)t[0].get<int>(), t[1].get<int>(), t[2].get<int>(), t[3].get<int>() };
                a.Track = std::max(0, aj.value("track", 0));
                a.Start = std::max(0, aj.value("start", 0));
                a.Length = std::max(1, aj.value("length", 384));
                for (const json& pj : aj.value("points", json::array()))
                    if (pj.is_array() && pj.size() >= 2) a.Points.push_back({ pj[0].get<int>(), std::clamp(pj[1].get<float>(), 0.0f, 1.0f) });
                a.SortPoints();
                if (out.Param(a.Target)) out.Automation.push_back(std::move(a));
            }
        }
        catch (const json::exception&)
        {
            return false;
        }
        if (out.Patterns.empty()) out.Patterns.push_back({});
        return true;
    }

    bool Song::Save(const fs::path& path) const
    {
        std::ofstream file(path, std::ios::binary);
        if (!file) return false;
        file << ToJson().dump(1);
        return (bool)file;
    }

    bool Song::Load(const fs::path& path, Song& out)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file) return false;
        json j = json::parse(file, nullptr, false);
        if (j.is_discarded() || !FromJson(j, out))
        {
            Log::Error("'{}' is not a valid song file", path.filename().string());
            return false;
        }
        return true;
    }

    Song Song::Default()
    {
        Song s;
        auto drum = [&](const char* name, DrumKind kind, float volume) {
            Channel c;
            c.Name = name;
            c.Type = InstrumentType::Drum;
            c.Drum.Kind = kind;
            c.Volume = volume;
            s.Channels.push_back(c);
        };
        drum("Kick", DrumKind::Kick, 0.85f);
        drum("Clap", DrumKind::Clap, 0.6f);
        drum("Hat", DrumKind::ClosedHat, 0.45f);
        drum("Snare", DrumKind::Snare, 0.6f);
        Channel bass;
        bass.Name = "Synth";
        bass.Type = InstrumentType::Synth;
        bass.Key = 48;
        bass.Volume = 0.6f;
        s.Channels.push_back(bass);
        Pattern p;
        p.Name = "Pattern 1";
        s.Patterns.push_back(p);
        // Four on the floor, claps on 2 and 4, off-beat hats.
        for (int step : { 0, 4, 8, 12 }) s.ToggleStep(0, 0, step);
        for (int step : { 4, 12 }) s.ToggleStep(0, 1, step);
        for (int step : { 2, 6, 10, 14 }) s.ToggleStep(0, 2, step);
        return s;
    }
}
