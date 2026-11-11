#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

struct stbtt_fontinfo;

namespace Dimension
{
    // COLR version 1 color glyphs (the gradient emoji of Windows 11's Segoe UI Emoji, Noto Color Emoji's COLRv1 build):
    // each base glyph is a graph of paints (layers, solid colors, linear / radial / sweep gradients, glyph outlines as
    // clips, transforms and compositing) rasterized here on the CPU into a straight-alpha RGBA image. Variations are
    // read at their default values.
    namespace ColrV1
    {
        struct Table
        {
            const uint8_t* Colr = nullptr; // the COLR table
            size_t Size = 0;
            uint32_t BaseList = 0, LayerList = 0, ClipList = 0; // offsets in it (0 = none)

            // From the COLR table; false when it is version 0 (or broken).
            bool Load(const uint8_t* colr, size_t size);
            // The base glyph's paint (an offset in the table, 0 when the glyph has none).
            uint32_t FindPaint(uint16_t glyph) const;
            // The glyph's clip box in font units (false when the clip list has none).
            bool ClipBox(uint16_t glyph, float& xMin, float& yMin, float& xMax, float& yMax) const;
        };

        // Renders a base glyph at `scale` (pixels per font unit) with the palette (RGBA8, sRGB; 0xFFFF entries are the
        // text's color, white). The image covers pixels [x0, x0 + width) x [y0, y0 + height) in stb_truetype's bitmap
        // space (y down), with a clear pixel all round. False when the glyph has no paint or covers nothing.
        bool Render(const Table& table, const stbtt_fontinfo& font, float scale, uint16_t glyph, const std::vector<uint32_t>& palette,
                    std::vector<uint32_t>& rgba, int& x0, int& y0, int& width, int& height);
        // The same from a whole font file (its first font, palette 0) for a character at a pixel height; false when the
        // font has no COLR v1 paint for it. (A tool and test entry: fonts render through Font.)
        bool RenderCodepoint(const uint8_t* fontData, size_t size, uint32_t codepoint, float pixelHeight, std::vector<uint32_t>& rgba,
                             int& width, int& height);
    }
}
