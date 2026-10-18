#pragma once
#include "Dimension/Core/Base.h"
#include "Dimension/Core/Guid.h"

namespace Dimension
{
    // A game project: a folder containing Project.dproj and an Assets/ directory.
    struct ProjectConfig
    {
        std::string Name = "New Project";
        AssetHandle StartScene = AssetHandle::None();
        uint32_t WindowWidth = 1280, WindowHeight = 720;
        std::string GameModule = "SandboxGame"; // DLL (without extension) containing scripts and systems
    };

    class Project
    {
    public:
        static constexpr const char* kFileName = "Project.dproj";

        // Loads (or creates) the project in `directory` and initializes the asset database.
        static bool Open(const fs::path& directory);
        static bool Save();
        static bool IsOpen();

        static ProjectConfig& GetConfig();
        static const fs::path& GetDirectory();
        static fs::path GetAssetsDirectory();
    };
}
