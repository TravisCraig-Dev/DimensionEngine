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
    };

    // TrueType font baked into a texture atlas (Latin-1, codepoints 32..255) with stb_truetype.
    class Font
    {
    public:
        static constexpr float kBakeSize = 64.0f;
        static constexpr uint32_t kFirstChar = 32, kCharCount = 224;

        static Ref<Font> LoadFromMemory(const uint8_t* data, size_t size, const std::string& name);
        static Ref<Font> LoadFromFile(const fs::path& path);

        const Glyph& GetGlyph(uint32_t codepoint) const;
        const Ref<Texture2D>& GetAtlas() const { return m_Atlas; }
        // Bindless index of the atlas with kDistanceFieldBit set: the UI and sprite shaders read its alpha as a signed
        // distance (0.5 on the outline) instead of coverage.
        static constexpr uint32_t kDistanceFieldBit = 0x80000000u;
        // With it: a multi-channel field (the median of RGB is the distance; alpha the plain one).
        static constexpr uint32_t kMultiChannelBit = 0x40000000u;
        static constexpr uint32_t kTextureIndexMask = 0x3FFFFFFFu;
        uint32_t GetAtlasIndex() const;
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
        std::array<Glyph, kCharCount> m_Glyphs{};
        Ref<Texture2D> m_Atlas;
        float m_Ascent = 0, m_Descent = 0, m_LineGap = 0;
    };
}
