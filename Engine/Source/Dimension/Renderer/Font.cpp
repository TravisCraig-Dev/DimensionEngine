#include "Dimension/Platform/Platform.h"
#include <array>
#include <cstdlib>
#include <chrono>
#include "Dimension/Renderer/Font.h"
#include "Dimension/Renderer/Texture.h"
#include "Dimension/Core/Log.h"
#include <fstream>
#include <climits>
#include <cstring>

#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>
#include <bit>
#include <unordered_map>
#include "Dimension/Renderer/ColrV1.h"
#include <unordered_map>
#include <msdfgen.h>

namespace Dimension
{
    namespace
    {
        // A glyph's outline as an msdfgen shape (font units, y up); false for glyphs without one (space).
        bool GlyphShape(const stbtt_fontinfo& info, int glyph, msdfgen::Shape& shape)
        {
            stbtt_vertex* v = nullptr;
            int count = stbtt_GetGlyphShape(&info, glyph, &v);
            if (count <= 0) return false;
            msdfgen::Contour* contour = nullptr;
            msdfgen::Point2 at;
            for (int i = 0; i < count; ++i)
            {
                msdfgen::Point2 p(v[i].x, v[i].y);
                switch (v[i].type)
                {
                case STBTT_vmove: contour = &shape.addContour(); break;
                case STBTT_vline: if (contour && p != at) contour->addEdge(msdfgen::EdgeHolder(at, p)); break;
                case STBTT_vcurve: if (contour) contour->addEdge(msdfgen::EdgeHolder(at, msdfgen::Point2(v[i].cx, v[i].cy), p)); break;
                case STBTT_vcubic:
                    if (contour) contour->addEdge(msdfgen::EdgeHolder(at, msdfgen::Point2(v[i].cx, v[i].cy), msdfgen::Point2(v[i].cx1, v[i].cy1), p));
                    break;
                }
                at = p;
            }
            stbtt_FreeShape(&info, v);
            shape.normalize();
            if (shape.contours.empty()) return false;
            msdfgen::edgeColoringSimple(shape, 3.0);
            return true;
        }

        // Decodes one UTF-8 codepoint; invalid sequences yield U+FFFD.
        uint32_t NextCodepoint(std::string_view s, size_t& i)
        {
            uint8_t c = (uint8_t)s[i++];
            if (c < 0x80) return c;
            int extra = (c >> 5) == 0x6 ? 1 : (c >> 4) == 0xE ? 2 : (c >> 3) == 0x1E ? 3 : -1;
            if (extra < 0) return 0xFFFD;
            uint32_t cp = c & (0x3F >> extra);
            for (int k = 0; k < extra; ++k)
            {
                if (i >= s.size() || ((uint8_t)s[i] & 0xC0) != 0x80) return 0xFFFD;
                cp = (cp << 6) | ((uint8_t)s[i++] & 0x3F);
            }
            return cp;
        }
    }

    namespace
    {
        uint16_t ReadU16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }
        uint32_t ReadU32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

        // A font's color glyph tables (COLR version 0 records, which COLR v1 fonts keep for older readers, and the first
        // CPAL palette): each base glyph is drawn as a stack of plain glyphs, each filled with one palette color.
        struct ColorTables
        {
            const uint8_t* Bases = nullptr; // BaseGlyphRecord: glyph, first layer, layer count (sorted by glyph)
            const uint8_t* Layers = nullptr; // LayerRecord: glyph, palette index (0xFFFF: the text's color)
            uint32_t BaseCount = 0, LayerCount = 0;
            std::vector<uint32_t> Palette; // RGBA8 (sRGB), from CPAL's palette 0
            ColrV1::Table V1;               // COLR version 1 paints (gradients), used before the version 0 layers
            bool HasV1 = false;

