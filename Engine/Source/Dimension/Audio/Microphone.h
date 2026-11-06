#pragma once
#include "Dimension/Core/Base.h"
#include <string>
#include <vector>

namespace Dimension
{
    // Microphone input (like Unity's Microphone): records from a capture device into a buffer of `seconds` (mono,
    // floats), optionally looping over it; scripts read the level (voice-activated actions), the samples, or save the
    // recording as a WAV file (which can then be played as an audio clip).
    class Microphone
    {
    public:
        static std::vector<std::string> Devices();
        // device: index into Devices(), -1 = the system default. sampleRate 0 = the device's own.
        static bool Start(int device = -1, float seconds = 10.0f, bool loop = true, uint32_t sampleRate = 0);
        static void Stop();
        static bool IsRecording();
        static uint32_t GetSampleRate();
        // Frames recorded so far (with Loop, the write position wraps at the buffer's length).
        static uint64_t GetPosition();
        static size_t GetBufferFrames();
        // Copies recorded samples in order (oldest first, at most the buffer's length); returns the count.
        static size_t GetSamples(std::vector<float>& out);
        // The samples recorded since `cursor` (a count of frames recorded, as GetPosition gives; it moves on to now).
        // Anything older than the buffer's length is gone: reading skips ahead to what is still there.
        static size_t ReadSince(uint64_t& cursor, std::vector<float>& out);
        // RMS of the last ~50 ms (0-1), and its peak.
        static float GetLevel();
        static float GetPeak();
        // The recording so far as 16-bit mono PCM.
        static bool SaveWav(const fs::path& path);
    };
}
