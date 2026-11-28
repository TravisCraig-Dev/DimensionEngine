#pragma once
#include "Dimension/Image/ImageDocument.h"
#include <string>
#include <vector>

// Image Mode's tools for game art: tiling textures (offset with wrap-around, seams blended away), normal maps from
// height, and palettes (built in and from .gpl/.hex/.pal files; colors reduced to one, with dithering).
namespace Dimension::Imaging
{
    // The picture moved by `by` pixels, what leaves one side coming in on the other (Photoshop's Offset, wrap around).
    Bitmap OffsetWrap(const Bitmap& image, glm::ivec2 by);
    // Tileable: the picture blended with itself moved half its size, the seams hidden; `blend` (0..1) how wide the
    // blending band is.
    Bitmap MakeSeamless(const Bitmap& image, float blend);
    // A normal map from the picture as height (its gray, by alpha): RGB = the surface's normal * 0.5 + 0.5. `invertY`:
    // green down (DirectX); `wrap`: the edges see the other side (for tiling textures).
    Bitmap NormalMap(const Bitmap& height, float strength, bool invertY, bool wrap);

    struct Palette
    {
        std::string Name;
        std::vector<glm::vec3> Colors; // 0..1, sRGB
        std::string File;               // (where it was read from; empty for built-in ones)
    };
    const std::vector<Palette>& BuiltInPalettes();
    // .gpl (GIMP), .hex (one RRGGBB a line) and .pal (JASC).
    bool LoadPalette(const fs::path& file, Palette& out);
    bool SavePalette(const fs::path& file, const Palette& palette); // (.gpl)
    // The picture's most important colors (median cut).
    std::vector<glm::vec3> MedianCut(const Bitmap& image, int count);

    enum class Dither { None, FloydSteinberg, Ordered, Count };
    const char* DitherName(Dither dither);
    // Every color the nearest of the palette's; dithering spreads the difference. `hardAlpha`: pixels fully clear or
    // fully solid (pixel art). Blended by the selection where there is one.
    Bitmap Quantize(const Bitmap& image, const std::vector<glm::vec3>& palette, Dither dither, bool hardAlpha, const Mask* selection = nullptr);
    int NearestColor(const std::vector<glm::vec3>& palette, const glm::vec3& color);
}
