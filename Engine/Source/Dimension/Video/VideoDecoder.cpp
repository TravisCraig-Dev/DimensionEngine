#include "Dimension/Video/VideoDecoder.h"
#include "Dimension/Core/Log.h"
#include <algorithm>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <mfreadwrite.h>
#include <mutex>
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")

namespace Dimension
{
    namespace
    {
        template <class T> void Release(T*& p)
        {
            if (p) p->Release();
            p = nullptr;
        }

        // Media Foundation for the whole process, started on first use (MFStartup counts; it is never shut down while
        // the engine runs). Each thread using it joins the multi-threaded COM apartment.
        bool StartMediaFoundation()
        {
            static std::once_flag once;
            static bool ok = false;
            std::call_once(once, [] { ok = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET)); });
            HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            (void)hr; // S_FALSE / RPC_E_CHANGED_MODE: already initialized on this thread, fine either way
            return ok;
        }

        IMFSourceReader* OpenReader(const fs::path& file, bool video)
        {
            IMFAttributes* attributes = nullptr;
            if (FAILED(MFCreateAttributes(&attributes, 2))) return nullptr;
            if (video) attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE); // decode and convert to RGB32
            IMFSourceReader* reader = nullptr;
            HRESULT hr = MFCreateSourceReaderFromURL(file.wstring().c_str(), attributes, &reader);
            Release(attributes);
            if (FAILED(hr)) return nullptr;
            DWORD stream = video ? (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM : (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM;
            reader->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
            if (FAILED(reader->SetStreamSelection(stream, TRUE)))
            {
                Release(reader);
                return nullptr;
            }
            IMFMediaType* type = nullptr;
            MFCreateMediaType(&type);
            type->SetGUID(MF_MT_MAJOR_TYPE, video ? MFMediaType_Video : MFMediaType_Audio);
            type->SetGUID(MF_MT_SUBTYPE, video ? MFVideoFormat_RGB32 : MFAudioFormat_Float);
            hr = reader->SetCurrentMediaType(stream, nullptr, type);
            Release(type);
            if (FAILED(hr))
            {
                Release(reader);
                return nullptr;
            }
            return reader;
        }

        void Rewind(IMFSourceReader* reader, double seconds = 0.0)
        {
            if (!reader) return;
            PROPVARIANT start;
            PropVariantInit(&start);
            start.vt = VT_I8;
            start.hVal.QuadPart = (LONGLONG)(std::max(seconds, 0.0) * 1e7);
            reader->SetCurrentPosition(GUID_NULL, start);
            PropVariantClear(&start);
        }
    }

    struct VideoDecoder::Native
    {
        IMFSourceReader* Video = nullptr;
        IMFSourceReader* Audio = nullptr;
        LONG Stride = 0;              // bytes per row of the decoded picture (negative: rows from the bottom)
        uint32_t FrameWidth = 0, FrameHeight = 0; // the decoded picture, padding included
        uint32_t OffsetX = 0, OffsetY = 0;        // where the shown part starts in it
        ~Native()
        {
            Release(Video);
            Release(Audio);
        }
    };

    VideoDecoder::~VideoDecoder() = default;

    std::unique_ptr<VideoDecoder> VideoDecoder::Open(const fs::path& file)
    {
        if (!StartMediaFoundation()) return nullptr;
        std::unique_ptr<VideoDecoder> d(new VideoDecoder());
        d->m_Native = std::make_unique<Native>();
        Native& n = *d->m_Native;
        VideoInfo& info = d->m_Info;
        n.Video = OpenReader(file, true);
        n.Audio = OpenReader(file, false);
        if (!n.Video && !n.Audio) return nullptr;
        IMFSourceReader* any = n.Video ? n.Video : n.Audio;
        PROPVARIANT duration;
        PropVariantInit(&duration);
        if (SUCCEEDED(any->GetPresentationAttribute((DWORD)MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &duration)) && duration.vt == VT_UI8)
            info.Duration = (double)duration.uhVal.QuadPart / 1e7;
        PropVariantClear(&duration);
        if (n.Video)
        {
            IMFMediaType* type = nullptr;
            if (SUCCEEDED(n.Video->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &type)))
            {
                UINT32 w = 0, h = 0, num = 0, den = 0;
                MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &w, &h);
                MFGetAttributeRatio(type, MF_MT_FRAME_RATE, &num, &den);
                n.FrameWidth = w;
                n.FrameHeight = h;
                UINT32 stride = 0;
                n.Stride = SUCCEEDED(type->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride)) ? (LONG)stride : (LONG)(w * 4);
                info.Width = w;
                info.Height = h;
                // Decoders pad the picture to whole macroblocks (1080 lines come out as 1088): the aperture is the part shown.
                MFVideoArea area{};
                if (SUCCEEDED(type->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, (UINT8*)&area, sizeof(area), nullptr)) && area.Area.cx > 0 && area.Area.cy > 0)
                {
                    n.OffsetX = (uint32_t)std::max<LONG>(area.OffsetX.value, 0);
                    n.OffsetY = (uint32_t)std::max<LONG>(area.OffsetY.value, 0);
                    info.Width = std::min<uint32_t>((uint32_t)area.Area.cx, w - n.OffsetX);
                    info.Height = std::min<uint32_t>((uint32_t)area.Area.cy, h - n.OffsetY);
                }
                info.FrameRate = den ? (double)num / den : 0.0;
                info.HasVideo = w > 0 && h > 0;
                Release(type);
            }
        }
        if (n.Audio)
        {
            IMFMediaType* type = nullptr;
            if (SUCCEEDED(n.Audio->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, &type)))
            {
                info.Channels = MFGetAttributeUINT32(type, MF_MT_AUDIO_NUM_CHANNELS, 0);
                info.SampleRate = MFGetAttributeUINT32(type, MF_MT_AUDIO_SAMPLES_PER_SECOND, 0);
                info.HasAudio = info.Channels > 0 && info.SampleRate > 0;
                Release(type);
            }
        }
        if (!info.HasVideo) Release(n.Video);
        if (!info.HasAudio) Release(n.Audio);
        if (!info.HasVideo && !info.HasAudio) return nullptr;
        return d;
    }

    bool VideoDecoder::Probe(const fs::path& file, VideoInfo& info)
    {
        auto d = Open(file);
        if (!d) return false;
        info = d->Info();
        return true;
    }

    bool VideoDecoder::NextFrame(double& time, std::vector<uint32_t>& rgba)
    {
        Native& n = *m_Native;
        if (!n.Video) return false;
        for (int attempts = 0; attempts < 64; ++attempts)
        {
            DWORD stream = 0, flags = 0;
            LONGLONG stamp = 0;
            IMFSample* sample = nullptr;
            if (FAILED(n.Video->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &stream, &flags, &stamp, &sample))) return false;
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
            {
                Release(sample);
                return false;
            }
            if (!sample) continue; // a gap: read on
            IMFMediaBuffer* buffer = nullptr;
            bool ok = false;
            if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buffer)))
            {
                BYTE* data = nullptr;
                DWORD length = 0;
                if (SUCCEEDED(buffer->Lock(&data, nullptr, &length)))
                {
                    LONG pitch = n.Stride;
                    size_t rowBytes = (size_t)std::abs(pitch);
                    if (rowBytes >= n.FrameWidth * 4ull && length >= rowBytes * n.FrameHeight)
                    {
                        // Bottom-up pictures (negative stride) start at their last row.
                        const BYTE* first = pitch < 0 ? data + rowBytes * (n.FrameHeight - 1) : data;
                        rgba.resize((size_t)m_Info.Width * m_Info.Height);
                        for (uint32_t y = 0; y < m_Info.Height; ++y)
                        {
                            const BYTE* row = first + (ptrdiff_t)pitch * (ptrdiff_t)(y + n.OffsetY) + n.OffsetX * 4;
                            uint32_t* out = rgba.data() + (size_t)y * m_Info.Width;
                            for (uint32_t x = 0; x < m_Info.Width; ++x)
                            {
                                const BYTE* p = row + x * 4; // B G R X
                                out[x] = (uint32_t)p[2] | (uint32_t)p[1] << 8 | (uint32_t)p[0] << 16 | 0xFF000000u;
                            }
                        }
                        ok = true;
                    }
                    buffer->Unlock();
                }
            }
            Release(buffer);
            Release(sample);
            if (ok)
            {
                time = (double)stamp / 1e7;
                return true;
            }
        }
        return false;
    }

    bool VideoDecoder::NextAudio(double& time, std::vector<float>& samples)
    {
        Native& n = *m_Native;
        if (!n.Audio) return false;
        for (int attempts = 0; attempts < 64; ++attempts)
        {
            DWORD stream = 0, flags = 0;
            LONGLONG stamp = 0;
            IMFSample* sample = nullptr;
            if (FAILED(n.Audio->ReadSample((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, &stream, &flags, &stamp, &sample))) return false;
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
            {
                Release(sample);
                return false;
            }
            if (!sample) continue;
            IMFMediaBuffer* buffer = nullptr;
            bool ok = false;
            if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buffer)))
            {
                BYTE* data = nullptr;
                DWORD length = 0;
                if (SUCCEEDED(buffer->Lock(&data, nullptr, &length)))
                {
                    samples.resize(length / sizeof(float));
                    std::memcpy(samples.data(), data, samples.size() * sizeof(float));
                    buffer->Unlock();
                    ok = !samples.empty();
                }
            }
            Release(buffer);
            Release(sample);
            if (ok)
            {
                time = (double)stamp / 1e7;
                return true;
            }
        }
        return false;
    }

    void VideoDecoder::RewindVideo() { Rewind(m_Native->Video); }
    void VideoDecoder::RewindAudio() { Rewind(m_Native->Audio); }
    void VideoDecoder::SeekVideo(double seconds) { Rewind(m_Native->Video, seconds); }
    void VideoDecoder::SeekAudio(double seconds) { Rewind(m_Native->Audio, seconds); }
}

#elif defined(DIM_FFMPEG)
#include "VideoDecoderFFmpeg.inl"
#else

// No decoder on this platform (Linux built without FFmpeg's development libraries): videos do not open.
namespace Dimension
{
    struct VideoDecoder::Native {};
    VideoDecoder::~VideoDecoder() = default;
    std::unique_ptr<VideoDecoder> VideoDecoder::Open(const fs::path&) { return nullptr; }
    bool VideoDecoder::Probe(const fs::path&, VideoInfo&) { return false; }
    bool VideoDecoder::NextFrame(double&, std::vector<uint32_t>&) { return false; }
    bool VideoDecoder::NextAudio(double&, std::vector<float>&) { return false; }
    void VideoDecoder::RewindVideo() {}
    void VideoDecoder::RewindAudio() {}
    void VideoDecoder::SeekVideo(double) {}
    void VideoDecoder::SeekAudio(double) {}
}

#endif
