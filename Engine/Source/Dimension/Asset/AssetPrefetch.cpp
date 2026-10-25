#include "Dimension/Asset/AssetPrefetch.h"
#include "Dimension/Renderer/Mesh.h"
#include <stb_image.h>
#include <chrono>
#include <mutex>
#include <unordered_map>

namespace Dimension
{
    std::optional<DecodedImage> DecodedImage::Decode(const fs::path& file)
    {
        DecodedImage out;
        int w = 0, h = 0, channels = 0;
        std::string utf8 = file.string();
        if (stbi_is_hdr(utf8.c_str()))
        {
            float* hdr = stbi_loadf(utf8.c_str(), &w, &h, &channels, 4);
            if (!hdr) return std::nullopt;
            out.Rgba32f.assign(hdr, hdr + (size_t)w * h * 4);
            stbi_image_free(hdr);
        }
        else
        {
            stbi_uc* pixels = stbi_load(utf8.c_str(), &w, &h, &channels, 4);
            if (!pixels) return std::nullopt;
            out.Rgba8.assign(pixels, pixels + (size_t)w * h * 4);
            stbi_image_free(pixels);
        }
        out.Width = (uint32_t)w;
        out.Height = (uint32_t)h;
        return out;
    }

    namespace AssetPrefetch
    {
        namespace
        {
            using Clock = std::chrono::steady_clock;
            struct Entry
            {
                std::optional<DecodedImage> Image;
                std::shared_ptr<MeshData> Mesh;
                Clock::time_point Stored;
            };
            std::mutex s_Mutex;
            std::unordered_map<std::string, Entry> s_Entries;
            std::string Key(const fs::path& file) { return fs::weakly_canonical(file).generic_string(); }
        }

        void StoreImage(const fs::path& file, DecodedImage image)
        {
            std::string key = Key(file);
            std::lock_guard lock(s_Mutex);
            s_Entries[key] = { std::move(image), nullptr, Clock::now() };
        }

        std::optional<DecodedImage> TakeImage(const fs::path& file)
        {
            std::string key = Key(file);
            std::lock_guard lock(s_Mutex);
            auto it = s_Entries.find(key);
            if (it == s_Entries.end() || !it->second.Image) return std::nullopt;
            std::optional<DecodedImage> image = std::move(it->second.Image);
            s_Entries.erase(it);
            return image;
        }

        void StoreMesh(const fs::path& file, std::shared_ptr<MeshData> mesh)
        {
            std::string key = Key(file);
            std::lock_guard lock(s_Mutex);
            s_Entries[key] = { std::nullopt, std::move(mesh), Clock::now() };
        }

        std::shared_ptr<MeshData> FindMesh(const fs::path& file)
        {
            std::string key = Key(file);
            std::lock_guard lock(s_Mutex);
            auto it = s_Entries.find(key);
            return it != s_Entries.end() ? it->second.Mesh : nullptr;
        }

        bool Contains(const fs::path& file)
        {
            std::string key = Key(file);
            std::lock_guard lock(s_Mutex);
            return s_Entries.contains(key);
        }

        void Trim(float seconds)
        {
            auto now = Clock::now();
            std::lock_guard lock(s_Mutex);
            std::erase_if(s_Entries, [&](const auto& kv) { return std::chrono::duration<float>(now - kv.second.Stored).count() > seconds; });
        }

        void Clear()
        {
            std::lock_guard lock(s_Mutex);
            s_Entries.clear();
        }

        size_t Size()
        {
            std::lock_guard lock(s_Mutex);
            return s_Entries.size();
        }
    }
}
