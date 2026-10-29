#pragma once
#include "Dimension/Core/Base.h"
#include <string>
#include <vector>

namespace Dimension
{
    // A Project window favorite (like Unity's): a folder, or a saved search ("t:Texture l:Wall").
    struct Favorite
    {
        std::string Name;
        std::string Folder; // Assets-relative ("" = Assets) when it is a folder
        std::string Search; // the query when it is a saved search
        bool IsSearch() const { return !Search.empty(); }
        bool operator==(const Favorite&) const = default;
    };

    // The open project's favorites, pinned at the top of the Project window's folder tree, in the order added. They are
    // kept per user in the project's .dimension/Favorites.json (like the recent scenes).
    namespace ProjectFavorites
    {
        // Folders that no longer exist are left out (and kept, should they come back).
        std::vector<Favorite> List();
        // A folder (absolute, or relative to Assets), named after it; false if it is already a favorite or not a folder.
        bool AddFolder(const fs::path& folder);
        // A search, named after the query unless named; false if it is empty or already saved.
        bool AddSearch(const std::string& query, const std::string& name = {});
        void Remove(const Favorite& favorite);
        void Rename(const Favorite& favorite, const std::string& name);
    }
}
