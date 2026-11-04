#include "Dimension/Scene/Tilemap.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <map>

namespace Dimension
{
    void TilemapComponent::Decode()
    {
        m_Cells.clear();
        const char* p = Data.data();
        const char* end = p + Data.size();
        while (p < end)
        {
            int32_t v[3];
            bool ok = true;
            for (int i = 0; i < 3 && ok; ++i)
            {
                auto r = std::from_chars(p, end, v[i]);
                ok = r.ec == std::errc();
                p = r.ptr;
                if (ok && i < 2) { ok = p < end && *p == ','; ++p; }
            }
            if (ok && v[2] >= 0) m_Cells[Key(v[0], v[1])] = v[2];
            while (p < end && *p != ';') ++p; // skip to the next cell (also recovers from garbage)
            if (p < end) ++p;
        }
        m_DecodedHash = std::hash<std::string>{}(Data);
        m_Decoded = true;
        ++m_Revision;
    }

    void TilemapComponent::Encode()
    {
        // Sorted (row, then column) so saved scenes diff cleanly.
        std::map<std::pair<int32_t, int32_t>, int32_t> sorted;
        for (auto& [k, t] : m_Cells) sorted[{ KeyY(k), KeyX(k) }] = t;
        Data.clear();
        for (auto& [yx, t] : sorted)
        {
            Data += std::to_string(yx.second);
            Data += ',';
            Data += std::to_string(yx.first);
            Data += ',';
            Data += std::to_string(t);
            Data += ';';
        }
        m_DecodedHash = std::hash<std::string>{}(Data);
        m_Decoded = true;
    }

    const std::unordered_map<int64_t, int32_t>& TilemapComponent::Cells()
    {
        if (!m_Decoded || std::hash<std::string>{}(Data) != m_DecodedHash) Decode();
        return m_Cells;
    }

    int32_t TilemapComponent::GetTile(int32_t x, int32_t y)
    {
        auto& cells = Cells();
        auto it = cells.find(Key(x, y));
        return it != cells.end() ? it->second : -1;
    }

    void TilemapComponent::SetTile(int32_t x, int32_t y, int32_t tile)
    {
        Cells();
        auto it = m_Cells.find(Key(x, y));
        if (tile < 0 ? it == m_Cells.end() : it != m_Cells.end() && it->second == tile) return; // unchanged
        if (tile < 0) m_Cells.erase(it);
        else m_Cells[Key(x, y)] = tile;
        Encode();
        ++m_Revision;
    }

    void TilemapComponent::Clear()
    {
        m_Cells.clear();
        Encode();
        ++m_Revision;
    }

    std::vector<glm::vec4> TilemapComponent::ColliderBoxes()
    {
        std::map<int32_t, std::vector<int32_t>> rows;
        for (auto& [k, t] : Cells()) rows[KeyY(k)].push_back(KeyX(k));
        std::vector<glm::vec4> boxes;
        for (auto& [y, xs] : rows)
        {
            std::sort(xs.begin(), xs.end());
            size_t i = 0;
            while (i < xs.size())
            {
                size_t j = i;
                while (j + 1 < xs.size() && xs[j + 1] == xs[j] + 1) ++j;
                boxes.push_back({ xs[i] * CellSize, y * CellSize, float(xs[j] - xs[i] + 1) * CellSize, CellSize });
                i = j + 1;
            }
        }
        return boxes;
    }

    glm::ivec2 TilemapComponent::LocalToCell(const glm::vec2& local) const
    {
        float cs = CellSize > 0.0f ? CellSize : 1.0f;
        return { (int32_t)std::floor(local.x / cs), (int32_t)std::floor(local.y / cs) };
    }

    glm::vec3 TilemapComponent::CellToWorld(const glm::mat4& world, const glm::ivec2& cell) const
    {
        float cs = CellSize > 0.0f ? CellSize : 1.0f;
        return glm::vec3(world * glm::vec4((glm::vec2(cell) + 0.5f) * cs, 0.0f, 1.0f));
    }

    glm::ivec2 TilemapComponent::WorldToCell(const glm::mat4& world, const glm::vec3& point) const
    {
        return LocalToCell(glm::vec2(glm::inverse(world) * glm::vec4(point, 1.0f)));
    }

    bool TilemapComponent::CellBounds(glm::ivec2& min, glm::ivec2& max)
    {
        auto& cells = Cells();
        if (cells.empty()) return false;
        min = glm::ivec2(INT32_MAX);
        max = glm::ivec2(INT32_MIN);
        for (auto& [k, t] : cells)
        {
            glm::ivec2 c(KeyX(k), KeyY(k));
            min = glm::min(min, c);
            max = glm::max(max, c);
        }
        return true;
    }
}
