#pragma once
#include "Dimension/Music/Mixer.h"
#include <cstdint>
#include <memory>

namespace Dimension::Music
{
    // A running effect: its state (filters, delay lines) between blocks. Made off the audio thread (it may allocate);
    // Process runs on it and does not.
    class EffectProcessor
    {
    public:
        virtual ~EffectProcessor() = default;
        EffectType GetType() const { return m_Type; }
        // Processes interleaved stereo in place: `e`'s settings, `tempo` in BPM (for synced times).
        void Process(const Effect& e, float* stereo, uint32_t frames, float tempo, int stepsPerBeat);
        virtual void Reset() = 0;

        static std::unique_ptr<EffectProcessor> Create(EffectType type, uint32_t sampleRate);

    protected:
        EffectProcessor(EffectType type, uint32_t sampleRate) : m_Type(type), m_SampleRate((float)sampleRate) {}
        // The wet signal into `wet` from `dry` (frames interleaved stereo).
        virtual void Wet(const Effect& e, const float* dry, float* wet, uint32_t frames, float tempo, int stepsPerBeat) = 0;
        EffectType m_Type;
        float m_SampleRate;
        std::vector<float> m_Scratch;
    };
}
