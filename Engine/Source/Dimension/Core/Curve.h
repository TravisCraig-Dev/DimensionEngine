#pragma once
#include <glm/glm.hpp>
#include <vector>

namespace Dimension
{
    // How a smooth curve's tangents at a key are chosen: automatically (monotone, never overshooting), or set by hand
    // as one slope through the key (Free) or separate incoming and outgoing slopes (Broken).
    enum class CurveTangentMode : uint8_t { Auto = 0, Free = 1, Broken = 2 };

    struct CurveKey
    {
        float Time = 0.0f;  // normalized 0-1
        float Value = 0.0f;
        float InTangent = 0.0f, OutTangent = 0.0f; // slopes (value per unit time) used unless Mode is Auto
        CurveTangentMode Mode = CurveTangentMode::Auto;
        bool operator==(const CurveKey&) const = default;
    };

    // Curve over normalized time (a compact take on Unity's AnimationCurve): piecewise linear, or Smooth, a cubic
    // through the keys whose tangents are automatic (monotone: flat at peaks, so it never overshoots a key) unless
    // a key's tangents were set by hand. Values before the first key / after the last key are held.
    struct Curve
    {
        std::vector<CurveKey> Keys;
        bool Smooth = false;

        static Curve Constant(float v) { return { { { 0.0f, v }, { 1.0f, v } } }; }
        static Curve Linear(float from, float to) { return { { { 0.0f, from }, { 1.0f, to } } }; }

        float Evaluate(float t) const;
        void Sort();
        // The automatic (monotone) slope at key i, and the slopes a smooth curve actually uses on each side of it.
        float AutoTangent(size_t i) const;
        float InSlope(size_t i) const;
        float OutSlope(size_t i) const;
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
