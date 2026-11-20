#pragma once
#include "Dimension/Core/Base.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace Dimension::Music
{
    // Song time is counted in ticks: 96 to a beat (a quarter note), so a 16th-note step is 24.
    constexpr int kTicksPerBeat = 96;

    // Attack, decay and release in seconds; sustain 0..1.
    struct Envelope
    {
        float Attack = 0.005f, Decay = 0.2f, Sustain = 0.7f, Release = 0.2f;
    };

    enum class InstrumentType : int { Drum = 0, Synth, Sampler };
    const char* InstrumentTypeName(InstrumentType t);

    // A synthesized drum (no samples needed): its sound shaped by Tune (semitones), Decay (x its natural length) and
    // Tone (0..1: darker to brighter / less to more noise).
    enum class DrumKind : int { Kick = 0, Snare, Clap, ClosedHat, OpenHat, Tom, Rim, Count };
    const char* DrumKindName(DrumKind k);
    struct DrumParams
    {
        DrumKind Kind = DrumKind::Kick;
        float Tune = 0.0f, Decay = 1.0f, Tone = 0.5f;
    };

    enum class Waveform : int { Saw = 0, Square, Triangle, Sine, Noise, Count };
    const char* WaveformName(Waveform w);
    // A two-oscillator subtractive synth: oscillators mixed, through a resonant low-pass filter opened by its envelope.
    struct SynthParams
    {
        Waveform Wave1 = Waveform::Saw, Wave2 = Waveform::Square;
        float Mix = 0.35f;          // 0 = oscillator 1 only, 1 = oscillator 2 only
        int Octave2 = 0;            // oscillator 2's octave offset
        float Detune = 0.08f;       // oscillator 2 detune in semitones
        float PulseWidth = 0.5f;    // square waves' duty cycle
        float Cutoff = 2400.0f;     // Hz
        float Resonance = 0.25f;    // 0..1
        float FilterEnv = 0.5f;     // how far the filter envelope opens the cutoff (0..1, up to 5 octaves)
        float Glide = 0.0f;         // seconds (monophonic glide when > 0)
        Envelope Amp{ 0.005f, 0.25f, 0.7f, 0.25f };
        Envelope Filter{ 0.002f, 0.35f, 0.15f, 0.3f };
    };

    // Plays an audio file across the keyboard: at its own pitch on RootKey, resampled for other keys.
    struct SamplerParams
    {
        uint64_t Sample = 0;        // the audio asset's GUID (0 = none)
        std::string SamplePath;     // Assets-relative (or absolute) path, used when the GUID does not resolve
        int RootKey = 60;
        bool OneShot = true;        // play to the end whatever the note's length (drum hits)
        bool Reverse = false;
        Envelope Amp{ 0.001f, 0.0f, 1.0f, 0.05f };
    };

    // A row of the Channel Rack: one instrument with its level, pan and mute.
    struct Channel
    {
        std::string Name = "Channel";
        InstrumentType Type = InstrumentType::Drum;
        float Volume = 0.78f;       // 0..1.25 (gain)
        float Pan = 0.0f;           // -1..1
        bool Mute = false, Solo = false;
        int Key = 60;               // the note the step sequencer plays
        uint32_t Color = 0;         // RGBA (0: picked from the name)
        DrumParams Drum;
        SynthParams Synth;
        SamplerParams Sampler;
    };

    struct Note
    {
        int Channel = 0;
        int Start = 0, Length = 24; // ticks from the pattern's start
        int Key = 60;               // MIDI note (60 = C5 in FL's naming, middle C)
        float Velocity = 0.8f;      // 0..1
        bool operator==(const Note&) const = default;
    };

    // A pattern: notes for any of the channels, looped in Pattern mode and placed on the Playlist in Song mode.
    struct Pattern
    {
        std::string Name = "Pattern 1";
        int Bars = 1;               // its length (the step sequencer shows Bars * steps per bar)
        std::vector<Note> Notes;
    };

    // A pattern placed on the Playlist (Song mode): Start and Length in ticks.
    struct PlaylistClip
    {
        int Pattern = 0;
        int Track = 0;
        int Start = 0, Length = 0;  // Length 0: the pattern's own length
    };

    // A song (.dsong file, JSON): tempo, channels, patterns and their arrangement.
    struct Song
    {
        std::string Name = "Song";
        float Tempo = 130.0f;       // BPM
        int BeatsPerBar = 4, StepsPerBeat = 4;
        float MasterVolume = 0.8f;
        float Swing = 0.0f;         // 0..1: every second step is delayed by up to half a step
        std::vector<Channel> Channels;
        std::vector<Pattern> Patterns;
        std::vector<PlaylistClip> Playlist;

        int StepTicks() const { return kTicksPerBeat / std::max(1, StepsPerBeat); }
        int BarTicks() const { return kTicksPerBeat * std::max(1, BeatsPerBar); }
        int PatternTicks(int pattern) const;
        // The Playlist's end (ticks); 0 when empty.
        int SongTicks() const;
        double TicksPerSecond() const { return Tempo / 60.0 * kTicksPerBeat; }

        // The step sequencer: whether `channel` has a note starting on `step` of `pattern`, and toggling it (a note of
        // one step on the channel's key).
        bool HasStep(int pattern, int channel, int step) const;
        void ToggleStep(int pattern, int channel, int step);
        // Removes a channel and its notes (later channels' notes move down one).
        void RemoveChannel(int channel);

        nlohmann::json ToJson() const;
        static bool FromJson(const nlohmann::json& j, Song& out);
        bool Save(const fs::path& path) const;
        static bool Load(const fs::path& path, Song& out);
        // What File > New Song starts with: a kick, clap, hats and snare, a bass synth and a beat in Pattern 1.
        static Song Default();
    };

    // A channel's color (its own, or one picked from its name).
    uint32_t ChannelColor(const Channel& c);
}
