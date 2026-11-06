#include "Dimension/Video/VideoPlayer.h"
#include "Dimension/Video/VideoDecoder.h"
#include "Dimension/Asset/AssetManager.h"
#include "Dimension/Audio/AudioEngine.h"
#include "Dimension/Audio/AudioMixer.h"
#include "Dimension/Renderer/Material.h"
#include "Dimension/Scene/Scene.h"
#include "Dimension/Core/Log.h"
#define MA_NO_ENCODING // as where the implementation is built (AudioEngine.cpp)
#include <miniaudio.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace Dimension
{
    namespace
    {
        // One playing clip: its decoding thread, the pictures decoded ahead and the sound waiting for the audio engine.
        struct Stream;

        // The sound as a miniaudio data source reading the ring the decoding thread fills (silence when it runs dry,
        // and the clock does not move on then: the pictures wait for the sound).
        struct SoundSource
        {
            ma_data_source_base Base;
            Stream* Owner = nullptr;
        };

        struct Stream
        {
            fs::path File;
            std::atomic<bool> Quit{ false }, Ready{ false }, Failed{ false }, Loop{ true };
            std::atomic<bool> VideoEnded{ false }, AudioEnded{ false };
            bool WantAudio = true;
            VideoInfo Info;
            std::thread Thread;
            struct Frame
            {
                double Time;
                std::vector<uint32_t> Pixels;
            };
            std::mutex Mutex;
            std::deque<Frame> Frames; // decoded ahead, in order
            // Sound: the ring (made before the thread starts filling it), what miniaudio has played.
            ma_pcm_rb Ring{};
            bool RingReady = false;
            std::atomic<uint64_t> Played{ 0 };
            SoundSource Source;
            ma_sound Sound{};
            bool SoundReady = false;

            ~Stream()
            {
                Quit = true;
                if (Thread.joinable()) Thread.join();
                if (SoundReady) ma_sound_uninit(&Sound);
                if (SoundReady) ma_data_source_uninit(&Source.Base);
                if (RingReady) ma_pcm_rb_uninit(&Ring);
            }
        };

        ma_result SourceRead(ma_data_source* ds, void* out, ma_uint64 frameCount, ma_uint64* framesRead)
        {
            Stream& s = *reinterpret_cast<SoundSource*>(ds)->Owner;
            uint32_t channels = s.Info.Channels;
            auto* dst = static_cast<float*>(out);
            ma_uint64 done = 0;
            while (done < frameCount)
            {
                ma_uint32 n = (ma_uint32)std::min<ma_uint64>(frameCount - done, 1u << 20);
                void* buffer = nullptr;
                if (ma_pcm_rb_acquire_read(&s.Ring, &n, &buffer) != MA_SUCCESS || n == 0) break;
                std::memcpy(dst + done * channels, buffer, (size_t)n * channels * sizeof(float));
                ma_pcm_rb_commit_read(&s.Ring, n);
                done += n;
            }
            s.Played += done;
            if (done < frameCount) std::memset(dst + done * channels, 0, (size_t)(frameCount - done) * channels * sizeof(float));
            if (framesRead) *framesRead = frameCount;
            return MA_SUCCESS;
        }
        ma_result SourceSeek(ma_data_source*, ma_uint64) { return MA_NOT_IMPLEMENTED; }
        ma_result SourceFormat(ma_data_source* ds, ma_format* format, ma_uint32* channels, ma_uint32* rate, ma_channel* map, size_t mapCap)
        {
            Stream& s = *reinterpret_cast<SoundSource*>(ds)->Owner;
            *format = ma_format_f32;
            *channels = s.Info.Channels;
            *rate = s.Info.SampleRate;
            if (map) ma_channel_map_init_standard(ma_standard_channel_map_default, map, mapCap, s.Info.Channels);
            return MA_SUCCESS;
        }
        ma_result SourceCursor(ma_data_source*, ma_uint64* cursor)
        {
            *cursor = 0;
            return MA_NOT_IMPLEMENTED;
        }
        ma_result SourceLength(ma_data_source*, ma_uint64* length)
        {
            *length = 0;
            return MA_NOT_IMPLEMENTED;
        }
        ma_data_source_vtable s_SourceVTable = { SourceRead, SourceSeek, SourceFormat, SourceCursor, SourceLength, nullptr, 0 };

        // The decoding thread: opens the clip, then keeps a few pictures and half a second of sound ready, starting over
        // at the end while Loop is on (each stream's times go on from the clip's length, so the clock never steps back).
        void Decode(Stream& s)
        {
            std::unique_ptr<VideoDecoder> decoder = VideoDecoder::Open(s.File);
            if (!decoder)
            {
                s.Failed = true;
                return;
            }
            s.Info = decoder->Info();
            if (!s.WantAudio) s.Info.HasAudio = false;
            if (s.Info.HasAudio)
            {
                s.RingReady = ma_pcm_rb_init(ma_format_f32, s.Info.Channels, std::max(s.Info.SampleRate / 2, 4096u), nullptr, nullptr, &s.Ring) == MA_SUCCESS;
                if (!s.RingReady) s.Info.HasAudio = false;
            }
            s.Ready = true;
            double length = std::max(s.Info.Duration, 1e-3);
            double videoLoop = 0.0, audioLoop = 0.0;
            std::vector<float> pending;
            size_t pendingAt = 0;
            (void)audioLoop;
            while (!s.Quit)
            {
                bool busy = false;
                if (s.Info.HasVideo && !s.VideoEnded)
                {
                    size_t queued;
                    {
                        std::lock_guard lock(s.Mutex);
                        queued = s.Frames.size();
                    }
                    if (queued < 4)
                    {
                        Stream::Frame frame;
                        if (decoder->NextFrame(frame.Time, frame.Pixels))
                        {
                            frame.Time += videoLoop;
                            std::lock_guard lock(s.Mutex);
                            s.Frames.push_back(std::move(frame));
                        }
                        else if (s.Loop)
                        {
                            decoder->RewindVideo();
                            videoLoop += length;
                        }
                        else
                            s.VideoEnded = true;
                        busy = true;
                    }
                }
                if (s.Info.HasAudio && !s.AudioEnded)
                {
                    uint32_t channels = s.Info.Channels;
                    if (pendingAt >= pending.size())
                    {
                        double t;
                        pendingAt = 0;
                        if (!decoder->NextAudio(t, pending))
                        {
                            pending.clear();
                            if (s.Loop) decoder->RewindAudio();
                            else s.AudioEnded = true;
                        }
                        busy = true;
                    }
                    while (pendingAt < pending.size())
                    {
                        ma_uint32 n = (ma_uint32)((pending.size() - pendingAt) / channels);
                        void* buffer = nullptr;
                        if (n == 0 || ma_pcm_rb_acquire_write(&s.Ring, &n, &buffer) != MA_SUCCESS || n == 0) break;
                        std::memcpy(buffer, pending.data() + pendingAt, (size_t)n * channels * sizeof(float));
                        ma_pcm_rb_commit_write(&s.Ring, n);
                        pendingAt += (size_t)n * channels;
                        busy = true;
                    }
                    if (pendingAt < pending.size() && pending.size() / channels == 0) pendingAt = pending.size();
                }
                if (!busy) std::this_thread::sleep_for(std::chrono::milliseconds(3));
            }
        }

        struct Player
        {
            std::unique_ptr<Stream> S;
            AssetHandle Texture = AssetHandle::None();
            bool Playing = false, Paused = false, Bound = false, Seen = false;
            double Clock = 0.0;     // seconds of clip played (without sound; with sound the sound's clock)
            double Length = 0.0;
            glm::uvec2 Size{ 0 };
            uint64_t Shown = 0;
            AssetHandle Clip = AssetHandle::None();
        };

        uint64_t KeyOf(Entity e) { return (uint64_t)e.Index | (uint64_t)e.Generation << 32; }
        std::unordered_map<const Scene*, std::unordered_map<uint64_t, Player>> s_Players;

        Player* Find(Scene& scene, Entity e)
        {
            auto it = s_Players.find(&scene);
            if (it == s_Players.end()) return nullptr;
            auto p = it->second.find(KeyOf(e));
            return p == it->second.end() ? nullptr : &p->second;
        }

        void Start(Player& p, const VideoPlayerComponent& vp)
        {
            fs::path file = AssetManager::GetAbsolutePath(vp.Clip);
            p.S.reset();
            if (file.empty()) return;
            p.S = std::make_unique<Stream>();
            p.S->File = file;
            p.S->Loop = vp.Loop;
            p.S->WantAudio = vp.PlayAudio && AudioEngine::IsAvailable();
            p.S->Source.Owner = p.S.get();
            p.Clock = 0.0;
            p.Clip = vp.Clip;
            Stream* raw = p.S.get();
            p.S->Thread = std::thread([raw] { Decode(*raw); });
        }

        // Shows the picture where the player's object can: its UI Image, Sprite Renderer, or Mesh Renderer (its own copy
        // of its material, the picture as its base color and, with Emission, glowing).
        void Bind(Scene& scene, Entity e, Player& p, const VideoPlayerComponent& vp)
        {
            p.Bound = true;
            if (vp.RenderMode != VideoRenderMode::MaterialOverride || !p.Texture.IsValid()) return;
            auto& reg = scene.GetRegistry();
            if (auto* image = reg.TryGet<UIImageComponent>(e)) image->Texture = p.Texture;
            else if (auto* sprite = reg.TryGet<SpriteRendererComponent>(e)) sprite->Texture = p.Texture;
            else if (reg.Has<MeshRendererComponent>(e))
                if (Ref<Material> m = AssetManager::GetMaterial(scene.GetRendererMaterialInstance(e)))
                {
                    m->BaseColorMap = p.Texture;
                    if (vp.Emission > 0.0f)
                    {
                        m->EmissionMap = p.Texture;
                        m->EmissionColor = glm::vec3(1.0f);
                        m->EmissionStrength = vp.Emission;
                    }
                }
        }

        void StartSound(Player& p, const VideoPlayerComponent& vp)
        {
            Stream& s = *p.S;
            if (!s.Info.HasAudio || s.SoundReady || !AudioEngine::IsAvailable()) return;
            ma_data_source_config config = ma_data_source_config_init();
            config.vtable = &s_SourceVTable;
            if (ma_data_source_init(&config, &s.Source.Base) != MA_SUCCESS) return;
            auto* engine = static_cast<ma_engine*>(AudioEngine::GetNative());
            auto* group = static_cast<ma_sound_group*>(AudioMixer::GetGroupNode(vp.Output.empty() ? 0 : AudioMixer::FindGroup(vp.Output)));
            if (ma_sound_init_from_data_source(engine, &s.Source.Base, MA_SOUND_FLAG_NO_SPATIALIZATION, group, &s.Sound) != MA_SUCCESS)
            {
                ma_data_source_uninit(&s.Source.Base);
                Log::Warn("Video Player: the clip's sound could not be played");
                return;
            }
            s.SoundReady = true;
        }

        void Halt(Player& p)
        {
            if (p.S && p.S->SoundReady) ma_sound_stop(&p.S->Sound);
        }
    }

    namespace Video
    {
        void Update(Scene& scene, float unscaledDt)
        {
            auto& players = s_Players[&scene];
            auto& reg = scene.GetRegistry();
            for (auto& [key, p] : players) p.Seen = false;
            for (auto [e, vp] : reg.ViewOf<VideoPlayerComponent>())
            {
                uint64_t key = KeyOf(e);
                auto it = players.find(key);
                if (it == players.end())
                {
                    // A new player: Play On Awake starts it.
                    it = players.emplace(key, Player{}).first;
                    if (vp.PlayOnAwake && vp.Clip.IsValid())
                    {
                        Start(it->second, vp);
                        it->second.Playing = true;
                    }
                }
                Player& p = it->second;
                p.Seen = true;
                if (!p.S) continue;
                Stream& s = *p.S;
                s.Loop = vp.Loop;
                if (s.Failed)
                {
                    Log::Error("Video Player: cannot play '{}'", s.File.string());
                    p.S.reset();
                    p.Playing = false;
                    continue;
                }
                if (!s.Ready) continue;
                if (!p.Bound)
                {
                    // Prepared: the picture's texture and the sound.
                    p.Length = s.Info.Duration;
                    p.Size = { s.Info.Width, s.Info.Height };
                    if (s.Info.HasVideo && !p.Texture.IsValid())
                        p.Texture = AssetManager::CreateRuntimeTexture(s.Info.Width, s.Info.Height, true, false, &scene);
                    Bind(scene, e, p, vp);
                    StartSound(p, vp);
                    if (s.SoundReady && p.Playing && !p.Paused) ma_sound_start(&s.Sound);
                }
                if (s.SoundReady)
                {
                    ma_sound_set_volume(&s.Sound, std::max(vp.Volume, 0.0f));
                    ma_sound_set_pitch(&s.Sound, std::clamp(vp.PlaybackSpeed, 0.01f, 10.0f));
                }
                if (!p.Playing || p.Paused) continue;
                // The clock: the sound's when it has one, else real time at the playback speed.
                if (s.SoundReady) p.Clock = (double)s.Played.load() / (double)s.Info.SampleRate;
                else p.Clock += (double)unscaledDt * std::max(vp.PlaybackSpeed, 0.0f);
                // The newest picture due by now.
                bool got = false;
                Stream::Frame frame;
                {
                    std::lock_guard lock(s.Mutex);
                    while (!s.Frames.empty() && s.Frames.front().Time <= p.Clock + 1e-4)
                    {
                        frame = std::move(s.Frames.front());
                        s.Frames.pop_front();
                        got = true;
                    }
                }
                if (got && p.Texture.IsValid())
                {
                    AssetManager::SetRuntimeTexturePixels(p.Texture, frame.Pixels.data(), frame.Pixels.size());
                    ++p.Shown;
                }
                // The end (not looping): every picture shown and the sound played out.
                bool framesLeft;
                {
                    std::lock_guard lock(s.Mutex);
                    framesLeft = !s.Frames.empty();
                }
                bool videoDone = !s.Info.HasVideo || (s.VideoEnded && !framesLeft);
                bool audioDone = !s.SoundReady || (s.AudioEnded && ma_pcm_rb_available_read(&s.Ring) == 0);
                if (!vp.Loop && videoDone && audioDone)
                {
                    p.Playing = false;
                    Halt(p);
                }
            }
            // Players whose objects (or Video Player components) are gone.
            for (auto it = players.begin(); it != players.end();)
                if (!it->second.Seen)
                {
                    if (it->second.Texture.IsValid()) AssetManager::DestroyRuntimeTexture(it->second.Texture);
                    it = players.erase(it);
                }
                else
                    ++it;
        }

        void OnRuntimeStop(Scene& scene) { s_Players.erase(&scene); } // streams stop; runtime textures go with the scene

        bool Play(Scene& scene, Entity e)
        {
            auto* vp = scene.IsValid(e) ? scene.GetRegistry().TryGet<VideoPlayerComponent>(e) : nullptr;
            if (!vp || !vp->Clip.IsValid()) return false;
            Player& p = s_Players[&scene][KeyOf(e)];
            if (p.Paused && p.S && p.Clip == vp->Clip)
            {
                p.Paused = false;
                p.Playing = true;
                if (p.S->SoundReady) ma_sound_start(&p.S->Sound);
                return true;
            }
            if (p.Playing && p.S && p.Clip == vp->Clip) return true;
            Start(p, *vp);
            p.Bound = false;
            p.Paused = false;
            p.Playing = p.S != nullptr;
            return p.Playing;
        }

        void Pause(Scene& scene, Entity e)
        {
            Player* p = Find(scene, e);
            if (!p || !p->Playing) return;
            p->Paused = true;
            Halt(*p);
        }

        void Stop(Scene& scene, Entity e)
        {
            Player* p = Find(scene, e);
            if (!p) return;
            p->S.reset();
            p->Playing = p->Paused = false;
            p->Clock = 0.0;
        }

        bool IsPlaying(Scene& scene, Entity e)
        {
            Player* p = Find(scene, e);
            return p && p->Playing && !p->Paused;
        }
        bool IsPaused(Scene& scene, Entity e)
        {
            Player* p = Find(scene, e);
            return p && p->Paused;
        }
        bool IsPrepared(Scene& scene, Entity e)
        {
            Player* p = Find(scene, e);
            return p && p->Bound;
        }
        double GetTime(Scene& scene, Entity e)
        {
            Player* p = Find(scene, e);
            if (!p) return 0.0;
            return p->Length > 0.0 ? std::fmod(p->Clock, p->Length) : p->Clock;
        }
        double GetLength(Scene& scene, Entity e)
        {
            Player* p = Find(scene, e);
            return p ? p->Length : 0.0;
        }
        glm::uvec2 GetSize(Scene& scene, Entity e)
        {
            Player* p = Find(scene, e);
            return p ? p->Size : glm::uvec2(0);
        }
        AssetHandle GetTexture(Scene& scene, Entity e)
        {
            Player* p = Find(scene, e);
            return p ? p->Texture : AssetHandle::None();
        }
        uint64_t GetFrameCount(Scene& scene, Entity e)
        {
            Player* p = Find(scene, e);
            return p ? p->Shown : 0;
        }
    }
}
