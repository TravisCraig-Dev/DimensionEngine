#include "Dimension/Renderer/Font.h"
#include "Dimension/Renderer/Texture.h"
#include "Dimension/Core/Log.h"
#include <fstream>

#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>
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

    Ref<Font> Font::LoadFromMemory(const uint8_t* data, size_t size, const std::string& name)
    {
        stbtt_fontinfo info;
        if (!stbtt_InitFont(&info, data, stbtt_GetFontOffsetForIndex(data, 0)))
        {
            Log::Error("Invalid font '{}'", name);
            return nullptr;
        }

        // Multi-channel signed distance field glyphs (MTSDF): RGB hold three distances whose median is the distance to
        // the outline with its corners kept sharp, alpha the true distance (for outlines and soft effects); 0.5 is on
        // the outline, with kSpread pixels of falloff each way at the bake size. Text stays crisp at any size.
        constexpr int kWidth = 2048, kHeight = 1024, kSpread = 6;
        std::vector<uint32_t> rgba(size_t(kWidth) * kHeight, 0u); // outside everywhere (the gutters too)
        auto toByte = [](float d) { return (uint32_t)std::clamp((int)std::lround(d * 255.0f), 0, 255); };
        auto font = MakeRef<Font>();
        float scale = stbtt_ScaleForPixelHeight(&info, kBakeSize);
        int ascent, descent, lineGap;
        stbtt_GetFontVMetrics(&info, &ascent, &descent, &lineGap);
        font->m_Ascent = ascent * scale;
        font->m_Descent = descent * scale;
        font->m_LineGap = lineGap * scale;

        int penX = 1, penY = 1, rowHeight = 0;
        bool full = false;
        for (uint32_t i = 0; i < kCharCount; ++i)
        {
            int cp = (int)(kFirstChar + i);
            Glyph& g = font->m_Glyphs[i];
            int advance, bearing;
            stbtt_GetCodepointHMetrics(&info, cp, &advance, &bearing);
            g.Advance = advance * scale;
            // The glyph's box at the bake size, padded by the spread (as stb_truetype's own SDF does).
            int x0, y0, x1, y1;
            stbtt_GetCodepointBitmapBox(&info, cp, scale, scale, &x0, &y0, &x1, &y1);
            if (x1 <= x0 || y1 <= y0) continue; // blank glyph (space)
            int w = x1 - x0 + 2 * kSpread, h = y1 - y0 + 2 * kSpread, xoff = x0 - kSpread, yoff = y0 - kSpread;
            // Shelf packing, one pixel apart.
            if (penX + w + 1 > kWidth) { penX = 1; penY += rowHeight + 1; rowHeight = 0; }
            if (penY + h + 1 > kHeight) { full = true; continue; }
            msdfgen::Shape shape;
            if (GlyphShape(info, cp, shape))
            {
                // Pixel (x, row from the bottom) <- font units: x = scale * (fx + tx), row = scale * (fy + ty).
                msdfgen::Bitmap<float, 4> field(w, h);
                msdfgen::Projection projection(msdfgen::Vector2(scale), msdfgen::Vector2(-xoff / scale, (h + yoff) / scale));
                msdfgen::generateMTSDF(field, shape, msdfgen::SDFTransformation(projection, msdfgen::Range(2.0 * kSpread / scale)));
                for (int y = 0; y < h; ++y)
                    for (int x = 0; x < w; ++x)
                    {
                        const float* d = field(x, h - 1 - y); // msdfgen rows go up, the atlas's down
                        rgba[size_t(penY + y) * kWidth + penX + x] = toByte(d[0]) | toByte(d[1]) << 8 | toByte(d[2]) << 16 | toByte(d[3]) << 24;
                    }
            }
            else
            {
                // No outline msdfgen can use: stb_truetype's single-channel field in all four channels.
                int sw = 0, sh = 0, sx = 0, sy = 0;
                unsigned char* sdf = stbtt_GetCodepointSDF(&info, scale, cp, kSpread, 128, 128.0f / kSpread, &sw, &sh, &sx, &sy);
                if (!sdf) continue;
                for (int y = 0; y < std::min(sh, h); ++y)
                    for (int x = 0; x < std::min(sw, w); ++x)
                    {
                        uint32_t d = sdf[size_t(y) * sw + x];
                        rgba[size_t(penY + y) * kWidth + penX + x] = d | d << 8 | d << 16 | d << 24;
                    }
                stbtt_FreeSDF(sdf, nullptr);
            }
            g.UVMin = { penX / (float)kWidth, penY / (float)kHeight };
            g.UVMax = { (penX + w) / (float)kWidth, (penY + h) / (float)kHeight };
            g.Min = { (float)xoff, (float)yoff };
            g.Max = { (float)(xoff + w), (float)(yoff + h) };
            penX += w + 1;
            rowHeight = std::max(rowHeight, h);
        }
        if (full) Log::Warn("Font '{}': some glyphs did not fit in the atlas", name);

        TextureSettings settings;
        settings.SRGB = false;
        settings.GenerateMips = true;
        font->m_Atlas = MakeRef<Texture2D>(rgba.data(), kWidth, kHeight, settings, name.c_str());
        return font;
    }

    uint32_t Font::GetAtlasIndex() const { return m_Atlas ? (m_Atlas->GetBindlessIndex() | kDistanceFieldBit | kMultiChannelBit) : 0u; }

    Ref<Font> Font::LoadFromFile(const fs::path& path)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in)
        {
            Log::Error("Cannot open font '{}'", path.string());
            return nullptr;
        }
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        return LoadFromMemory(bytes.data(), bytes.size(), path.filename().string());
    }

    const Glyph& Font::GetGlyph(uint32_t cp) const
    {
        if (cp < kFirstChar || cp >= kFirstChar + kCharCount) cp = '?';
        return m_Glyphs[cp - kFirstChar];
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

        // Pixel size of a character: <size=N> is in the text's size units, <size=N%> scales.
        auto charPx = [&](const StyledChar& c) { return (c.Size > 0.0f ? c.Size * sizeUnit : pixelSize) * c.SizeScale; };
        auto boldOffset = [&](const StyledChar& c) { return c.Bold ? charPx(c) * 0.045f : 0.0f; };
        auto advance = [&](const StyledChar& c) { return GetGlyph(c.Codepoint).Advance * charPx(c) / kBakeSize + boldOffset(c); };
        struct Line
        {
            std::vector<const StyledChar*> Chars;
            float Width = 0.0f;
            float Px = 0.0f; // largest character size on the line (its height and ascent follow it)
        };
        std::vector<Line> lines(1);

        // Greedy word wrap.
        std::vector<const StyledChar*> word;
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
        for (const StyledChar& c : chars)
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
        float top = 0.0f;
        for (const Line& line : lines)
        {
            float x = align == TextAlign::Left ? 0.0f : align == TextAlign::Center ? (blockWidth - line.Width) * 0.5f : blockWidth - line.Width;
            float baseline = top + m_Ascent * line.Px / kBakeSize;
            for (const StyledChar* c : line.Chars)
            {
                const Glyph& g = GetGlyph(c->Codepoint);
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
                    if (c->Bold) // faux bold: the glyph again, a little to the right
                    {
                        TextQuad b = q;
                        b.Min.x += boldOffset(*c);
                        b.Max.x += boldOffset(*c);
                        quads.push_back(b);
                    }
                }
                // Underline below the baseline, strikethrough through the middle of lowercase letters.
                auto rule = [&](float y, float thickness) {
                    TextQuad q{ { x, y - thickness * 0.5f }, { x + adv, y + thickness * 0.5f }, { 0, 0 }, { 1, 1 } };
                    q.Color = c->Color;
                    q.HasColor = c->HasColor;
                    q.Solid = true;
                    quads.push_back(q);
                };
                float px = charPx(*c), thickness = std::max(1.0f, px * 0.06f);
                if (c->Underline) rule(baseline + px * 0.12f, thickness);
                if (c->Strikethrough) rule(baseline - m_Ascent * s * 0.32f, thickness);
                x += adv;
            }
            top += GetLineHeight() * line.Px / kBakeSize;
        }
        return quads;
    }
}
