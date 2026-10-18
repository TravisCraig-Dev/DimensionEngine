#include "Dimension/Renderer/Font.h"
#include "Dimension/Renderer/Texture.h"
#include "Dimension/Core/Log.h"
#include <fstream>

#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>

namespace Dimension
{
    namespace
    {
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

        constexpr int kWidth = 2048, kHeight = 1024;
        std::vector<uint8_t> coverage(size_t(kWidth) * kHeight);
        std::vector<stbtt_packedchar> packed(kCharCount);
        stbtt_pack_context pack;
        if (!stbtt_PackBegin(&pack, coverage.data(), kWidth, kHeight, 0, 2, nullptr))
            return nullptr;
        stbtt_PackSetOversampling(&pack, 1, 1);
        int ok = stbtt_PackFontRange(&pack, data, 0, kBakeSize, (int)kFirstChar, (int)kCharCount, packed.data());
        stbtt_PackEnd(&pack);
        if (!ok) Log::Warn("Font '{}': some glyphs did not fit in the atlas", name);

        auto font = MakeRef<Font>();
        float scale = stbtt_ScaleForPixelHeight(&info, kBakeSize);
        int ascent, descent, lineGap;
        stbtt_GetFontVMetrics(&info, &ascent, &descent, &lineGap);
        font->m_Ascent = ascent * scale;
        font->m_Descent = descent * scale;
        font->m_LineGap = lineGap * scale;

        for (uint32_t i = 0; i < kCharCount; ++i)
        {
            const stbtt_packedchar& p = packed[i];
            Glyph& g = font->m_Glyphs[i];
            g.UVMin = { p.x0 / (float)kWidth, p.y0 / (float)kHeight };
            g.UVMax = { p.x1 / (float)kWidth, p.y1 / (float)kHeight };
            g.Min = { p.xoff, p.yoff };
            g.Max = { p.xoff2, p.yoff2 };
            g.Advance = p.xadvance;
        }

        // White RGB with coverage in alpha, so the sprite/UI shaders can tint it.
        std::vector<uint32_t> rgba(coverage.size());
        for (size_t i = 0; i < coverage.size(); ++i) rgba[i] = 0x00FFFFFFu | (uint32_t(coverage[i]) << 24);
        TextureSettings settings;
        settings.SRGB = false;
        settings.GenerateMips = true;
        font->m_Atlas = MakeRef<Texture2D>(rgba.data(), kWidth, kHeight, settings, name.c_str());
        return font;
    }

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

    std::vector<TextQuad> Font::Layout(std::string_view text, float pixelSize, float wrapWidth, TextAlign align,
                                       float alignWidth, glm::vec2& outSize) const
    {
        const float scale = pixelSize / kBakeSize;
        struct Line
        {
            std::vector<uint32_t> Chars;
            float Width = 0.0f;
        };
        std::vector<Line> lines(1);

        // Greedy word wrap.
        std::vector<uint32_t> word;
        float wordWidth = 0.0f;
        auto flushWord = [&] {
            Line& line = lines.back();
            if (wrapWidth > 0.0f && !line.Chars.empty() && line.Width + wordWidth > wrapWidth)
            {
                // Drop the trailing space of the line we are breaking.
                if (!line.Chars.empty() && line.Chars.back() == ' ')
                {
                    line.Width -= GetGlyph(' ').Advance * scale;
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
        for (size_t i = 0; i < text.size();)
        {
            uint32_t cp = NextCodepoint(text, i);
            if (cp == '\r') continue;
            if (cp == '\n')
            {
                flushWord();
                lines.emplace_back();
                continue;
            }
            word.push_back(cp);
            wordWidth += GetGlyph(cp).Advance * scale;
            if (cp == ' ') flushWord();
        }
        flushWord();

        float lineHeight = GetLineHeight() * scale;
        float widest = 0.0f;
        for (auto& l : lines) widest = std::max(widest, l.Width);
        float blockWidth = alignWidth > 0.0f ? alignWidth : widest;
        outSize = { widest, lines.size() * lineHeight };

        std::vector<TextQuad> quads;
        for (size_t li = 0; li < lines.size(); ++li)
        {
            const Line& line = lines[li];
            float x = align == TextAlign::Left ? 0.0f : align == TextAlign::Center ? (blockWidth - line.Width) * 0.5f : blockWidth - line.Width;
            float baseline = li * lineHeight + m_Ascent * scale;
            for (uint32_t cp : line.Chars)
            {
                const Glyph& g = GetGlyph(cp);
                if (g.Max.x > g.Min.x && cp != ' ')
                    quads.push_back({ { x + g.Min.x * scale, baseline + g.Min.y * scale }, { x + g.Max.x * scale, baseline + g.Max.y * scale },
                                      g.UVMin, g.UVMax });
                x += g.Advance * scale;
            }
        }
        return quads;
    }
}
