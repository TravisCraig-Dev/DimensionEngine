#pragma once
#include <glm/glm.hpp>
#include <vector>

namespace Dimension
{
    struct CurveKey
    {
        float Time = 0.0f;  // normalized 0-1
        float Value = 0.0f;
        bool operator==(const CurveKey&) const = default;
    };

    // Piecewise-linear curve over normalized time (a compact take on Unity's AnimationCurve).
    // Values before the first key / after the last key are held.
    struct Curve
    {
        std::vector<CurveKey> Keys;

        static Curve Constant(float v) { return { { { 0.0f, v }, { 1.0f, v } } }; }
        static Curve Linear(float from, float to) { return { { { 0.0f, from }, { 1.0f, to } } }; }

        float Evaluate(float t) const;
        void Sort();
        bool operator==(const Curve&) const = default;
    };

    struct GradientKey
    {
        float Time = 0.0f;       // normalized 0-1
        glm::vec4 Color{ 1.0f }; // sRGB color + alpha
        bool operator==(const GradientKey&) const = default;
    };

    // Color gradient over normalized time (Unity's Gradient, with combined color/alpha keys).
    struct Gradient
    {
        std::vector<GradientKey> Keys;

        static Gradient Constant(const glm::vec4& c) { return { { { 0.0f, c }, { 1.0f, c } } }; }
        static Gradient Linear(const glm::vec4& from, const glm::vec4& to) { return { { { 0.0f, from }, { 1.0f, to } } }; }

        glm::vec4 Evaluate(float t) const;
        void Sort();
        bool operator==(const Gradient&) const = default;
    };
}
