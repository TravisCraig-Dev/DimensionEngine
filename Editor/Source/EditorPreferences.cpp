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
            GridPlane = std::clamp(j.value("gridPlane", GridPlane), 0, 2);
            GridCellSize = std::max(0.0f, j.value("gridCellSize", GridCellSize));
            GridOpacity = std::clamp(j.value("gridOpacity", GridOpacity), 0.0f, 1.0f);
            GridFollowSelection = j.value("gridFollowSelection", GridFollowSelection);
            ClearConsoleOnPlay = j.value("clearConsoleOnPlay", ClearConsoleOnPlay);
            AutosaveMinutes = j.value("autosaveMinutes", AutosaveMinutes);
            SaveSceneBeforePlay = j.value("saveSceneBeforePlay", SaveSceneBeforePlay);
            Theme = j.value("theme", Theme);
            Accent = j.value("accent", Accent);
            Snap = j.value("snap", Snap);
            ProjectListView = j.value("projectListView", ProjectListView);
            SnapMove = j.value("snapMove", SnapMove);
            SnapRotate = j.value("snapRotate", SnapRotate);
            SnapScale = j.value("snapScale", SnapScale);
            Shortcuts = j.value("shortcuts", nlohmann::json::object());
            FoldedComponents = j.value("foldedComponents", std::vector<std::string>{});
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
                          { "gridPlane", GridPlane },
                          { "gridCellSize", GridCellSize },
                          { "gridOpacity", GridOpacity },
                          { "gridFollowSelection", GridFollowSelection },
                          { "clearConsoleOnPlay", ClearConsoleOnPlay },
                          { "autosaveMinutes", AutosaveMinutes },
                          { "saveSceneBeforePlay", SaveSceneBeforePlay },
                          { "theme", Theme },
                          { "accent", Accent },
                          { "snap", Snap },
                          { "projectListView", ProjectListView },
                          { "snapMove", SnapMove },
                          { "snapRotate", SnapRotate },
                          { "snapScale", SnapScale },
                          { "shortcuts", Shortcuts },
                          { "foldedComponents", FoldedComponents } };
        std::ofstream(FilePath()) << j.dump(2);
    }
}
