#pragma once
#include "Dimension/Asset/AssetManager.h"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace Dimension
{
    // Import defaults per folder (like Unity's import presets with folder filters): a rule gives the import settings
    // new assets of a type get when they arrive in a folder (or any folder under it; the deepest rule wins). Rules are
    // kept in the project settings. Batch editing changes the import settings of many assets at once.
    struct ImportRule
    {
        std::string Folder; // Assets-relative ("" = everywhere)
        AssetType Type = AssetType::None;
        nlohmann::json Settings = nlohmann::json::object();
    };

    namespace ImportDefaults
    {
        // The settings an import rule carries for a type (textures: filter, srgb, mips; models: scale, animations,
        // importMaterials); not labels, sprites or per-asset lists.
        const std::vector<std::string>& Keys(AssetType type);
        // The rule for an asset at an Assets-relative path (null: none).
        const ImportRule* For(const fs::path& assetPath, AssetType type);
        // Adds or replaces the rule of a folder and type (only its import keys are kept); removes it.
        void Set(const std::string& folder, AssetType type, const nlohmann::json& settings);
        void Remove(const std::string& folder, AssetType type);
        // Applies a folder's rule to the assets already in it (and under it, where no deeper rule applies); returns
        // how many changed.
        int ApplyToExisting(const std::string& folder, AssetType type);
        // Sets these settings on every asset given (other settings kept); returns how many changed.
        int ApplySettings(const std::vector<AssetHandle>& assets, const nlohmann::json& changes);
    }
}
