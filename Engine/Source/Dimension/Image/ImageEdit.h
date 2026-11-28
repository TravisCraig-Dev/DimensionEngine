#pragma once
#include "Dimension/Image/ImageDocument.h"

// Image Mode's selections (soft masks of the canvas: 255 selected, 0 not), transforms (pixels carried by a projective
// map: move, scale, rotate, skew, distort, perspective) and whole-canvas changes (crop, canvas size, image size, rotate
// and flip the canvas).
namespace Dimension::Imaging
{
    // How a new selection joins the one there is (Photoshop's New, Add, Subtract, Intersect).
    enum class SelectOp { Replace, Add, Subtract, Intersect };

    // Shapes: the rectangle (or the ellipse in it) between two corners, and a closed polygon (antialiased: the pixels'
    // covered parts; else whole pixels whose centers are inside).
    Mask SelectRect(uint32_t width, uint32_t height, const glm::vec2& a, const glm::vec2& b, bool ellipse, bool antialias);
    Mask SelectPolygon(uint32_t width, uint32_t height, const std::vector<glm::vec2>& points, bool antialias);
    // The magic wand: pixels like the one at `at` (tolerance 0..255), joined to it or anywhere.
    Mask SelectColor(const Bitmap& image, glm::ivec2 at, float tolerance, bool contiguous);
    // Color Range: every pixel by how close it is to `color` (fuzziness 0..255: soft edges).
    Mask SelectColorRange(const Bitmap& image, const glm::vec4& color, float fuzziness);
    // A layer's transparency as a selection (Ctrl+click its thumbnail).
    Mask SelectAlpha(const Bitmap& image);

    void Combine(Mask& selection, const Mask& shape, SelectOp op);
    void InvertSelection(Mask& selection);
    void Feather(Mask& selection, float radius);
    void Expand(Mask& selection, int pixels); // negative: contract
    Mask Border(const Mask& selection, int width);
    bool IsEmpty(const Mask& selection);
    Rect Bounds(const Mask& selection, uint8_t threshold = 1);

    // The selection's edge (between pixels at least half selected and the rest) as horizontal and vertical runs, in
    // pixel corners; at most `limit` of them.
    struct Segment
    {
        glm::ivec2 A, B;
    };
    std::vector<Segment> Outline(const Mask& selection, size_t limit = 1000000);

    // ---- Pixels and selections

    Rect ContentBounds(const Bitmap& image); // the pixels that are not clear
    // The selected pixels (alpha by the selection; all of them without one), cropped to `bounds` (canvas-relative).
    Bitmap CopyPixels(const Bitmap& image, const Mask* selection, Rect& bounds);
    // `piece` drawn over `image` with its top-left at `at`.
    void Blit(Bitmap& image, const Bitmap& piece, glm::ivec2 at);
    // Alpha taken away where selected (all of it without a selection); returns the changed pixels.
    Rect ClearPixels(Bitmap& image, const Mask* selection);
    // A mask's values moved towards `value` where selected.
    Rect FillMask(Mask& mask, const Mask* selection, uint8_t value);
    // Edit > Fill: the color laid over the layer where selected (or its mask by the color's gray).
    Rect FillSelection(Document& doc, int layer, const glm::vec4& color, const Mask* selection, bool mask);

    // ---- Transforms

    enum class Resample { Nearest, Bilinear };
    // The projective map taking the four points `from` to the four points `to` (pixel coordinates).
    glm::mat3 Homography(const glm::vec2 from[4], const glm::vec2 to[4]);
    glm::vec2 Apply(const glm::mat3& m, const glm::vec2& p);
    // The pixels (only the selected ones when there is a selection: the rest stay) carried by `m` (from where they are
    // to where they go), laid over what stays.
    Bitmap TransformPixels(const Bitmap& image, const glm::mat3& m, const Mask* selection, Resample method);
    // A mask (a layer's, or the selection itself) carried by `m`; `outside` where nothing lands.
    Mask TransformMask(const Mask& mask, const glm::mat3& m, uint8_t outside, Resample method);

    Bitmap ResampleBitmap(const Bitmap& image, uint32_t width, uint32_t height, Resample method);
    Mask ResampleMask(const Mask& mask, uint32_t width, uint32_t height, Resample method);

    // ---- The canvas (every layer, mask and the selection)

    // A canvas `width` x `height` with the old one's top-left at `offset` (crop: negative; Canvas Size: the anchor's
    // place). New areas are clear (masks: white).
    void Reframe(Document& doc, uint32_t width, uint32_t height, glm::ivec2 offset);
    // Image Size: everything resampled to `width` x `height`.
    void ResizeImage(Document& doc, uint32_t width, uint32_t height, Resample method);
    // Quarter turns clockwise (1, 2, 3; negative: counterclockwise).
    void RotateCanvas(Document& doc, int quarters);
    void FlipCanvas(Document& doc, bool horizontal);
}
