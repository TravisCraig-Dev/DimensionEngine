#pragma once
#include "Dimension/Image/ImageDocument.h"
#include <nlohmann/json_fwd.hpp>
#include <functional>

// Image Mode's adjustments (per-color changes: levels, curves, hue/saturation...), filters (changes that look at
// neighbors: blurs, sharpening, noise...) and layer effects (shadows, glows, stroke, overlays) rendered from a layer.
namespace Dimension::Imaging
{
    // ---- Adjustments

    // An adjustment made ready to apply (its lookup tables built).
    struct PreparedAdjustment
    {
        const Adjustment* Settings = nullptr;
        std::array<std::array<float, 256>, 3> Lut{}; // (per channel, for the adjustments that are curves of each value)
        bool UsesLut = false;
    };
    PreparedAdjustment Prepare(const Adjustment& adjustment);
    // A straight color (0..1) through the adjustment.
    glm::vec3 Adjust(const PreparedAdjustment& prepared, const glm::vec3& color);
    // The whole picture through it (blended by the selection where there is one); alpha kept.
    Bitmap ApplyAdjustment(const Bitmap& image, const Adjustment& adjustment, const Mask* selection);
    // Curves' smooth line through its points (monotone: never overshooting between them), at x (0..1).
    float CurveAt(const std::vector<glm::vec2>& points, float x);
    // How many pixels have each value: all (luminance), red, green, blue (0..3).
    std::array<uint32_t, 256> Histogram(const Bitmap& image, int channel);

    // ---- Filters

    enum class FilterType
    {
        GaussianBlur,
        BoxBlur,
        MotionBlur,
        UnsharpMask,
        Sharpen,
        AddNoise,
        Median,
        HighPass,
        Mosaic,
        Emboss,
        FindEdges,
        Offset,       // wrap-around (tiling)
        MakeSeamless, // tileable
        NormalMap,    // from height
        ReduceColors, // to a palette, dithered
        Count
    };
    const char* FilterName(FilterType type);
    struct FilterSettings
    {
        FilterType Type = FilterType::GaussianBlur;
        float Radius = 4.0f;     // blurs, Unsharp Mask, Median, High Pass; Mosaic's cell size
        float Amount = 1.0f;     // Unsharp Mask (1 = 100%), Add Noise (0..4 = 0..400%), Emboss
        float Threshold = 0.0f;  // Unsharp Mask (0..255)
        float Angle = 0.0f;      // Motion Blur, Emboss (degrees)
        float Distance = 10.0f;  // Motion Blur (pixels), Emboss height
        bool Monochrome = false; // Add Noise
        bool Gaussian = true;    // Add Noise: Gaussian (else uniform)
        uint32_t Seed = 1;
        glm::ivec2 Shift{ 0 };   // Offset
        bool InvertY = false;    // Normal Map: green down (DirectX)
        bool Wrap = true;        // Normal Map: the edges see the other side
        std::vector<glm::vec3> Palette; // Reduce Colors (empty: the picture's own, Colors of them)
        int Colors = 16;
        int DitherKind = 0;      // Reduce Colors: Imaging::Dither
        bool HardAlpha = false;  // Reduce Colors: pixels clear or solid
    };
    // The picture through the filter (blended by the selection where there is one; outside it unchanged).
    Bitmap ApplyFilter(const Bitmap& image, const FilterSettings& filter, const Mask* selection);
    // A mask (as gray) through an adjustment or a filter.
    Mask ApplyToMask(const Mask& mask, const std::function<Bitmap(const Bitmap&)>& change);
    // A Gaussian-like blur of premultiplied colors (three box passes; sigma about `radius`).
    Bitmap GaussianBlur(const Bitmap& image, float radius);
    // Box blur passes done elsewhere (the GPU, ImageGpu.h) for big pictures: `passes` box passes of `radius` each way
    // over premultiplied colors; false: not done (the CPU does them). Null: always the CPU.
    using BoxBlurHook = bool (*)(std::vector<glm::vec4>& colors, int width, int height, int radius, int passes);
    void SetBoxBlurHook(BoxBlurHook hook);

    // ---- Layer effects

    // The layer's effects rendered from its pixels (and its mask).
    std::shared_ptr<const StyledLayer> RenderEffects(const Layer& layer, uint32_t width, uint32_t height);
    // The squared distance (pixels) from each pixel to the nearest one where `inside` is set (0 there).
    std::vector<float> DistanceField(const std::vector<uint8_t>& inside, int width, int height);

    // ---- The file (the .dimage header's JSON)

    nlohmann::json ToJson(const Adjustment& adjustment);
    Adjustment AdjustmentFromJson(const nlohmann::json& j);
    nlohmann::json ToJson(const LayerEffects& effects);
    LayerEffects EffectsFromJson(const nlohmann::json& j);
}
