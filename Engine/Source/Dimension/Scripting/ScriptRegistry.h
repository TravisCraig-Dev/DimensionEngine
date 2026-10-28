#pragma once
#include "Dimension/Scripting/EditModeScripts.h"
#include "Dimension/Reflection/TypeInfo.h"
#include <deque>
#include <memory>

namespace Dimension
{
    class ScriptableEntity;

    struct ScriptInfo
    {
        std::string Name;
        std::function<std::shared_ptr<ScriptableEntity>()> Create;
        std::function<void*(ScriptableEntity*)> ToObject; // base -> most-derived pointer for field access
        std::vector<FieldInfo> Fields;
        std::string Module = "native"; // "native" (C++ game module) or "csharp"
        std::string Menu;                  // Add Component menu path ("Category/Name"); empty = the Scripts section
        std::vector<std::string> Requires; // components (registry names) added with the script in the editor
        bool ExecuteInEditMode = false;    // also runs in the editor outside play mode (see EditModeScripts)
    };

    // Editor options of a C++ script (what C# scripts say with [AddComponentMenu] and [RequireComponent]).
    struct ScriptOptions
    {
        std::string Menu;                  // "Category/Name" in the Add Component menu
        std::vector<std::string> Requires; // component names added with the script, e.g. { "Rigidbody" }
        bool ExecuteInEditMode = false;    // C#'s [ExecuteInEditMode]
    };

    // Registry of native script classes, filled by game code at startup.
    class ScriptRegistry
    {
    public:
        template <class T>
        static FieldListBuilder<T> Register(const std::string& name, ScriptOptions options = {})
        {
            ScriptInfo& info = Scripts().emplace_back();
            info.Name = name;
            info.Menu = std::move(options.Menu);
            info.Requires = std::move(options.Requires);
            info.ExecuteInEditMode = options.ExecuteInEditMode;
            info.Create = [] { return std::make_shared<T>(); };
            info.ToObject = [](ScriptableEntity* base) -> void* { return static_cast<T*>(base); };
            return FieldListBuilder<T>(info.Fields);
        }

        static const ScriptInfo* Find(const std::string& name)
        {
            for (auto& s : Scripts())
                if (s.Name == name) return &s;
            return nullptr;
        }

        static const std::deque<ScriptInfo>& All() { return Scripts(); }
        // Removes every registered script.
        static void Clear() { Scripts().clear(); }
        // Removes the scripts of one module (game module or C# assembly unload).
        static void ClearModule(const std::string& module)
        {
            EditModeScripts::Clear(); // their instances may be of these classes
            std::erase_if(Scripts(), [&](const ScriptInfo& s) { return s.Module == module; });
        }
        // Registration for scripts whose classes are only known at runtime (C#).
        static ScriptInfo& RegisterDynamic(const std::string& name, const std::string& module)
        {
            ScriptInfo& info = Scripts().emplace_back();
            info.Name = name;
            info.Module = module;
            return info;
        }

    private:
        static std::deque<ScriptInfo>& Scripts(); // storage lives in the engine DLL
    };
}
