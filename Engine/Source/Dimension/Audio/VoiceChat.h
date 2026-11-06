#pragma once
#include "Dimension/Core/Base.h"
#include <memory>
#include <string>

namespace Dimension
{
    // Voice chat between two peers over UDP, with no server: what this side says (the microphone, or samples given to
    // Submit) goes out in 20 ms Opus packets to the other side, and what arrives is decoded (lost packets concealed)
    // into a short jitter buffer that the audio engine plays. A thread of its own does the capturing, coding and
    // networking, so it runs whatever the game does. Both sides run the same thing, each pointed at the other.
    class VoiceChat
    {
    public:
        static constexpr uint32_t kSampleRate = 48000;
        static constexpr uint32_t kFrameSamples = 960; // 20 ms

        struct Options
        {
            bool UseMicrophone = true; // capture the default microphone (else only what Submit gives)
            bool Playback = true;      // play what arrives (else ReadReceived takes it: tests, recording)
            int Bitrate = 24000;       // bits per second
        };
        struct Stats
        {
            uint64_t PacketsSent = 0, PacketsReceived = 0, PacketsLost = 0;
            float LocalLevel = 0.0f;  // RMS of what was last sent
            float RemoteLevel = 0.0f; // RMS of what was last received
        };

        VoiceChat();
        ~VoiceChat();
        VoiceChat(const VoiceChat&) = delete;
        VoiceChat& operator=(const VoiceChat&) = delete;

        // Listens on localPort and talks to remoteHost:remotePort (a name or address). False when the port cannot be
        // opened or the host found.
        bool Start(uint16_t localPort, const std::string& remoteHost, uint16_t remotePort, const Options& options);
        bool Start(uint16_t localPort, const std::string& remoteHost, uint16_t remotePort) { return Start(localPort, remoteHost, remotePort, Options{}); }
        void Stop();
        bool IsRunning() const;

        // Push to talk: nothing is sent while off (on by default).
        void SetTransmitting(bool transmitting);
        bool IsTransmitting() const;
        // Voice activation: 20 ms frames quieter than this RMS are not sent (0 = send everything).
        void SetActivationThreshold(float rms);
        float GetActivationThreshold() const;
        void SetVolume(float volume); // what arrives
        float GetVolume() const;

        // Mono samples at kSampleRate to send, as if the microphone had picked them up.
        void Submit(const float* samples, size_t count);
        // Without Playback: takes up to `count` received samples (fewer when not that many have arrived).
        size_t ReadReceived(float* out, size_t count);
        Stats GetStats() const;

        // The one scripts use (C# VoiceChat).
        static VoiceChat& Default();

    private:
        struct State;
        std::unique_ptr<State> m_State;
    };
}
