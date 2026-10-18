#pragma once
#include "Dimension/Core/Base.h"
#include "Dimension/ECS/Entity.h"
#include "Dimension/Core/Guid.h"
#include "Dimension/Audio/AudioMixer.h"

namespace Dimension
{
    class Scene;

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
    };

    // Per-scene audio playback for AudioSource components while playing.
    class AudioWorld
    {
    public:
        explicit AudioWorld(Scene& scene);
        ~AudioWorld();

        void Start();  // creates sounds, starts PlayOnAwake sources
        void Update(); // listener + spatial source positions
        void CreateSourcesFor(Entity root);
        void OnEntityDestroyed(Entity e);

        static void Play(Scene& scene, Entity e);
        static void Stop(Scene& scene, Entity e);
        static bool IsPlaying(Scene& scene, Entity e);
        // Fire-and-forget sound through the entity's Audio Source settings (output group, spatial position).
        static void PlayOneShot(Scene& scene, Entity e, AssetHandle clip, float volumeScale = 1.0f);

    private:
        void CreateSource(Entity e, bool autoplay);
        Scene& m_Scene;
        std::vector<std::shared_ptr<void>> m_OneShots; // ma_sound, released when finished
    };
}
