#include "Dimension/Audio/Microphone.h"
#include "Dimension/Core/Log.h"
#define MA_NO_ENCODING // as where the implementation is built (AudioEngine.cpp)
#include <miniaudio.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>

namespace Dimension
{
    namespace
    {
        struct Recorder
        {
            ma_device Device;
            bool DeviceValid = false;
            std::vector<float> Buffer; // mono ring
            std::atomic<uint64_t> Written{ 0 };
            bool Loop = true;
            uint32_t SampleRate = 48000;
            std::atomic<float> Level{ 0.0f }, Peak{ 0.0f };
        };
        std::unique_ptr<Recorder> s_Recorder;
        std::mutex s_Mutex;

        void OnCapture(ma_device* device, void* /*output*/, const void* input, ma_uint32 frames)
        {
            auto* r = static_cast<Recorder*>(device->pUserData);
            const float* in = static_cast<const float*>(input);
            if (!r || !in || r->Buffer.empty()) return;
            uint64_t at = r->Written.load(std::memory_order_relaxed);
            size_t size = r->Buffer.size();
            double sum = 0.0;
            float peak = 0.0f;
            for (ma_uint32 i = 0; i < frames; ++i)
            {
                float v = in[i];
                sum += (double)v * v;
                peak = std::max(peak, std::abs(v));
                if (!r->Loop && at >= size) continue; // full: stop storing (still metering)
                r->Buffer[(size_t)(at % size)] = v;
                ++at;
            }
            r->Written.store(at, std::memory_order_release);
            // Level: RMS smoothed over ~50 ms.
            float rms = frames ? (float)std::sqrt(sum / frames) : 0.0f;
            float a = 1.0f - std::exp(-(float)frames / (0.05f * (float)r->SampleRate));
            r->Level.store(r->Level.load(std::memory_order_relaxed) + (rms - r->Level.load(std::memory_order_relaxed)) * a, std::memory_order_relaxed);
            r->Peak.store(peak, std::memory_order_relaxed);
        }
    }

    std::vector<std::string> Microphone::Devices()
    {
        std::vector<std::string> names;
        ma_context context;
        if (ma_context_init(nullptr, 0, nullptr, &context) != MA_SUCCESS) return names;
        ma_device_info* playback = nullptr;
        ma_device_info* capture = nullptr;
        ma_uint32 playbackCount = 0, captureCount = 0;
        if (ma_context_get_devices(&context, &playback, &playbackCount, &capture, &captureCount) == MA_SUCCESS)
            for (ma_uint32 i = 0; i < captureCount; ++i) names.push_back(capture[i].name);
        ma_context_uninit(&context);
        return names;
    }

    bool Microphone::Start(int device, float seconds, bool loop, uint32_t sampleRate)
    {
        Stop();
        std::lock_guard lock(s_Mutex);
        auto r = std::make_unique<Recorder>();
        ma_context context;
        bool haveContext = ma_context_init(nullptr, 0, nullptr, &context) == MA_SUCCESS;
        ma_device_id id{};
        bool useId = false;
        if (haveContext && device >= 0)
        {
            ma_device_info* playback = nullptr;
            ma_device_info* capture = nullptr;
            ma_uint32 playbackCount = 0, captureCount = 0;
            if (ma_context_get_devices(&context, &playback, &playbackCount, &capture, &captureCount) == MA_SUCCESS && (ma_uint32)device < captureCount)
            {
                id = capture[device].id;
                useId = true;
            }
        }
        ma_device_config config = ma_device_config_init(ma_device_type_capture);
        config.capture.format = ma_format_f32;
        config.capture.channels = 1;
        config.capture.pDeviceID = useId ? &id : nullptr;
        config.sampleRate = sampleRate; // 0 = the device's
        config.dataCallback = OnCapture;
        config.pUserData = r.get();
        bool ok = ma_device_init(nullptr, &config, &r->Device) == MA_SUCCESS;
        if (haveContext) ma_context_uninit(&context);
        if (!ok)
        {
            Log::Warn("Microphone: no capture device available");
            return false;
        }
        r->DeviceValid = true;
        r->SampleRate = r->Device.sampleRate;
        r->Loop = loop;
        r->Buffer.assign((size_t)std::max(1.0f, seconds * (float)r->SampleRate), 0.0f);
        if (ma_device_start(&r->Device) != MA_SUCCESS)
        {
            ma_device_uninit(&r->Device);
            Log::Warn("Microphone: the capture device would not start");
            return false;
        }
        Log::Info("Microphone recording at {} Hz ({:.1f} s{})", r->SampleRate, seconds, loop ? ", looping" : "");
        s_Recorder = std::move(r);
        return true;
    }

