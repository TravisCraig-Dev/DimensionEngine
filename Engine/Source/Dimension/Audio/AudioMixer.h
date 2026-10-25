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
    };
}
