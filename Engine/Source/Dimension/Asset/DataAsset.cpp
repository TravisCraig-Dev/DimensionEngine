#include "Dimension/Asset/DataAsset.h"
#include "Dimension/Asset/AssetManager.h"
#include "Dimension/Core/Log.h"
#include <algorithm>
#include <fstream>
#include <unordered_map>

namespace Dimension
{
    namespace
    {
        std::deque<DataAssets::ClassInfo> s_Classes;
        std::function<void(AssetHandle)> s_ChangedHook;

        // Each asset's class, read once per version of its file (the Inspector's pickers ask every frame).
        struct CachedClass
        {
            fs::file_time_type Time{};
            std::string Class;
        };
        std::unordered_map<uint64_t, CachedClass> s_ClassCache;
    }

    namespace DataAssets
    {
        ClassInfo& AddClass(const std::string& name, const std::string& module)
        {
            std::erase_if(s_Classes, [&](const ClassInfo& c) { return c.Name == name; });
            ClassInfo& info = s_Classes.emplace_back();
            info.Name = name;
            info.Module = module;
            return info;
        }

        void ClearModule(const std::string& module)
        {
            std::erase_if(s_Classes, [&](const ClassInfo& c) { return c.Module == module; });
        }

        const std::deque<ClassInfo>& Classes() { return s_Classes; }

        const ClassInfo* FindClass(const std::string& name)
        {
            for (const ClassInfo& c : s_Classes)
                if (c.Name == name) return &c;
            return nullptr;
        }

        bool IsA(const std::string& name, const std::string& base)
        {
            if (name.empty()) return false;
            if (base.empty() || base == "Dimension.DataAsset" || name == base) return true;
            const ClassInfo* info = FindClass(name);
            return info && std::find(info->Bases.begin(), info->Bases.end(), base) != info->Bases.end();
        }

        nlohmann::json Read(AssetHandle handle)
        {
            const AssetMetadata* meta = AssetManager::IsInitialized() ? AssetManager::GetMetadata(handle) : nullptr;
            if (!meta || meta->Type != AssetType::DataAsset) return nullptr;
            std::ifstream in(AssetManager::GetAbsolutePath(handle));
            nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
            if (j.is_discarded() || !j.is_object() || !j.contains("class") || !j["class"].is_string()) return nullptr;
            if (!j.contains("fields") || !j["fields"].is_object()) j["fields"] = nlohmann::json::object();
            return j;
        }

        std::string ClassOf(AssetHandle handle)
        {
            const AssetMetadata* meta = AssetManager::IsInitialized() ? AssetManager::GetMetadata(handle) : nullptr;
            if (!meta || meta->Type != AssetType::DataAsset) return {};
            std::error_code ec;
            auto time = fs::last_write_time(AssetManager::GetAbsolutePath(handle), ec);
            CachedClass& cached = s_ClassCache[(uint64_t)handle];
            if (cached.Time != time || cached.Class.empty())
            {
                nlohmann::json j = Read(handle);
                cached.Time = time;
                cached.Class = j.is_object() ? j["class"].get<std::string>() : std::string();
            }
            return cached.Class;
        }

        AssetHandle Write(const fs::path& absolutePath, const std::string& className, const nlohmann::json& fields)
        {
            nlohmann::json j{ { "class", className }, { "fields", fields.is_object() ? fields : nlohmann::json::object() } };
            {
                std::ofstream out(absolutePath);
                if (!out) return AssetHandle::None();
                out << j.dump(2);
            }
            AssetHandle h = AssetManager::GetHandle(absolutePath);
            if (!h.IsValid()) h = AssetManager::Import(absolutePath);
            if (h.IsValid())
            {
                std::error_code ec;
                s_ClassCache[(uint64_t)h] = { fs::last_write_time(absolutePath, ec), className };
                NotifyChanged(h);
            }
            return h;
        }

        AssetHandle Create(const fs::path& absolutePath, const std::string& className)
        {
            const ClassInfo* info = FindClass(className);
            if (!info)
            {
                Log::Warn("No data asset class '{}'", className);
                return AssetHandle::None();
            }
            nlohmann::json fields = nlohmann::json::object();
            if (info->Create)
                if (std::shared_ptr<void> storage = info->Create())
                    for (const FieldInfo& f : info->Fields) fields[f.Name] = FieldToJson(f, storage.get(), nullptr);
            return Write(absolutePath, className, fields);
        }

        void SetChangedHook(std::function<void(AssetHandle)> hook) { s_ChangedHook = std::move(hook); }

        void NotifyChanged(AssetHandle handle)
        {
            if (s_ChangedHook) s_ChangedHook(handle);
        }

        void OnRuntimeStart() { NotifyChanged(AssetHandle::None()); }
    }
}
