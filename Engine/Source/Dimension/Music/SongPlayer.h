#pragma once
#include "Dimension/Music/Song.h"
#include "Dimension/Music/Effects.h"
#include "Dimension/Music/Plugin.h"
#include <array>
#include <atomic>
#include <memory>
#include <mutex>

namespace Dimension::Music
{
    // A decoded audio file for the Sampler: interleaved stereo floats at the file's own rate.
    struct SampleData
    {
        std::vector<float> Frames; // L, R, L, R...
        uint32_t SampleRate = 0;
        size_t FrameCount() const { return Frames.size() / 2; }
    };
    // Decoded once and shared (WAV, MP3, FLAC, Ogg Vorbis); null when the file cannot be read. Thread-safe.
    std::shared_ptr<const SampleData> LoadSample(const fs::path& file);
    // The Sampler's file: its GUID through the asset database, else its path (Assets-relative or absolute).
    fs::path ResolveSamplePath(const SamplerParams& s);

    // Plays a song: sequences its notes (Pattern mode loops one pattern, Song mode plays the Playlist) and synthesizes
    // the channels' instruments. Render runs on the audio thread (or offline, for rendering to a file); the rest can
    // be called from any thread.
    class SongPlayer
    {
    public:
        enum class Mode { Pattern, Song };
        static constexpr int kMaxVoices = 96;
        static constexpr int kMaxMeters = 64;  // channels
        static constexpr int kMaxMixer = 32;   // mixer tracks (the Master and 31 inserts)
        static constexpr uint32_t kChunk = 64; // frames between automation updates

        explicit SongPlayer(uint32_t sampleRate = 48000);
        ~SongPlayer();
        uint32_t GetSampleRate() const { return m_SampleRate; }

        // A copy of the song is played (call again after each edit); samples are loaded here, not on the audio thread.
        void SetSong(const Song& song);
        // Where the song's plug-ins live (instances by their references' ids). Set before SetSong; without one, plug-in
        // channels and effects are silent / pass through.
        void SetRack(std::shared_ptr<PluginRack> rack);
        void SetMode(Mode mode);
        Mode GetMode() const { return m_Mode; }
        void SetPattern(int pattern);
        int GetPattern() const { return m_Pattern; }

        void Play();
        void Pause();               // stops where it is
        // A click on each beat while playing (higher on the bar's first): for recording; never in renders.
        void SetMetronome(bool on) { m_Metronome = on; }
        bool GetMetronome() const { return m_Metronome; }
        void Stop();                // back to the start; notes released
        bool IsPlaying() const { return m_Playing; }
        double GetTick() const { return m_Tick; }
        void SetTick(double tick);

        // Notes outside the sequence (auditioning a channel, typing keyboard, MIDI): held until NoteOff.
        void NoteOn(int channel, int key, float velocity);
        void NoteOff(int channel, int key);
        void AllNotesOff();

        // Fills `frames` interleaved stereo frames (and moves the song on while playing).
        void Render(float* out, uint32_t frames);

        // Peak levels since the last call (0..1+): a channel's (both sides), the master's left and right.
        float TakeChannelPeak(int channel);
        std::array<float, 2> TakeMasterPeak();
        float TakeMixerPeak(int track); // after its level and pan
        int ActiveVoices() const;

        struct Voice;
    private:
        struct Event
        {
            uint32_t Frame;
            int Channel, Key;
            float Velocity;
            double LengthFrames;
        };
        void Trigger(int channel, int key, float velocity, double lengthFrames);
        void Collect(double from, uint32_t frameBase, uint32_t count, double framesPerTick, std::vector<Event>& events) const;
        double LoopEnd() const;
        float RenderVoice(Voice& v, float& left, float& right);
        void RenderChunk(float* out, uint32_t frames);
        void PluginNote(int channel, int key, float velocity, bool on, int frame);
        void ApplyAutomation(double tick);

        uint32_t m_SampleRate;
        mutable std::mutex m_Mutex;
        Song m_Song;
        std::vector<std::shared_ptr<const SampleData>> m_Samples; // per channel
        Mode m_Mode = Mode::Pattern;
        int m_Pattern = 0;
        std::atomic<bool> m_Playing{ false };
        std::atomic<double> m_Tick{ 0.0 };
        std::unique_ptr<Voice[]> m_Voices;
        uint64_t m_VoiceAge = 0;
        uint32_t m_NoiseSeed = 0x12345678u;
        std::vector<Event> m_Events; // (reused each block)
        std::array<std::atomic<float>, kMaxMeters> m_ChannelPeaks{};
        std::atomic<float> m_PeakL{ 0.0f }, m_PeakR{ 0.0f };
        std::array<std::atomic<float>, kMaxMixer> m_MixerPeaks{};
        std::vector<std::vector<std::unique_ptr<EffectProcessor>>> m_Fx; // [track][slot]
        std::vector<float> m_Bus;                                         // [track] kChunk stereo frames each
        // Plug-ins: the rack, each channel's instrument and each effect slot's (null where none), notes waiting for
        // their ends, and scratch buffers.
        std::shared_ptr<PluginRack> m_Rack;
        std::vector<std::shared_ptr<PluginInstance>> m_ChannelPlugins;
        std::vector<std::vector<std::shared_ptr<PluginInstance>>> m_EffectPlugins;
        struct PluginNoteOff
        {
            int Channel, Key;
            double Frames; // until it ends
        };
        std::vector<PluginNoteOff> m_PluginNoteOffs;
        std::vector<float> m_PluginBuffer, m_PluginDry;
        int64_t m_SongSamples = 0;
        // Audio clips' files (by clip), and the metronome's click.
        std::vector<std::shared_ptr<const SampleData>> m_ClipSamples;
        std::atomic<bool> m_Metronome{ false };
        double m_LastBeat = -1.0;
        float m_ClickTime = -1.0f, m_ClickFreq = 1000.0f;
    };

    // Renders a song offline: Pattern mode plays `pattern` `loops` times, Song mode the Playlist once; then `tail`
    // seconds for the sounds to ring out. Interleaved stereo at `sampleRate`.
    std::vector<float> RenderSong(const Song& song, uint32_t sampleRate, SongPlayer::Mode mode, int pattern = 0, int loops = 1, float tail = 1.5f);
    // 16-bit PCM WAV of interleaved stereo frames (clipped to -1..1).
    bool WriteWav(const fs::path& path, const std::vector<float>& stereo, uint32_t sampleRate);

    // The sound card for Music Mode: plays a SongPlayer (one at a time) on its own output stream.
    class MusicOutput
    {
    public:
        static bool Start(std::shared_ptr<SongPlayer> player);
        static void Stop();
        static bool IsRunning();
    };
}
