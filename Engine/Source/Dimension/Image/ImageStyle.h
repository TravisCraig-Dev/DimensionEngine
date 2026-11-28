#pragma once
#include "Dimension/Modeling/Paint.h"
#include <glm/glm.hpp>
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

// The settings layers carry beside their pixels: blend modes, adjustments (adjustment layers, and Image > Adjustments)
// and layer effects (Photoshop's layer styles), plus the effects' rendered result kept with the layer.
namespace Dimension::Imaging
{
    // Photoshop's layer blend modes, in its menu's order and groups. Pass Through is for groups only: their layers blend
    // straight into what is under the group (as if they were not grouped), the group's opacity and mask fading them.
    enum class LayerBlend
    {
        Normal = 0,
        Dissolve,
        Darken,
        Multiply,
        ColorBurn,
        LinearBurn,
        DarkerColor,
        Lighten,
        Screen,
        ColorDodge,
        LinearDodge,
        LighterColor,
        Overlay,
        SoftLight,
        HardLight,
        VividLight,
        LinearLight,
        PinLight,
        HardMix,
        Difference,
        Exclusion,
        Subtract,
        Divide,
        Hue,
        Saturation,
        Color,
        Luminosity,
        PassThrough,
        Count
    };

    enum class AdjustmentType
    {
        BrightnessContrast,
        Levels,
        Curves,
        Exposure,
        HueSaturation,
        ColorBalance,
        Vibrance,
        BlackWhite,
        PhotoFilter,
        Invert,
        Posterize,
        Threshold,
        GradientMap,
        Count
    };

    // An adjustment's settings (each type uses its own fields). Channels: 0 = RGB (all), 1 red, 2 green, 3 blue.
    struct Adjustment
    {
        AdjustmentType Type = AdjustmentType::BrightnessContrast;
        float Brightness = 0.0f, Contrast = 0.0f; // -100..100
        // Levels: input black and white points, gamma (midtones), output black and white points.
        std::array<float, 4> InBlack{ 0, 0, 0, 0 }, InWhite{ 1, 1, 1, 1 }, Gamma{ 1, 1, 1, 1 }, OutBlack{ 0, 0, 0, 0 }, OutWhite{ 1, 1, 1, 1 };
        // Curves: points (input, output) 0..1, sorted by input.
        std::array<std::vector<glm::vec2>, 4> Curve{ { { { 0, 0 }, { 1, 1 } }, { { 0, 0 }, { 1, 1 } }, { { 0, 0 }, { 1, 1 } }, { { 0, 0 }, { 1, 1 } } } };
        float Exposure = 0.0f, Offset = 0.0f, ExposureGamma = 1.0f; // stops, -0.5..0.5, 0.1..10
        float Hue = 0.0f, Saturation = 0.0f, Lightness = 0.0f;     // -180..180, -100..100, -100..100
        bool Colorize = false;                                      // (Hue 0..360 and Saturation 0..100 then set the color)
        // Color Balance: shadows, midtones, highlights; each cyan-red, magenta-green, yellow-blue (-100..100).
        std::array<glm::vec3, 3> Balance{ glm::vec3(0.0f), glm::vec3(0.0f), glm::vec3(0.0f) };
        bool PreserveLuminosity = true;                             // (Color Balance, Photo Filter)
        float Vibrance = 0.0f, VibranceSaturation = 0.0f;           // -100..100
        // Black & White: reds, yellows, greens, cyans, blues, magentas (percent); an optional tint.
        std::array<float, 6> Weights{ 40, 60, 40, 60, 20, 80 };
        bool Tint = false;
        glm::vec3 TintColor{ 0.88f, 0.76f, 0.57f };
        glm::vec3 FilterColor{ 0.93f, 0.54f, 0.10f }; // Photo Filter (warming)
        float Density = 0.25f;
        int Levels = 4;                        // Posterize
        float ThresholdLevel = 0.5f;           // Threshold (0..1)
        glm::vec3 MapFrom{ 0.0f }, MapTo{ 1.0f }; // Gradient Map: shadows to highlights
    };
    const char* AdjustmentName(AdjustmentType type);

    struct ShadowEffect // Drop Shadow, Inner Shadow
    {
        bool Enabled = false;
        glm::vec3 Color{ 0.0f };
        float Opacity = 0.75f;
        LayerBlend Mode = LayerBlend::Multiply;
        float Angle = 120.0f;   // where the light comes from (degrees, counterclockwise from the right)
        float Distance = 5.0f;  // pixels
        float Spread = 0.0f;    // 0..1 (Inner Shadow: choke)
        float Size = 5.0f;      // pixels of blur
    };
    struct GlowEffect // Outer Glow, Inner Glow
    {
        bool Enabled = false;
        glm::vec3 Color{ 1.0f, 1.0f, 0.75f };
        float Opacity = 0.75f;
        LayerBlend Mode = LayerBlend::Screen;
        float Spread = 0.0f;
        float Size = 5.0f;
    };
    struct StrokeEffect
    {
        bool Enabled = false;
        glm::vec3 Color{ 0.0f };
        float Opacity = 1.0f;
        float Size = 3.0f;
        int Position = 0; // 0 outside, 1 inside, 2 center
    };
    struct OverlayEffect // Color Overlay
    {
        bool Enabled = false;
        glm::vec3 Color{ 1.0f, 0.0f, 0.0f };
        float Opacity = 1.0f;
        LayerBlend Mode = LayerBlend::Normal;
    };
    struct GradientEffect // Gradient Overlay (across the layer's content)
    {
        bool Enabled = false;
        glm::vec3 From{ 0.0f }, To{ 1.0f };
        float Opacity = 1.0f;
        LayerBlend Mode = LayerBlend::Normal;
        float Angle = 90.0f; // (90: from the bottom up)
        bool Reverse = false;
    };
    struct LayerEffects
    {
        ShadowEffect DropShadow, InnerShadow;
        GlowEffect OuterGlow, InnerGlow;
        StrokeEffect Stroke;
        OverlayEffect ColorOverlay;
        GradientEffect GradientOverlay;

        bool Any() const;
        uint64_t Hash() const; // (changes when any setting does)
    };

    // A layer's effects rendered: what goes under its pixels (shadow, glow, outside stroke; bottom first) and its pixels
    // with the overlays, inner effects and inside stroke on them. Kept with the layer until its pixels or effects change.
    struct StyledLayer
    {
        // What it was drawn from, held (a raw address could be reused by a new buffer once the old one went).
        std::shared_ptr<const void> Pixels, MaskKey;
        uint64_t Key = 0;
        struct Under
        {
            glm::vec3 Color{ 0.0f };
            LayerBlend Mode = LayerBlend::Normal;
            float Opacity = 1.0f;
            std::vector<uint8_t> Alpha;
        };
        std::vector<Under> Below;
        Modeling::Paint::Image Content;
    };
}
