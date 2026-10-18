#pragma once
#include "Dimension/Core/Base.h"

// Game modules export this function; it registers scripts, systems and components.
#define DIM_GAME_MODULE_ENTRY extern "C" __declspec(dllexport) void DimensionGameRegister()

namespace Dimension
{
    // Loads a game DLL (scripts/systems/components) and supports hot reload.
    //
    // The DLL is copied to a uniquely named file before loading so the original stays
    // writable for the compiler/linker. Reloading requires that no objects created by the
    // module are alive: no playing scene, no script instances, no pools of game-defined
    // components (callers serialize and recreate scenes around Reload()).
    class GameModule
    {
    public:
        static bool Load(const fs::path& dllPath);
        static void Unload();
        static bool Reload();
        static bool IsLoaded();
        static const fs::path& GetPath();

        // True when the DLL on disk is newer than the loaded copy and is no longer being written.
        static bool HasChangedOnDisk();
    };
}
