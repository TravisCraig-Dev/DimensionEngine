#pragma once
#include <array>
#include <string>
#include <vector>

// The song's Mixer (tracks with effect chains) and automation: kept apart from Song.h's notes and patterns.
namespace Dimension::Music
{
    // ---- Effects ----

    enum class EffectType : int { EQ = 0, Compressor, Delay, Reverb, Filter, Count };
    const char* EffectTypeName(EffectType t);
    constexpr int kEffectParams = 6;
    constexpr int kMaxEffects = 8;  // slots per mixer track

    // One of an effect type's settings: its name, range and default (Log: shown and dragged on a log scale).
    struct EffectParamInfo
    {
        const char* Name = nullptr;
        float Min = 0.0f, Max = 1.0f, Default = 0.0f;
        const char* Format = "%.2f";
        bool Log = false;
    };
    struct EffectInfo
    {
        int ParamCount = 0;
        std::array<EffectParamInfo, kEffectParams> Params{};
        float DefaultMix = 1.0f;
        bool Send = false; // the wet signal is added to the dry (delay, reverb) rather than replacing it
    };
    const EffectInfo& GetEffectInfo(EffectType t);

    // An effect in a mixer track's chain: its type, settings (by GetEffectInfo's order) and wet amount.
    struct Effect
    {
        EffectType Type = EffectType::EQ;
        bool Enabled = true;
        float Mix = 1.0f;
        std::array<float, kEffectParams> P{};
        static Effect Make(EffectType type); // with the type's defaults
    };

    // A mixer track: channels routed to it are summed, run through its effects, then its level and pan, and sent to
    // its output (Master, or a later track). Track 0 is the Master.
    struct MixerTrack
    {
        std::string Name;
        float Volume = 1.0f;   // gain 0..1.25
        float Pan = 0.0f;      // balance -1..1
        bool Mute = false;
        int Output = 0;        // 0 = Master; else a track after this one (so routing never loops)
        std::vector<Effect> Effects;
    };

    // ---- Automation ----

    // A setting automation can move.
    enum class ParamKind : int { ChannelVolume = 0, ChannelPan, SynthCutoff, SynthResonance, MixerVolume, MixerPan, EffectParam, EffectMix };
    struct ParamRef
    {
        ParamKind Kind = ParamKind::ChannelVolume;
        int Index = 0;  // the channel, or the mixer track
        int Slot = 0;   // effect slot (EffectParam, EffectMix)
        int Param = 0;  // effect setting (EffectParam)
        bool operator==(const ParamRef&) const = default;
    };

    struct AutomationPoint
    {
        int Tick = 0;       // from the clip's start
        float Value = 0.0f; // 0..1 across the setting's range
        bool operator==(const AutomationPoint&) const = default;
    };

    // An automation clip on the Playlist: while Song mode plays over it, the setting follows its curve (straight lines
    // between points; before the first and after the last, their values).
    struct AutomationClip
    {
        ParamRef Target;
        int Track = 0;
        int Start = 0, Length = 384;
        std::vector<AutomationPoint> Points; // by Tick
        float ValueAt(double tick) const;    // tick from the clip's start
        void SortPoints();
    };
}
