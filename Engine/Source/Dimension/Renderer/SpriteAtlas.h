#pragma once
#include "Dimension/Core/Base.h"
#include "Dimension/Core/Guid.h"
#include <glm/glm.hpp>
#include <unordered_map>
#include <vector>

namespace Dimension
{
    class Texture2D;

    // Rectangles packed into one page: shelves of the tallest first, Padding apart (and from the edges), on the
    // smallest power-of-two square (or twice as wide) that holds them all, up to Max Size. What does not fit even
    // then is left out (Placed false).
    struct RectPacking
    {
        uint32_t Width = 0, Height = 0;
        std::vector<glm::ivec2> Positions; // per rectangle: its top-left corner
        std::vector<bool> Placed;
    };
    RectPacking PackRects(const std::vector<glm::ivec2>& sizes, int padding, int maxSize);

    // A Sprite Atlas (.spriteatlas, like Unity's): sprite textures packed into one image. Sprite Renderers showing
    // any of them draw from the atlas instead (their UVs remapped into its region), so a scene's sprites share one
    // texture; builds ship it with the sprites. Each sprite's edge pixels are repeated into the padding around it,
    // so filtering does not pick up its neighbours. The atlas is filtered like its first sprite.
    struct SpriteAtlas
    {
        std::vector<AssetHandle> Sprites;
        int32_t Padding = 2;
        int32_t MaxSize = 2048;

        // Packed (Pack): the page's pixels (RGBA8, rows top-down), and per sprite its region in UVs (xy min, zw max).
        uint32_t Width = 0, Height = 0;
        std::vector<uint32_t> Pixels;
        std::unordered_map<AssetHandle, glm::vec4> Regions;
        std::vector<AssetHandle> Left; // not packed: missing, unreadable, or did not fit
        Ref<Texture2D> Texture;        // the page on the GPU (CreateTexture)

        static Ref<SpriteAtlas> Load(const fs::path& path);
        bool Save(const fs::path& path) const;
        // Reads the sprites' image files and packs them (CPU only).
        void Pack();
        // Uploads the packed page (filtered like the first sprite).
        void CreateTexture();
    };
}
