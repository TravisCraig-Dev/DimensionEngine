#pragma once
#include "Dimension/Core/Base.h"
#include <memory>
#include <vector>

namespace Dimension
{
    struct VideoInfo
    {
        uint32_t Width = 0, Height = 0; // the picture shown (the decoder's padding cropped off)
        double Duration = 0.0;          // seconds
        double FrameRate = 0.0;
        bool HasVideo = false, HasAudio = false;
        uint32_t SampleRate = 0, Channels = 0; // its sound, as decoded (32-bit float, interleaved)
    };

    // Decodes a video file (MP4 / H.264, MOV, WMV, AVI... whatever Windows Media Foundation reads): its pictures as
    // RGBA8 and its sound as float samples, each read in order from its own reader so the two can run ahead apart.
    // Not thread-safe: one thread at a time (the Video Player's decoding thread).
    class VideoDecoder
    {
    public:
        static std::unique_ptr<VideoDecoder> Open(const fs::path& file);
        ~VideoDecoder();
        VideoDecoder(const VideoDecoder&) = delete;
        VideoDecoder& operator=(const VideoDecoder&) = delete;

        const VideoInfo& Info() const { return m_Info; }
        // The next picture: when it shows (seconds from the start) and its pixels (rows from the top, R G B A bytes).
        // False at the end.
        bool NextFrame(double& time, std::vector<uint32_t>& rgba);
        // The next run of sound: when it starts and its samples (Channels interleaved). False at the end.
        bool NextAudio(double& time, std::vector<float>& samples);
        // Back to the start (each stream on its own).
        void RewindVideo();
        void RewindAudio();
        // To a time (seconds): reading goes on from the key picture at or before it (the pictures up to it come first)
        // and from about that time in the sound.
        void SeekVideo(double seconds);
        void SeekAudio(double seconds);

        // What a file holds without decoding it (Inspector, tests).
        static bool Probe(const fs::path& file, VideoInfo& info);

    private:
        VideoDecoder() = default;
        struct Native;
        std::unique_ptr<Native> m_Native;
        VideoInfo m_Info;
    };
}
