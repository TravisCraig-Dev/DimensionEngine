#pragma once
#include "Dimension/Asset/AssetManager.h"
#include <string>
#include <vector>

namespace Dimension
{
    // A row of the Project window's list view: a folder or a file in the Assets folder.
    struct AssetListEntry
    {
        fs::path Path; // absolute
        std::string Name;
        bool Folder = false;
        AssetHandle Handle = AssetHandle::None();
        AssetType Type = AssetType::None;
        uintmax_t Size = 0; // bytes (folders: 0)
        fs::file_time_type Modified{};
    };

    enum class AssetSort { Name, Type, Size, Modified };

    namespace AssetListing
    {
        AssetListEntry Describe(const fs::path& path);
        // The list view's "Type" column: "Folder", the asset type, or "File" for files the asset database ignores.
        std::string TypeName(const AssetListEntry& entry);
        // Folders first (by name), then the files by `by` (ties by name); name order ignores case.
        void Sort(std::vector<AssetListEntry>& entries, AssetSort by, bool ascending = true);
        // "812 B", "4.2 KB", "13.0 MB".
        std::string FormatSize(uintmax_t bytes);
        // Local time, "2026-10-28 21:05".
        std::string FormatTime(fs::file_time_type time);
    }
}
