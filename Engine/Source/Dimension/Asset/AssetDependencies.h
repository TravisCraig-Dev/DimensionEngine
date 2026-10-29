#pragma once
#include "Dimension/Core/Base.h"
#include "Dimension/Core/Guid.h"
#include <nlohmann/json.hpp>
#include <unordered_set>
#include <vector>

namespace Dimension
{
    // What a build needs from the Assets folder.
    struct AssetDependencySet
    {
        std::unordered_set<AssetHandle> Assets; // assets to ship (with their .meta files)
        std::vector<fs::path> ExtraFiles;        // other files assets load by path (glTF buffers/images, OBJ .mtl), Assets-relative
    };

    namespace AssetDependencies
    {
        // Everything reachable from `roots`: JSON assets (scenes, prefabs, materials, animator controllers) and every
        // asset's import settings are scanned for asset GUIDs; .gltf and .obj models add the files they reference.
        // Assets under Assets/Resources are included too (scripts may load them by path) unless `resources` is false.
        AssetDependencySet Collect(const std::vector<AssetHandle>& roots, bool resources = true);
        // The assets one asset references itself (its file and import settings; not what those reference in turn).
        std::vector<AssetHandle> Direct(AssetHandle asset);
        // The assets that reference this one directly (the Project window's Used By).
        std::vector<AssetHandle> UsedBy(AssetHandle asset);
        // The asset GUIDs in a JSON value (a component's or a scene's saved values), each once, in order found.
        std::vector<AssetHandle> References(const nlohmann::json& value);
    }
}
