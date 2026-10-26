#include "Dimension/Audio/Convolver.h"
#include <algorithm>
#include <cmath>

namespace Dimension
{
    void Convolver::FFT(std::vector<std::complex<float>>& a, bool inverse)
    {
        size_t n = a.size();
        for (size_t i = 1, j = 0; i < n; ++i) // bit reversal
        {
            size_t bit = n >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) std::swap(a[i], a[j]);
        }
        for (size_t len = 2; len <= n; len <<= 1)
        {
            double angle = 2.0 * 3.14159265358979323846 / (double)len * (inverse ? 1.0 : -1.0);
            std::complex<float> step((float)std::cos(angle), (float)std::sin(angle));
            for (size_t i = 0; i < n; i += len)
            {
                std::complex<float> w(1.0f, 0.0f);
                for (size_t k = 0; k < len / 2; ++k)
                {
                    std::complex<float> u = a[i + k], v = a[i + k + len / 2] * w;
                    a[i + k] = u + v;
                    a[i + k + len / 2] = u - v;
                    w *= step;
                }
            }
        }
        if (inverse)
        {
            float s = 1.0f / (float)n;
            for (auto& x : a) x *= s;
        }
    }

    void Convolver::Init(const float* impulse, size_t impulseFrames, int impulseChannels, int channels, int blockSize)
    {
        m_Partitions.clear();
        m_History.clear();
        m_InputWindow.clear();
        if (!impulse || impulseFrames == 0 || impulseChannels <= 0 || channels <= 0) return;
        int block = 64;
        while (block < blockSize) block <<= 1;
        m_Channels = channels;
        m_Block = block;
        m_Fft = block * 2;
        m_PartitionCount = (impulseFrames + block - 1) / block;

        // Unit energy per channel keeps the level independent of the response's length and loudness.
        std::vector<float> norm(impulseChannels, 0.0f);
        for (int c = 0; c < impulseChannels; ++c)
        {
            double sum = 0.0;
            for (size_t f = 0; f < impulseFrames; ++f) sum += (double)impulse[f * impulseChannels + c] * impulse[f * impulseChannels + c];
            norm[c] = sum > 0.0 ? (float)(1.0 / std::sqrt(sum)) : 0.0f;
        }
        m_Partitions.resize(channels);
        m_History.assign(channels, std::vector<std::vector<std::complex<float>>>(m_PartitionCount, std::vector<std::complex<float>>(m_Fft)));
        m_InputWindow.assign(channels, std::vector<float>(m_Fft, 0.0f));
        for (int c = 0; c < channels; ++c)
        {
            int ic = c % impulseChannels;
            auto& parts = m_Partitions[c];
            parts.resize(m_PartitionCount);
            for (size_t p = 0; p < m_PartitionCount; ++p)
            {
                // Each partition zero-padded to the FFT size (the second half stays zero: overlap-save).
                std::vector<std::complex<float>> spectrum(m_Fft, 0.0f);
                for (int k = 0; k < block; ++k)
                {
                    size_t f = p * block + k;
                    if (f < impulseFrames) spectrum[k] = impulse[f * impulseChannels + ic] * norm[ic];
                }
                FFT(spectrum, false);
                parts[p] = std::move(spectrum);
            }
        }
        m_InBlock.assign((size_t)block * channels, 0.0f);
        m_OutBlock.assign((size_t)block * channels, 0.0f);
        m_Scratch.assign(m_Fft, 0.0f);
        m_Accum.assign(m_Fft, 0.0f);
        m_Fill = 0;
        m_HistoryHead = 0;
    }

    void Convolver::Reset()
    {
        for (auto& ch : m_History)
            for (auto& slot : ch) std::fill(slot.begin(), slot.end(), std::complex<float>(0.0f));
        for (auto& w : m_InputWindow) std::fill(w.begin(), w.end(), 0.0f);
        std::fill(m_InBlock.begin(), m_InBlock.end(), 0.0f);
        std::fill(m_OutBlock.begin(), m_OutBlock.end(), 0.0f);
        m_Fill = 0;
    }

    void Convolver::ProcessBlock()
    {
        // Newest spectrum goes in the slot after the head; partition p multiplies the input from p blocks ago.
        m_HistoryHead = (m_HistoryHead + m_PartitionCount - 1) % m_PartitionCount;
        for (int c = 0; c < m_Channels; ++c)
        {
            auto& window = m_InputWindow[c];
            std::copy(window.begin() + m_Block, window.end(), window.begin()); // slide: last block, then the new one
            for (int k = 0; k < m_Block; ++k) window[m_Block + k] = m_InBlock[(size_t)k * m_Channels + c];
            for (int k = 0; k < m_Fft; ++k) m_Scratch[k] = window[k];
            FFT(m_Scratch, false);
            m_History[c][m_HistoryHead] = m_Scratch;
            std::fill(m_Accum.begin(), m_Accum.end(), std::complex<float>(0.0f));
            for (size_t p = 0; p < m_PartitionCount; ++p)
            {
                const auto& x = m_History[c][(m_HistoryHead + p) % m_PartitionCount];
                const auto& h = m_Partitions[c][p];
                for (int k = 0; k < m_Fft; ++k) m_Accum[k] += x[k] * h[k];
            }
            FFT(m_Accum, true);
            // Overlap-save: the second half is the circularly-uncorrupted output block.
            for (int k = 0; k < m_Block; ++k) m_OutBlock[(size_t)k * m_Channels + c] = m_Accum[m_Block + k].real();
        }
    }

    void Convolver::Process(const float* in, float* out, size_t frames, float wet, float dry)
    {
        if (!IsReady())
        {
            for (size_t i = 0; i < frames * (size_t)std::max(m_Channels, 1); ++i) out[i] = in[i] * dry;
            return;
        }
        for (size_t f = 0; f < frames; ++f)
        {
            for (int c = 0; c < m_Channels; ++c)
            {
                size_t i = f * m_Channels + c;
                float x = in[i];
                float y = m_OutBlock[m_Fill * m_Channels + c];
                m_InBlock[m_Fill * m_Channels + c] = x;
                out[i] = x * dry + y * wet;
            }
            if (++m_Fill == (size_t)m_Block)
            {
                ProcessBlock();
                m_Fill = 0;
            }
        }
    }
}