            bool Load(const uint8_t* data, size_t size, uint32_t fontStart)
            {
                if (fontStart + 12 > size) return false;
                uint32_t colr = 0, cpal = 0, colrLength = 0, cpalLength = 0;
                uint16_t tables = ReadU16(data + fontStart + 4);
                for (uint16_t i = 0; i < tables; ++i)
                {
                    const uint8_t* rec = data + fontStart + 12 + 16 * i;
                    if (rec + 16 > data + size) return false;
                    if (!std::memcmp(rec, "COLR", 4)) colr = ReadU32(rec + 8), colrLength = ReadU32(rec + 12);
                    if (!std::memcmp(rec, "CPAL", 4)) cpal = ReadU32(rec + 8), cpalLength = ReadU32(rec + 12);
                }
                if (!colr || !cpal || colr + 14 > size || cpal + 12 > size || colr + colrLength > size || cpal + cpalLength > size) return false;
                const uint8_t* c = data + colr;
                HasV1 = V1.Load(c, colrLength);
                BaseCount = ReadU16(c + 2);
                uint32_t baseOffset = ReadU32(c + 4), layerOffset = ReadU32(c + 8);
                LayerCount = ReadU16(c + 12);
                if (baseOffset + BaseCount * 6ull > colrLength || layerOffset + LayerCount * 4ull > colrLength) BaseCount = LayerCount = 0;
                if (!BaseCount && !HasV1) return false;
                Bases = c + baseOffset;
                Layers = c + layerOffset;
                const uint8_t* p = data + cpal;
                uint16_t entries = ReadU16(p + 2), records = ReadU16(p + 6);
                uint32_t recordsOffset = ReadU32(p + 8);
                uint16_t first = ReadU16(p + 12);
                if (recordsOffset + records * 4ull > cpalLength || first + entries > records) return false;
                for (uint16_t i = 0; i < entries; ++i)
                {
                    const uint8_t* bgra = p + recordsOffset + (first + i) * 4;
                    Palette.push_back((uint32_t)bgra[2] | (uint32_t)bgra[1] << 8 | (uint32_t)bgra[0] << 16 | (uint32_t)bgra[3] << 24);
                }
                return true;
            }
            // The layers of a glyph: (first, count), count 0 when it is not a color glyph.
            std::pair<uint32_t, uint32_t> Find(int glyph) const
            {
                uint32_t lo = 0, hi = BaseCount;
                while (lo < hi)
                {
                    uint32_t mid = (lo + hi) / 2;
                    uint16_t g = ReadU16(Bases + mid * 6);
                    if (g == glyph) return { ReadU16(Bases + mid * 6 + 2), ReadU16(Bases + mid * 6 + 4) };
                    if (g < glyph) lo = mid + 1;
                    else hi = mid;
                }
                return { 0, 0 };
            }
        };
    }

    namespace
    {
        // Glyph substitution (GSUB) as emoji fonts use it: the lookups of the ccmp, liga, rlig, calt and clig features, in
        // order, each over the whole run; single substitutions, ligatures (a skin tone, a profession, a rainbow flag)
        // and chaining contexts by coverage (a family's members turned into the narrower forms that overlap), through
        // extension lookups too. Other lookup kinds are skipped.
        struct GlyphSubstitution
        {
            const uint8_t* Table = nullptr;
            size_t Size = 0;
            std::vector<uint16_t> Lookups; // the features' lookups, in order

            bool In(const uint8_t* p, size_t bytes) const { return Table && p >= Table && p + bytes <= Table + Size; }
            uint16_t U16(const uint8_t* p) const { return In(p, 2) ? ReadU16(p) : 0; }
            uint32_t U32(const uint8_t* p) const { return In(p, 4) ? ReadU32(p) : 0; }

            void Load(const uint8_t* data, size_t size, uint32_t fontStart)
            {
                if (fontStart + 12 > size) return;
                uint16_t tables = ReadU16(data + fontStart + 4);
                for (uint16_t i = 0; i < tables; ++i)
                {
                    const uint8_t* rec = data + fontStart + 12 + 16 * i;
                    if (rec + 16 > data + size) return;
                    if (std::memcmp(rec, "GSUB", 4)) continue;
                    uint32_t offset = ReadU32(rec + 8), length = ReadU32(rec + 12);
                    if (offset + (size_t)length > size || length < 10) return;
                    Table = data + offset;
                    Size = length;
                }
                if (!Table) return;
                const uint8_t* features = Table + U16(Table + 6);
                std::vector<uint16_t> lookups;
                for (uint16_t i = 0, n = U16(features); i < n; ++i)
                {
                    const uint8_t* rec = features + 2 + 6 * i;
                    if (!In(rec, 6)) break;
                    if (std::memcmp(rec, "ccmp", 4) && std::memcmp(rec, "liga", 4) && std::memcmp(rec, "rlig", 4) && std::memcmp(rec, "calt", 4) &&
                        std::memcmp(rec, "clig", 4))
                        continue;
                    const uint8_t* feature = features + U16(rec + 4);
                    for (uint16_t k = 0, m = U16(feature + 2); k < m; ++k) lookups.push_back(U16(feature + 4 + 2 * k));
                }
                std::sort(lookups.begin(), lookups.end());
                lookups.erase(std::unique(lookups.begin(), lookups.end()), lookups.end());
                Lookups = std::move(lookups);
            }

            int Coverage(const uint8_t* c, uint16_t glyph) const
            {
                uint16_t format = U16(c), count = U16(c + 2);
                if (format == 1)
                {
                    for (uint16_t i = 0; i < count; ++i)
                        if (U16(c + 4 + 2 * i) == glyph) return i;
                }
                else if (format == 2)
                    for (uint16_t i = 0; i < count; ++i)
                    {
                        const uint8_t* r = c + 4 + 6 * i;
                        uint16_t first = U16(r), last = U16(r + 2);
                        if (glyph >= first && glyph <= last) return U16(r + 4) + glyph - first;
                    }
                return -1;
            }

            // Applies lookup `index` at `pos`: true when something was substituted there (`consumed`: input glyphs it
            // covered, the run past them is next).
            bool ApplyAt(uint16_t index, std::vector<uint16_t>& glyphs, size_t pos, size_t& consumed, int depth = 0) const
            {
                if (depth > 4 || pos >= glyphs.size()) return false;
                const uint8_t* list = Table + U16(Table + 8);
                if (index >= U16(list)) return false;
                const uint8_t* lookup = list + U16(list + 2 + 2 * index);
                uint16_t type = U16(lookup), subtables = U16(lookup + 4);
                for (uint16_t j = 0; j < subtables; ++j)
                {
                    const uint8_t* st = lookup + U16(lookup + 6 + 2 * j);
                    uint16_t kind = type;
                    if (type == 7) // extension: the real subtable further away
                    {
                        kind = U16(st + 2);
                        st = st + U32(st + 4);
                    }
                    if (!In(st, 4)) continue;
                    if (kind == 1)
                    {
                        int ci = Coverage(st + U16(st + 2), glyphs[pos]);
                        if (ci < 0) continue;
                        glyphs[pos] = U16(st) == 1 ? (uint16_t)(glyphs[pos] + (int16_t)U16(st + 4)) : U16(st + 6 + 2 * ci);
                        consumed = 1;
                        return true;
                    }
                    if (kind == 4)
                    {
                        int ci = Coverage(st + U16(st + 2), glyphs[pos]);
                        if (ci < 0) continue;
                        const uint8_t* set = st + U16(st + 6 + 2 * ci);
                        uint16_t best = 0, bestCount = 0;
                        for (uint16_t k = 0, n = U16(set); k < n; ++k)
                        {
                            const uint8_t* lig = set + U16(set + 2 + 2 * k);
                            uint16_t count = U16(lig + 2);
                            if (count <= bestCount || pos + count > glyphs.size()) continue;
                            bool match = true;
                            for (uint16_t m = 1; m < count && match; ++m) match = glyphs[pos + m] == U16(lig + 4 + 2 * (m - 1));
                            if (match)
                            {
                                best = U16(lig);
                                bestCount = count;
                            }
                        }
                        if (!bestCount) continue;
                        glyphs[pos] = best;
                        glyphs.erase(glyphs.begin() + pos + 1, glyphs.begin() + pos + bestCount);
                        consumed = 1;
                        return true;
                    }
                    if (kind == 6 && U16(st) == 3)
                    {
                        const uint8_t* p = st + 2;
                        uint16_t backCount = U16(p);
                        const uint8_t* back = p + 2;
                        p += 2 + 2 * backCount;
                        uint16_t inputCount = U16(p);
                        const uint8_t* input = p + 2;
                        p += 2 + 2 * inputCount;
                        uint16_t aheadCount = U16(p);
                        const uint8_t* ahead = p + 2;
                        p += 2 + 2 * aheadCount;
                        uint16_t records = U16(p);
                        if (inputCount == 0 || pos < backCount || pos + inputCount + aheadCount > glyphs.size()) continue;
                        bool match = true;
                        for (uint16_t k = 0; k < inputCount && match; ++k) match = Coverage(st + U16(input + 2 * k), glyphs[pos + k]) >= 0;
                        for (uint16_t k = 0; k < backCount && match; ++k) match = Coverage(st + U16(back + 2 * k), glyphs[pos - 1 - k]) >= 0;
                        for (uint16_t k = 0; k < aheadCount && match; ++k) match = Coverage(st + U16(ahead + 2 * k), glyphs[pos + inputCount + k]) >= 0;
                        if (!match) continue;
                        for (uint16_t r = 0; r < records; ++r)
                        {
                            size_t used = 0;
                            ApplyAt(U16(p + 4 + 4 * r), glyphs, pos + U16(p + 2 + 4 * r), used, depth + 1);
                        }
                        consumed = inputCount;
                        return true;
                    }
                }
                return false;
            }

            void Shape(std::vector<uint16_t>& glyphs) const
            {
                if (!Table) return;
                for (uint16_t index : Lookups)
                    for (size_t pos = 0; pos < glyphs.size();)
                    {
                        size_t consumed = 0;
                        pos += ApplyAt(index, glyphs, pos, consumed) ? std::max<size_t>(consumed, 1) : 1;
                    }
            }
        };
    }

    namespace
    {
        // Glyph positioning (GPOS) as text layout needs it: the pair adjustments of the kern feature (glyph pairs and
        // class pairs) and the mark-to-base attachments of the mark feature (an accent, a vowel sign or an emoji
        // modifier placed on its base by the font's anchors), through extension lookups too.
        struct GlyphPositioning
        {
            const uint8_t* Table = nullptr;
            size_t Size = 0;
            std::vector<uint16_t> KernLookups, MarkLookups;

            bool In(const uint8_t* p, size_t bytes) const { return Table && p >= Table && p + bytes <= Table + Size; }
            uint16_t U16(const uint8_t* p) const { return In(p, 2) ? ReadU16(p) : 0; }
            int16_t S16(const uint8_t* p) const { return (int16_t)U16(p); }
            uint32_t U32(const uint8_t* p) const { return In(p, 4) ? ReadU32(p) : 0; }

            void Load(const uint8_t* data, size_t size, uint32_t fontStart)
            {
                if (fontStart + 12 > size) return;
                uint16_t tables = ReadU16(data + fontStart + 4);
                for (uint16_t i = 0; i < tables; ++i)
                {
                    const uint8_t* rec = data + fontStart + 12 + 16 * i;
                    if (rec + 16 > data + size) return;
                    if (std::memcmp(rec, "GPOS", 4)) continue;
                    uint32_t offset = ReadU32(rec + 8), length = ReadU32(rec + 12);
                    if (offset + (size_t)length > size || length < 10) return;
                    Table = data + offset;
                    Size = length;
                }
                if (!Table) return;
                const uint8_t* features = Table + U16(Table + 6);
                for (uint16_t i = 0, n = U16(features); i < n; ++i)
                {
                    const uint8_t* rec = features + 2 + 6 * i;
                    if (!In(rec, 6)) break;
                    std::vector<uint16_t>* into = !std::memcmp(rec, "kern", 4) ? &KernLookups : !std::memcmp(rec, "mark", 4) ? &MarkLookups : nullptr;
                    if (!into) continue;
                    const uint8_t* feature = features + U16(rec + 4);
                    for (uint16_t k = 0, m = U16(feature + 2); k < m; ++k) into->push_back(U16(feature + 4 + 2 * k));
                }
                for (auto* list : { &KernLookups, &MarkLookups })
                {
                    std::sort(list->begin(), list->end());
                    list->erase(std::unique(list->begin(), list->end()), list->end());
                }
            }

            int Coverage(const uint8_t* c, uint16_t glyph) const
            {
                uint16_t format = U16(c), count = U16(c + 2);
                if (format == 1)
                {
                    uint16_t lo = 0, hi = count; // sorted glyphs
                    while (lo < hi)
                    {
                        uint16_t mid = (uint16_t)((lo + hi) / 2), g = U16(c + 4 + 2 * mid);
                        if (g == glyph) return mid;
                        if (g < glyph) lo = mid + 1;
                        else hi = mid;
                    }
                }
                else if (format == 2)
                    for (uint16_t i = 0; i < count; ++i)
                    {
                        const uint8_t* r = c + 4 + 6 * i;
                        uint16_t first = U16(r), last = U16(r + 2);
                        if (glyph >= first && glyph <= last) return U16(r + 4) + glyph - first;
                    }
                return -1;
            }

            int ClassOf(const uint8_t* cd, uint16_t glyph) const
            {
                uint16_t format = U16(cd);
                if (format == 1)
                {
                    uint16_t start = U16(cd + 2), count = U16(cd + 4);
                    return glyph >= start && glyph < start + count ? U16(cd + 6 + 2 * (glyph - start)) : 0;
                }
                if (format == 2)
                    for (uint16_t i = 0, n = U16(cd + 2); i < n; ++i)
                    {
                        const uint8_t* r = cd + 4 + 6 * i;
                        if (glyph >= U16(r) && glyph <= U16(r + 2)) return U16(r + 4);
                    }
                return 0;
            }

            // Each subtable of a lookup (extensions resolved) until `f` says it applied.
            template <typename F>
            void Subtables(uint16_t index, F&& f) const
            {
                const uint8_t* list = Table + U16(Table + 8);
                if (index >= U16(list)) return;
                const uint8_t* lookup = list + U16(list + 2 + 2 * index);
                uint16_t type = U16(lookup), count = U16(lookup + 4);
                for (uint16_t j = 0; j < count; ++j)
                {
                    const uint8_t* st = lookup + U16(lookup + 6 + 2 * j);
                    uint16_t kind = type;
                    if (type == 9) // extension: the real subtable further away
                    {
                        kind = U16(st + 2);
                        st = st + U32(st + 4);
                    }
                    if (In(st, 4) && f(kind, st)) return;
                }
            }

            static int ValueSize(uint16_t format) { return 2 * std::popcount((unsigned)(format & 0xFF)); }
            static int XAdvanceAt(uint16_t format) { return format & 4 ? 2 * std::popcount((unsigned)(format & 3)) : -1; }

            bool HasKerning() const { return !KernLookups.empty(); }

            // The pair's advance adjustment (font units) for the first glyph.
            int Kern(uint16_t a, uint16_t b) const
            {
                int total = 0;
                for (uint16_t index : KernLookups)
                    Subtables(index, [&](uint16_t kind, const uint8_t* st) {
                        if (kind != 2) return false;
                        int ci = Coverage(st + U16(st + 2), a);
                        if (ci < 0) return false;
                        uint16_t f1 = U16(st + 4), f2 = U16(st + 6);
                        int x = XAdvanceAt(f1), size1 = ValueSize(f1), size2 = ValueSize(f2);
                        if (U16(st) == 1)
                        {
                            const uint8_t* set = st + U16(st + 10 + 2 * ci);
                            int lo = 0, hi = U16(set), stride = 2 + size1 + size2; // pairs sorted by the second glyph
                            while (lo < hi)
                            {
                                int mid = (lo + hi) / 2;
                                const uint8_t* rec = set + 2 + mid * stride;
                                uint16_t g = U16(rec);
                                if (g == b)
                                {
                                    if (x >= 0) total += S16(rec + 2 + x);
                                    return true;
                                }
                                if (g < b) lo = mid + 1;
                                else hi = mid;
                            }
                            return false;
                        }
                        if (U16(st) == 2)
                        {
                            int c1 = ClassOf(st + U16(st + 8), a), c2 = ClassOf(st + U16(st + 10), b);
                            uint16_t count1 = U16(st + 12), count2 = U16(st + 14);
                            if (c1 >= count1 || c2 >= count2) return false;
                            const uint8_t* rec = st + 16 + (size_t(c1) * count2 + c2) * (size1 + size2);
                            if (x >= 0) total += S16(rec + x);
                            return true;
                        }
                        return false;
                    });
                return total;
            }

            // Where a mark goes on its base: the base's anchor less the mark's (font units, y up). False when the
            // font does not attach them.
            bool Attach(uint16_t base, uint16_t mark, int& dx, int& dy) const
            {
                bool found = false;
                for (uint16_t index : MarkLookups)
                {
                    Subtables(index, [&](uint16_t kind, const uint8_t* st) {
                        if (kind != 4 || U16(st) != 1) return false;
                        int mi = Coverage(st + U16(st + 2), mark), bi = Coverage(st + U16(st + 4), base);
                        if (mi < 0 || bi < 0) return false;
                        uint16_t classes = U16(st + 6);
                        const uint8_t* marks = st + U16(st + 8);
                        const uint8_t* bases = st + U16(st + 10);
                        const uint8_t* record = marks + 2 + 4 * mi;
                        uint16_t markClass = U16(record);
                        if (markClass >= classes) return false;
                        uint16_t baseAnchor = U16(bases + 2 + (size_t(bi) * classes + markClass) * 2);
                        if (!baseAnchor) return false;
                        const uint8_t* ba = bases + baseAnchor;
                        const uint8_t* ma = marks + U16(record + 2);
                        dx = S16(ba + 2) - S16(ma + 2);
                        dy = S16(ba + 4) - S16(ma + 4);
                        found = true;
                        return true;
                    });
                    if (found) return true;
                }
                return false;
            }
        };
    }

    // The font file (stb_truetype reads it in place), the atlas pixels kept for glyphs baked later, and those glyphs.
    struct Font::Dynamic
    {
        std::vector<uint8_t> Data;
        stbtt_fontinfo Info{};
        float Scale = 1.0f;
        std::string Name;
        static constexpr int kWidth = 2048, kHeight = 1024, kSpread = 6;
        std::vector<uint32_t> Rgba;
        int PenX = 1, PenY = 1, RowHeight = 0;
        bool Full = false, Dirty = false;
        std::unordered_map<uint32_t, Glyph> Glyphs;
        std::unordered_map<uint32_t, bool> Has;
        // Color glyphs: their own atlas (allocated with the first one).
        ColorTables Colors;
        bool HasColors = false;
        GlyphSubstitution Substitution; // (color fonts: joined emoji sequences)
        GlyphPositioning Positioning;   // kerning and marks
        mutable std::unordered_map<uint32_t, float> KernCache; // first << 16 | second -> bake pixels
        static constexpr int kColorSize = 1024;
        std::vector<uint32_t> ColorRgba;
        int ColorPenX = 1, ColorPenY = 1, ColorRowHeight = 0;
        bool ColorFull = false, ColorDirty = false;

        // Bakes a color glyph: its layers rasterized at the bake size and stacked in their palette colors. False when
        // it is not one (or does not fit).
        bool BakeColor(int glyph, Glyph& g)
        {
            // COLR v1: the paint graph rendered (gradients and all).
            if (Colors.HasV1)
            {
                std::vector<uint32_t> image;
                int ix, iy, w, h;
                if (ColrV1::Render(Colors.V1, Info, Scale, (uint16_t)glyph, Colors.Palette, image, ix, iy, w, h))
                {
                    if (ColorRgba.empty()) ColorRgba.assign(size_t(kColorSize) * kColorSize, 0u);
                    if (ColorPenX + w + 1 > kColorSize) { ColorPenX = 1; ColorPenY += ColorRowHeight + 1; ColorRowHeight = 0; }
                    if (ColorPenY + h + 1 > kColorSize) { ColorFull = true; return false; }
                    for (int y = 0; y < h; ++y)
                        std::copy(image.begin() + size_t(y) * w, image.begin() + size_t(y + 1) * w, ColorRgba.begin() + size_t(ColorPenY + y) * kColorSize + ColorPenX);
                    int advance, bearing;
                    stbtt_GetGlyphHMetrics(&Info, glyph, &advance, &bearing);
                    g.Advance = advance * Scale;
                    g.UVMin = { ColorPenX / (float)kColorSize, ColorPenY / (float)kColorSize };
                    g.UVMax = { (ColorPenX + w) / (float)kColorSize, (ColorPenY + h) / (float)kColorSize };
                    g.Min = { (float)ix, (float)iy };
                    g.Max = { (float)(ix + w), (float)(iy + h) };
                    g.Color = true;
                    ColorPenX += w + 1;
                    ColorRowHeight = std::max(ColorRowHeight, h);
                    ColorDirty = true;
                    return true;
                }
            }
            auto [first, count] = Colors.Find(glyph);
            if (count == 0 || first + count > Colors.LayerCount) return false;
            int x0 = INT32_MAX, y0 = INT32_MAX, x1 = INT32_MIN, y1 = INT32_MIN;
            for (uint32_t i = 0; i < count; ++i)
            {
                int bx0, by0, bx1, by1;
                stbtt_GetGlyphBitmapBox(&Info, ReadU16(Colors.Layers + (first + i) * 4), Scale, Scale, &bx0, &by0, &bx1, &by1);
                if (bx1 <= bx0 || by1 <= by0) continue;
                x0 = std::min(x0, bx0), y0 = std::min(y0, by0), x1 = std::max(x1, bx1), y1 = std::max(y1, by1);
            }
            if (x1 <= x0 || y1 <= y0) return false;
            int w = x1 - x0 + 2, h = y1 - y0 + 2; // a pixel of clear border all round (filtering, mips)
            if (ColorRgba.empty()) ColorRgba.assign(size_t(kColorSize) * kColorSize, 0u);
            if (ColorPenX + w + 1 > kColorSize) { ColorPenX = 1; ColorPenY += ColorRowHeight + 1; ColorRowHeight = 0; }
            if (ColorPenY + h + 1 > kColorSize) { ColorFull = true; return false; }
            // Premultiplied sums while stacking, straight alpha in the atlas.
            std::vector<float> sum(size_t(w) * h * 4, 0.0f);
            std::vector<uint8_t> coverage;
            for (uint32_t i = 0; i < count; ++i)
            {
                int layerGlyph = ReadU16(Colors.Layers + (first + i) * 4);
                uint16_t paletteIndex = ReadU16(Colors.Layers + (first + i) * 4 + 2);
                uint32_t rgba = paletteIndex < Colors.Palette.size() ? Colors.Palette[paletteIndex] : 0xFFFFFFFFu; // 0xFFFF: the text's color (white here)
                float cr = (rgba & 255) / 255.0f, cg = (rgba >> 8 & 255) / 255.0f, cb = (rgba >> 16 & 255) / 255.0f, ca = (rgba >> 24) / 255.0f;
                int bx0, by0, bx1, by1;
                stbtt_GetGlyphBitmapBox(&Info, layerGlyph, Scale, Scale, &bx0, &by0, &bx1, &by1);
                int lw = bx1 - bx0, lh = by1 - by0;
                if (lw <= 0 || lh <= 0) continue;
                coverage.assign(size_t(lw) * lh, 0);
                stbtt_MakeGlyphBitmap(&Info, coverage.data(), lw, lh, lw, Scale, Scale, layerGlyph);
                for (int y = 0; y < lh; ++y)
                    for (int x = 0; x < lw; ++x)
                    {
                        float a = coverage[size_t(y) * lw + x] / 255.0f * ca;
                        if (a <= 0.0f) continue;
                        float* d = &sum[(size_t(by0 - y0 + 1 + y) * w + (bx0 - x0 + 1 + x)) * 4];
                        d[0] = cr * a + d[0] * (1.0f - a);
                        d[1] = cg * a + d[1] * (1.0f - a);
                        d[2] = cb * a + d[2] * (1.0f - a);
                        d[3] = a + d[3] * (1.0f - a);
                    }
            }
            auto toByte = [](float v) { return (uint32_t)std::clamp((int)std::lround(v * 255.0f), 0, 255); };
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                    const float* d = &sum[(size_t(y) * w + x) * 4];
                    float a = d[3], inv = a > 1e-6f ? 1.0f / a : 0.0f;
                    ColorRgba[size_t(ColorPenY + y) * kColorSize + ColorPenX + x] =
                        toByte(d[0] * inv) | toByte(d[1] * inv) << 8 | toByte(d[2] * inv) << 16 | toByte(a) << 24;
                }
            int advance, bearing;
            stbtt_GetGlyphHMetrics(&Info, glyph, &advance, &bearing);
            g.Advance = advance * Scale;
            g.UVMin = { ColorPenX / (float)kColorSize, ColorPenY / (float)kColorSize };
            g.UVMax = { (ColorPenX + w) / (float)kColorSize, (ColorPenY + h) / (float)kColorSize };
            g.Min = { (float)(x0 - 1), (float)(y0 - 1) };
            g.Max = { (float)(x1 + 1), (float)(y1 + 1) };
            g.Color = true;
            ColorPenX += w + 1;
            ColorRowHeight = std::max(ColorRowHeight, h);
            ColorDirty = true;
            return true;
        }

        // Bakes a glyph's multi-channel distance field into the atlas (shelf packing); false when it does not fit.
        // A codepoint, or a glyph by its index (kGlyphIndexBit set: shaped emoji sequences).
        bool Bake(uint32_t cp, Glyph& g)
        {
            constexpr int kWidthC = kWidth, kHeightC = kHeight;
            int glyph = (cp & kGlyphIndexBit) ? (int)(cp & 0xFFFFu) : stbtt_FindGlyphIndex(&Info, (int)cp);
            if (HasColors && glyph && BakeColor(glyph, g)) return true;
            auto toByte = [](float d) { return (uint32_t)std::clamp((int)std::lround(d * 255.0f), 0, 255); };
            int advance, bearing;
            stbtt_GetGlyphHMetrics(&Info, glyph, &advance, &bearing);
            g.Advance = advance * Scale;
            int x0, y0, x1, y1;
            stbtt_GetGlyphBitmapBox(&Info, glyph, Scale, Scale, &x0, &y0, &x1, &y1);
            if (x1 <= x0 || y1 <= y0) return true; // blank (a space)
            int w = x1 - x0 + 2 * kSpread, h = y1 - y0 + 2 * kSpread, xoff = x0 - kSpread, yoff = y0 - kSpread;
            if (PenX + w + 1 > kWidthC) { PenX = 1; PenY += RowHeight + 1; RowHeight = 0; }
            if (PenY + h + 1 > kHeightC) { Full = true; return false; }
            msdfgen::Shape shape;
            if (GlyphShape(Info, glyph, shape))
            {
                // Pixel (x, row from the bottom) <- font units: x = scale * (fx + tx), row = scale * (fy + ty).
                msdfgen::Bitmap<float, 4> field(w, h);
                msdfgen::Projection projection(msdfgen::Vector2(Scale), msdfgen::Vector2(-xoff / Scale, (h + yoff) / Scale));
                msdfgen::generateMTSDF(field, shape, msdfgen::SDFTransformation(projection, msdfgen::Range(2.0 * kSpread / Scale)));
                for (int y = 0; y < h; ++y)
                    for (int x = 0; x < w; ++x)
                    {
                        const float* d = field(x, h - 1 - y); // msdfgen rows go up, the atlas's down
                        Rgba[size_t(PenY + y) * kWidthC + PenX + x] = toByte(d[0]) | toByte(d[1]) << 8 | toByte(d[2]) << 16 | toByte(d[3]) << 24;
                    }
            }
            else
            {
                // No outline msdfgen can use: stb_truetype's single-channel field in all four channels.
                int sw = 0, sh = 0, sx = 0, sy = 0;
                unsigned char* sdf = stbtt_GetGlyphSDF(&Info, Scale, glyph, kSpread, 128, 128.0f / kSpread, &sw, &sh, &sx, &sy);
                if (!sdf) return true;
                for (int y = 0; y < std::min(sh, h); ++y)
                    for (int x = 0; x < std::min(sw, w); ++x)
                    {
                        uint32_t d = sdf[size_t(y) * sw + x];
                        Rgba[size_t(PenY + y) * kWidthC + PenX + x] = d | d << 8 | d << 16 | d << 24;
                    }
                stbtt_FreeSDF(sdf, nullptr);
            }
            g.UVMin = { PenX / (float)kWidthC, PenY / (float)kHeightC };
            g.UVMax = { (PenX + w) / (float)kWidthC, (PenY + h) / (float)kHeightC };
            g.Min = { (float)xoff, (float)yoff };
            g.Max = { (float)(xoff + w), (float)(yoff + h) };
            PenX += w + 1;
            RowHeight = std::max(RowHeight, h);
            Dirty = true;
            return true;
        }
    };

    // The baked Latin-1 glyphs of a font, cached per user (%LOCALAPPDATA%\Dimension\FontCache): the atlas rows they
    // use, the packing position and the glyph metrics, keyed by the font's bytes and the bake settings. Color fonts
    // (their glyphs in a second atlas) are not cached.
    namespace
    {
        constexpr uint32_t kFontCacheVersion = 1;

        fs::path FontCachePath(const std::vector<uint8_t>& data)
        {
            uint64_t h = 1469598103934665603ull;
            auto mix = [&](const void* p, size_t n) {
                for (size_t i = 0; i < n; ++i) h = (h ^ static_cast<const uint8_t*>(p)[i]) * 1099511628211ull;
            };
            mix(data.data(), data.size());
            uint32_t params[] = { kFontCacheVersion, (uint32_t)Font::kBakeSize, 6u, 2048u, 1024u, Font::kFirstChar, Font::kCharCount };
            mix(params, sizeof(params));
            fs::path base = Platform::UserCacheDirectory() / "Dimension";
            char name[32];
            std::snprintf(name, sizeof(name), "%016llx.fontcache", (unsigned long long)h);
            return base / "FontCache" / name;
        }

        struct FontCacheHeader
        {
            char Magic[4] = { 'D', 'F', 'N', 'T' };
            uint32_t Version = kFontCacheVersion;
            int32_t PenX = 0, PenY = 0, RowHeight = 0, Full = 0;
            uint32_t Rows = 0, Width = 0;
        };
    }

    bool Font::LoadBakedLatin1(Dynamic& dyn, std::array<Glyph, kCharCount>& glyphs)
    {
        if (dyn.HasColors) return false;
        std::ifstream in(FontCachePath(dyn.Data), std::ios::binary);
        if (!in) return false;
        FontCacheHeader h;
        in.read(reinterpret_cast<char*>(&h), sizeof(h));
        if (!in || std::memcmp(h.Magic, "DFNT", 4) != 0 || h.Version != kFontCacheVersion || h.Width != (uint32_t)Dynamic::kWidth ||
            h.Rows > (uint32_t)Dynamic::kHeight)
            return false;
        std::array<Glyph, kCharCount> read{};
        in.read(reinterpret_cast<char*>(read.data()), sizeof(read));
        std::vector<uint32_t> rows(size_t(h.Rows) * Dynamic::kWidth);
        in.read(reinterpret_cast<char*>(rows.data()), rows.size() * sizeof(uint32_t));
        if (!in) return false;
        std::copy(rows.begin(), rows.end(), dyn.Rgba.begin());
        glyphs = read;
        dyn.PenX = h.PenX, dyn.PenY = h.PenY, dyn.RowHeight = h.RowHeight, dyn.Full = h.Full != 0;
        dyn.Dirty = true;
        return true;
    }

    void Font::StoreBakedLatin1(const Dynamic& dyn, const std::array<Glyph, kCharCount>& glyphs)
    {
        if (dyn.HasColors) return;
        fs::path path = FontCachePath(dyn.Data);
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);
        FontCacheHeader h;
        h.PenX = dyn.PenX, h.PenY = dyn.PenY, h.RowHeight = dyn.RowHeight, h.Full = dyn.Full ? 1 : 0;
        h.Width = Dynamic::kWidth;
        h.Rows = (uint32_t)std::min(dyn.PenY + dyn.RowHeight + 1, Dynamic::kHeight);
        fs::path temp = path;
        temp += ".tmp";
        {
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            if (!out) return;
            out.write(reinterpret_cast<const char*>(&h), sizeof(h));
            out.write(reinterpret_cast<const char*>(glyphs.data()), sizeof(glyphs));
            out.write(reinterpret_cast<const char*>(dyn.Rgba.data()), size_t(h.Rows) * Dynamic::kWidth * sizeof(uint32_t));
            if (!out) return;
        }
        fs::rename(temp, path, ec); // (whole or not at all: another editor may be reading)
        if (ec) fs::remove(temp, ec);
    }

    Ref<Font> Font::LoadFromMemory(const uint8_t* data, size_t size, const std::string& name, bool bakeLatin1)
    {
        auto dyn = std::make_shared<Dynamic>();
        dyn->Data.assign(data, data + size);
        dyn->Name = name;
        if (!stbtt_InitFont(&dyn->Info, dyn->Data.data(), stbtt_GetFontOffsetForIndex(dyn->Data.data(), 0)))
        {
            Log::Error("Invalid font '{}'", name);
            return nullptr;
        }

        // Multi-channel signed distance field glyphs (MTSDF): RGB hold three distances whose median is the distance to
        // the outline with its corners kept sharp, alpha the true distance (for outlines and soft effects); 0.5 is on
        // the outline, with kSpread pixels of falloff each way at the bake size. Text stays crisp at any size.
        dyn->Rgba.assign(size_t(Dynamic::kWidth) * Dynamic::kHeight, 0u); // outside everywhere (the gutters too)
        dyn->HasColors = dyn->Colors.Load(dyn->Data.data(), dyn->Data.size(), (uint32_t)stbtt_GetFontOffsetForIndex(dyn->Data.data(), 0));
        if (dyn->HasColors) dyn->Substitution.Load(dyn->Data.data(), dyn->Data.size(), (uint32_t)stbtt_GetFontOffsetForIndex(dyn->Data.data(), 0));
        dyn->Positioning.Load(dyn->Data.data(), dyn->Data.size(), (uint32_t)stbtt_GetFontOffsetForIndex(dyn->Data.data(), 0));
        auto font = MakeRef<Font>();
        font->m_Dynamic = dyn;
        font->m_Latin1Baked = bakeLatin1;
        dyn->Scale = stbtt_ScaleForPixelHeight(&dyn->Info, kBakeSize);
        int ascent, descent, lineGap;
        stbtt_GetFontVMetrics(&dyn->Info, &ascent, &descent, &lineGap);
        font->m_Ascent = ascent * dyn->Scale;
        font->m_Descent = descent * dyn->Scale;
        font->m_LineGap = lineGap * dyn->Scale;
        if (bakeLatin1 && !LoadBakedLatin1(*dyn, font->m_Glyphs))
        {
            // Baking 224 distance fields takes a while (seconds in a debug build): kept for the next start.
            for (uint32_t i = 0; i < kCharCount; ++i) dyn->Bake(kFirstChar + i, font->m_Glyphs[i]);
            StoreBakedLatin1(*dyn, font->m_Glyphs);
        }
        if (dyn->Full) Log::Warn("Font '{}': some glyphs did not fit in the atlas", name);
        font->GetAtlasIndex(); // the atlas texture, now
        return font;
    }

    uint32_t Font::GetAtlasIndex() const
    {
        if (m_Dynamic && (m_Dynamic->Dirty || !m_Atlas))
        {
            // Glyphs were baked since the atlas went up: upload it again (the old one goes once the GPU is done with it).
            TextureSettings settings;
            settings.SRGB = false;
            settings.GenerateMips = true;
            m_Atlas = MakeRef<Texture2D>(m_Dynamic->Rgba.data(), Dynamic::kWidth, Dynamic::kHeight, settings, m_Dynamic->Name.c_str());
            m_Dynamic->Dirty = false;
        }
        return m_Atlas ? (m_Atlas->GetBindlessIndex() | kDistanceFieldBit | kMultiChannelBit) : 0u;
    }

    bool Font::HasColorGlyphs() const { return m_Dynamic && m_Dynamic->HasColors; }

    float Font::KernPx(int first, int second) const
    {
        if (!m_Dynamic || first <= 0 || second <= 0) return 0.0f;
        uint32_t key = (uint32_t)first << 16 | (uint32_t)(second & 0xFFFF);
        if (auto it = m_Dynamic->KernCache.find(key); it != m_Dynamic->KernCache.end()) return it->second;
        int units = m_Dynamic->Positioning.HasKerning() ? m_Dynamic->Positioning.Kern((uint16_t)first, (uint16_t)second)
                                                         : stbtt_GetGlyphKernAdvance(&m_Dynamic->Info, first, second);
        return m_Dynamic->KernCache[key] = units * m_Dynamic->Scale;
    }

    uint32_t Font::GetColorAtlasIndex() const
    {
        if (!m_Dynamic || m_Dynamic->ColorRgba.empty()) return 0u;
        if (m_Dynamic->ColorDirty || !m_ColorAtlas)
        {
            TextureSettings settings;
            settings.SRGB = true; // palette colors are sRGB
            settings.GenerateMips = true;
            m_ColorAtlas = MakeRef<Texture2D>(m_Dynamic->ColorRgba.data(), Dynamic::kColorSize, Dynamic::kColorSize, settings, (m_Dynamic->Name + " (color)").c_str());
            m_Dynamic->ColorDirty = false;
        }
        return m_ColorAtlas->GetBindlessIndex();
    }

    Ref<Font> Font::LoadFromFile(const fs::path& path, bool bakeLatin1)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in)
        {
            Log::Error("Cannot open font '{}'", path.string());
            return nullptr;
        }
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        return LoadFromMemory(bytes.data(), bytes.size(), path.filename().string(), bakeLatin1);
    }

    bool Font::HasGlyph(uint32_t cp) const
    {
        if (cp == ' ' || cp == '\t') return true;
        if (cp & kGlyphIndexBit) return m_Dynamic != nullptr; // a shaped glyph of this font
        if (!m_Dynamic) return cp >= kFirstChar && cp < kFirstChar + kCharCount;
        auto it = m_Dynamic->Has.find(cp);
        if (it != m_Dynamic->Has.end()) return it->second;
        bool has = stbtt_FindGlyphIndex(&m_Dynamic->Info, (int)cp) != 0;
        m_Dynamic->Has[cp] = has;
        return has;
    }

    const Glyph& Font::GetGlyph(uint32_t cp) const
    {
        if (m_Latin1Baked && cp >= kFirstChar && cp < kFirstChar + kCharCount) return m_Glyphs[cp - kFirstChar];
        if (m_Dynamic)
        {
            if (auto it = m_Dynamic->Glyphs.find(cp); it != m_Dynamic->Glyphs.end()) return it->second;
            if (HasGlyph(cp))
            {
                Glyph g;
                if (m_Dynamic->Bake(cp, g) || g.Advance > 0.0f) return m_Dynamic->Glyphs[cp] = g;
                if (m_Dynamic->ColorFull)
                {
                    static bool colorWarned = false;
                    if (!colorWarned) Log::Warn("Font '{}': its color atlas is full", m_Dynamic->Name);
                    colorWarned = true;
                }
                static bool warned = false;
                if (!warned) Log::Warn("Font '{}': its atlas is full; later characters show as '?'", m_Dynamic->Name);
                warned = true;
            }
        }
        if (cp != '?') return GetGlyph('?');
        static const Glyph none;
        return m_Latin1Baked ? m_Glyphs['?' - kFirstChar] : none;
    }

    namespace
    {
        std::vector<Ref<Font>>& Fallbacks()
        {
            static std::vector<Ref<Font>> fonts;
            return fonts;
        }
        bool& FallbacksSet()
        {
            static bool set = false;
            return set;
        }
    }

    void Font::SetFallbackFonts(std::vector<Ref<Font>> fonts)
    {
        Fallbacks() = std::move(fonts);
        FallbacksSet() = true;
    }

    void Font::ReleaseFallbackFonts()
    {
        Fallbacks().clear();
        FallbacksSet() = false;
    }

    const std::vector<Ref<Font>>& Font::GetFallbackFonts()
    {
        if (!FallbacksSet())
        {
            FallbacksSet() = true;
            std::vector<fs::path> candidates;
#ifdef _WIN32
            for (const char* name : { "segoeui.ttf", "seguiemj.ttf", "seguisym.ttf", "msyh.ttc", "malgun.ttf", "Nirmala.ttf" })
                candidates.push_back(fs::path("C:/Windows/Fonts") / name);
#else
            for (const char* p : { "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
                                   "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", "/System/Library/Fonts/Supplemental/Arial Unicode.ttf" })
                candidates.push_back(p);
#endif
            std::error_code ec;
            for (const fs::path& p : candidates)
                if (fs::exists(p, ec))
                    if (auto f = LoadFromFile(p, false)) Fallbacks().push_back(f);
        }
        return Fallbacks();
    }

    namespace
    {
        // Arabic letters U+0621..U+064A: their first presentation form (isolated; then final, initial, medial for
        // letters joining both sides) in Arabic Presentation Forms-B, and whether they join the next letter.
        struct ArabicForms
        {
            uint16_t Isolated;
            bool Dual;
        };
        const ArabicForms* ArabicLetter(uint32_t cp)
        {
            static const ArabicForms table[] = {
                { 0xFE80, false }, { 0xFE81, false }, { 0xFE83, false }, { 0xFE85, false }, { 0xFE87, false }, { 0xFE89, true },  // 0621-0626
                { 0xFE8D, false }, { 0xFE8F, true },  { 0xFE93, false }, { 0xFE95, true },  { 0xFE99, true },  { 0xFE9D, true },  // 0627-062C
                { 0xFEA1, true },  { 0xFEA5, true },  { 0xFEA9, false }, { 0xFEAB, false }, { 0xFEAD, false }, { 0xFEAF, false }, // 062D-0632
                { 0xFEB1, true },  { 0xFEB5, true },  { 0xFEB9, true },  { 0xFEBD, true },  { 0xFEC1, true },  { 0xFEC5, true },  // 0633-0638
                { 0xFEC9, true },  { 0xFECD, true },  { 0, false },      { 0, false },      { 0, false },      { 0, false },      // 0639-063E
                { 0, false },      { 0, false },      { 0xFED1, true },  { 0xFED5, true },  { 0xFED9, true },  { 0xFEDD, true },  // 063F-0644
                { 0xFEE1, true },  { 0xFEE5, true },  { 0xFEE9, true },  { 0xFEED, false }, { 0xFEEF, false }, { 0xFEF1, true },  // 0645-064A
            };
            if (cp < 0x0621 || cp > 0x064A) return nullptr;
            const ArabicForms& f = table[cp - 0x0621];
            return f.Isolated ? &f : nullptr;
        }
        bool Transparent(uint32_t cp) { return (cp >= 0x064B && cp <= 0x065F) || cp == 0x0670; } // harakat
        bool JoinsNext(uint32_t cp) { return cp == 0x0640 || (ArabicLetter(cp) && ArabicLetter(cp)->Dual); }
        bool JoinsPrev(uint32_t cp) { return cp == 0x0640 || ArabicLetter(cp); }

        enum class BidiClass { L, R, EN, N };
        BidiClass ClassOf(uint32_t cp)
        {
            if ((cp >= 0x0590 && cp <= 0x08FF) || (cp >= 0xFB1D && cp <= 0xFDFF) || (cp >= 0xFE70 && cp <= 0xFEFF))
                return (cp >= 0x0660 && cp <= 0x0669) || (cp >= 0x06F0 && cp <= 0x06F9) ? BidiClass::EN : BidiClass::R;
            if (cp >= '0' && cp <= '9') return BidiClass::EN;
            if ((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') || (cp >= 0xC0 && cp != 0xD7 && cp != 0xF7 && cp < 0x2000) || cp >= 0x2E80)
                return BidiClass::L;
            return BidiClass::N;
        }
        uint32_t Mirrored(uint32_t cp)
        {
            switch (cp)
            {
            case '(': return ')'; case ')': return '(';
            case '[': return ']'; case ']': return '[';
            case '{': return '}'; case '}': return '{';
            case '<': return '>'; case '>': return '<';
            case 0xAB: return 0xBB; case 0xBB: return 0xAB;
            default: return cp;
            }
        }
    }

    void Font::ShapeArabic(std::vector<StyledChar>& chars)
    {
        auto neighbour = [&](size_t i, int dir) -> uint32_t {
            for (int64_t j = (int64_t)i + dir; j >= 0 && j < (int64_t)chars.size(); j += dir)
                if (!Transparent(chars[j].Codepoint)) return chars[j].Codepoint;
            return 0;
        };
        std::vector<StyledChar> out;
        out.reserve(chars.size());
        for (size_t i = 0; i < chars.size(); ++i)
        {
            StyledChar c = chars[i];
            const ArabicForms* f = ArabicLetter(c.Codepoint);
            if (!f)
            {
                out.push_back(c);
                continue;
            }
            bool prev = JoinsNext(neighbour(i, -1));
            uint32_t nextCp = neighbour(i, +1);
            // Lam then alef: one ligature (isolated or final).
            if (c.Codepoint == 0x0644 && (nextCp == 0x0622 || nextCp == 0x0623 || nextCp == 0x0625 || nextCp == 0x0627))
            {
                uint32_t lig = nextCp == 0x0622 ? 0xFEF5 : nextCp == 0x0623 ? 0xFEF7 : nextCp == 0x0625 ? 0xFEF9 : 0xFEFB;
                c.Codepoint = lig + (prev ? 1 : 0);
                out.push_back(c);
                size_t j = i + 1;
                while (j < chars.size() && Transparent(chars[j].Codepoint)) out.push_back(chars[j++]);
                i = j; // the alef is in the ligature
                continue;
            }
            bool next = f->Dual && JoinsPrev(nextCp);
            int form = f->Dual ? (prev && next ? 3 : prev ? 1 : next ? 2 : 0) : (prev ? 1 : 0);
            c.Codepoint = f->Isolated + form;
            out.push_back(c);
        }
        chars = std::move(out);
    }

    void Font::ReorderBidi(std::vector<StyledChar*>& line)
    {
        size_t n = line.size();
        std::vector<BidiClass> cls(n);
        bool anyR = false;
        for (size_t i = 0; i < n; ++i)
        {
            cls[i] = ClassOf(line[i]->Codepoint);
            anyR |= cls[i] == BidiClass::R;
        }
        if (!anyR) return; // all left-to-right: nothing moves
        BidiClass para = BidiClass::L;
        for (BidiClass c : cls)
            if (c == BidiClass::L || c == BidiClass::R) { para = c; break; }
        bool rtl = para == BidiClass::R;
        // Neutrals between two characters of one direction take it (numbers count as right-to-left in a right-to-left
        // paragraph); others the paragraph's.
        auto strongOf = [&](BidiClass c) { return c == BidiClass::EN ? (rtl ? BidiClass::R : BidiClass::L) : c; };
        std::vector<int> level(n);
        for (size_t i = 0; i < n; ++i)
        {
            BidiClass c = cls[i];
            if (c == BidiClass::N)
            {
                BidiClass before = para, after = para;
                for (int64_t j = (int64_t)i - 1; j >= 0; --j)
                    if (cls[j] != BidiClass::N) { before = strongOf(cls[j]); break; }
                for (size_t j = i + 1; j < n; ++j)
                    if (cls[j] != BidiClass::N) { after = strongOf(cls[j]); break; }
                c = before == after ? before : para;
            }
            level[i] = c == BidiClass::R ? 1 : (rtl ? 2 : 0); // numbers and left-to-right text: even, above the paragraph
        }
        // Reverse every run at or above each level, highest first.
        int maxLevel = *std::max_element(level.begin(), level.end());
        for (int k = maxLevel; k >= 1; --k)
            for (size_t i = 0; i < n;)
            {
                if (level[i] < k) { ++i; continue; }
                size_t j = i;
                while (j < n && level[j] >= k) ++j;
                std::reverse(line.begin() + i, line.begin() + j);
                std::reverse(level.begin() + i, level.begin() + j);
                i = j;
            }
        for (size_t i = 0; i < n; ++i)
            if (level[i] % 2 == 1) line[i]->Codepoint = Mirrored(line[i]->Codepoint);
    }

    namespace
    {
        bool ParseColor(std::string_view v, glm::vec4& out)
        {
            static const std::pair<const char*, glm::vec4> named[] = {
                { "red", { 1, 0, 0, 1 } },         { "green", { 0, 1, 0, 1 } },     { "blue", { 0, 0, 1, 1 } },
                { "yellow", { 1, 0.92f, 0.016f, 1 } }, { "orange", { 1, 0.5f, 0, 1 } }, { "purple", { 0.63f, 0.13f, 0.94f, 1 } },
                { "cyan", { 0, 1, 1, 1 } },        { "magenta", { 1, 0, 1, 1 } },   { "white", { 1, 1, 1, 1 } },
                { "black", { 0, 0, 0, 1 } },       { "grey", { 0.5f, 0.5f, 0.5f, 1 } }, { "gray", { 0.5f, 0.5f, 0.5f, 1 } },
            };
            for (auto& [name, color] : named)
                if (v == name) { out = color; return true; }
            if (v.empty() || v[0] != '#') return false;
            v.remove_prefix(1);
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            for (char c : v)
                if (hex(c) < 0) return false;
            if (v.size() == 3 || v.size() == 4)
            {
                for (size_t k = 0; k < 4; ++k) out[(int)k] = k < v.size() ? hex(v[k]) * 17 / 255.0f : 1.0f;
                return true;
            }
            if (v.size() == 6 || v.size() == 8)
            {
                for (size_t k = 0; k < 4; ++k) out[(int)k] = k * 2 < v.size() ? (hex(v[k * 2]) * 16 + hex(v[k * 2 + 1])) / 255.0f : 1.0f;
                return true;
            }
            return false;
        }
    }

    std::vector<StyledChar> Font::ParseRichText(std::string_view text)
    {
        std::vector<StyledChar> out;
        int bold = 0, italic = 0, underline = 0, strike = 0;
        std::vector<std::pair<glm::vec4, bool>> colors;
        std::vector<std::pair<float, float>> sizes;
        auto current = [&] {
            StyledChar s;
            s.Bold = bold > 0;
            s.Italic = italic > 0;
            s.Underline = underline > 0;
            s.Strikethrough = strike > 0;
            if (!colors.empty()) { s.Color = colors.back().first; s.HasColor = true; }
            if (!sizes.empty()) { s.Size = sizes.back().first; s.SizeScale = sizes.back().second; }
            return s;
        };
        // Applies one tag (the text between '<' and '>'); false when it is not a tag we know.
        auto apply = [&](std::string tag) {
            for (char& c : tag) c = (char)std::tolower((unsigned char)c);
            auto counter = [&](std::string_view name) -> int* {
                if (name == "b") return &bold;
                if (name == "i") return &italic;
                if (name == "u") return &underline;
                if (name == "s") return &strike;
                return nullptr;
            };
            if (!tag.empty() && tag[0] == '/')
            {
                std::string_view name = std::string_view(tag).substr(1);
                if (int* c = counter(name)) { *c = std::max(0, *c - 1); return true; }
                if (name == "color") { if (!colors.empty()) colors.pop_back(); return true; }
                if (name == "size") { if (!sizes.empty()) sizes.pop_back(); return true; }
                return false;
            }
            if (int* c = counter(tag)) { ++*c; return true; }
            size_t eq = tag.find('=');
            if (eq == std::string::npos) return false;
            std::string name = tag.substr(0, eq), value = tag.substr(eq + 1);
            if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') && value.back() == value.front()) value = value.substr(1, value.size() - 2);
            if (name == "color")
            {
                glm::vec4 c;
                if (!ParseColor(value, c)) return false;
                colors.push_back({ c, true });
                return true;
            }
            if (name == "size")
            {
                bool percent = !value.empty() && value.back() == '%';
                if (percent) value.pop_back();
                char* end = nullptr;
                float v = std::strtof(value.c_str(), &end);
                if (value.empty() || end != value.c_str() + value.size() || v <= 0.0f) return false;
                float outerSize = sizes.empty() ? -1.0f : sizes.back().first, outerScale = sizes.empty() ? 1.0f : sizes.back().second;
                sizes.push_back(percent ? std::make_pair(outerSize, outerScale * v / 100.0f) : std::make_pair(v, 1.0f));
                return true;
            }
            return false;
        };
        for (size_t i = 0; i < text.size();)
        {
            if (text[i] == '<')
            {
                size_t close = text.find('>', i + 1);
                size_t nextOpen = text.find('<', i + 1);
                if (close != std::string_view::npos && close - i <= 64 && (nextOpen == std::string_view::npos || nextOpen > close))
                {
                    std::string tag(text.substr(i + 1, close - i - 1));
                    std::string lower = tag;
                    for (char& c : lower) c = (char)std::tolower((unsigned char)c);
                    if (lower == "br" || lower == "br/" || lower == "br /")
                    {
                        StyledChar s = current();
                        s.Codepoint = '\n';
                        out.push_back(s);
                        i = close + 1;
                        continue;
                    }
                    if (apply(tag))
                    {
                        i = close + 1;
                        continue;
                    }
                }
            }
            StyledChar s = current();
            s.Codepoint = NextCodepoint(text, i);
            out.push_back(s);
        }
        return out;
    }

    std::string Font::StripRichText(std::string_view text)
    {
        std::string out;
        for (const StyledChar& c : ParseRichText(text))
        {
            uint32_t cp = c.Codepoint;
            if (cp < 0x80) out += (char)cp;
            else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
            else if (cp < 0x10000) { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
            else { out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3F)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
        }
        return out;
    }

    std::vector<TextQuad> Font::Layout(std::string_view text, float pixelSize, float wrapWidth, TextAlign align,
                                       float alignWidth, glm::vec2& outSize, bool richText, float sizeUnit) const
    {
        std::vector<StyledChar> chars;
        if (richText) chars = ParseRichText(text);
        else
            for (size_t i = 0; i < text.size();)
            {
                StyledChar c;
                c.Codepoint = NextCodepoint(text, i);
                chars.push_back(c);
            }

        ShapeArabic(chars);
        // Which font draws each character: this one, else the first fallback that has it. Joiners and variation
        // selectors go with the emoji before them.
        auto joiner = [](uint32_t cp) { return cp == 0x200D || cp == 0xFE0E || cp == 0xFE0F; };
        for (size_t i = 0; i < chars.size(); ++i)
        {
            StyledChar& c = chars[i];
            c.Source = this;
            if (joiner(c.Codepoint) && i > 0 && chars[i - 1].Source->HasColorGlyphs())
            {
                c.Source = chars[i - 1].Source;
                continue;
            }
            if (c.Codepoint < 0x100 || c.Codepoint == '\n' || HasGlyph(c.Codepoint)) continue;
            for (const Ref<Font>& f : GetFallbackFonts())
                if (f && f->HasGlyph(c.Codepoint)) { c.Source = f.get(); break; }
            // A combining mark from another font than its base's: the base (and the marks between) from that font too
            // when it has them, so the font's anchors can place the mark.
            auto combining = [](uint32_t cp) {
                return (cp >= 0x300 && cp < 0x370) || (cp >= 0x1AB0 && cp < 0x1B00) || (cp >= 0x1DC0 && cp < 0x1E00) || (cp >= 0x20D0 && cp < 0x2100) ||
                       (cp >= 0xFE20 && cp < 0xFE30);
            };
            if (combining(c.Codepoint) && c.Source != this && i > 0)
            {
                size_t first = i;
                while (first > 0 && i - first < 4 && combining(chars[first - 1].Codepoint)) --first;
                if (first > 0 && chars[first - 1].Codepoint != ' ' && chars[first - 1].Codepoint != '\n') --first; // the base
                bool all = first < i;
                for (size_t k = first; k < i && all; ++k) all = c.Source->HasGlyph(chars[k].Codepoint);
                if (all)
                    for (size_t k = first; k < i; ++k) chars[k].Source = c.Source;
            }
        }
        // Emoji runs shaped by their font: skin tones, professions, families and the like become the glyphs it joins
        // them into (drawn by glyph index); selectors and leftover joiners take no room.
        {
            std::vector<StyledChar> shaped;
            shaped.reserve(chars.size());
            for (size_t i = 0; i < chars.size();)
            {
                const Font* source = chars[i].Source;
                if (!source->HasColorGlyphs() || !source->m_Dynamic->Substitution.Table)
                {
                    if (!joiner(chars[i].Codepoint)) shaped.push_back(chars[i]);
                    ++i;
                    continue;
                }
                size_t end = i;
                std::vector<uint16_t> glyphs;
                std::vector<size_t> origin; // the char each glyph came from (its style)
                for (; end < chars.size() && chars[end].Source == source && chars[end].Codepoint != '\n' && chars[end].Codepoint != ' '; ++end)
                {
                    uint32_t cp = chars[end].Codepoint;
                    if (cp == 0xFE0E || cp == 0xFE0F) continue;
                    glyphs.push_back((uint16_t)stbtt_FindGlyphIndex(&source->m_Dynamic->Info, (int)cp));
                    origin.push_back(end);
                }
                if (end == i) end = i + 1;
                std::vector<uint16_t> before = glyphs;
                source->m_Dynamic->Substitution.Shape(glyphs);
                // Joiners the font did not join take no room (and draw nothing), as shapers treat them.
                uint16_t zwj = (uint16_t)stbtt_FindGlyphIndex(&source->m_Dynamic->Info, 0x200D);
                if (zwj) std::erase(glyphs, zwj);
                if (glyphs == before)
                {
                    // Nothing joined: the characters as they were.
                    for (size_t k = i; k < end; ++k)
                        if (!joiner(chars[k].Codepoint)) shaped.push_back(chars[k]);
                }
                else
                    for (uint16_t g : glyphs)
                    {
                        StyledChar c = chars[origin.front()];
                        c.Codepoint = kGlyphIndexBit | g;
                        shaped.push_back(c);
                    }
                i = end;
            }
            chars = std::move(shaped);
        }
        // Positioning from the fonts' GPOS: marks (an accent, a vowel sign) placed on their base by its anchors, taking
        // no room, then kerning between neighbouring glyphs of one font at one size (marks between them skipped).
        {
            auto glyphOf = [](const StyledChar& c) -> int {
                if (!c.Source || !c.Source->m_Dynamic) return 0;
                return (c.Codepoint & kGlyphIndexBit) ? (int)(c.Codepoint & 0xFFFFu) : stbtt_FindGlyphIndex(&c.Source->m_Dynamic->Info, (int)c.Codepoint);
            };
            auto sameRun = [](const StyledChar& a, const StyledChar& b) {
                return a.Source == b.Source && a.Size == b.Size && a.SizeScale == b.SizeScale && a.Bold == b.Bold;
            };
            std::vector<int> glyph(chars.size());
            std::vector<int> baseOf(chars.size(), -1);
            int base = -1;
            for (size_t i = 0; i < chars.size(); ++i)
            {
                StyledChar& c = chars[i];
                glyph[i] = glyphOf(c);
                if (c.Codepoint == '\n' || c.Codepoint == ' ' || !glyph[i])
                {
                    base = -1;
                    continue;
                }
                int dx, dy;
                if (base >= 0 && sameRun(chars[base], c) && c.Source->m_Dynamic->Positioning.Attach((uint16_t)glyph[base], (uint16_t)glyph[i], dx, dy))
                {
                    c.Mark = true;
                    c.MarkOffset = { dx * c.Source->m_Dynamic->Scale, -dy * c.Source->m_Dynamic->Scale };
                    baseOf[i] = base;
                    continue;
                }
                base = (int)i;
            }
            int last = -1; // the last glyph that is not a mark
            for (size_t i = 0; i < chars.size(); ++i)
            {
                if (chars[i].Mark) continue;
                if (last >= 0 && glyph[last] && glyph[i] && chars[i].Codepoint != ' ' && chars[last].Codepoint != ' ' && sameRun(chars[last], chars[i]))
                    chars[last].Kern = chars[last].Source->KernPx(glyph[last], glyph[i]);
                last = chars[i].Codepoint == '\n' ? -1 : (int)i;
            }
            // A mark's offset from its own pen: back over its base's advance (and kerning) to the base's origin.
            for (size_t i = 0; i < chars.size(); ++i)
                if (chars[i].Mark)
                {
                    const StyledChar& b = chars[baseOf[i]];
                    chars[i].MarkOffset.x -= b.Source->GetGlyph(b.Codepoint).Advance + b.Kern;
                }
        }
        // Pixel size of a character: <size=N> is in the text's size units, <size=N%> scales.
        auto charPx = [&](const StyledChar& c) { return (c.Size > 0.0f ? c.Size * sizeUnit : pixelSize) * c.SizeScale; };
        auto boldOffset = [&](const StyledChar& c) { return c.Bold ? charPx(c) * 0.045f : 0.0f; };
        auto advance = [&](const StyledChar& c) {
            return c.Mark ? 0.0f : (c.Source->GetGlyph(c.Codepoint).Advance + c.Kern) * charPx(c) / kBakeSize + boldOffset(c);
        };
        struct Line
        {
            std::vector<StyledChar*> Chars;
            float Width = 0.0f;
            float Px = 0.0f; // largest character size on the line (its height and ascent follow it)
        };
        std::vector<Line> lines(1);

        // Greedy word wrap.
        std::vector<StyledChar*> word;
        float wordWidth = 0.0f;
        auto flushWord = [&] {
            Line& line = lines.back();
            if (wrapWidth > 0.0f && !line.Chars.empty() && line.Width + wordWidth > wrapWidth)
            {
                // Drop the trailing space of the line we are breaking.
                if (!line.Chars.empty() && line.Chars.back()->Codepoint == ' ')
                {
                    line.Width -= advance(*line.Chars.back());
                    line.Chars.pop_back();
                }
                lines.emplace_back();
            }
            Line& target = lines.back();
            target.Chars.insert(target.Chars.end(), word.begin(), word.end());
            target.Width += wordWidth;
            word.clear();
            wordWidth = 0.0f;
        };
        for (StyledChar& c : chars)
        {
            if (c.Codepoint == '\r') continue;
            if (c.Codepoint == '\n')
            {
                flushWord();
                if (lines.back().Chars.empty()) lines.back().Px = std::max(lines.back().Px, charPx(c));
                lines.emplace_back();
                continue;
            }
            word.push_back(&c);
            wordWidth += advance(c);
            if (c.Codepoint == ' ') flushWord();
        }
        flushWord();

        float widest = 0.0f, height = 0.0f;
        for (auto& l : lines)
        {
            widest = std::max(widest, l.Width);
            for (const StyledChar* c : l.Chars) l.Px = std::max(l.Px, charPx(*c));
            if (l.Px <= 0.0f) l.Px = pixelSize;
            height += GetLineHeight() * l.Px / kBakeSize;
        }
        float blockWidth = alignWidth > 0.0f ? alignWidth : widest;
        outSize = { widest, height };

        std::vector<TextQuad> quads;
        std::vector<const Font*> quadFonts; // the font of each quad (fallback quads get its atlas at the end)
        float top = 0.0f;
        for (Line& line : lines)
        {
            ReorderBidi(line.Chars); // visual order (right-to-left runs reversed)
            float x = align == TextAlign::Left ? 0.0f : align == TextAlign::Center ? (blockWidth - line.Width) * 0.5f : blockWidth - line.Width;
            float baseline = top + m_Ascent * line.Px / kBakeSize;
            for (const StyledChar* c : line.Chars)
            {
                const Glyph& g = c->Source->GetGlyph(c->Codepoint);
                float s = charPx(*c) / kBakeSize, adv = advance(*c);
                if (g.Max.x > g.Min.x && c->Codepoint != ' ')
                {
                    float ox = c->MarkOffset.x * s, oy = c->MarkOffset.y * s;
                    TextQuad q{ { x + ox + g.Min.x * s, baseline + oy + g.Min.y * s }, { x + ox + g.Max.x * s, baseline + oy + g.Max.y * s }, g.UVMin, g.UVMax };
                    q.Color = c->Color;
                    q.HasColor = c->HasColor;
                    if (g.Color)
                    {
                        // Emoji keep their own colors; the text's (or a tag's) alpha still fades them.
                        q.ColorGlyph = true;
                        q.HasColor = true;
                        q.Color = glm::vec4(1.0f, 1.0f, 1.0f, c->HasColor ? c->Color.a : 1.0f);
                    }
                    q.Skew = c->Italic ? 0.2f : 0.0f;
                    // Renderers move the top edge right by Skew * height; shifting the quad back by the part below
                    // the baseline slants it around the baseline.
                    float below = q.Max.y - baseline;
                    q.Min.x -= q.Skew * below;
                    q.Max.x -= q.Skew * below;
                    quads.push_back(q);
                    quadFonts.push_back(c->Source);
                    if (c->Bold) // faux bold: the glyph again, a little to the right
                    {
                        TextQuad b = q;
                        b.Min.x += boldOffset(*c);
                        b.Max.x += boldOffset(*c);
                        quads.push_back(b);
                        quadFonts.push_back(c->Source);
                    }
                }
                // Underline below the baseline, strikethrough through the middle of lowercase letters.
                auto rule = [&](float y, float thickness) {
                    TextQuad q{ { x, y - thickness * 0.5f }, { x + adv, y + thickness * 0.5f }, { 0, 0 }, { 1, 1 } };
                    q.Color = c->Color;
                    q.HasColor = c->HasColor;
                    q.Solid = true;
                    quads.push_back(q);
                    quadFonts.push_back(this);
                };
                float px = charPx(*c), thickness = std::max(1.0f, px * 0.06f);
                if (c->Underline) rule(baseline + px * 0.12f, thickness);
                if (c->Strikethrough) rule(baseline - m_Ascent * s * 0.32f, thickness);
                x += adv;
            }
            top += GetLineHeight() * line.Px / kBakeSize;
        }
        // Characters from fallback fonts: their atlases (after all the glyphs above were baked).
        for (size_t i = 0; i < quads.size(); ++i)
            if (quads[i].ColorGlyph) quads[i].Atlas = quadFonts[i]->GetColorAtlasIndex();
            else if (quadFonts[i] != this) quads[i].Atlas = quadFonts[i]->GetAtlasIndex();
        return quads;
    }
}
