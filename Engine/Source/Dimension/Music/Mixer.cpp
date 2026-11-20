#include "Dimension/Music/Mixer.h"
#include <algorithm>

namespace Dimension::Music
{
    const char* EffectTypeName(EffectType t)
    {
        static const char* names[] = { "EQ", "Compressor", "Delay", "Reverb", "Filter" };
        return names[std::clamp((int)t, 0, (int)EffectType::Count - 1)];
    }

    const EffectInfo& GetEffectInfo(EffectType t)
    {
        static const EffectInfo infos[] = {
            // EQ: low shelf, a bell in the middle, high shelf.
            { 6, { { { "Low", -18.0f, 18.0f, 0.0f, "%.1f dB" }, { "Mid Freq", 100.0f, 10000.0f, 1000.0f, "%.0f Hz", true },
                     { "Mid", -18.0f, 18.0f, 0.0f, "%.1f dB" }, { "Mid Width", 0.3f, 4.0f, 1.0f, "Q %.2f", true },
                     { "High", -18.0f, 18.0f, 0.0f, "%.1f dB" }, { "Output", -18.0f, 18.0f, 0.0f, "%.1f dB" } } }, 1.0f, false },
            { 5, { { { "Threshold", -48.0f, 0.0f, -18.0f, "%.1f dB" }, { "Ratio", 1.0f, 20.0f, 4.0f, "%.1f:1", true },
                     { "Attack", 0.1f, 200.0f, 10.0f, "%.1f ms", true }, { "Release", 5.0f, 1000.0f, 120.0f, "%.0f ms", true },
                     { "Makeup", 0.0f, 24.0f, 0.0f, "%.1f dB" } } }, 1.0f, false },
            // Delay: time in steps (synced to the tempo).
            { 4, { { { "Time", 1.0f, 16.0f, 3.0f, "%.0f steps" }, { "Feedback", 0.0f, 0.95f, 0.45f, "%.2f" },
                     { "Ping-Pong", 0.0f, 1.0f, 1.0f, "%.0f" }, { "Tone", 0.0f, 1.0f, 0.6f, "%.2f" } } }, 0.3f, true },
            { 3, { { { "Room Size", 0.0f, 1.0f, 0.6f, "%.2f" }, { "Damping", 0.0f, 1.0f, 0.5f, "%.2f" }, { "Width", 0.0f, 1.0f, 1.0f, "%.2f" } } },
              0.25f, true },
            // Filter: 0 low pass, 1 high pass, 2 band pass.
            { 3, { { { "Mode", 0.0f, 2.0f, 0.0f, "%.0f" }, { "Cutoff", 20.0f, 20000.0f, 20000.0f, "%.0f Hz", true },
                     { "Resonance", 0.0f, 1.0f, 0.2f, "%.2f" } } }, 1.0f, false },
        };
        return infos[std::clamp((int)t, 0, (int)EffectType::Count - 1)];
    }

    Effect Effect::Make(EffectType type)
    {
        Effect e;
        e.Type = type;
        const EffectInfo& info = GetEffectInfo(type);
        e.Mix = info.DefaultMix;
        for (int i = 0; i < info.ParamCount; ++i) e.P[i] = info.Params[i].Default;
        return e;
    }

    float AutomationClip::ValueAt(double tick) const
    {
        if (Points.empty()) return 0.5f;
        if (tick <= Points.front().Tick) return Points.front().Value;
        for (size_t i = 1; i < Points.size(); ++i)
            if (tick <= Points[i].Tick)
            {
                const AutomationPoint& a = Points[i - 1];
                const AutomationPoint& b = Points[i];
                if (b.Tick <= a.Tick) return b.Value;
                float t = (float)((tick - a.Tick) / (b.Tick - a.Tick));
                return a.Value + (b.Value - a.Value) * t;
            }
        return Points.back().Value;
    }

    void AutomationClip::SortPoints()
    {
        std::stable_sort(Points.begin(), Points.end(), [](const AutomationPoint& a, const AutomationPoint& b) { return a.Tick < b.Tick; });
    }
}
