#pragma once
#include "Dimension/Core/Base.h"
#include <cstdint>
#include <string>
#include <vector>

namespace Dimension
{
    // A packed archive of a folder's files (".pak"): the shipped game's Assets in one file. Layout: "DIMPAK1\0", the
    // file count and a 64-bit hash of every path and byte (which names its unpacked copy), the table (path as UTF-8,
    // offset, size), then the files' bytes.
    namespace AssetPack
    {
        struct Entry
        {
            std::string Path; // relative, with '/'
            uint64_t Offset = 0, Size = 0;
        };

        // Packs `files` (relative to `root`) into `pak`. False when a file cannot be read or the pack written.
        bool Write(const fs::path& pak, const fs::path& root, const std::vector<fs::path>& files);
        // Every file under `root`, relative (for packing a whole folder).
        std::vector<fs::path> ListFiles(const fs::path& root);
        // The table and hash of a pack (false when it is not one).
        bool Read(const fs::path& pak, std::vector<Entry>& entries, uint64_t& hash);
        // Unpacks it into `folder` (made). False when it is not a pack or a file cannot be written.
        bool Extract(const fs::path& pak, const fs::path& folder);
        // Where a pack is unpacked for a product: once per pack contents, in the user's local app data
        // (<LOCALAPPDATA>/<product>/AssetCache/<hash>), done the first time and reused (a marker file says it is complete).
        // Returns the folder, or an empty path when unpacking failed.
        fs::path Mount(const fs::path& pak, const std::string& product);
        // An asset bundle (a pack of a project folder with its .meta files, made by the editor's Build Asset Bundle)
        // added to the running game: unpacked into Assets/Bundles/<pack name> (again only when its contents changed)
        // and imported, so its scenes, prefabs and other assets work like the game's own (their GUIDs kept). Returns
        // the folder Assets-relative ("Bundles/<name>"), or empty when it is not a pack. Needs an open asset database.
        std::string LoadBundle(const fs::path& pak);
        // Every asset in a loaded bundle, Assets-relative (no .meta files).
        std::vector<std::string> BundleAssets(const fs::path& pak);
    }
}
