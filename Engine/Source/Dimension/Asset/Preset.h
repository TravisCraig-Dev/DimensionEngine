#pragma once
#include "Dimension/Asset/AssetManager.h"
#include "Dimension/Scene/Scene.h"
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace Dimension
{
    // A component's settings saved as an asset (a .preset file, like Unity's Presets): applied to the same component on
    // other objects, and per component type a project default applied to new ones (Add Component). References to
    // objects are left out (they belong to one scene).
    struct Preset
    {
        std::string Component;                            // its registry name ("Point Light")
        nlohmann::json Fields = nlohmann::json::object(); // field name -> value, as scenes save them

        // The component's current settings on `e` (none when it has no such component).
        static std::optional<Preset> Capture(Scene& scene, Entity e, const std::string& component);
        // Sets the fields the preset has (the others keep theirs); false when `e` lacks the component.
        bool ApplyTo(Scene& scene, Entity e) const;

        nlohmann::json ToJson() const;
        static Preset FromJson(const nlohmann::json& j);
        bool Save(const fs::path& path) const;
        static std::optional<Preset> Load(const fs::path& path);
    };

    namespace Presets
    {
        // A preset asset (cached until its file changes).
        std::optional<Preset> Get(AssetHandle handle);
        // Every preset asset of a component type, by path.
        std::vector<AssetHandle> For(const std::string& component);
        // The project's default preset for a component type (None: none), kept in the project settings.
        AssetHandle DefaultFor(const std::string& component);
        void SetDefault(const std::string& component, AssetHandle preset);
        // Applies the component type's default preset to `e` (just given the component); true when there was one.
        bool ApplyDefault(Scene& scene, Entity e, const std::string& component);
    }
}
