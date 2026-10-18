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
        float GetAscent() const { return m_Ascent; }   // pixels at bake size
        float GetDescent() const { return m_Descent; } // negative
        float GetLineHeight() const { return m_Ascent - m_Descent + m_LineGap; }

        // Lays out UTF-8 text at `pixelSize`. Lines wrap at word boundaries when wrapWidth > 0
        // and are aligned within alignWidth (defaults to the widest line).
        std::vector<TextQuad> Layout(std::string_view text, float pixelSize, float wrapWidth, TextAlign align,
                                     float alignWidth, glm::vec2& outSize) const;

    private:
        std::array<Glyph, kCharCount> m_Glyphs{};
        Ref<Texture2D> m_Atlas;
        float m_Ascent = 0, m_Descent = 0, m_LineGap = 0;
    };
}
