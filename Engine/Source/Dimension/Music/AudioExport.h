#pragma once
#include "Dimension/Core/Base.h"
#include <vector>

// Writing rendered songs: Ogg Opus files (WAV is WriteWav in SongPlayer.h) and peak normalizing.
namespace Dimension::Music
{
    // Ogg Opus (.opus) of interleaved stereo at 48 kHz (Opus's rate), at `kbps` (6..510). The engine plays these
    // like any other clip.
    bool WriteOpus(const fs::path& path, const std::vector<float>& stereo, uint32_t sampleRate, int kbps = 160);
    // The loudest sample (0..), and scaling so it reaches `peakDb` (e.g. -1 dB); silence is left alone.
    float PeakOf(const std::vector<float>& samples);
    void Normalize(std::vector<float>& samples, float peakDb = -1.0f);
}
