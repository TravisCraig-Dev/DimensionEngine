#include "Dimension/Music/Effects.h"
#include <algorithm>
#include <cmath>

namespace Dimension::Music
{
    namespace
    {
        constexpr float kPi = 3.14159265358979f;
        float DbToGain(float db) { return std::pow(10.0f, db / 20.0f); }

        // RBJ cookbook biquad (transposed direct form II), one per side.
        struct Biquad
        {
            float B0 = 1, B1 = 0, B2 = 0, A1 = 0, A2 = 0;
            float Z1[2] = { 0, 0 }, Z2[2] = { 0, 0 };
            void Set(float b0, float b1, float b2, float a0, float a1, float a2)
            {
                B0 = b0 / a0, B1 = b1 / a0, B2 = b2 / a0, A1 = a1 / a0, A2 = a2 / a0;
            }
            void LowShelf(float freq, float db, float sr)
            {
                float A = std::pow(10.0f, db / 40.0f), w = 2 * kPi * freq / sr, c = std::cos(w), s = std::sin(w);
                float alpha = s / 2 * std::sqrt(2.0f), sa = 2 * std::sqrt(A) * alpha;
                Set(A * ((A + 1) - (A - 1) * c + sa), 2 * A * ((A - 1) - (A + 1) * c), A * ((A + 1) - (A - 1) * c - sa), (A + 1) + (A - 1) * c + sa,
                    -2 * ((A - 1) + (A + 1) * c), (A + 1) + (A - 1) * c - sa);
            }
            void HighShelf(float freq, float db, float sr)
            {
                float A = std::pow(10.0f, db / 40.0f), w = 2 * kPi * freq / sr, c = std::cos(w), s = std::sin(w);
                float alpha = s / 2 * std::sqrt(2.0f), sa = 2 * std::sqrt(A) * alpha;
                Set(A * ((A + 1) + (A - 1) * c + sa), -2 * A * ((A - 1) + (A + 1) * c), A * ((A + 1) + (A - 1) * c - sa), (A + 1) - (A - 1) * c + sa,
                    2 * ((A - 1) - (A + 1) * c), (A + 1) - (A - 1) * c - sa);
            }
            void Peak(float freq, float db, float q, float sr)
            {
                float A = std::pow(10.0f, db / 40.0f), w = 2 * kPi * std::min(freq, sr * 0.45f) / sr, c = std::cos(w), s = std::sin(w);
                float alpha = s / (2 * std::max(0.05f, q));
                Set(1 + alpha * A, -2 * c, 1 - alpha * A, 1 + alpha / A, -2 * c, 1 - alpha / A);
            }
            float Run(float x, int ch)
            {
                float y = B0 * x + Z1[ch];
                Z1[ch] = B1 * x - A1 * y + Z2[ch];
                Z2[ch] = B2 * x - A2 * y;
                return y;
            }
            void Reset() { Z1[0] = Z1[1] = Z2[0] = Z2[1] = 0; }
        };

        class EQ : public EffectProcessor
        {
        public:
            explicit EQ(uint32_t sr) : EffectProcessor(EffectType::EQ, sr) {}
            void Reset() override { m_Low.Reset(), m_Mid.Reset(), m_High.Reset(); }
        protected:
            void Wet(const Effect& e, const float* dry, float* wet, uint32_t frames, float, int) override
            {
                m_Low.LowShelf(200.0f, e.P[0], m_SampleRate);
                m_Mid.Peak(e.P[1], e.P[2], e.P[3], m_SampleRate);
                m_High.HighShelf(5000.0f, e.P[4], m_SampleRate);
                float out = DbToGain(e.P[5]);
                for (uint32_t f = 0; f < frames; ++f)
                    for (int c = 0; c < 2; ++c) wet[f * 2 + c] = m_High.Run(m_Mid.Run(m_Low.Run(dry[f * 2 + c], c), c), c) * out;
            }
        private:
            Biquad m_Low, m_Mid, m_High;
        };

