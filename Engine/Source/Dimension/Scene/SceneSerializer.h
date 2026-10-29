#pragma once
#include "Dimension/Scene/Scene.h"

namespace Dimension
{
    // JSON scene format:
    // { "name": ..., "settings": {...},
    //   "entities": [ { "guid": n, "parent": n|0, "components": { "<Component>": { field: value } } } ] }
    // Entities are written parent-first, so loading preserves hierarchy and sibling order.
    class SceneSerializer
    {
    public:
        static nlohmann::json Serialize(Scene& scene);
        // Only these root objects (with their children), in this order: one scene file of several open together.
        static nlohmann::json Serialize(Scene& scene, const std::vector<Entity>& roots);
        static Ref<Scene> Deserialize(const nlohmann::json& data);

        static bool SaveToFile(Scene& scene, const fs::path& path);
        static Ref<Scene> LoadFromFile(const fs::path& path);

        // Entity subtrees (prefabs, duplicate, copy/paste).
        static nlohmann::json SerializeEntityTree(Scene& scene, Entity root);
        // Instantiates a subtree. With newIdentity, every entity gets a fresh Guid and
        // references inside the subtree are remapped to the new copies.
        // `mapping` (optional) receives (serialized guid, created entity) pairs in record order.
        static Entity DeserializeEntityTree(Scene& scene, const nlohmann::json& tree, Entity parent, bool newIdentity = true,
                                            std::vector<std::pair<uint64_t, Entity>>* mapping = nullptr);

        static bool SavePrefab(Scene& scene, Entity root, const fs::path& path);

        // Adds every entity of a serialized scene to `scene` (keeping Guids unless one is already taken, in which case
        // all get new ones). Returns the created root entities.
        static std::vector<Entity> DeserializeInto(Scene& scene, const nlohmann::json& data);
        static void ReadSettings(const nlohmann::json& settings, SceneSettings& out);
    };
}
