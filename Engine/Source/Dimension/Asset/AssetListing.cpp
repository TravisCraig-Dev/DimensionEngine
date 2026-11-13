#include "Dimension/Asset/AssetListing.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>

namespace Dimension::AssetListing
{
    namespace
    {
        std::string Lower(std::string s)
        {
            for (auto& c : s) c = (char)std::tolower((unsigned char)c);
            return s;
        }
    }

    AssetListEntry Describe(const fs::path& path)
    {
        AssetListEntry e;
        e.Path = path;
        std::error_code ec;
        e.Folder = fs::is_directory(path, ec);
        e.Name = e.Folder ? path.filename().string() : path.stem().string();
        e.Modified = fs::last_write_time(path, ec);
        if (!e.Folder)
        {
            e.Size = fs::file_size(path, ec);
            if (ec) e.Size = 0;
            e.Handle = AssetManager::GetHandle(path);
            if (const AssetMetadata* m = AssetManager::GetMetadata(e.Handle)) e.Type = m->Type;
        }
        return e;
    }

    std::string TypeName(const AssetListEntry& entry)
    {
        if (entry.Folder) return "Folder";
        return entry.Type == AssetType::None ? "File" : AssetTypeToString(entry.Type);
    }

    void Sort(std::vector<AssetListEntry>& entries, AssetSort by, bool ascending)
    {
        std::stable_sort(entries.begin(), entries.end(), [&](const AssetListEntry& a, const AssetListEntry& b) {
            if (a.Folder != b.Folder) return a.Folder;
            std::string na = Lower(a.Name), nb = Lower(b.Name);
            if (a.Folder) return na < nb; // folders keep name order
            // Ties (same type, size or time) fall back to the name, ascending either way.
            auto order = [&](auto x, auto y) {
                if (x == y) return na < nb;
                return ascending ? x < y : y < x;
            };
            switch (by)
            {
            case AssetSort::Type: return order(Lower(TypeName(a)), Lower(TypeName(b)));
            case AssetSort::Size: return order(a.Size, b.Size);
            case AssetSort::Modified: return order(a.Modified, b.Modified);
            default:
                if (na == nb) return a.Path < b.Path;
                return ascending ? na < nb : nb < na;
            }
        });
    }

    std::string FormatSize(uintmax_t bytes)
    {
        char buf[32];
        if (bytes < 1024) std::snprintf(buf, sizeof(buf), "%llu B", (unsigned long long)bytes);
        else if (bytes < 1024ull * 1024) std::snprintf(buf, sizeof(buf), "%.1f KB", bytes / 1024.0);
        else if (bytes < 1024ull * 1024 * 1024) std::snprintf(buf, sizeof(buf), "%.1f MB", bytes / (1024.0 * 1024.0));
        else std::snprintf(buf, sizeof(buf), "%.1f GB", bytes / (1024.0 * 1024.0 * 1024.0));
        return buf;
    }

    std::string FormatTime(fs::file_time_type time)
    {
        auto system = std::chrono::clock_cast<std::chrono::system_clock>(time);
        std::time_t t = std::chrono::system_clock::to_time_t(system);
        std::tm local{};
#ifdef _WIN32
        localtime_s(&local, &t);
#else
        localtime_r(&t, &local);
#endif
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &local);
        return buf;
    }
}
