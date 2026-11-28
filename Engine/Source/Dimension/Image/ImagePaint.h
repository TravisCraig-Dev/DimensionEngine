#pragma once
#include "Dimension/Image/ImageDocument.h"

// Image Mode's painting tools on a document's layers (or their masks): brush strokes (painting, erasing, cloning,
// healing, smudging, blurring, sharpening, dodging, burning and sponging), gradients, the paint bucket and the eyedropper.
// Colors are straight-alpha sRGB values, 0..1.
namespace Dimension::Imaging
{
    // What a stroke does where the brush goes.
    enum class BrushTool
    {
        Paint,   // the color (the brush and the pencil)
        Erase,   // alpha away
        Clone,   // the colors of Source, SourceOffset away (the clone stamp)
        Heal,    // cloned, then matched to the colors around it when the stroke ends (the healing brush)
        Smudge,  // drags the colors along
        Blur,    // softens
        Sharpen, // crisps
        Dodge,   // lightens
        Burn,    // darkens
        Sponge,  // takes saturation away (or adds it)
        Count
    };
    // Which tones dodging and burning work on most.
    enum class ToneRange { Shadows, Midtones, Highlights };

    struct BrushSettings
    {
        glm::vec4 Color{ 0.0f, 0.0f, 0.0f, 1.0f };
        float Size = 24.0f;      // diameter in pixels (the tip's long side)
        float Hardness = 0.8f;   // 0: soft all the way in, 1: full strength to the rim
        float Opacity = 1.0f;    // the most one stroke covers (overlaps within a stroke never pass it); Strength/Exposure
        float Flow = 1.0f;       // how much each dab adds (low flow builds up as the brush goes over a spot again)
        float Spacing = 0.25f;   // dabs apart, as a fraction of the size
        float Roundness = 1.0f;  // the tip's short side over its long side (0.05..1)
        float Angle = 0.0f;      // the tip's long side, degrees counterclockwise from the right
        bool Erase = false;      // (the same as Tool = Erase)
        bool Aliased = false;    // the pencil: hard pixels, no soft edge
        BrushTool Tool = BrushTool::Paint;
        ToneRange Range = ToneRange::Midtones; // Dodge, Burn
        bool Saturate = false;                 // Sponge: adds saturation instead
        // Clone and Heal: the picture the colors come from (canvas-sized) and where, from the painted pixel.
        const Bitmap* Source = nullptr;
        glm::vec2 SourceOffset{ 0.0f };
        const Mask* Selection = nullptr; // painting only where selected (none: everywhere)
        // Mirrors: 1 across the vertical line x = SymmetryCenter.x, 2 across the horizontal one (both: four dabs).
        int Symmetry = 0;
        glm::vec2 SymmetryCenter{ 0.0f };
        bool PixelPerfect = false; // the pencil: no doubled corner pixels on diagonals (one-pixel lines)
    };

    // The weight of a dab at `offset` pixels from its center (the tip's shape: roundness and angle).
    float DabWeight(const BrushSettings& brush, const glm::vec2& offset);
    // The same for a round tip, by distance.
    float DabWeight(const BrushSettings& brush, float distance);

    // One stroke on one layer (or its mask), from mouse down to mouse up. Painting tools build each pixel's coverage up
    // dab by dab (by Flow) and show the layer's pixels from the start of the stroke with the tool's change laid over by
    // coverage x Opacity; Smudge, Blur and Sharpen work on the pixels as they are, dab after dab.
    class Stroke
    {
    public:
        Stroke(Document& doc, int layer, const BrushSettings& brush, bool mask = false);
        ~Stroke();
        // The brush moved to `p` (pixels; the first call puts down one dab, later ones dab along the way from the last).
        void To(const glm::vec2& p);
        // The end of the stroke (Heal matches its colors here); called by the destructor when not called before.
        void Finish();
        // The pixels changed since the last call.
        Rect TakeChanged();
        const Rect& Bounds() const { return m_Bounds; } // all the stroke changed

    private:
        void Dab(const glm::vec2& center);
        void DabAt(const glm::vec2& center);
        void Heal();
        float Sel(size_t i) const { return m_Brush.Selection ? m_Brush.Selection->Values[i] * (1.0f / 255.0f) : 1.0f; }

        Document& m_Doc;
        int m_Layer;
        bool m_Mask;
        BrushSettings m_Brush;
        std::shared_ptr<const Bitmap> m_Original;
        std::shared_ptr<const Mask> m_OriginalMask;
        std::vector<float> m_Coverage;
        std::vector<glm::vec4> m_Carried; // Smudge: the colors the brush holds (premultiplied), around its center
        int m_CarriedSide = 0;
        bool m_HasCarried = false;
        glm::vec2 m_Last{ 0.0f };
        float m_Carry = 0.0f; // distance walked since the last dab
        bool m_Started = false, m_Finished = false;
        Rect m_Changed, m_Bounds;
        std::vector<glm::ivec2> m_Trail; // (pixel-perfect: the pencil's last pixels)
    };

    struct FillSettings
    {
        glm::vec4 Color{ 0.0f, 0.0f, 0.0f, 1.0f };
        float Tolerance = 32.0f;   // 0..255: how far a pixel's channels may be from the clicked one's
        bool Contiguous = true;    // only pixels joined to the clicked one
        bool SampleAllLayers = false;
        float Opacity = 1.0f;
        const Mask* Selection = nullptr;
    };
    // The paint bucket at pixel `at` on `layer`; returns the changed pixels.
    Rect Fill(Document& doc, int layer, glm::ivec2 at, const FillSettings& fill);
    // Which pixels of `source` match the one at `at` (1 = yes), as Fill picks them.
    std::vector<uint8_t> MatchRegion(const Bitmap& source, glm::ivec2 at, float tolerance, bool contiguous);

    enum class GradientShape { Linear, Radial, Angle, Reflected, Diamond, Count };
    const char* GradientShapeName(GradientShape shape);
    struct GradientSettings
    {
        GradientShape Shape = GradientShape::Linear;
        glm::vec4 From{ 0, 0, 0, 1 }, To{ 1, 1, 1, 1 }; // straight colors (alpha 0: transparent)
        float Opacity = 1.0f;
        bool Reverse = false;
        bool Dither = true; // a little noise against banding
        const Mask* Selection = nullptr;
    };
    // Where `p` falls (0..1) on the gradient dragged from `a` to `b`.
    float GradientAt(GradientShape shape, const glm::vec2& a, const glm::vec2& b, const glm::vec2& p);
    // The gradient over the whole layer (or its mask, by the colors' gray); returns the changed pixels.
    Rect DrawGradient(Document& doc, int layer, const glm::vec2& a, const glm::vec2& b, const GradientSettings& gradient, bool mask = false);

    // The color at a pixel, averaged over a square `radius` pixels around it (0 = the pixel alone); straight alpha.
    glm::vec4 SampleColor(const Bitmap& image, glm::ivec2 at, int radius = 0);
}
