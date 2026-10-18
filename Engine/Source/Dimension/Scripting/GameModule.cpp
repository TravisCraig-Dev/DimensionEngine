#include "Dimension/Scripting/GameModule.h"
#include "Dimension/Scripting/ScriptRegistry.h"
#include "Dimension/Scene/ComponentRegistry.h"
#include "Dimension/Scene/System.h"
#include "Dimension/Core/Log.h"
#include <chrono>

#ifdef _WIN32
#include <windows.h>
#endif

namespace Dimension
{
    namespace
    {
        void* s_Handle = nullptr;
        fs::path s_Path;
        fs::path s_LoadedCopy;
        fs::file_time_type s_LoadedTime{};
        int s_Generation = 0;

        fs::path HotDirectory(const fs::path& dll) { return dll.parent_path() / ".hotreload"; }
    }

    bool GameModule::Load(const fs::path& dllPath)
    {
#ifdef _WIN32
        if (s_Handle) Unload();
        std::error_code ec;
        if (!fs::exists(dllPath))
        {
            Log::Error("Game module not found: {}", dllPath.string());
            return false;
        }

        // Copy first: the loaded image locks its file, and the linker must be able to overwrite the original.
        fs::path hotDir = HotDirectory(dllPath);
        fs::create_directories(hotDir, ec);
        if (s_Generation == 0)
            for (auto& old : fs::directory_iterator(hotDir, ec)) fs::remove(old.path(), ec); // leftovers from earlier runs
        fs::path copy = hotDir / (dllPath.stem().string() + "_" + std::to_string(++s_Generation) + ".dll");
        auto writeTime = fs::last_write_time(dllPath, ec);
        if (!fs::copy_file(dllPath, copy, fs::copy_options::overwrite_existing, ec))
        {
            Log::Error("Could not copy game module ({})", ec.message());
            return false;
        }

        HMODULE module = LoadLibraryW(copy.c_str());
        if (!module)
        {
            Log::Error("LoadLibrary failed for '{}' (error {})", copy.string(), GetLastError());
            return false;
        }
        auto entry = (void (*)())GetProcAddress(module, "DimensionGameRegister");
        if (!entry)
        {
            Log::Error("'{}' does not export DimensionGameRegister (use DIM_GAME_MODULE_ENTRY)", dllPath.filename().string());
            FreeLibrary(module);
            return false;
        }
        entry();

        s_Handle = module;
        s_Path = dllPath;
        s_LoadedCopy = copy;
        s_LoadedTime = writeTime;
        Log::Info("Loaded game module '{}' ({} scripts, {} systems)", dllPath.filename().string(), ScriptRegistry::All().size(),
                  SystemRegistry::All().size());
        return true;
#else
        Log::Error("Game modules are only supported on Windows in this version");
        return false;
#endif
    }

    void GameModule::Unload()
    {
#ifdef _WIN32
        if (!s_Handle) return;
        // Everything registered by the module points into its code; drop it before unmapping.
        ScriptRegistry::ClearModule("native");
        SystemRegistry::Clear();
        ComponentRegistry::RemoveGameComponents();
        FreeLibrary((HMODULE)s_Handle);
        s_Handle = nullptr;
        std::error_code ec;
        fs::remove(s_LoadedCopy, ec);
#endif
    }

    bool GameModule::Reload()
    {
        fs::path path = s_Path;
        if (path.empty()) return false;
        Unload();
        return Load(path);
    }

    bool GameModule::IsLoaded() { return s_Handle != nullptr; }
    const fs::path& GameModule::GetPath() { return s_Path; }

    bool GameModule::HasChangedOnDisk()
    {
        if (s_Path.empty()) return false;
        std::error_code ec;
        auto time = fs::last_write_time(s_Path, ec);
        if (ec || time <= s_LoadedTime) return false;
        // Wait until the linker has finished writing (file untouched for a moment).
        auto age = fs::file_time_type::clock::now() - time;
        return age > std::chrono::milliseconds(750);
    }
}
