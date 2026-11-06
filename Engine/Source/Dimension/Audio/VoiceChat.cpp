#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#endif
#include "Dimension/Audio/VoiceChat.h"
#include "Dimension/Audio/AudioEngine.h"
#include "Dimension/Audio/Microphone.h"
#include "Dimension/Core/Log.h"
#define MA_NO_ENCODING // as where the implementation is built (AudioEngine.cpp)
#include <miniaudio.h>
#include <opus.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace Dimension
{
    namespace
    {
#ifdef _WIN32
        using Socket = SOCKET;
        constexpr Socket kNoSocket = INVALID_SOCKET;
        bool StartSockets()
        {
            static std::once_flag once;
            static bool ok = false;
            std::call_once(once, [] {
                WSADATA data;
                ok = WSAStartup(MAKEWORD(2, 2), &data) == 0;
            });
            return ok;
        }
        void CloseSocket(Socket s) { closesocket(s); }
#else
        using Socket = int;
        constexpr Socket kNoSocket = -1;
        void CloseSocket(Socket) {}
#endif
        constexpr uint8_t kMagic0 = 'D', kMagic1 = 'V'; // packet: magic, sequence (16 bits), Opus frame

        struct ReceiveSource
        {
            ma_data_source_base Base;
            struct VoiceRing* Ring = nullptr;
        };

        // Decoded speech waiting to be played (or read): a mono float ring the networking thread writes.
        struct VoiceRing
        {
            ma_pcm_rb Ring{};
            std::atomic<float> Volume{ 1.0f };
        };

        ma_result SourceRead(ma_data_source* ds, void* out, ma_uint64 frameCount, ma_uint64* framesRead)
        {
            VoiceRing& v = *reinterpret_cast<ReceiveSource*>(ds)->Ring;
            auto* dst = static_cast<float*>(out);
            ma_uint64 done = 0;
            while (done < frameCount)
            {
                ma_uint32 n = (ma_uint32)std::min<ma_uint64>(frameCount - done, 1u << 20);
                void* buffer = nullptr;
                if (ma_pcm_rb_acquire_read(&v.Ring, &n, &buffer) != MA_SUCCESS || n == 0) break;
                float volume = v.Volume.load();
                const float* src = static_cast<const float*>(buffer);
                for (ma_uint32 i = 0; i < n; ++i) dst[done + i] = src[i] * volume;
                ma_pcm_rb_commit_read(&v.Ring, n);
                done += n;
            }
            if (done < frameCount) std::memset(dst + done, 0, (size_t)(frameCount - done) * sizeof(float)); // nothing yet: silence
            if (framesRead) *framesRead = frameCount;
            return MA_SUCCESS;
        }
        ma_result SourceSeek(ma_data_source*, ma_uint64) { return MA_NOT_IMPLEMENTED; }
        ma_result SourceFormat(ma_data_source*, ma_format* format, ma_uint32* channels, ma_uint32* rate, ma_channel* map, size_t mapCap)
        {
            *format = ma_format_f32;
            *channels = 1;
            *rate = VoiceChat::kSampleRate;
            if (map) ma_channel_map_init_standard(ma_standard_channel_map_default, map, mapCap, 1);
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
        ma_data_source_vtable s_VTable = { SourceRead, SourceSeek, SourceFormat, SourceCursor, SourceLength, nullptr, 0 };

        float Rms(const float* s, size_t n)
        {
            double sum = 0.0;
            for (size_t i = 0; i < n; ++i) sum += (double)s[i] * s[i];
            return n ? (float)std::sqrt(sum / (double)n) : 0.0f;
        }
    }

    struct VoiceChat::State
    {
        Options Opts;
        std::thread Thread;
        std::atomic<bool> Quit{ false }, Running{ false }, Transmitting{ true };
        std::atomic<float> Threshold{ 0.0f };
        Socket Sock = kNoSocket;
        sockaddr_in Remote{};
        OpusEncoder* Encoder = nullptr;
        OpusDecoder* Decoder = nullptr;
        VoiceRing Received;
        bool RingReady = false;
        ReceiveSource Source;
        ma_sound Sound{};
        bool SoundReady = false;
        // What to send: the microphone's new samples and anything submitted, gathered into 20 ms frames.
        std::mutex SubmitMutex;
        std::vector<float> Submitted;
        std::vector<float> Pending;
        uint64_t MicCursor = 0;
        bool StartedMicrophone = false;
        uint16_t SendSequence = 0;
        int32_t LastReceived = -1;
        std::atomic<uint64_t> Sent{ 0 }, Received_{ 0 }, Lost{ 0 };
        std::atomic<float> LocalLevel{ 0.0f }, RemoteLevel{ 0.0f };

        void Close()
        {
            Quit = true;
            if (Thread.joinable()) Thread.join();
            if (SoundReady)
            {
                ma_sound_uninit(&Sound);
                ma_data_source_uninit(&Source.Base);
                SoundReady = false;
            }
            if (RingReady)
            {
                ma_pcm_rb_uninit(&Received.Ring);
                RingReady = false;
            }
            if (Encoder) opus_encoder_destroy(Encoder);
            if (Decoder) opus_decoder_destroy(Decoder);
            Encoder = nullptr;
            Decoder = nullptr;
            if (Sock != kNoSocket) CloseSocket(Sock);
            Sock = kNoSocket;
            if (StartedMicrophone) Microphone::Stop();
            StartedMicrophone = false;
            Running = false;
        }

        void Deliver(const float* samples, size_t count)
        {
            RemoteLevel = Rms(samples, count);
            size_t at = 0;
            while (at < count)
            {
                ma_uint32 n = (ma_uint32)(count - at);
                void* buffer = nullptr;
                if (ma_pcm_rb_acquire_write(&Received.Ring, &n, &buffer) != MA_SUCCESS || n == 0) break; // full: the rest is dropped
                std::memcpy(buffer, samples + at, (size_t)n * sizeof(float));
                ma_pcm_rb_commit_write(&Received.Ring, n);
                at += n;
            }
        }

        void Loop()
        {
            std::vector<float> fresh;
            std::vector<unsigned char> packet(4000);
            float decoded[kFrameSamples * 6];
            uint32_t micRate = Microphone::IsRecording() ? Microphone::GetSampleRate() : kSampleRate;
            while (!Quit)
            {
                bool busy = false;
                // Gather what to send.
                if (Opts.UseMicrophone && Microphone::IsRecording() && Microphone::ReadSince(MicCursor, fresh) > 0)
                {
                    micRate = Microphone::GetSampleRate();
                    if (micRate == kSampleRate) Pending.insert(Pending.end(), fresh.begin(), fresh.end());
                    else if (micRate > 0)
                    {
                        // Another rate: resampled linearly to 48 kHz.
                        double step = (double)micRate / kSampleRate;
                        for (double t = 0.0; t < (double)fresh.size() - 1.0; t += step)
                        {
                            size_t i = (size_t)t;
                            float f = (float)(t - (double)i);
                            Pending.push_back(fresh[i] + (fresh[i + 1] - fresh[i]) * f);
                        }
                    }
                }
                {
                    std::lock_guard lock(SubmitMutex);
                    if (!Submitted.empty())
                    {
                        Pending.insert(Pending.end(), Submitted.begin(), Submitted.end());
                        Submitted.clear();
                    }
                }
                // Encode and send whole frames.
                size_t at = 0;
                while (Pending.size() - at >= kFrameSamples)
                {
                    const float* frame = Pending.data() + at;
                    at += kFrameSamples;
                    float level = Rms(frame, kFrameSamples);
                    LocalLevel = level;
                    if (!Transmitting || level < Threshold.load()) continue;
                    int bytes = opus_encode_float(Encoder, frame, (int)kFrameSamples, packet.data() + 4, (opus_int32)packet.size() - 4);
                    if (bytes <= 0) continue;
                    packet[0] = kMagic0;
                    packet[1] = kMagic1;
                    packet[2] = (unsigned char)(SendSequence >> 8);
                    packet[3] = (unsigned char)(SendSequence & 255);
                    ++SendSequence;
                    if (sendto(Sock, (const char*)packet.data(), bytes + 4, 0, (const sockaddr*)&Remote, sizeof(Remote)) > 0) ++Sent;
                    busy = true;
                }
                if (at) Pending.erase(Pending.begin(), Pending.begin() + (ptrdiff_t)at);
                // Receive everything waiting.
                for (;;)
                {
                    unsigned char in[4000];
                    int got = recvfrom(Sock, (char*)in, (int)sizeof(in), 0, nullptr, nullptr);
                    if (got <= 4) break;
                    if (in[0] != kMagic0 || in[1] != kMagic1) continue;
                    busy = true;
                    uint16_t seq = (uint16_t)(in[2] << 8 | in[3]);
                    if (LastReceived >= 0)
                    {
                        uint16_t expected = (uint16_t)(LastReceived + 1);
                        int16_t gap = (int16_t)(seq - expected);
                        if (gap < 0) continue; // late or repeated: dropped
                        // Lost frames (up to 3) filled in by the decoder's concealment.
                        for (int k = 0; k < std::min<int>(gap, 3); ++k)
                        {
                            int n = opus_decode_float(Decoder, nullptr, 0, decoded, (int)kFrameSamples, 0);
                            if (n > 0) Deliver(decoded, (size_t)n);
                        }
                        Lost += (uint64_t)gap;
                    }
                    LastReceived = seq;
                    int n = opus_decode_float(Decoder, in + 4, got - 4, decoded, (int)(sizeof(decoded) / sizeof(float)), 0);
                    if (n > 0)
                    {
                        Deliver(decoded, (size_t)n);
                        ++Received_;
                    }
                }
                if (!busy) std::this_thread::sleep_for(std::chrono::milliseconds(4));
            }
        }
    };

    VoiceChat::VoiceChat() : m_State(std::make_unique<State>()) {}
    VoiceChat::~VoiceChat() { Stop(); }

    VoiceChat& VoiceChat::Default()
    {
        static VoiceChat* chat = new VoiceChat(); // never destroyed: AudioEngine::Shutdown stops it
        return *chat;
    }

    bool VoiceChat::Start(uint16_t localPort, const std::string& remoteHost, uint16_t remotePort, const Options& options)
    {
        Stop();
        m_State = std::make_unique<State>();
        State& s = *m_State;
        s.Opts = options;
#ifdef _WIN32
        if (!StartSockets()) return false;
        addrinfo hints{}, *found = nullptr;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        if (getaddrinfo(remoteHost.c_str(), std::to_string(remotePort).c_str(), &hints, &found) != 0 || !found)
        {
            Log::Error("Voice chat: cannot find '{}'", remoteHost);
            return false;
        }
        std::memcpy(&s.Remote, found->ai_addr, sizeof(s.Remote));
        freeaddrinfo(found);
        s.Sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s.Sock == kNoSocket) return false;
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl(INADDR_ANY);
        local.sin_port = htons(localPort);
        if (bind(s.Sock, (const sockaddr*)&local, sizeof(local)) != 0)
        {
            Log::Error("Voice chat: port {} is taken", localPort);
            s.Close();
            return false;
        }
        u_long nonBlocking = 1;
        ioctlsocket(s.Sock, FIONBIO, &nonBlocking);
        // A peer not listening yet must not make the next receive fail (Windows reports its "port unreachable" there).
        BOOL report = FALSE;
        DWORD returned = 0;
        WSAIoctl(s.Sock, _WSAIOW(IOC_VENDOR, 12) /* SIO_UDP_CONNRESET */, &report, sizeof(report), nullptr, 0, &returned, nullptr, nullptr);
#else
        return false;
#endif
        int error = 0;
        s.Encoder = opus_encoder_create((opus_int32)kSampleRate, 1, OPUS_APPLICATION_VOIP, &error);
        s.Decoder = opus_decoder_create((opus_int32)kSampleRate, 1, &error);
        if (!s.Encoder || !s.Decoder)
        {
            s.Close();
            return false;
        }
        opus_encoder_ctl(s.Encoder, OPUS_SET_BITRATE(std::clamp(options.Bitrate, 6000, 128000)));
        s.RingReady = ma_pcm_rb_init(ma_format_f32, 1, kSampleRate / 2, nullptr, nullptr, &s.Received.Ring) == MA_SUCCESS;
        if (!s.RingReady)
        {
            s.Close();
            return false;
        }
        if (options.UseMicrophone)
        {
            if (!Microphone::IsRecording()) s.StartedMicrophone = Microphone::Start(-1, 1.0f, true, kSampleRate);
            s.MicCursor = Microphone::GetPosition();
            if (!Microphone::IsRecording()) Log::Warn("Voice chat: no microphone (only what scripts submit is sent)");
        }
        if (options.Playback && AudioEngine::IsAvailable())
        {
            s.Source.Ring = &s.Received;
            ma_data_source_config config = ma_data_source_config_init();
            config.vtable = &s_VTable;
            if (ma_data_source_init(&config, &s.Source.Base) == MA_SUCCESS)
            {
                if (ma_sound_init_from_data_source(static_cast<ma_engine*>(AudioEngine::GetNative()), &s.Source.Base, MA_SOUND_FLAG_NO_SPATIALIZATION,
                                                   nullptr, &s.Sound) == MA_SUCCESS)
                {
                    s.SoundReady = true;
                    ma_sound_start(&s.Sound);
                }
                else
                    ma_data_source_uninit(&s.Source.Base);
            }
        }
        s.Running = true;
        State* raw = &s;
        s.Thread = std::thread([raw] { raw->Loop(); });
        Log::Info("Voice chat: port {} talking to {}:{}", localPort, remoteHost, remotePort);
        return true;
    }

    void VoiceChat::Stop()
    {
        if (m_State) m_State->Close();
    }

    bool VoiceChat::IsRunning() const { return m_State && m_State->Running; }
    void VoiceChat::SetTransmitting(bool transmitting) { m_State->Transmitting = transmitting; }
    bool VoiceChat::IsTransmitting() const { return m_State->Transmitting; }
    void VoiceChat::SetActivationThreshold(float rms) { m_State->Threshold = std::max(rms, 0.0f); }
    float VoiceChat::GetActivationThreshold() const { return m_State->Threshold; }
    void VoiceChat::SetVolume(float volume) { m_State->Received.Volume = std::max(volume, 0.0f); }
    float VoiceChat::GetVolume() const { return m_State->Received.Volume; }

    void VoiceChat::Submit(const float* samples, size_t count)
    {
        if (!samples || !count) return;
        std::lock_guard lock(m_State->SubmitMutex);
        m_State->Submitted.insert(m_State->Submitted.end(), samples, samples + count);
    }

    size_t VoiceChat::ReadReceived(float* out, size_t count)
    {
        State& s = *m_State;
        if (!s.RingReady || s.SoundReady) return 0;
        size_t done = 0;
        while (done < count)
        {
            ma_uint32 n = (ma_uint32)(count - done);
            void* buffer = nullptr;
            if (ma_pcm_rb_acquire_read(&s.Received.Ring, &n, &buffer) != MA_SUCCESS || n == 0) break;
            std::memcpy(out + done, buffer, (size_t)n * sizeof(float));
            ma_pcm_rb_commit_read(&s.Received.Ring, n);
            done += n;
        }
        return done;
    }

    VoiceChat::Stats VoiceChat::GetStats() const
    {
        const State& s = *m_State;
        Stats st;
        st.PacketsSent = s.Sent;
        st.PacketsReceived = s.Received_;
        st.PacketsLost = s.Lost;
        st.LocalLevel = s.LocalLevel;
        st.RemoteLevel = s.RemoteLevel;
        return st;
    }
}
