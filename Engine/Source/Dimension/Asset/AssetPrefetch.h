#pragma once
#include "Dimension/Core/Base.h"
#include <memory>
#include <optional>
#include <vector>

namespace Dimension
{
    struct MeshData;

    // A decoded image file (CPU only): 8-bit RGBA, or 32-bit float RGBA for HDR files.
    struct DecodedImage
    {
        uint32_t Width = 0, Height = 0;
        std::vector<uint8_t> Rgba8;
        std::vector<float> Rgba32f; // HDR files
        bool IsHdr() const { return !Rgba32f.empty(); }
        static std::optional<DecodedImage> Decode(const fs::path& file); // thread-safe
    };

    // Asset data decoded ahead of time on a worker thread (asynchronous scene loads). The asset manager takes from
    // here instead of reading the file again; GPU uploads still happen on the main thread. Keyed by absolute path.
    namespace AssetPrefetch
    {
        void StoreImage(const fs::path& file, DecodedImage image);
        std::optional<DecodedImage> TakeImage(const fs::path& file);
        void StoreMesh(const fs::path& file, std::shared_ptr<MeshData> mesh);
        std::shared_ptr<MeshData> FindMesh(const fs::path& file); // shared: GPU meshes and collider data both use it
        bool Contains(const fs::path& file);
        // Drops entries older than `seconds` (prefetched data nobody asked for).
        void Trim(float seconds);
        void Clear();
        size_t Size();
    }
}
