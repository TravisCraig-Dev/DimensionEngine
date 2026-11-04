#pragma once
#include "Dimension/Core/Base.h"
#include "Dimension/Core/Guid.h"
#include "Dimension/Reflection/TypeInfo.h"
#include <nlohmann/json.hpp>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Dimension
{
    // Data assets (like Unity's ScriptableObject): a `.asset` file holding the fields of a script class made for data
    // (C#'s DataAsset), {"class": "Game.EnemyStats", "fields": {...}} with the fields as script fields save them.
    // Scripts reference them from fields of that class; while playing, every reference to one asset is the same
    // object.
    namespace DataAssets
    {
        // A data class the scripts define (registered when they load).
        struct ClassInfo
        {
            std::string Name;   // "Game.EnemyStats"
            std::string Module; // "csharp"
            std::vector<std::string> Bases; // the classes it derives from (fields of those types take it)
            std::vector<FieldInfo> Fields;  // Access relative to Create()'s storage
            std::function<std::shared_ptr<void>()> Create; // storage holding the class's defaults
        };
        ClassInfo& AddClass(const std::string& name, const std::string& module);
        void ClearModule(const std::string& module);
        const std::deque<ClassInfo>& Classes();
        const ClassInfo* FindClass(const std::string& name);
        // Whether an asset of class `name` fits a field of class `base` (itself, one it derives from, or any for
        // "Dimension.DataAsset").
        bool IsA(const std::string& name, const std::string& base);

        // The asset's class ("" when not a data asset or unreadable) and its file ({class, fields}, or null).
        std::string ClassOf(AssetHandle handle);
        nlohmann::json Read(AssetHandle handle);
        // Writes a data asset's file (a new one is imported) and tells the scripts it changed; the handle (none when
        // writing failed).
        AssetHandle Write(const fs::path& absolutePath, const std::string& className, const nlohmann::json& fields);
        // A new asset of a class with its defaults.
        AssetHandle Create(const fs::path& absolutePath, const std::string& className);

        // The scripts' side: told when an asset's file was written (live objects take the new values) and when play
        // starts (handle none: every object is made again from its file).
        void SetChangedHook(std::function<void(AssetHandle)> hook);
        void NotifyChanged(AssetHandle handle);
        void OnRuntimeStart();
    }
}