        class Compressor : public EffectProcessor
        {
        public:
            explicit Compressor(uint32_t sr) : EffectProcessor(EffectType::Compressor, sr) {}
            void Reset() override { m_Env = 0.0f; }
            float GainReductionDb() const { return m_LastReduction; }
        protected:
            void Wet(const Effect& e, const float* dry, float* wet, uint32_t frames, float, int) override
            {
                float threshold = e.P[0], ratio = std::max(1.0f, e.P[1]);
                float att = std::exp(-1.0f / (std::max(0.05f, e.P[2]) * 0.001f * m_SampleRate));
                float rel = std::exp(-1.0f / (std::max(1.0f, e.P[3]) * 0.001f * m_SampleRate));
                float makeup = DbToGain(e.P[4]);
                for (uint32_t f = 0; f < frames; ++f)
                {
                    // Stereo-linked peak level, smoothed (attack rising, release falling), in dB above the threshold.
                    float level = std::max(std::fabs(dry[f * 2]), std::fabs(dry[f * 2 + 1]));
                    float coef = level > m_Env ? att : rel;
                    m_Env = coef * m_Env + (1.0f - coef) * level;
                    float db = 20.0f * std::log10(std::max(m_Env, 1e-6f));
                    float over = db - threshold;
                    float reduction = over > 0.0f ? over * (1.0f - 1.0f / ratio) : 0.0f;
                    float g = DbToGain(-reduction) * makeup;
                    m_LastReduction = reduction;
                    wet[f * 2] = dry[f * 2] * g;
                    wet[f * 2 + 1] = dry[f * 2 + 1] * g;
                }
            }
        private:
            float m_Env = 0.0f, m_LastReduction = 0.0f;
        };

        class Delay : public EffectProcessor
        {
        public:
            explicit Delay(uint32_t sr) : EffectProcessor(EffectType::Delay, sr), m_Line((size_t)sr * 4 * 2, 0.0f) {}
            void Reset() override
            {
                std::fill(m_Line.begin(), m_Line.end(), 0.0f);
                m_Tone[0] = m_Tone[1] = 0.0f;
            }
        protected:
            void Wet(const Effect& e, const float* dry, float* wet, uint32_t frames, float tempo, int stepsPerBeat) override
            {
                size_t length = m_Line.size() / 2;
                float stepSeconds = 60.0f / std::max(1.0f, tempo) / std::max(1, stepsPerBeat);
                size_t delay = std::clamp((size_t)(std::round(e.P[0]) * stepSeconds * m_SampleRate), (size_t)1, length - 1);
                float feedback = std::clamp(e.P[1], 0.0f, 0.95f);
                bool pingPong = e.P[2] >= 0.5f;
                float tone = std::exp(-2.0f * kPi * (800.0f + 15000.0f * std::clamp(e.P[3], 0.0f, 1.0f)) / m_SampleRate); // feedback low pass
                for (uint32_t f = 0; f < frames; ++f)
                {
                    size_t read = (m_Write + length - delay) % length;
                    float l = m_Line[read * 2], r = m_Line[read * 2 + 1];
                    wet[f * 2] = l;
                    wet[f * 2 + 1] = r;
                    // Feedback (darkened each time round); ping-pong crosses the sides, fed from the middle.
                    m_Tone[0] = l + (m_Tone[0] - l) * tone;
                    m_Tone[1] = r + (m_Tone[1] - r) * tone;
                    float inL = dry[f * 2], inR = dry[f * 2 + 1];
                    if (pingPong)
                    {
                        m_Line[m_Write * 2] = (inL + inR) * 0.5f + m_Tone[1] * feedback;
                        m_Line[m_Write * 2 + 1] = m_Tone[0] * feedback;
                    }
                    else
                    {
                        m_Line[m_Write * 2] = inL + m_Tone[0] * feedback;
                        m_Line[m_Write * 2 + 1] = inR + m_Tone[1] * feedback;
                    }
                    m_Write = (m_Write + 1) % length;
                }
            }
        private:
            std::vector<float> m_Line;
            size_t m_Write = 0;
            float m_Tone[2] = { 0, 0 };
        };

