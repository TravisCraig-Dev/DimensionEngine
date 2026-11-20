#include "Dimension/Scripting/GameModule.h"
#include "Dimension/Scripting/ScriptRegistry.h"
#include "Dimension/Scene/ComponentRegistry.h"
#include "Dimension/Scene/System.h"
#include "Dimension/Core/Log.h"
#include "Dimension/Platform/Platform.h"
#include <chrono>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
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
        // Where copies go when the module's own folder can't be written (an installed editor: Program Files, /opt).
        fs::path UserHotDirectory() { return Platform::UserCacheDirectory() / "Dimension" / "hotreload"; }

#ifdef _WIN32
        void* OpenLibrary(const fs::path& path, std::string& error)
        {
            HMODULE module = LoadLibraryW(path.c_str());
            if (!module) error = "error " + std::to_string(GetLastError());
            return module;
        }
        void* FindSymbol(void* module, const char* name) { return reinterpret_cast<void*>(GetProcAddress((HMODULE)module, name)); }
        void CloseLibrary(void* module) { FreeLibrary((HMODULE)module); }
#else
        // RTLD_LOCAL: each reload's copy keeps its own symbols (the engine's come from the already loaded library).
        void* OpenLibrary(const fs::path& path, std::string& error)
        {
            void* module = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
            if (!module) error = dlerror();
            return module;
        }
        void* FindSymbol(void* module, const char* name) { return dlsym(module, name); }
        void CloseLibrary(void* module) { dlclose(module); }
#endif
    }

    bool GameModule::Load(const fs::path& dllPath)
    {
        if (s_Handle) Unload();
        std::error_code ec;
        if (!fs::exists(dllPath))
        {
            Log::Error("Game module not found: {}", dllPath.string());
            return false;
        }

        // Copy first: the loaded image locks its file (Windows), and the linker must be able to overwrite the original; a
        // fresh name also makes dlopen load the new code rather than return the library it already has (Linux).
        auto writeTime = fs::last_write_time(dllPath, ec);
        bool first = s_Generation == 0;
        std::string name = dllPath.stem().string() + "_" + std::to_string(++s_Generation) + dllPath.extension().string();
        fs::path copy;
        for (const fs::path& hotDir : { HotDirectory(dllPath), UserHotDirectory() })
        {
            ec.clear();
            fs::create_directories(hotDir, ec);
            if (first)
                for (auto& old : fs::directory_iterator(hotDir, ec)) fs::remove(old.path(), ec); // leftovers from earlier runs
            ec.clear();
            if (fs::copy_file(dllPath, hotDir / name, fs::copy_options::overwrite_existing, ec))
            {
                copy = hotDir / name;
                break;
            }
        }
        if (copy.empty())
        {
            Log::Error("Could not copy game module ({})", ec.message());
            return false;
        }

        std::string error;
        void* module = OpenLibrary(copy, error);
        if (!module)
        {
            Log::Error("Could not load the game module '{}' ({})", copy.string(), error);
            return false;
        }
        auto entry = reinterpret_cast<void (*)()>(FindSymbol(module, "DimensionGameRegister"));
        if (!entry)
        {
            Log::Error("'{}' does not export DimensionGameRegister (use DIM_GAME_MODULE_ENTRY)", dllPath.filename().string());
            CloseLibrary(module);
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
    }

    void GameModule::Unload()
    {
        if (!s_Handle) return;
        // Everything registered by the module points into its code; drop it before unmapping.
        ScriptRegistry::ClearModule("native");
        SystemRegistry::Clear();
        ComponentRegistry::RemoveGameComponents();
        CloseLibrary(s_Handle);
        s_Handle = nullptr;
        std::error_code ec;
        fs::remove(s_LoadedCopy, ec);
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
