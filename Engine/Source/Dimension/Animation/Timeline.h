#pragma once
#include "Dimension/Core/Base.h"
#include "Dimension/ECS/Entity.h"
#include <array>
#include <string>
#include <vector>

namespace Dimension
{
    class Scene;

    // A timeline asset (".timeline"), like a Unity Timeline / Animation clip for properties: tracks of keyframes that
    // animate any numeric field of any component (Transform position, light intensity, material color, a script's
    // public float, ...) of the timeline's object or the objects under it (by name path), plus events that call a
    // function on the object's scripts. A Timeline component plays it.
    enum class TimelineInterpolation : int32_t { Smooth = 0, Linear = 1, Step = 2 };

    struct TimelineKey
    {
        float Time = 0.0f;
        std::array<float, 4> Value{}; // as many channels as the field has (1 for floats, 3 for vectors, ...)
    };

    struct TimelineTrack
    {
        std::string Path;      // child objects by name from the timeline's object ("Door/Hinge"); empty = the object itself
        std::string Component; // as in the Inspector, e.g. "Transform", "Point Light"
        std::string Field;     // e.g. "Position", "Intensity"
        TimelineInterpolation Interpolation = TimelineInterpolation::Smooth;
        bool Muted = false;
        std::vector<TimelineKey> Keys; // sorted by time
    };

    struct TimelineEvent
    {
        float Time = 0.0f;
        std::string Function = "OnTimelineEvent"; // called like an animation event (OnAnimationEvent(Function))
    };

    struct TimelineAsset
    {
        float Duration = 5.0f;
        std::vector<TimelineTrack> Tracks;
        std::vector<TimelineEvent> Events;

        static Ref<TimelineAsset> Load(const fs::path& path);
        bool Save(const fs::path& path) const;
    };

    namespace Timeline
    {
        // The track's value at `time` (clamped to its first and last key); returns the channel count (0 = no keys).
        int Sample(const TimelineTrack& track, float time, float out[4]);
        // Adds a key or replaces the one at (about) the same time; keeps keys sorted.
        void SetKey(TimelineTrack& track, float time, const float* value, int channels);
        // The object a track drives under `root`, and the path to an object from it (empty when it is root itself;
        // "" with ok = false when target is not under root).
        Entity ResolvePath(Scene& scene, Entity root, const std::string& path);
        std::string PathOf(Scene& scene, Entity root, Entity target, bool* ok = nullptr);
        // Writes every (unmuted) track's value at `time`. Returns how many fields were set.
        int Apply(Scene& scene, Entity root, const TimelineAsset& timeline, float time);
        // Reads a track's field from its object now (for keying the current value). Returns the channel count.
        int ReadCurrent(Scene& scene, Entity root, const TimelineTrack& track, float out[4]);
        // Plays the scene's Timeline components (play mode), firing events passed this frame.
        void Update(Scene& scene, float dt);
        void Restart(Scene& scene);
    }
}