    void Microphone::Stop()
    {
        std::lock_guard lock(s_Mutex);
        if (s_Recorder && s_Recorder->DeviceValid)
        {
            ma_device_uninit(&s_Recorder->Device); // stops and waits for the callback
            s_Recorder->DeviceValid = false;
        }
    }

    bool Microphone::IsRecording() { return s_Recorder && s_Recorder->DeviceValid; }
    uint32_t Microphone::GetSampleRate() { return s_Recorder ? s_Recorder->SampleRate : 0; }
    uint64_t Microphone::GetPosition() { return s_Recorder ? s_Recorder->Written.load(std::memory_order_acquire) : 0; }
    size_t Microphone::GetBufferFrames() { return s_Recorder ? s_Recorder->Buffer.size() : 0; }
    float Microphone::GetLevel() { return s_Recorder ? s_Recorder->Level.load(std::memory_order_relaxed) : 0.0f; }
    float Microphone::GetPeak() { return s_Recorder ? s_Recorder->Peak.load(std::memory_order_relaxed) : 0.0f; }

    size_t Microphone::GetSamples(std::vector<float>& out)
    {
        out.clear();
        if (!s_Recorder) return 0;
        const auto& buf = s_Recorder->Buffer;
        uint64_t written = s_Recorder->Written.load(std::memory_order_acquire);
        size_t size = buf.size(), count = (size_t)std::min<uint64_t>(written, size);
        out.resize(count);
        size_t start = written > size ? (size_t)(written % size) : 0; // oldest sample when the ring has wrapped
        for (size_t i = 0; i < count; ++i) out[i] = buf[(start + i) % size];
        return count;
    }

    size_t Microphone::ReadSince(uint64_t& cursor, std::vector<float>& out)
    {
        out.clear();
        if (!s_Recorder) return 0;
        const auto& buf = s_Recorder->Buffer;
        uint64_t written = s_Recorder->Written.load(std::memory_order_acquire);
        size_t size = buf.size();
        if (size == 0 || cursor >= written)
        {
            cursor = std::min(cursor, written);
            return 0;
        }
        if (written - cursor > size) cursor = written - size; // overwritten since: from the oldest still kept
        out.resize((size_t)(written - cursor));
        for (size_t i = 0; i < out.size(); ++i) out[i] = buf[(size_t)((cursor + i) % size)];
        cursor = written;
        return out.size();
    }

    bool Microphone::SaveWav(const fs::path& path)
    {
        std::vector<float> samples;
        GetSamples(samples);
        uint32_t rate = GetSampleRate();
        if (samples.empty() || rate == 0) return false;
        std::ofstream out(path, std::ios::binary);
        if (!out) return false;
        auto u32 = [&](uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
        auto u16 = [&](uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
        uint32_t dataBytes = (uint32_t)samples.size() * 2;
        out.write("RIFF", 4);
        u32(36 + dataBytes);
        out.write("WAVEfmt ", 8);
        u32(16);
        u16(1); // PCM
        u16(1); // mono
        u32(rate);
        u32(rate * 2);
        u16(2);
        u16(16);
        out.write("data", 4);
        u32(dataBytes);
        for (float s : samples) u16((uint16_t)(int16_t)std::lround(std::clamp(s, -1.0f, 1.0f) * 32767.0f));
        return (bool)out;
    }
}
