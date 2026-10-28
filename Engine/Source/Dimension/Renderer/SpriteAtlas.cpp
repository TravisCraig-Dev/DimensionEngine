#include "Dimension/Renderer/SpriteAtlas.h"
#include "Dimension/Asset/AssetManager.h"
#include "Dimension/Core/Log.h"
#include "Dimension/Renderer/Texture.h"
#include <nlohmann/json.hpp>
#include <stb_image.h>
#include <algorithm>
#include <fstream>
#include <numeric>

namespace Dimension
{
    RectPacking PackRects(const std::vector<glm::ivec2>& sizes, int padding, int maxSize)
    {
        RectPacking out;
        size_t n = sizes.size();
        padding = std::max(padding, 0);
        maxSize = std::max(maxSize, 16);
        std::vector<size_t> order(n);
        std::iota(order.begin(), order.end(), 0);
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return sizes[a].y > sizes[b].y; });
        // Shelf packing on a page of width w, height h: true when all fit.
        auto place = [&](int w, int h, bool partial) {
            out.Positions.assign(n, glm::ivec2(0));
            out.Placed.assign(n, false);
            int x = padding, y = padding, shelf = 0;
            bool all = true;
            for (size_t i : order)
            {
                glm::ivec2 s = sizes[i];
                if (s.x <= 0 || s.y <= 0 || s.x + 2 * padding > w || s.y + 2 * padding > h)
                {
                    all = false;
                    continue;
                }
                if (x + s.x + padding > w)
                {
                    y += shelf + padding;
                    x = padding;
                    shelf = 0;
                }
                if (y + s.y + padding > h)
                {
                    all = false;
                    if (!partial) return false;
                    continue;
                }
                out.Positions[i] = { x, y };
                out.Placed[i] = true;
                x += s.x + padding;
                shelf = std::max(shelf, s.y);
            }
            return all;
        };
        for (int size = 16; size <= maxSize; size *= 2)
            for (int wide = 0; wide < 2; ++wide)
            {
                int w = wide ? std::min(size * 2, maxSize) : size, h = size;
                if (wide && w == size) continue;
                if (place(w, h, false))
                {
                    out.Width = (uint32_t)w;
                    out.Height = (uint32_t)h;
                    return out;
                }
            }
        place(maxSize, maxSize, true);
        out.Width = out.Height = (uint32_t)maxSize;
        return out;
    }

    Ref<SpriteAtlas> SpriteAtlas::Load(const fs::path& path)
    {
        auto atlas = MakeRef<SpriteAtlas>();
        try
        {
            nlohmann::json j = nlohmann::json::parse(std::ifstream(path));
            for (const auto& s : j.value("Sprites", nlohmann::json::array()))
                if (s.is_number()) atlas->Sprites.push_back(AssetHandle(s.get<uint64_t>()));
            atlas->Padding = std::clamp(j.value("Padding", 2), 0, 64);
            atlas->MaxSize = std::clamp(j.value("Max Size", 2048), 64, 8192);
        }
        catch (const std::exception& e)
        {
            Log::Warn("Could not read sprite atlas '{}': {}", path.string(), e.what());
        }
        return atlas;
    }

    bool SpriteAtlas::Save(const fs::path& path) const
    {
        nlohmann::json j;
        j["Sprites"] = nlohmann::json::array();
        for (AssetHandle h : Sprites) j["Sprites"].push_back((uint64_t)h);
        j["Padding"] = Padding;
        j["Max Size"] = MaxSize;
        std::ofstream out(path);
        if (!out) return false;
        out << j.dump(2);
        return true;
    }

    void SpriteAtlas::Pack()
    {
        Regions.clear();
        Left.clear();
        Pixels.clear();
        Width = Height = 0;
        Texture.reset();
        struct Source
        {
            AssetHandle Handle;
            int W = 0, H = 0;
            std::vector<uint32_t> Pixels;
        };
        std::vector<Source> sources;
        for (AssetHandle h : Sprites)
        {
            if (std::any_of(sources.begin(), sources.end(), [&](const Source& s) { return s.Handle == h; })) continue;
            const AssetMetadata* meta = AssetManager::GetMetadata(h);
            int w = 0, hgt = 0, channels = 0;
            stbi_uc* data = meta && meta->Type == AssetType::Texture ? stbi_load(AssetManager::GetAbsolutePath(h).string().c_str(), &w, &hgt, &channels, 4) : nullptr;
            if (!data)
            {
                Left.push_back(h);
                continue;
            }
            Source s{ h, w, hgt, std::vector<uint32_t>((size_t)w * hgt) };
            std::memcpy(s.Pixels.data(), data, s.Pixels.size() * 4);
            stbi_image_free(data);
            sources.push_back(std::move(s));
        }
        std::vector<glm::ivec2> sizes;
        for (const Source& s : sources) sizes.push_back({ s.W, s.H });
        RectPacking packing = PackRects(sizes, Padding, MaxSize);
        Width = packing.Width;
        Height = packing.Height;
        Pixels.assign((size_t)Width * Height, 0u);
        for (size_t i = 0; i < sources.size(); ++i)
        {
            const Source& s = sources[i];
            if (!packing.Placed[i])
            {
                Left.push_back(s.Handle);
                continue;
            }
            glm::ivec2 at = packing.Positions[i];
            // The sprite, its edges repeated out across the padding (half way to the next one).
            int bleed = Padding / 2;
            for (int y = -bleed; y < s.H + bleed; ++y)
                for (int x = -bleed; x < s.W + bleed; ++x)
                {
                    int px = at.x + x, py = at.y + y;
                    if (px < 0 || py < 0 || px >= (int)Width || py >= (int)Height) continue;
                    int sx = std::clamp(x, 0, s.W - 1), sy = std::clamp(y, 0, s.H - 1);
                    Pixels[(size_t)py * Width + px] = s.Pixels[(size_t)sy * s.W + sx];
                }
            Regions[s.Handle] = glm::vec4((float)at.x / Width, (float)at.y / Height, (float)(at.x + s.W) / Width, (float)(at.y + s.H) / Height);
        }
        if (!Left.empty()) Log::Warn("Sprite atlas: {} sprite(s) not packed (missing, unreadable, or larger than Max Size allows)", Left.size());
    }

    void SpriteAtlas::CreateTexture()
    {
        if (Pixels.empty() || Width == 0) return;
        TextureSettings ts;
        if (!Sprites.empty())
            if (const AssetMetadata* meta = AssetManager::GetMetadata(Sprites.front()))
            {
                ts.Filter = (TextureFilter)meta->ImportSettings.value("filter", 0);
                ts.SRGB = meta->ImportSettings.value("srgb", true);
                ts.GenerateMips = meta->ImportSettings.value("mips", true);
            }
        Texture = MakeRef<Texture2D>(Pixels.data(), Width, Height, ts, "SpriteAtlas");
    }
}
