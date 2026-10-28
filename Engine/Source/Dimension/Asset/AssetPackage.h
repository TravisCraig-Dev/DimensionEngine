#pragma once
#include "Dimension/Asset/AssetManager.h"
#include <optional>
#include <string>
#include <vector>

namespace Dimension
{
    // Asset packages (a .dimensionpackage file, like Unity's .unitypackage): files of the Assets folder with their .meta
    // files, so the assets keep their GUIDs and the references between them hold in the project that imports them.
    namespace AssetPackage
    {
        struct Entry
        {
            fs::path Path;    // Assets-relative
            std::string Data; // the file's bytes
            std::string Meta; // its .meta file ("" when it has none: plain files such as glTF buffers)
            uint64_t Guid = 0; // from the .meta (0 without one)
        };
        struct Contents
        {
            std::vector<Entry> Files;
        };

        // Every asset in an Assets-relative folder and the folders under it ("" for all).
        std::vector<AssetHandle> AssetsIn(const fs::path& folder);
        // The Assets-relative files of `assets` and, with `dependencies`, of everything they reference (as a build
        // collects them), sorted.
        std::vector<fs::path> FilesFor(const std::vector<AssetHandle>& assets, bool dependencies);
        // Writes those files (each with its .meta) into a package.
        bool Export(const std::vector<fs::path>& files, const fs::path& package);
        std::optional<Contents> Read(const fs::path& package);

        // What importing a file would do: add it (with its GUID), find the same file there, replace a different one (the
        // project's own GUID kept), or add it under a new GUID (another asset of the project holds its own; references
        // to it from the package then reach that one).
        enum class Status { New, Same, Changed, GuidTaken };
        Status Check(const Entry& entry);
        // Writes the chosen entries (all when `chosen` is empty) into the Assets folder and imports them; returns how
        // many were written.
        int Import(const Contents& contents, const std::vector<bool>& chosen = {});
    }
}
