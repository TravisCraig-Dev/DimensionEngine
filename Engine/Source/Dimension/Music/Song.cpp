#include "Dimension/Music/Song.h"
#include "Dimension/Core/Log.h"
#include <fstream>

namespace Dimension::Music
{
    using json = nlohmann::json;

    const char* InstrumentTypeName(InstrumentType t)
    {
        switch (t)
        {
        case InstrumentType::Drum: return "Drum";
        case InstrumentType::Synth: return "Synth";
        case InstrumentType::Sampler: return "Sampler";
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
        return end;
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
        j["masterVolume"] = MasterVolume;
        j["swing"] = Swing;
        json channels = json::array();
        for (const Channel& c : Channels)
        {
            json cj{ { "name", c.Name }, { "type", InstrumentTypeName(c.Type) }, { "volume", c.Volume }, { "pan", c.Pan },
                     { "mute", c.Mute }, { "solo", c.Solo }, { "key", c.Key }, { "color", c.Color } };
            if (c.Type == InstrumentType::Drum)
                cj["drum"] = { { "kind", (int)c.Drum.Kind }, { "tune", c.Drum.Tune }, { "decay", c.Drum.Decay }, { "tone", c.Drum.Tone } };
            if (c.Type == InstrumentType::Synth)
            {
                const SynthParams& s = c.Synth;
                cj["synth"] = { { "wave1", (int)s.Wave1 }, { "wave2", (int)s.Wave2 }, { "mix", s.Mix }, { "octave2", s.Octave2 }, { "detune", s.Detune },
                                { "pulseWidth", s.PulseWidth }, { "cutoff", s.Cutoff }, { "resonance", s.Resonance }, { "filterEnv", s.FilterEnv },
                                { "glide", s.Glide }, { "amp", EnvToJson(s.Amp) }, { "filter", EnvToJson(s.Filter) } };
            }
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
            out.MasterVolume = j.value("masterVolume", 0.8f);
            out.Swing = j.value("swing", 0.0f);
            for (const json& cj : j.value("channels", json::array()))
            {
                Channel c;
                c.Name = cj.value("name", "Channel");
                std::string type = cj.value("type", "Drum");
                c.Type = type == "Synth" ? InstrumentType::Synth : type == "Sampler" ? InstrumentType::Sampler : InstrumentType::Drum;
                c.Volume = cj.value("volume", 0.78f);
                c.Pan = cj.value("pan", 0.0f);
                c.Mute = cj.value("mute", false);
                c.Solo = cj.value("solo", false);
                c.Key = std::clamp(cj.value("key", 60), 0, 127);
                c.Color = cj.value("color", 0u);
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
