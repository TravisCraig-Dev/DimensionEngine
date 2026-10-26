#pragma once
#include "Dimension/Core/Base.h"
#include "Dimension/Core/Guid.h"
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
    // function on the object's scripts. A Timeline component plays it. Besides property tracks there are clip tracks:
    // Activation (the object is active only during its clips), Audio (each clip plays an audio clip through the
    // object's Audio Source), Animation (each clip plays an animation clip on the object's Animator, overriding its
    // controller, with overlapping clips crossfading) and Control (each clip plays another timeline on the object:
    // nested sequences, with its tracks resolved from that object, its events and its audio).
    enum class TimelineInterpolation : int32_t { Smooth = 0, Linear = 1, Step = 2 };
    enum class TimelineTrackKind : int32_t { Property = 0, Activation = 1, Audio = 2, Animation = 3, Control = 4 };

    struct TimelineClip
    {
        float Start = 0.0f, Duration = 1.0f;
        AssetHandle Audio = AssetHandle::None(); // Audio tracks
        std::string Animation;                   // Animation tracks: the clip's name in the Animator's model
        AssetHandle Timeline = AssetHandle::None(); // Control tracks: the timeline played
        float Speed = 1.0f;                      // Animation and Control: playback rate
        float ClipIn = 0.0f;                     // seconds into the audio, animation or timeline where it starts
        bool Loop = true;                        // Animation and Control: wrap past the end (otherwise hold the last frame)
        float End() const { return Start + Duration; }
    };

    struct TimelineKey
    {
        float Time = 0.0f;
        std::array<float, 4> Value{}; // as many channels as the field has (1 for floats, 3 for vectors, ...)
    };

    struct TimelineTrack
    {
        TimelineTrackKind Kind = TimelineTrackKind::Property;
        std::string Path;      // child objects by name from the timeline's object ("Door/Hinge"); empty = the object itself
        std::string Component; // as in the Inspector, e.g. "Transform", "Point Light"
        std::string Field;     // e.g. "Position", "Intensity"
        TimelineInterpolation Interpolation = TimelineInterpolation::Smooth;
        bool Muted = false;
        std::vector<TimelineKey> Keys; // sorted by time
        std::vector<TimelineClip> Clips; // clip tracks, sorted by start
        bool IsClipTrack() const { return Kind != TimelineTrackKind::Property; }
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
        // Writes every (unmuted) track's state at `time`: property values, activation, animation poses and the nested
        // timelines of control clips (audio plays only while playing). Returns how many tracks applied.
        int Apply(Scene& scene, Entity root, const TimelineAsset& timeline, float time);
        // Audio clips starting in (from, to] (and stopping ones that ended), for playback.
        void PlayAudio(Scene& scene, Entity root, const TimelineAsset& timeline, float from, float to);
        // A control clip's time in its timeline (Clip In, Speed, then looped or held at the end) at timeline time `time`.
        float ControlTime(const TimelineClip& clip, float nestedDuration, float time);
        // Playback from `from` to `to` (from < 0 includes things at 0): audio clips started and stopped, and the events
        // passed, as (object, function) pairs, on this timeline and on the timelines its control clips play.
        void Advance(Scene& scene, Entity root, const TimelineAsset& timeline, float from, float to, std::vector<std::pair<Entity, std::string>>& events);
        // Hands Animators driven by the timeline's animation tracks back to their own playback.
        void Release(Scene& scene, Entity root, const TimelineAsset& timeline);
        // Sorts clips by start (after editing).
        void SortClips(TimelineTrack& track);
        // Reads a track's field from its object now (for keying the current value). Returns the channel count.
        int ReadCurrent(Scene& scene, Entity root, const TimelineTrack& track, float out[4]);
        // Plays the scene's Timeline components (play mode), firing events passed this frame.
        void Update(Scene& scene, float dt);
        void Restart(Scene& scene);
    }
}
