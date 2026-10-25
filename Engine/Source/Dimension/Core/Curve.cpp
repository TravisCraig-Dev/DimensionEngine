#include "Dimension/Core/Curve.h"
#include <algorithm>

namespace Dimension
{
    namespace
    {
        // Shared lookup: index of the segment containing t and the blend factor within it.
        template <class Key> bool Segment(const std::vector<Key>& keys, float t, size_t& i, float& f)
        {
            if (keys.size() < 2 || t <= keys.front().Time) { i = 0; f = 0.0f; return false; }
            if (t >= keys.back().Time) { i = keys.size() - 1; f = 0.0f; return false; }
            i = 0;
            while (i + 1 < keys.size() && keys[i + 1].Time < t) ++i;
            float span = keys[i + 1].Time - keys[i].Time;
            f = span > 1e-6f ? (t - keys[i].Time) / span : 0.0f;
            return true;
        }
    }

    namespace
    {
        // Fritsch-Carlson tangent (slope, value per unit time) at key i: the harmonic mean of the neighbouring
        // secant slopes, zero at local extrema, one-sided at the ends. Keeps the cubic monotone between keys.
        float MonotoneTangent(const std::vector<CurveKey>& keys, size_t i)
        {
            auto secant = [&](size_t a) {
                float dt = keys[a + 1].Time - keys[a].Time;
                return dt > 1e-6f ? (keys[a + 1].Value - keys[a].Value) / dt : 0.0f;
            };
            size_t n = keys.size();
            if (i == 0) return secant(0);
            if (i == n - 1) return secant(n - 2);
            float d0 = secant(i - 1), d1 = secant(i);
            if (d0 * d1 <= 0.0f) return 0.0f;
            float w0 = 2.0f * (keys[i + 1].Time - keys[i].Time) + (keys[i].Time - keys[i - 1].Time);
            float w1 = (keys[i + 1].Time - keys[i].Time) + 2.0f * (keys[i].Time - keys[i - 1].Time);
            return (w0 + w1) / (w0 / d0 + w1 / d1);
        }
    }

    float Curve::Evaluate(float t) const
    {
        if (Keys.empty()) return 1.0f;
        size_t i;
        float f;
        if (!Segment(Keys, t, i, f)) return Keys[i].Value;
        const CurveKey& a = Keys[i];
        const CurveKey& b = Keys[i + 1];
        if (!Smooth) return a.Value + (b.Value - a.Value) * f;
        // Cubic Hermite between the two keys.
        float dt = b.Time - a.Time;
        float m0 = MonotoneTangent(Keys, i) * dt, m1 = MonotoneTangent(Keys, i + 1) * dt;
        float f2 = f * f, f3 = f2 * f;
        return (2 * f3 - 3 * f2 + 1) * a.Value + (f3 - 2 * f2 + f) * m0 + (-2 * f3 + 3 * f2) * b.Value + (f3 - f2) * m1;
    }

    void Curve::Sort()
    {
        std::stable_sort(Keys.begin(), Keys.end(), [](const CurveKey& a, const CurveKey& b) { return a.Time < b.Time; });
    }

    glm::vec4 Gradient::Evaluate(float t) const
    {
        if (Keys.empty()) return glm::vec4(1.0f);
        size_t i;
        float f;
        if (!Segment(Keys, t, i, f)) return Keys[i].Color;
        return glm::mix(Keys[i].Color, Keys[i + 1].Color, f);
    }

    void Gradient::Sort()
    {
        std::stable_sort(Keys.begin(), Keys.end(), [](const GradientKey& a, const GradientKey& b) { return a.Time < b.Time; });
    }
}
