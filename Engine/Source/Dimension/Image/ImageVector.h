#pragma once
#include "Dimension/Modeling/Paint.h"
#include <glm/glm.hpp>
#include <nlohmann/json_fwd.hpp>
#include <array>
#include <string>
#include <vector>

// Image Mode's vector layers: shapes (rectangles, rounded rectangles, ellipses, polygons, stars, lines; a fill and a
// stroke) and text (a font, a size, a color, lines, alignment), kept as their settings and drawn into pixels whenever
// those change, so they stay sharp however they are moved, scaled or rotated.
namespace Dimension::Imaging
{
    using VectorBitmap = Modeling::Paint::Image;

    enum class ShapeKind { Rectangle, RoundedRectangle, Ellipse, Polygon, Star, Line, Count };
    const char* ShapeKindName(ShapeKind kind);

    struct ShapeData
    {
        ShapeKind Kind = ShapeKind::Rectangle;
        glm::vec2 Center{ 0.0f };
        glm::vec2 Size{ 100.0f, 100.0f }; // (a line: its length and weight)
        float Rotation = 0.0f;           // degrees, clockwise on screen
        float Radius = 10.0f;            // rounded rectangle corners
        int Sides = 5;                   // polygon, star (points)
        float Inset = 0.5f;              // star: how deep its valleys go (0..0.95)
        bool Fill = true;
        glm::vec4 FillColor{ 0.2f, 0.5f, 0.9f, 1.0f };
        bool Stroke = false;
        glm::vec4 StrokeColor{ 0.0f, 0.0f, 0.0f, 1.0f };
        float StrokeWidth = 3.0f;
        int StrokeAlign = 0;             // 0 inside, 1 center, 2 outside
    };

    struct TextData
    {
        std::string Text = "Text";       // UTF-8; '\n' starts a line
        std::string Font;                // a font file (empty: the built-in Roboto)
        float Size = 48.0f;              // pixels per em
        glm::vec4 Color{ 0.0f, 0.0f, 0.0f, 1.0f };
        int Align = 0;                   // 0 left, 1 center, 2 right (of Position)
        float Leading = 1.2f;            // line spacing (x size)
        float Tracking = 0.0f;           // extra space between letters (pixels)
        glm::vec2 Position{ 0.0f };      // where the first line's baseline starts (or centers, or ends)
        float Rotation = 0.0f;           // degrees, clockwise on screen
        bool Antialias = true;
        bool FauxBold = false, FauxItalic = false;
    };

    // The shape's outline (canvas pixels).
    std::vector<glm::vec2> ShapeOutline(const ShapeData& shape);
    // Its box's corners (top-left, top-right, bottom-right, bottom-left, rotated).
    std::array<glm::vec2, 4> ShapeCorners(const ShapeData& shape);
    VectorBitmap RenderShape(const ShapeData& shape, uint32_t width, uint32_t height);

    // Where the text's characters go, in its own frame (x along the line from the anchor, y down from the first
    // baseline; before its rotation and position).
    struct TextLayout
    {
        struct Line
        {
            size_t Start = 0, End = 0;     // bytes of the text (End: before the '\n')
            float X = 0.0f, Baseline = 0.0f, Width = 0.0f;
            std::vector<float> Carets;     // x before each character and after the last
            std::vector<size_t> Offsets;   // the byte each caret stands before
        };
        std::vector<Line> Lines;
        float Ascent = 0.0f, Descent = 0.0f, LineHeight = 0.0f;
        glm::vec2 Min{ 0.0f }, Max{ 0.0f }; // the box around all lines
    };
    TextLayout LayoutText(const TextData& text);
    std::array<glm::vec2, 4> TextCorners(const TextData& text, const TextLayout& layout);
    glm::vec2 TextToCanvas(const TextData& text, const glm::vec2& local);
    glm::vec2 CanvasToText(const TextData& text, const glm::vec2& canvas);
    VectorBitmap RenderText(const TextData& text, uint32_t width, uint32_t height);

    // Fonts to choose from: the system's and those in `assets` (searched for .ttf/.otf), by name.
    struct FontFile
    {
        std::string Name;
        std::string Path;
    };
    const std::vector<FontFile>& FindFonts(const fs::path& assets);

    nlohmann::json ToJson(const ShapeData& shape);
    ShapeData ShapeFromJson(const nlohmann::json& j);
    nlohmann::json ToJson(const TextData& text);
    TextData TextFromJson(const nlohmann::json& j);
}
