#pragma once
#include <complex>
#include <cstddef>
#include <vector>

namespace Dimension
{
    // Convolution reverb: plays a signal through a recorded (or generated) impulse response, the "sound of a room".
    // Uniformly partitioned FFT convolution: the impulse response is cut into blocks whose spectra are multiplied with
    // the spectra of the last input blocks, so the cost grows with the response's length only linearly. Processing is
    // in fixed blocks, so the output lags the input by BlockSize frames. Interleaved float audio.
    class Convolver
    {
    public:
        // `impulse` is interleaved with `impulseChannels` channels (1 = the same response for every channel, otherwise
        // channel c uses impulse channel c % impulseChannels). It is normalized to unit energy, so Wet stays a level.
        void Init(const float* impulse, size_t impulseFrames, int impulseChannels, int channels, int blockSize = 1024);
        bool IsReady() const { return !m_Partitions.empty(); }
        int GetLatency() const { return m_Block; }
        // out = in * dry + convolved * wet (in and out may alias).
        void Process(const float* in, float* out, size_t frames, float wet, float dry = 1.0f);
        void Reset();

        // In-place iterative radix-2 FFT (size a power of two); inverse includes the 1/N scale.
        static void FFT(std::vector<std::complex<float>>& data, bool inverse);

    private:
        void ProcessBlock();

        int m_Channels = 0, m_Block = 0, m_Fft = 0;
        size_t m_PartitionCount = 0;
        // Per channel: partition spectra of the response, the spectra of the last inputs (ring), the last 2 blocks of
        // input (overlap-save), the block being collected and the block being played.
        std::vector<std::vector<std::vector<std::complex<float>>>> m_Partitions; // [channel][partition][bin]
        std::vector<std::vector<std::vector<std::complex<float>>>> m_History;    // [channel][slot][bin]
        std::vector<std::vector<float>> m_InputWindow;                          // [channel][2 * block]
        std::vector<float> m_InBlock, m_OutBlock;                               // interleaved, block frames
        size_t m_Fill = 0, m_HistoryHead = 0;
        std::vector<std::complex<float>> m_Scratch, m_Accum;
    };
}
