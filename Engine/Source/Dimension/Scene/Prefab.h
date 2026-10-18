#pragma once
#include "Dimension/Scene/Scene.h"
#include <nlohmann/json.hpp>

namespace Dimension
{
    // Prefab instances (like Unity): entities instantiated from a .prefab asset keep a
    // PrefabLinkComponent mapping them to their source entity in the file. Edits made to an
    // instance are recorded as overrides; Sync() pulls everything else from the asset.
    //
    // Override keys: "Component.Field", "+Component" (added on the instance), "-Component" (removed).
    // The root's position, rotation and name always belong to the instance.
    namespace Prefabs
    {
        Entity Instantiate(Scene& scene, AssetHandle prefab, Entity parent = Entity::Null());

        // Saves `root` (with children) as a prefab file and links the entities to it.
        AssetHandle CreateFromEntity(Scene& scene, Entity root, const fs::path& path);

        Entity FindInstanceRoot(Scene& scene, Entity e);
        bool IsInstanceOwned(bool isRoot, const std::string& component, const std::string& field);

        // Writes the instance into its prefab asset, clears its overrides and updates other instances.
        bool Apply(Scene& scene, Entity instanceRoot);
        // Discards overrides and objects added to the instance.
        void Revert(Scene& scene, Entity instanceRoot);
        // Removes the link: the entities become ordinary scene objects.
        void Unpack(Scene& scene, Entity instanceRoot);

        void Sync(Scene& scene, Entity instanceRoot);
        void SyncAll(Scene& scene);

        void RecordFieldOverride(Scene& scene, Entity e, const std::string& component, const std::string& field);
        void RecordComponentAdded(Scene& scene, Entity e, const std::string& component);
        void RecordComponentRemoved(Scene& scene, Entity e, const std::string& component);
        bool IsOverridden(Scene& scene, Entity e, const std::string& component, const std::string& field);

        // Rewrites EntityRef values inside a script's serialized fields.
        void RemapScriptEntityRefs(nlohmann::json& fields, const std::string& scriptClass, const std::function<uint64_t(uint64_t)>& remap);
    }
}
