#pragma once
#include "Dimension/Core/Base.h"
#include <glm/glm.hpp>
#include <optional>
#include <vector>

namespace Dimension
{
    // A saved Scene view camera: where it orbits, how far, which way it looks, and 2D with its size.
    struct CameraBookmark
    {
        glm::vec3 Pivot{ 0.0f };
        float Distance = 10.0f, Yaw = 0.0f, Pitch = 0.0f;
        bool Is2D = false;
        float OrthoSize = 6.0f;
        bool operator==(const CameraBookmark&) const = default;
    };

    // Scene view camera bookmarks (slots 1 to 9) per scene, kept per user in the project's .dimension/SceneBookmarks.json
    // by the scene's path (an unsaved scene's under ""), so they follow a scene without changing its file.
    namespace SceneBookmarks
    {
        constexpr int kSlots = 9;
        std::optional<CameraBookmark> Get(const fs::path& scene, int slot);
        void Set(const fs::path& scene, int slot, const CameraBookmark& bookmark);
        void Clear(const fs::path& scene, int slot);
        // The slots in use, in order.
        std::vector<int> Slots(const fs::path& scene);
    }
}
