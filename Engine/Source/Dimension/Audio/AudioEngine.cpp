#include "Dimension/Audio/AudioEngine.h"
#include "Dimension/Scene/Scene.h"
#include "Dimension/Asset/AssetManager.h"
#include "Dimension/Core/Log.h"

#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_ENCODING
#include <miniaudio.h>

namespace Dimension
{
    namespace
    {
        ma_engine* s_Engine = nullptr;

        ma_sound* SoundOf(AudioSourceComponent& src) { return static_cast<ma_sound*>(src.Runtime.get()); }
    }

    void AudioEngine::Init()
    {
        if (s_Engine) return;
        s_Engine = new ma_engine();
        if (ma_engine_init(nullptr, s_Engine) != MA_SUCCESS)
        {
            Log::Warn("No audio device available; audio disabled");
            delete s_Engine;
            s_Engine = nullptr;
            return;
        }
        Log::Info("Audio engine initialized ({} Hz)", ma_engine_get_sample_rate(s_Engine));
    }

    void AudioEngine::Shutdown()
    {
        if (!s_Engine) return;
        ma_engine_uninit(s_Engine);
        delete s_Engine;
        s_Engine = nullptr;
    }

    bool AudioEngine::IsAvailable() { return s_Engine != nullptr; }
    void* AudioEngine::GetNative() { return s_Engine; }
    void AudioEngine::SetMasterVolume(float v)
    {
        if (s_Engine) ma_engine_set_volume(s_Engine, v);
    }

    AudioWorld::AudioWorld(Scene& scene) : m_Scene(scene) {}

    AudioWorld::~AudioWorld()
    {
        for (auto [e, src] : m_Scene.GetRegistry().ViewOf<AudioSourceComponent>()) src.Runtime.reset();
    }

    void AudioWorld::CreateSource(Entity e, bool autoplay)
    {
        auto& src = m_Scene.GetRegistry().Get<AudioSourceComponent>(e);
        if (!s_Engine || src.Runtime || !src.Clip.IsValid()) return;
        fs::path path = AssetManager::GetAbsolutePath(src.Clip);
        auto* sound = new ma_sound();
        if (ma_sound_init_from_file_w(s_Engine, path.c_str(), MA_SOUND_FLAG_DECODE, nullptr, nullptr, sound) != MA_SUCCESS)
        {
            Log::Error("Failed to load audio clip '{}'", path.string());
            delete sound;
            return;
        }
        src.Runtime = std::shared_ptr<void>(sound, [](void* p) {
            ma_sound_uninit(static_cast<ma_sound*>(p));
            delete static_cast<ma_sound*>(p);
        });
        ma_sound_set_volume(sound, src.Volume);
        ma_sound_set_pitch(sound, src.Pitch);
        ma_sound_set_looping(sound, src.Loop);
        ma_sound_set_spatialization_enabled(sound, src.Spatial);
        ma_sound_set_min_distance(sound, src.MinDistance);
        ma_sound_set_max_distance(sound, src.MaxDistance);
        if (autoplay && src.PlayOnAwake && m_Scene.GetRegistry().Get<TransformComponent>(e).ActiveInHierarchy)
            ma_sound_start(sound);
    }

    void AudioWorld::Start()
    {
        std::vector<Entity> sources;
        for (auto [e, src] : m_Scene.GetRegistry().ViewOf<AudioSourceComponent>()) sources.push_back(e);
        for (Entity e : sources) CreateSource(e, true);
        Update();
    }

    void AudioWorld::CreateSourcesFor(Entity root)
    {
        if (m_Scene.GetRegistry().Has<AudioSourceComponent>(root)) CreateSource(root, true);
        for (Entity c : m_Scene.GetChildren(root)) CreateSourcesFor(c);
    }

    void AudioWorld::OnEntityDestroyed(Entity e)
    {
        if (auto* src = m_Scene.GetRegistry().TryGet<AudioSourceComponent>(e)) src->Runtime.reset();
    }

    void AudioWorld::Update()
    {
        if (!s_Engine) return;
        auto& reg = m_Scene.GetRegistry();
        Entity cam = m_Scene.GetPrimaryCamera();
        if (!cam.IsNull())
        {
            auto& t = reg.Get<TransformComponent>(cam);
            glm::vec3 p = t.GetWorldPosition(), f = t.Forward(), u = t.Up();
            ma_engine_listener_set_position(s_Engine, 0, p.x, p.y, p.z);
            ma_engine_listener_set_direction(s_Engine, 0, f.x, f.y, f.z);
            ma_engine_listener_set_world_up(s_Engine, 0, u.x, u.y, u.z);
        }
        for (auto [e, src, t] : reg.ViewOf<AudioSourceComponent, TransformComponent>())
        {
            ma_sound* s = SoundOf(src);
            if (!s) continue;
            glm::vec3 p = t.GetWorldPosition();
            ma_sound_set_position(s, p.x, p.y, p.z);
            ma_sound_set_volume(s, src.Volume);
        }
    }

    void AudioWorld::Play(Scene& scene, Entity e)
    {
        auto* src = scene.GetRegistry().TryGet<AudioSourceComponent>(e);
        if (!src || !SoundOf(*src)) return;
        ma_sound_seek_to_pcm_frame(SoundOf(*src), 0);
        ma_sound_start(SoundOf(*src));
    }

    void AudioWorld::Stop(Scene& scene, Entity e)
    {
        auto* src = scene.GetRegistry().TryGet<AudioSourceComponent>(e);
        if (src && SoundOf(*src)) ma_sound_stop(SoundOf(*src));
    }
}
