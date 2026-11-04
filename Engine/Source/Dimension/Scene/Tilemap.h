#pragma once
#include "Dimension/Core/Guid.h"
#include <glm/glm.hpp>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace Dimension
{
    // Grid of tiles from a sprite-sheet tileset (Unity's Tilemap + TilemapRenderer + TilemapCollider2D).
    // Cells live in the entity's local XY plane; cell (x, y) covers [x, x+1) * CellSize by [y, y+1) * CellSize.
    struct TilemapComponent
    {
        AssetHandle Tileset = AssetHandle::None(); // sprite sheet
        int32_t Columns = 4;                       // tiles per row in the sheet
        int32_t Rows = 2;
        float CellSize = 1.0f;
        glm::vec4 Color{ 1.0f };
        int32_t SortingOrder = 0;
        bool Collider = true;    // every painted tile is solid (Box2D, merged into row boxes)
        float Friction = 0.6f;
        std::string Data;        // serialized cells "x,y,tile;..." (kept in sync by SetTile)

        static int64_t Key(int32_t x, int32_t y) { return (int64_t(y) << 32) | uint32_t(x); }
        static int32_t KeyX(int64_t key) { return int32_t(uint32_t(key & 0xFFFFFFFF)); }
        static int32_t KeyY(int64_t key) { return int32_t(key >> 32); }

        int32_t GetTile(int32_t x, int32_t y);           // -1 = empty
        void SetTile(int32_t x, int32_t y, int32_t tile); // tile < 0 clears the cell
        void Clear();
        const std::unordered_map<int64_t, int32_t>& Cells(); // decodes Data if it changed (e.g. after loading)
        size_t Count() { return Cells().size(); }

        // Local-space rectangles covering every painted tile, merged into horizontal runs per row:
        // xy = min corner, zw = size.
        std::vector<glm::vec4> ColliderBoxes();
        // Cell containing a point in the tilemap's local space.
        glm::ivec2 LocalToCell(const glm::vec2& local) const;
        // The center of a cell in world space (`world` is the object's transform), and the cell under a world point
        // (projected onto the tilemap's plane).
        glm::vec3 CellToWorld(const glm::mat4& world, const glm::ivec2& cell) const;
        glm::ivec2 WorldToCell(const glm::mat4& world, const glm::vec3& point) const;
        // The smallest and largest painted cells (inclusive); false when nothing is painted.
        bool CellBounds(glm::ivec2& min, glm::ivec2& max);
        // Changes whenever the cells do (SetTile, Clear, or Data replaced), e.g. to remake colliders.
        uint32_t Revision() { Cells(); return m_Revision; }

    private:
        std::unordered_map<int64_t, int32_t> m_Cells;
        size_t m_DecodedHash = 0;
        bool m_Decoded = false;
        uint32_t m_Revision = 0;
        void Decode();
        void Encode();
    };
}
