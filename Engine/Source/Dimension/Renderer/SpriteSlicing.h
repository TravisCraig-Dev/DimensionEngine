#pragma once
#include "Dimension/Core/Base.h"
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace Dimension
{
    // A named part of a texture (Sprite Editor slices): x, y (from the top-left) and width, height in pixels. Kept in
    // the texture's import settings ("sprites"); a Sprite Renderer's Sprite picks one by name.
    struct SpriteRect
    {
        std::string Name;
        glm::ivec4 Rect{ 0 };
    };

    namespace SpriteSlicing
    {
        // An image file's pixels (RGBA8, rows top-down).
        bool LoadPixels(const fs::path& path, std::vector<uint32_t>& pixels, int& width, int& height);
        // Equal cells, row by row from the top-left (cells that are all transparent left out when pixels are given).
        std::vector<glm::ivec4> Grid(int width, int height, int columns, int rows, const std::vector<uint32_t>* pixels = nullptr);
        // Cells of a size (the last ones in a row or column cut to the image), from an offset, a gap apart.
        std::vector<glm::ivec4> GridBySize(int width, int height, glm::ivec2 cell, glm::ivec2 offset, glm::ivec2 gap,
                                           const std::vector<uint32_t>* pixels = nullptr);
        // Automatic: each group of touching pixels (8 ways) with alpha over the threshold, as its bounding box, row by
        // row from the top-left; boxes smaller than minSize on a side are left out.
        std::vector<glm::ivec4> Islands(const std::vector<uint32_t>& pixels, int width, int height, int alphaThreshold = 8, int minSize = 2);
        std::vector<SpriteRect> FromJson(const nlohmann::json& sprites);
        nlohmann::json ToJson(const std::vector<SpriteRect>& sprites);
    }
}
