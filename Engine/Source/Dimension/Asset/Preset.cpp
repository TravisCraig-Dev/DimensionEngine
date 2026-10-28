#include "Dimension/Asset/Preset.h"
#include "Dimension/Project/Project.h"
#include "Dimension/Scene/ComponentRegistry.h"
#include <algorithm>
#include <fstream>
#include <unordered_map>

namespace Dimension
{
    std::optional<Preset> Preset::Capture(Scene& scene, Entity e, const std::string& component)
    {
        const ComponentInfo* info = ComponentRegistry::Find(component);
        if (!info || !scene.IsValid(e) || !info->Has(scene, e)) return std::nullopt;
        Preset preset;
        preset.Component = component;
        void* data = info->Get(scene, e);
        for (const FieldInfo& f : info->Fields)
            if (f.Type != FieldType::EntityRef) preset.Fields[f.Name] = FieldToJson(f, data, nullptr);
        return preset;
    }

    bool Preset::ApplyTo(Scene& scene, Entity e) const
    {
        const ComponentInfo* info = ComponentRegistry::Find(Component);
        if (!info || !scene.IsValid(e) || !info->Has(scene, e)) return false;
        void* data = info->Get(scene, e);
        for (const FieldInfo& f : info->Fields)
            if (f.Type != FieldType::EntityRef && Fields.is_object() && Fields.contains(f.Name)) FieldFromJson(f, data, Fields[f.Name], nullptr);
        scene.UpdateTransforms();
        return true;
    }

    nlohmann::json Preset::ToJson() const { return { { "component", Component }, { "fields", Fields } }; }

    Preset Preset::FromJson(const nlohmann::json& j)
    {
        Preset p;
        p.Component = j.value("component", std::string());
        p.Fields = j.value("fields", nlohmann::json::object());
        return p;
    }

    bool Preset::Save(const fs::path& path) const
    {
        std::ofstream out(path);
        out << ToJson().dump(2);
        return (bool)out;
    }

    std::optional<Preset> Preset::Load(const fs::path& path)
    {
        std::ifstream in(path);
        if (!in) return std::nullopt;
        try
        {
            return FromJson(nlohmann::json::parse(in));
        }
        catch (const std::exception&)
        {
            return std::nullopt;
        }
    }

    namespace Presets
    {
        namespace
        {
            struct Cached
            {
                fs::file_time_type Time;
                Preset Value;
            };
            std::unordered_map<AssetHandle, Cached> s_Cache;
        }

        std::optional<Preset> Get(AssetHandle handle)
        {
            const AssetMetadata* meta = AssetManager::GetMetadata(handle);
            if (!meta || meta->Type != AssetType::Preset) return std::nullopt;
            fs::path path = AssetManager::GetAbsolutePath(handle);
            std::error_code ec;
            auto time = fs::last_write_time(path, ec);
            if (auto it = s_Cache.find(handle); it != s_Cache.end() && it->second.Time == time) return it->second.Value;
            auto loaded = Preset::Load(path);
            if (!loaded) return std::nullopt;
            s_Cache[handle] = { time, *loaded };
            return loaded;
        }

        std::vector<AssetHandle> For(const std::string& component)
        {
            std::vector<AssetHandle> out;
            for (AssetHandle h : AssetManager::Search("t:Preset")) // sorted by path
                if (auto p = Get(h); p && p->Component == component) out.push_back(h);
            return out;
        }

        AssetHandle DefaultFor(const std::string& component)
        {
            const auto& defaults = Project::GetConfig().DefaultPresets;
            auto it = defaults.find(component);
            return it == defaults.end() ? AssetHandle::None() : it->second;
        }

        void SetDefault(const std::string& component, AssetHandle preset)
        {
            auto& defaults = Project::GetConfig().DefaultPresets;
            if (preset.IsValid()) defaults[component] = preset;
            else defaults.erase(component);
        }

        bool ApplyDefault(Scene& scene, Entity e, const std::string& component)
        {
            AssetHandle h = DefaultFor(component);
            auto preset = h.IsValid() ? Get(h) : std::nullopt;
            return preset && preset->Component == component && preset->ApplyTo(scene, e);
        }
    }
}
