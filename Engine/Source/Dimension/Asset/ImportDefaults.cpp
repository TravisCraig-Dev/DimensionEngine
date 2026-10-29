#include "Dimension/Asset/ImportDefaults.h"
#include "Dimension/Project/Project.h"
#include <algorithm>

namespace Dimension::ImportDefaults
{
    namespace
    {
        // "Textures/Pixel/" and "./Textures/Pixel" are "Textures/Pixel"; "" and "." are the Assets folder.
        std::string Normalize(const std::string& folder)
        {
            std::string s = fs::path(folder).lexically_normal().generic_string();
            while (!s.empty() && s.back() == '/') s.pop_back();
            return s == "." ? std::string() : s;
        }

        // Whether `path` is in `folder` or under it ("" holds everything).
        bool Under(const fs::path& path, const std::string& folder)
        {
            if (folder.empty()) return true;
            fs::path f = fs::path(folder).lexically_normal();
            fs::path normal = path.lexically_normal();
            auto p = normal.begin();
            for (auto it = f.begin(); it != f.end(); ++it, ++p)
                if (p == normal.end() || *p != *it) return false;
            return true;
        }

        size_t Depth(const std::string& folder)
        {
            if (folder.empty()) return 0;
            fs::path f(folder);
            return (size_t)std::distance(f.begin(), f.end());
        }

        nlohmann::json OnlyKeys(const nlohmann::json& settings, AssetType type)
        {
            nlohmann::json out = nlohmann::json::object();
            for (const std::string& k : Keys(type))
                if (settings.is_object() && settings.contains(k)) out[k] = settings[k];
            return out;
        }
    }

    const std::vector<std::string>& Keys(AssetType type)
    {
        static const std::vector<std::string> texture = { "filter", "srgb", "mips" };
        static const std::vector<std::string> mesh = { "scale", "animations", "importMaterials" };
        static const std::vector<std::string> none;
        return type == AssetType::Texture ? texture : type == AssetType::Mesh ? mesh : none;
    }

    const ImportRule* For(const fs::path& assetPath, AssetType type)
    {
        const ImportRule* best = nullptr;
        for (const ImportRule& r : Project::GetConfig().ImportRules)
            if (r.Type == type && Under(assetPath.parent_path(), r.Folder) && (!best || Depth(r.Folder) > Depth(best->Folder))) best = &r;
        return best;
    }

    void Set(const std::string& folder, AssetType type, const nlohmann::json& settings)
    {
        std::string normal = Normalize(folder);
        auto& rules = Project::GetConfig().ImportRules;
        for (ImportRule& r : rules)
            if (r.Folder == normal && r.Type == type)
            {
                r.Settings = OnlyKeys(settings, type);
                return;
            }
        rules.push_back({ normal, type, OnlyKeys(settings, type) });
    }

    void Remove(const std::string& folder, AssetType type)
    {
        std::string normal = Normalize(folder);
        std::erase_if(Project::GetConfig().ImportRules, [&](const ImportRule& r) { return r.Folder == normal && r.Type == type; });
    }

    int ApplyToExisting(const std::string& folder, AssetType type)
    {
        std::string normal = Normalize(folder);
        std::vector<AssetHandle> targets;
        nlohmann::json settings;
        for (AssetHandle h : AssetManager::GetAssetsOfType(type))
        {
            const AssetMetadata* meta = AssetManager::GetMetadata(h);
            if (!meta || !Under(meta->Path.parent_path(), normal)) continue;
            const ImportRule* rule = For(meta->Path, type);
            if (!rule || rule->Folder != normal) continue; // a deeper rule speaks for it
            targets.push_back(h);
            settings = rule->Settings;
        }
        return targets.empty() ? 0 : ApplySettings(targets, settings);
    }

    int ApplySettings(const std::vector<AssetHandle>& assets, const nlohmann::json& changes)
    {
        int changed = 0;
        for (AssetHandle h : assets)
        {
            const AssetMetadata* meta = AssetManager::GetMetadata(h);
            if (!meta) continue;
            nlohmann::json settings = meta->ImportSettings;
            nlohmann::json before = settings;
            for (auto& [k, v] : changes.items()) settings[k] = v;
            if (settings == before) continue;
            AssetManager::SetImportSettings(h, settings);
            ++changed;
        }
        return changed;
    }
}
