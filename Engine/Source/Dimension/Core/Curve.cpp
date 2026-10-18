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

    float Curve::Evaluate(float t) const
    {
        if (Keys.empty()) return 1.0f;
        size_t i;
        float f;
        if (!Segment(Keys, t, i, f)) return Keys[i].Value;
        return Keys[i].Value + (Keys[i + 1].Value - Keys[i].Value) * f;
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
