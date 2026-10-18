#pragma once
#include "Dimension/Core/Base.h"

namespace Dimension
{
    // Hosts the .NET runtime (hostfxr) and exposes C# Behaviour classes as engine scripts.
    //
    // Managed code lives in Dimension.Core.dll (engine API) and a user scripts assembly that is
    // loaded into a collectible AssemblyLoadContext, so it can be rebuilt and reloaded while the
    // editor runs. C# classes register into ScriptRegistry under their full type name (module
    // "csharp"), so the Inspector, serialization, prefabs and play mode treat them like C++ scripts.
    class CSharpRuntime
    {
    public:
        // `managedDir` contains Dimension.Core.dll and its runtimeconfig.json.
        static bool Init(const fs::path& managedDir);
        static bool IsAvailable();

        // Loads (or reloads) a user scripts assembly. No script instances may be alive.
        static bool LoadScripts(const fs::path& assembly);
        static void UnloadScripts();
        static const fs::path& GetScriptsPath();
        // True when the assembly on disk is newer than the loaded one.
        static bool ScriptsChangedOnDisk();
        static size_t GetScriptCount();
    };
}
