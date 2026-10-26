#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace Dimension
{
    // One bus of the project's audio mixer (like a Unity AudioMixerGroup). Group 0 is Master.
    struct AudioMixerGroup
    {
        std::string Name = "Group";
        int Parent = 0;          // index of the parent group (ignored for Master)
        float VolumeDb = 0.0f;   // -80 dB = silent
        bool Mute = false;
        bool Solo = false;
        float LowPassHz = 0.0f;  // 0 = filter off

        // Insert effects, after the low-pass filter. On/off and amounts apply live; a new echo delay applies when the
        // mixer is rebuilt (play start).
        bool Echo = false;
        float EchoDelayMs = 300.0f;
        float EchoDecay = 0.4f;  // feedback per repeat (0-0.95)
        float EchoWet = 0.5f;    // level of the repeats
        bool Reverb = false;
        float ReverbRoomSize = 0.6f; // 0-1
        float ReverbDamping = 0.5f;  // high-frequency absorption, 0-1
        float ReverbWet = 0.35f;     // 0-1

        // Send: a copy of this group's output (after its volume and effects) also goes to another group, at Send
        // Level. Typically several groups send to one "return" group holding a shared reverb. Applies live; a send
        // that would loop back into this group is ignored.
        int SendTarget = -1;         // group index, -1 = no send
        float SendLevelDb = 0.0f;
    };

    // A saved state of the mixer (like Unity's AudioMixerSnapshot): each group's volume and low-pass cutoff, by group
    // name. Scripts blend to one over time, e.g. a muffled "Paused" mix.
    struct AudioMixerSnapshot
    {
        struct Value
        {
            std::string Group;
            float VolumeDb = 0.0f;
            float LowPassHz = 0.0f; // 0 = filter off
        };
        std::string Name = "Snapshot";
        std::vector<Value> Values;
    };

    // Signal level of a mixer group, linear amplitude (1 = full scale).
    struct AudioLevel
    {
        float Peak = 0.0f; // highest sample since the previous read
        float Rms = 0.0f;  // root mean square, smoothed over about 300 ms
    };

    // Measures a stream of interleaved samples. Process runs on the audio thread, Read on any other.
    class AudioMeter
    {
    public:
        static constexpr float kRmsSeconds = 0.3f;
        void Process(const float* samples, uint32_t frames, uint32_t channels, uint32_t sampleRate);
        AudioLevel Read(); // resets the peak
        void Reset() { m_Peak = 0.0f; m_MeanSquare = 0.0f; }

    private:
        std::atomic<float> m_Peak{ 0.0f };
        std::atomic<float> m_MeanSquare{ 0.0f };
    };

    // The project's mixer: Audio Sources route to a group by name; groups route to their parent and
    // finally Master. Edited in Window > Audio Mixer and stored in the project file. Volumes, mute,
    // solo and filters apply live; adding, removing or re-parenting groups applies when play starts.
    class AudioMixer
    {
    public:
        static constexpr float kSilentDb = -80.0f;

        static std::vector<AudioMixerGroup> DefaultGroups();
        static std::vector<AudioMixerGroup>& Groups(); // the open project's groups

        static int FindGroup(const std::string& name);  // -1 if missing
        // The send target each group really uses: -1 for none, out of range, itself, or a send that would create a
        // loop through parents and earlier groups' sends.
        static std::vector<int> ValidSends(const std::vector<AudioMixerGroup>& groups);
        // Volume of a group including mute/solo (not its parents; the graph multiplies those in).
        static float EffectiveGain(int group);
        static void SetVolumeDb(const std::string& group, float db);
        static float GetVolumeDb(const std::string& group);
        static float DbToLinear(float db);
        static float LinearToDb(float linear);

        // Device graph (no-ops without an audio device).
        static void Apply();               // pushes volumes and filters to the running graph
        static void MarkStructureDirty();  // groups added/removed/re-parented
        static void Rebuild();             // recreates the graph; only call when no sounds exist
        static void* GetGroupNode(int group); // ma_sound_group* (Master when out of range), null without a device
        // Post-fader level of a group (its sources and child groups, after volume and filter). Zero without a device
        // or before the graph is built.
        static AudioLevel GetLevel(int group);

        // Snapshots (stored in the project). Capture records every group's current volume and low-pass cutoff.
        static std::vector<AudioMixerSnapshot>& Snapshots();
        static int FindSnapshot(const std::string& name); // -1 if missing
        static void CaptureSnapshot(const std::string& name); // creates or overwrites
        // Blends every group in the snapshot from its current values to the snapshot's over `seconds` (0 = at
        // once): volumes linearly in dB, cutoffs in octaves. Groups the snapshot does not list are left alone.
        static bool TransitionToSnapshot(const std::string& name, float seconds);
        static bool IsTransitioning();
        static void CancelTransition();
        // Advances a transition (called every frame while playing).
        static void Update(float dt);
        static void Shutdown();

        // Reverb zones' influence on a group (runtime only): Weight 0 = none, 1 = the zone's reverb replaces the
        // group's own. Applied live to the group's reverb effect.
        struct ZoneReverb
        {
            float Weight = 0.0f, RoomSize = 0.0f, Damping = 0.0f, Wet = 0.0f;
            bool operator==(const ZoneReverb&) const = default;
        };
        static void SetZoneReverb(int group, const ZoneReverb& zone);
        static ZoneReverb GetZoneReverb(int group);
        static void ClearZoneReverbs();
        // The reverb a group runs with: its own effect settings faded toward the zone's by the zone's weight.
        struct ReverbParams { bool On = false; float RoomSize = 0.0f, Damping = 0.0f, Wet = 0.0f; };
        static ReverbParams EffectiveReverb(const AudioMixerGroup& group, const ZoneReverb& zone);
    };
}