        // Freeverb: 8 parallel damped combs into 4 series allpasses per side (the right side's lines a little longer).
        class Reverb : public EffectProcessor
        {
        public:
            explicit Reverb(uint32_t sr) : EffectProcessor(EffectType::Reverb, sr)
            {
                static const int combs[8] = { 1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617 };
                static const int allpasses[4] = { 556, 441, 341, 225 };
                float scale = sr / 44100.0f;
                for (int side = 0; side < 2; ++side)
                {
                    for (int i = 0; i < 8; ++i) m_Comb[side][i].Buffer.assign((size_t)((combs[i] + side * 23) * scale), 0.0f);
                    for (int i = 0; i < 4; ++i) m_All[side][i].Buffer.assign((size_t)((allpasses[i] + side * 23) * scale), 0.0f);
                }
            }
            void Reset() override
            {
                for (auto& side : m_Comb)
                    for (auto& c : side) std::fill(c.Buffer.begin(), c.Buffer.end(), 0.0f), c.Store = 0.0f;
                for (auto& side : m_All)
                    for (auto& a : side) std::fill(a.Buffer.begin(), a.Buffer.end(), 0.0f);
            }
        protected:
            void Wet(const Effect& e, const float* dry, float* wet, uint32_t frames, float, int) override
            {
                float room = 0.7f + 0.28f * std::clamp(e.P[0], 0.0f, 1.0f);
                float damp = 0.4f * std::clamp(e.P[1], 0.0f, 1.0f);
                float width = std::clamp(e.P[2], 0.0f, 1.0f);
                float wet1 = (width * 0.5f + 0.5f), wet2 = (1.0f - width) * 0.5f;
                for (uint32_t f = 0; f < frames; ++f)
                {
                    float input = (dry[f * 2] + dry[f * 2 + 1]) * 0.015f;
                    float out[2];
                    for (int side = 0; side < 2; ++side)
                    {
                        float acc = 0.0f;
                        for (Comb& c : m_Comb[side])
                        {
                            float y = c.Buffer[c.Index];
                            c.Store = y * (1.0f - damp) + c.Store * damp;
                            c.Buffer[c.Index] = input + c.Store * room;
                            if (++c.Index >= c.Buffer.size()) c.Index = 0;
                            acc += y;
                        }
                        for (Allpass& a : m_All[side])
                        {
                            float b = a.Buffer[a.Index];
                            float y = -acc + b;
                            a.Buffer[a.Index] = acc + b * 0.5f;
                            if (++a.Index >= a.Buffer.size()) a.Index = 0;
                            acc = y;
                        }
                        out[side] = acc;
                    }
                    wet[f * 2] = out[0] * wet1 + out[1] * wet2;
                    wet[f * 2 + 1] = out[1] * wet1 + out[0] * wet2;
                }
            }
        private:
            struct Comb
            {
                std::vector<float> Buffer;
                size_t Index = 0;
                float Store = 0.0f;
            };
            struct Allpass
            {
                std::vector<float> Buffer;
                size_t Index = 0;
            };
            Comb m_Comb[2][8];
            Allpass m_All[2][4];
        };

        class Filter : public EffectProcessor
        {
        public:
            explicit Filter(uint32_t sr) : EffectProcessor(EffectType::Filter, sr) {}
            void Reset() override { m_Ic1[0] = m_Ic1[1] = m_Ic2[0] = m_Ic2[1] = 0.0f; }
        protected:
            void Wet(const Effect& e, const float* dry, float* wet, uint32_t frames, float, int) override
            {
                int mode = std::clamp((int)std::lround(e.P[0]), 0, 2);
                float g = std::tan(kPi * std::clamp(e.P[1], 20.0f, m_SampleRate * 0.45f) / m_SampleRate);
                float k = 2.0f - 1.9f * std::clamp(e.P[2], 0.0f, 1.0f);
                float a1 = 1.0f / (1.0f + g * (g + k)), a2 = g * a1, a3 = g * a2;
                for (uint32_t f = 0; f < frames; ++f)
                    for (int c = 0; c < 2; ++c)
                    {
                        float x = dry[f * 2 + c];
                        float v3 = x - m_Ic2[c];
                        float v1 = a1 * m_Ic1[c] + a2 * v3;
                        float v2 = m_Ic2[c] + a2 * m_Ic1[c] + a3 * v3;
                        m_Ic1[c] = 2.0f * v1 - m_Ic1[c];
                        m_Ic2[c] = 2.0f * v2 - m_Ic2[c];
                        wet[f * 2 + c] = mode == 0 ? v2 : mode == 1 ? x - k * v1 - v2 : v1;
                    }
            }
        private:
            float m_Ic1[2] = { 0, 0 }, m_Ic2[2] = { 0, 0 };
        };
    }

    void EffectProcessor::Process(const Effect& e, float* stereo, uint32_t frames, float tempo, int stepsPerBeat)
    {
        if (m_Scratch.size() < (size_t)frames * 2) return; // (sized when made: blocks are never larger)
        Wet(e, stereo, m_Scratch.data(), frames, tempo, stepsPerBeat);
        float mix = std::clamp(e.Mix, 0.0f, 1.0f);
        if (GetEffectInfo(m_Type).Send)
            for (uint32_t i = 0; i < frames * 2; ++i) stereo[i] += m_Scratch[i] * mix;
        else
            for (uint32_t i = 0; i < frames * 2; ++i) stereo[i] += (m_Scratch[i] - stereo[i]) * mix;
    }

    std::unique_ptr<EffectProcessor> EffectProcessor::Create(EffectType type, uint32_t sampleRate)
    {
        std::unique_ptr<EffectProcessor> p;
        switch (type)
        {
        case EffectType::EQ: p = std::make_unique<EQ>(sampleRate); break;
        case EffectType::Compressor: p = std::make_unique<Compressor>(sampleRate); break;
        case EffectType::Delay: p = std::make_unique<Delay>(sampleRate); break;
        case EffectType::Reverb: p = std::make_unique<Reverb>(sampleRate); break;
        case EffectType::Filter: p = std::make_unique<Filter>(sampleRate); break;
        default: p = std::make_unique<EQ>(sampleRate); break;
        }
        p->m_Scratch.assign(256 * 2, 0.0f);
        return p;
    }
}
