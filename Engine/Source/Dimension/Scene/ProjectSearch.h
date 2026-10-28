#pragma once
#include "Dimension/Asset/AssetManager.h"
#include <string>
#include <vector>

namespace Dimension
{
    // The Search window's look into scene files that are not open (like Unity's Search): objects of the project's
    // scenes matched by the same terms as the Hierarchy's search: words in the object's name, "t:Type" a component it
    // has (spaces ignored, a prefix will do) and "tag:Name" its tag. Each file is read once until it changes.
    namespace ProjectSearch
    {
        struct SceneObject
        {
            AssetHandle Scene = AssetHandle::None();
            uint64_t Object = 0; // its GUID in that scene
            std::string Name;
        };
        // Matches in every scene asset but `skip` (the open one: search it with SceneSearch), by scene path, then in
        // the scene's order; at most `limit`.
        std::vector<SceneObject> FindInScenes(const std::string& query, AssetHandle skip = AssetHandle::None(), size_t limit = 200);
    }
}
