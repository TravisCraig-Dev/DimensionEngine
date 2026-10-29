#pragma once
#include "Dimension/Core/Base.h"
#include "Dimension/ECS/Entity.h"
#include "Dimension/Core/Guid.h"
#include "Dimension/Audio/AudioMixer.h"
#include <glm/glm.hpp>
#include <optional>
#include <vector>

namespace Dimension
{
    class Scene;

    // Format of an audio file (WAV, MP3, FLAC or Ogg Vorbis), read without an audio device.
    struct AudioClipInfo
    {
        uint32_t Channels = 0, SampleRate = 0;
        uint64_t Frames = 0;
        float Seconds = 0.0f;
        std::string Format; // "WAV", "MP3", "FLAC", "Ogg Vorbis"
    };

    // A clip's waveform for drawing: the lowest and highest sample of each of `Buckets` equal slices of the clip, per
    // channel (channel c's slice i at c * Buckets + i), in -1..1.
    struct AudioWaveform
    {
        uint32_t Channels = 0;
        size_t Buckets = 0;
        float Seconds = 0.0f;
        std::vector<float> Min, Max;
        // The loudest sample of the clip (0..1).
        float Peak() const;
    };

    // Process-wide audio device (miniaudio). Safe to use when no device exists:
    // everything becomes a no-op.
    class AudioEngine
    {
    public:
        static void Init();
        static void Shutdown();
        static bool IsAvailable();
        static void* GetNative(); // ma_engine*
        static void SetMasterVolume(float volume);
        static std::optional<AudioClipInfo> GetClipInfo(const fs::path& file);
        // Decodes the whole file (no audio device needed).
        static std::optional<AudioWaveform> GetWaveform(const fs::path& file, size_t buckets);

        // The editor's clip preview (the Inspector's Play): one clip at a time, straight to the output (not through
        // the mixer). Play returns false without an audio device or when the file cannot be read.
        static bool PreviewPlay(const fs::path& file, float offsetSeconds = 0.0f, bool loop = false);
        static void PreviewStop();
        static bool PreviewIsPlaying();
        // The clip previewed (playing or stopped at its end; empty after Stop) and where it is, in seconds.
        static const fs::path& PreviewFile();
        static float PreviewPosition();
    };

    // Per-scene audio playback for AudioSource components while playing.
    class AudioWorld
    {
    public:
        explicit AudioWorld(Scene& scene);
        ~AudioWorld();

        void Start();  // creates sounds, starts PlayOnAwake sources
        // Listener + spatial source positions, and occlusion (eased over `dt` seconds; works without an audio device).
        void Update(float dt = 0.0f);
        // Whether 3D colliders block the line from `listener` to the source entity (ignoring triggers, the source's
        // own hierarchy and the listener's).
        static bool IsOccluded(Scene& scene, const glm::vec3& listener, Entity listenerEntity, Entity source);
        // How much the line is blocked, 0..1: the sum of the blocking colliders' Audio Occluder amounts (1 each without
        // the component).
        static float Occlusion(Scene& scene, const glm::vec3& listener, Entity listenerEntity, Entity source);
        // The Audio Reverb Zones' combined effect on a mixer group for a listener position (weight 0 outside them all).
        static AudioMixer::ZoneReverb ReverbZoneAt(Scene& scene, const glm::vec3& listener, const std::string& group);
        void CreateSourcesFor(Entity root);
        void OnEntityDestroyed(Entity e);

        static void Play(Scene& scene, Entity e);
        static void Stop(Scene& scene, Entity e);
        static bool IsPlaying(Scene& scene, Entity e);
        // Fire-and-forget sound through the entity's Audio Source settings (output group, spatial position).
        static void PlayOneShot(Scene& scene, Entity e, AssetHandle clip, float volumeScale = 1.0f);
        // Switches the entity's Audio Source to `clip` (if it plays another) and plays it from `offset` seconds.
        static void PlayClip(Scene& scene, Entity e, AssetHandle clip, float offset = 0.0f);

    private:
        void CreateSource(Entity e, bool autoplay);
        Scene& m_Scene;
        std::vector<std::shared_ptr<void>> m_OneShots; // ma_sound, released when finished
    };
}
