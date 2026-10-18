#pragma once
#include "Dimension/Core/Base.h"
#include "Dimension/ECS/Entity.h"

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

    private:
        void CreateSource(Entity e, bool autoplay);
        Scene& m_Scene;
    };
}
