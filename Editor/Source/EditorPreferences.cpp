#include "EditorPreferences.h"
#include <Dimension/Core/Log.h>
#include <nlohmann/json.hpp>
#include <cstdlib>
#include <fstream>

namespace Editor
{
    using namespace Dimension;

    EditorPreferences& EditorPreferences::Get()
    {
        static EditorPreferences prefs;
        return prefs;
    }

    fs::path EditorPreferences::FilePath()
    {
        const char* appData = std::getenv("APPDATA");
        fs::path base = appData ? fs::path(appData) : fs::temp_directory_path();
        return base / "Dimension" / "EditorPreferences.json";
    }

    void EditorPreferences::Load()
    {
        std::ifstream in(FilePath());
        if (!in) return; // defaults
        try
        {
            auto j = nlohmann::json::parse(in);
            UIScale = j.value("uiScale", UIScale);
            CameraFlySpeed = j.value("cameraFlySpeed", CameraFlySpeed);
            CameraFieldOfView = j.value("cameraFieldOfView", CameraFieldOfView);
            ShowGrid = j.value("showGrid", ShowGrid);
            ClearConsoleOnPlay = j.value("clearConsoleOnPlay", ClearConsoleOnPlay);
            AutosaveMinutes = j.value("autosaveMinutes", AutosaveMinutes);
            SaveSceneBeforePlay = j.value("saveSceneBeforePlay", SaveSceneBeforePlay);
        }
        catch (const std::exception& e)
        {
            Log::Warn("Ignoring invalid editor preferences: {}", e.what());
        }
    }

    void EditorPreferences::Save() const
    {
        std::error_code ec;
        fs::create_directories(FilePath().parent_path(), ec);
        nlohmann::json j{ { "uiScale", UIScale },
                          { "cameraFlySpeed", CameraFlySpeed },
                          { "cameraFieldOfView", CameraFieldOfView },
                          { "showGrid", ShowGrid },
                          { "clearConsoleOnPlay", ClearConsoleOnPlay },
                          { "autosaveMinutes", AutosaveMinutes },
                          { "saveSceneBeforePlay", SaveSceneBeforePlay } };
        std::ofstream(FilePath()) << j.dump(2);
    }
}
