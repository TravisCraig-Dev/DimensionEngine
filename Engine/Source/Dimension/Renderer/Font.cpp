#include "Dimension/Renderer/Font.h"
#include "Dimension/Renderer/Texture.h"
#include "Dimension/Core/Log.h"
#include <fstream>

#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>
#include <unordered_map>
#include <msdfgen.h>

namespace Dimension
{
    namespace
    {
        // A glyph's outline as an msdfgen shape (font units, y up); false for glyphs without one (space).
        bool GlyphShape(const stbtt_fontinfo& info, int codepoint, msdfgen::Shape& shape)
        {
            stbtt_vertex* v = nullptr;
            int count = stbtt_GetCodepointShape(&info, codepoint, &v);
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

        // Bakes a glyph's multi-channel distance field into the atlas (shelf packing); false when it does not fit.
        bool Bake(uint32_t cp, Glyph& g)
        {
            constexpr int kWidthC = kWidth, kHeightC = kHeight;
            auto toByte = [](float d) { return (uint32_t)std::clamp((int)std::lround(d * 255.0f), 0, 255); };
            int advance, bearing;
            stbtt_GetCodepointHMetrics(&Info, (int)cp, &advance, &bearing);
            g.Advance = advance * Scale;
            int x0, y0, x1, y1;
            stbtt_GetCodepointBitmapBox(&Info, (int)cp, Scale, Scale, &x0, &y0, &x1, &y1);
            if (x1 <= x0 || y1 <= y0) return true; // blank (a space)
            int w = x1 - x0 + 2 * kSpread, h = y1 - y0 + 2 * kSpread, xoff = x0 - kSpread, yoff = y0 - kSpread;
            if (PenX + w + 1 > kWidthC) { PenX = 1; PenY += RowHeight + 1; RowHeight = 0; }
            if (PenY + h + 1 > kHeightC) { Full = true; return false; }
            msdfgen::Shape shape;
            if (GlyphShape(Info, (int)cp, shape))
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
                unsigned char* sdf = stbtt_GetCodepointSDF(&Info, Scale, (int)cp, kSpread, 128, 128.0f / kSpread, &sw, &sh, &sx, &sy);
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
        auto font = MakeRef<Font>();
        font->m_Dynamic = dyn;
        font->m_Latin1Baked = bakeLatin1;
        dyn->Scale = stbtt_ScaleForPixelHeight(&dyn->Info, kBakeSize);
        int ascent, descent, lineGap;
        stbtt_GetFontVMetrics(&dyn->Info, &ascent, &descent, &lineGap);
        font->m_Ascent = ascent * dyn->Scale;
        font->m_Descent = descent * dyn->Scale;
        font->m_LineGap = lineGap * dyn->Scale;
        if (bakeLatin1)
            for (uint32_t i = 0; i < kCharCount; ++i) dyn->Bake(kFirstChar + i, font->m_Glyphs[i]);
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
            for (const char* name : { "segoeui.ttf", "seguisym.ttf", "msyh.ttc", "malgun.ttf", "Nirmala.ttf" })
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
        // Which font draws each character: this one, else the first fallback that has it.
        for (StyledChar& c : chars)
        {
            c.Source = this;
            if (c.Codepoint < 0x100 || c.Codepoint == '\n' || HasGlyph(c.Codepoint)) continue;
            for (const Ref<Font>& f : GetFallbackFonts())
                if (f && f->HasGlyph(c.Codepoint)) { c.Source = f.get(); break; }
        }
        // Pixel size of a character: <size=N> is in the text's size units, <size=N%> scales.
        auto charPx = [&](const StyledChar& c) { return (c.Size > 0.0f ? c.Size * sizeUnit : pixelSize) * c.SizeScale; };
        auto boldOffset = [&](const StyledChar& c) { return c.Bold ? charPx(c) * 0.045f : 0.0f; };
        auto advance = [&](const StyledChar& c) { return c.Source->GetGlyph(c.Codepoint).Advance * charPx(c) / kBakeSize + boldOffset(c); };
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
                    TextQuad q{ { x + g.Min.x * s, baseline + g.Min.y * s }, { x + g.Max.x * s, baseline + g.Max.y * s }, g.UVMin, g.UVMax };
                    q.Color = c->Color;
                    q.HasColor = c->HasColor;
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
            if (quadFonts[i] != this) quads[i].Atlas = quadFonts[i]->GetAtlasIndex();
        return quads;
    }
}
