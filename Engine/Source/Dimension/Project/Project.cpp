#include "Dimension/Project/Project.h"
#include "Dimension/Asset/AssetManager.h"
#include "Dimension/Core/Log.h"
#include <nlohmann/json.hpp>
#include <fstream>

namespace Dimension
{
    namespace
    {
        ProjectConfig s_Config;
        fs::path s_Directory;
        bool s_Open = false;
    }

    bool Project::Open(const fs::path& directory)
    {
        std::error_code ec;
        fs::path dir = fs::weakly_canonical(directory, ec);
        fs::create_directories(dir / "Assets", ec);
        s_Directory = dir;
        s_Config = ProjectConfig();
        s_Config.Name = dir.filename().string();

        fs::path file = dir / kFileName;
        if (fs::exists(file))
        {
            try
            {
                auto j = nlohmann::json::parse(std::ifstream(file));
                s_Config.Name = j.value("name", s_Config.Name);
                s_Config.StartScene = Guid(j.value("startScene", uint64_t(0)));
                s_Config.WindowWidth = j.value("windowWidth", s_Config.WindowWidth);
                s_Config.WindowHeight = j.value("windowHeight", s_Config.WindowHeight);
                s_Config.GameModule = j.value("gameModule", s_Config.GameModule);
            }
            catch (const std::exception& e)
            {
                Log::Error("Invalid project file '{}': {}", file.string(), e.what());
                return false;
            }
        }
        s_Open = true;
        AssetManager::Init(dir / "Assets");
        if (!fs::exists(file)) Save();
        Log::Info("Opened project '{}' ({})", s_Config.Name, dir.string());
        return true;
    }

    bool Project::Save()
    {
        if (!s_Open) return false;
        nlohmann::json j{ { "name", s_Config.Name },
                          { "startScene", (uint64_t)s_Config.StartScene },
                          { "windowWidth", s_Config.WindowWidth },
                          { "windowHeight", s_Config.WindowHeight },
                          { "gameModule", s_Config.GameModule },
                          { "engineVersion", "0.1.0" } };
        std::ofstream out(s_Directory / kFileName);
        out << j.dump(2);
        return (bool)out;
    }

    bool Project::IsOpen() { return s_Open; }
    ProjectConfig& Project::GetConfig() { return s_Config; }
    const fs::path& Project::GetDirectory() { return s_Directory; }
    fs::path Project::GetAssetsDirectory() { return s_Directory / "Assets"; }
}
