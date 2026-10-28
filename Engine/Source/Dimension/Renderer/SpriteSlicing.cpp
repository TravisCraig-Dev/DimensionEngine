#include "Dimension/Renderer/SpriteSlicing.h"
#include <stb_image.h>
#include <algorithm>
#include <cstring>

namespace Dimension::SpriteSlicing
{
    namespace
    {
        bool Empty(const std::vector<uint32_t>& pixels, int width, const glm::ivec4& r)
        {
            for (int y = r.y; y < r.y + r.w; ++y)
                for (int x = r.x; x < r.x + r.z; ++x)
                    if ((pixels[(size_t)y * width + x] >> 24) > 0) return false;
            return true;
        }
    }

    bool LoadPixels(const fs::path& path, std::vector<uint32_t>& pixels, int& width, int& height)
    {
        int channels = 0;
        stbi_uc* data = stbi_load(path.string().c_str(), &width, &height, &channels, 4);
        if (!data) return false;
        pixels.resize((size_t)width * height);
        std::memcpy(pixels.data(), data, pixels.size() * 4);
        stbi_image_free(data);
        return true;
    }

    std::vector<glm::ivec4> Grid(int width, int height, int columns, int rows, const std::vector<uint32_t>* pixels)
    {
        std::vector<glm::ivec4> out;
        columns = std::max(columns, 1);
        rows = std::max(rows, 1);
        for (int r = 0; r < rows; ++r)
            for (int c = 0; c < columns; ++c)
            {
                int x0 = c * width / columns, x1 = (c + 1) * width / columns, y0 = r * height / rows, y1 = (r + 1) * height / rows;
                glm::ivec4 cell(x0, y0, x1 - x0, y1 - y0);
                if (cell.z <= 0 || cell.w <= 0) continue;
                if (pixels && pixels->size() == (size_t)width * height && Empty(*pixels, width, cell)) continue;
                out.push_back(cell);
            }
        return out;
    }

    std::vector<glm::ivec4> GridBySize(int width, int height, glm::ivec2 cell, glm::ivec2 offset, glm::ivec2 gap, const std::vector<uint32_t>* pixels)
    {
        std::vector<glm::ivec4> out;
        cell = glm::max(cell, glm::ivec2(1));
        gap = glm::max(gap, glm::ivec2(0));
        offset = glm::max(offset, glm::ivec2(0));
        for (int y = offset.y; y < height; y += cell.y + gap.y)
            for (int x = offset.x; x < width; x += cell.x + gap.x)
            {
                glm::ivec4 r(x, y, std::min(cell.x, width - x), std::min(cell.y, height - y));
                if (pixels && pixels->size() == (size_t)width * height && Empty(*pixels, width, r)) continue;
                out.push_back(r);
            }
        return out;
    }

    std::vector<glm::ivec4> Islands(const std::vector<uint32_t>& pixels, int width, int height, int alphaThreshold, int minSize)
    {
        std::vector<glm::ivec4> out;
        if (pixels.size() != (size_t)width * height) return out;
        std::vector<uint8_t> seen(pixels.size(), 0);
        std::vector<int> stack;
        auto solid = [&](int i) { return (int)(pixels[i] >> 24) > alphaThreshold; };
        for (int start = 0; start < (int)pixels.size(); ++start)
        {
            if (seen[start] || !solid(start)) continue;
            // Flood the group, growing its box.
            glm::ivec2 lo(width, height), hi(-1);
            stack.push_back(start);
            seen[start] = 1;
            while (!stack.empty())
            {
                int i = stack.back();
                stack.pop_back();
                int x = i % width, y = i / width;
                lo = glm::min(lo, glm::ivec2(x, y));
                hi = glm::max(hi, glm::ivec2(x, y));
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx)
                    {
                        int nx = x + dx, ny = y + dy;
                        if (nx < 0 || ny < 0 || nx >= width || ny >= height) continue;
                        int n = ny * width + nx;
                        if (seen[n] || !solid(n)) continue;
                        seen[n] = 1;
                        stack.push_back(n);
                    }
            }
            glm::ivec4 box(lo.x, lo.y, hi.x - lo.x + 1, hi.y - lo.y + 1);
            if (box.z >= minSize && box.w >= minSize) out.push_back(box);
        }
        // Row by row: top to bottom, a box joining the row above when its top is within half that row's first box,
        // each row left to right.
        std::sort(out.begin(), out.end(), [](const glm::ivec4& a, const glm::ivec4& b) { return a.y != b.y ? a.y < b.y : a.x < b.x; });
        for (size_t first = 0; first < out.size();)
        {
            size_t end = first + 1;
            while (end < out.size() && out[end].y < out[first].y + out[first].w / 2 + 1) ++end;
            std::sort(out.begin() + first, out.begin() + end, [](const glm::ivec4& a, const glm::ivec4& b) { return a.x < b.x; });
            first = end;
        }
        return out;
    }

    std::vector<SpriteRect> FromJson(const nlohmann::json& sprites)
    {
        std::vector<SpriteRect> out;
        if (!sprites.is_array()) return out;
        for (const auto& s : sprites)
        {
            if (!s.is_object()) continue;
            SpriteRect r;
            r.Name = s.value("name", std::string());
            auto rect = s.value("rect", nlohmann::json::array());
            if (rect.is_array() && rect.size() == 4)
                for (int k = 0; k < 4; ++k) r.Rect[k] = rect[k].get<int>();
            if (!r.Name.empty() && r.Rect.z > 0 && r.Rect.w > 0) out.push_back(std::move(r));
        }
        return out;
    }

    nlohmann::json ToJson(const std::vector<SpriteRect>& sprites)
    {
        nlohmann::json out = nlohmann::json::array();
        for (const SpriteRect& s : sprites) out.push_back({ { "name", s.Name }, { "rect", { s.Rect.x, s.Rect.y, s.Rect.z, s.Rect.w } } });
        return out;
    }
}
