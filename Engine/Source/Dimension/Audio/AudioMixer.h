#pragma once
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
        static void Shutdown();
    };
}
