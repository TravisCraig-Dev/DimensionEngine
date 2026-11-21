#pragma once
#include "Dimension/Core/Base.h"

namespace Dimension
{
    struct MeshData;
    struct DecodedImage;
    struct CompressedImage;

    // Import cache (like Unity's Library folder): the result of importing a source asset (decoded image pixels, a parsed
    // and tangent-generated model) is kept in the project's .dimension/Cache/Import, keyed by the file's path, size,
    // modification time and the importer version, so the next load reads it straight back. Builds ship the cache. A
    // changed file gets a new key; stale entries are left for ClearUnused. Off when no project is open.
    namespace ImportCache
    {
        bool LoadImage(const fs::path& source, DecodedImage& out);
        void StoreImage(const fs::path& source, const DecodedImage& image);
        // A texture's GPU-compressed blocks; `variant` names the settings that made them.
        bool LoadCompressedImage(const fs::path& source, const std::string& variant, CompressedImage& out);
        void StoreCompressedImage(const fs::path& source, const std::string& variant, const CompressedImage& image);
        // `variant` tells imports with other settings apart (empty for the defaults).
        bool LoadMesh(const fs::path& source, const std::string& variant, MeshData& out);
        void StoreMesh(const fs::path& source, const std::string& variant, const MeshData& mesh);

        fs::path Directory(); // empty without a project
        // Removes entries whose source file no longer has that key (changed or deleted). Returns how many.
        size_t ClearUnused(const fs::path& assetsDir);
        void SetEnabled(bool enabled); // on by default (tests turn it off to measure imports)
    }
}
