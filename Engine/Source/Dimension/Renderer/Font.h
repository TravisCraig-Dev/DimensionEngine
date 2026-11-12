#pragma once
#include "Dimension/Core/Base.h"
#include <glm/glm.hpp>
#include <array>

namespace Dimension
{
    class Texture2D;

    enum class TextAlign : int32_t { Left = 0, Center = 1, Right = 2 };
    enum class TextVAlign : int32_t { Top = 0, Middle = 1, Bottom = 2 };

    struct Glyph
    {
        glm::vec2 UVMin{ 0.0f }, UVMax{ 0.0f };
        glm::vec2 Min{ 0.0f }, Max{ 0.0f }; // quad relative to the pen on the baseline, y down, at bake size
        float Advance = 0.0f;
        bool Color = false; // a color glyph (emoji): UVs in the font's color atlas, drawn untinted
    };

    // One positioned glyph quad of laid-out text: pixel space, y down, relative to the block's top-left.
    struct TextQuad
    {
        glm::vec2 Min, Max;
        glm::vec2 UVMin, UVMax;
        // Rich text: a <color> tag's color (display space; alpha multiplies the text's), italic slant (the top edge
        // moves right by Skew * height), and Solid quads (underline, strikethrough) that use a white texture.
        glm::vec4 Color{ 1.0f };
        bool HasColor = false;
        float Skew = 0.0f;
        bool Solid = false;
        uint32_t Atlas = 0; // a fallback font's atlas (as GetAtlasIndex gives it) for a character this font lacks; 0: this font's
        bool ColorGlyph = false; // an emoji from a color atlas (Atlas): HasColor is white so the text's color only fades it
    };

    // A character of rich text with the style its tags give it.
    struct StyledChar
    {
        uint32_t Codepoint = 0;
        glm::vec4 Color{ 1.0f };
        bool HasColor = false;
        float Size = -1.0f; // <size> in the text's size units; < 0 = the text's own size
        float SizeScale = 1.0f; // <size=N%>, multiplied in
        bool Bold = false, Italic = false, Underline = false, Strikethrough = false;
        const class Font* Source = nullptr; // Layout: the font that draws it (this one or a fallback)
        // Layout, from the font's positioning (bake pixels): kerning added to its advance, and a mark's place on its
        // base relative to its own pen (a mark takes no room).
        float Kern = 0.0f;
        glm::vec2 MarkOffset{ 0.0f };
        bool Mark = false;
    };

    // TrueType font baked into a texture atlas with stb_truetype and msdfgen: Latin-1 (codepoints 32..255) up front,
    // any other character the font has when text first uses it (the atlas is uploaded again once a frame at most).
    // Characters it lacks come from the fallback fonts; Arabic is shaped and right-to-left runs reordered.
    class Font
    {
    public:
        static constexpr float kBakeSize = 64.0f;
        static constexpr uint32_t kFirstChar = 32, kCharCount = 224;

        // `bakeLatin1` false: every glyph is baked when first used (fallback fonts load at once that way).
        static Ref<Font> LoadFromMemory(const uint8_t* data, size_t size, const std::string& name, bool bakeLatin1 = true);
        static Ref<Font> LoadFromFile(const fs::path& path, bool bakeLatin1 = true);

