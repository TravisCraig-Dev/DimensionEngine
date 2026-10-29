#pragma once
#include "Dimension/Core/Base.h"
#include <vector>

namespace Dimension
{
    // The open project's recently opened scenes (File > Open Recent), newest first, at most kMax, kept in the project's
    // .dimension/RecentScenes.json (relative to the project, so the list moves with it).
    namespace RecentScenes
    {
        constexpr size_t kMax = 10;
        // The ones that still exist, as absolute paths.
        std::vector<fs::path> List();
        // Puts a scene first (moving it up when already listed).
        void Add(const fs::path& scene);
        void Remove(const fs::path& scene);
        void Clear();
    }
}