        const Glyph& GetGlyph(uint32_t codepoint) const; // baked on first use; '?' when the font lacks it
        bool HasGlyph(uint32_t codepoint) const;
        // Fonts asked, in order, for characters a font lacks. By default (first use) the system's: on Windows Segoe UI
        // (Greek, Cyrillic, Arabic, Hebrew...), Segoe UI Symbol, Microsoft YaHei (Chinese, Japanese kana), Malgun Gothic
        // (Korean) and Nirmala UI (Indic), as far as they are installed.
        static void SetFallbackFonts(std::vector<Ref<Font>> fonts);
        static const std::vector<Ref<Font>>& GetFallbackFonts();
        // Drops the fallback fonts (AssetManager::Shutdown, while the device lives); the next use loads them again.
        static void ReleaseFallbackFonts();
        // Text in its shaped, visual order: Arabic letters in their joined forms (and lam-alef ligatures), and right-to-
        // left runs reversed (a simplified Unicode bidirectional algorithm: a paragraph takes its first strong
        // character's direction, numbers stay left-to-right, brackets mirror). Layout does both; exposed for tests.
        static void ShapeArabic(std::vector<StyledChar>& chars);
        static void ReorderBidi(std::vector<StyledChar*>& line);
        const Ref<Texture2D>& GetAtlas() const { return m_Atlas; }
        // Bindless index of the atlas with kDistanceFieldBit set: the UI and sprite shaders read its alpha as a signed
        // distance (0.5 on the outline) instead of coverage.
        static constexpr uint32_t kDistanceFieldBit = 0x80000000u;
        // With it: a multi-channel field (the median of RGB is the distance; alpha the plain one).
        static constexpr uint32_t kMultiChannelBit = 0x40000000u;
        static constexpr uint32_t kTextureIndexMask = 0x3FFFFFFFu;
        // A StyledChar codepoint with this bit is a glyph index of its Source font (shaped emoji sequences).
        static constexpr uint32_t kGlyphIndexBit = 0x80000000u;
        uint32_t GetAtlasIndex() const;
        // Color glyphs (COLR/CPAL layered emoji, as Segoe UI Emoji has them) go in a second atlas of plain sRGB colors
        // with alpha; its bindless index (no flag bits), 0 while the font has none.
        uint32_t GetColorAtlasIndex() const;
        bool HasColorGlyphs() const;
        float GetAscent() const { return m_Ascent; }   // pixels at bake size
        float GetDescent() const { return m_Descent; } // negative
        float GetLineHeight() const { return m_Ascent - m_Descent + m_LineGap; }

        // Lays out UTF-8 text at `pixelSize`. Lines wrap at word boundaries when wrapWidth > 0
        // and are aligned within alignWidth (defaults to the widest line).
        // With `richText`, tags style the text (see ParseRichText); `sizeUnit` converts <size=N> to pixels.
        std::vector<TextQuad> Layout(std::string_view text, float pixelSize, float wrapWidth, TextAlign align,
                                     float alignWidth, glm::vec2& outSize, bool richText = false, float sizeUnit = 1.0f) const;

        // Rich text tags (like TextMeshPro's): <b>, <i>, <u>, <s>, <color=#RRGGBB> (also #RGB, #RRGGBBAA or a name:
        // red, green, blue, yellow, orange, purple, cyan, magenta, white, black, grey), <size=N> (text size units),
        // <size=150%>, and <br> (a line break), each closed by </b>, </color> and so on. Tags nest; an unknown or
        // malformed tag stays as literal text.
        static std::vector<StyledChar> ParseRichText(std::string_view text);
        // The text without its rich text tags.
        static std::string StripRichText(std::string_view text);

    private:
        struct Dynamic; // the font file, the atlas pixels and the glyphs baked since loading
        std::array<Glyph, kCharCount> m_Glyphs{};
        // The baked Latin-1 glyphs kept between runs (see Font.cpp); false when there are none for this font yet.
        static bool LoadBakedLatin1(Dynamic& dyn, std::array<Glyph, kCharCount>& glyphs);
        static void StoreBakedLatin1(const Dynamic& dyn, const std::array<Glyph, kCharCount>& glyphs);
        mutable Ref<Texture2D> m_Atlas;
        mutable Ref<Texture2D> m_ColorAtlas;
        std::shared_ptr<Dynamic> m_Dynamic;
        bool m_Latin1Baked = true;
        // Kerning between two of the font's glyphs, in bake pixels (GPOS pairs, else the kern table).
        float KernPx(int first, int second) const;
        float m_Ascent = 0, m_Descent = 0, m_LineGap = 0;
    };
}
