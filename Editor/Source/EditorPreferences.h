#pragma once
#include <Dimension/Core/Base.h>
#include <nlohmann/json.hpp>

namespace Editor
{
    // Per-user editor settings (Edit > Preferences), stored in %APPDATA%/Dimension/EditorPreferences.json.
    struct EditorPreferences
    {
        float UIScale = 1.0f;          // font/UI scale
        float CameraFlySpeed = 6.0f;   // scene camera speed when the editor starts
        float CameraFieldOfView = 60.0f;
        bool ShowGrid = true;
        // The Scene view grid: plane (0 XZ, 1 XY, 2 YZ), cell size (0: adapts to the view), opacity, and whether it
        // moves to the selected object's height.
        int GridPlane = 0;
        float GridCellSize = 0.0f;
        float GridOpacity = 1.0f;
        bool GridFollowSelection = false;
        // Inspector headers folded closed: component names, and "Script:<class>" for scripts (open unless listed).
        std::vector<std::string> FoldedComponents;
        bool ClearConsoleOnPlay = true;
        float AutosaveMinutes = 5.0f;  // 0 = off; backups go to <project>/.dimension/Autosave
        bool SaveSceneBeforePlay = false;
        // Enter Play Mode option (Unity's Reload Domain): the C# scripts load again as play starts, so static fields
        // start from their initial values each time. Off: play starts faster and statics keep what the last play left.
        bool ReloadScriptsOnPlay = true;
        int Theme = 0;                 // Dimension::ImGuiLayer::Theme: Dark, Light, Midnight
        uint32_t Accent = 0x3A79BB;    // 0xRRGGBB
        // Scene view snapping (the toolbar's Snap; Ctrl inverts it while held): moves land on the world grid, rotation
        // and scale go in steps.
        bool Snap = false;
        bool ProjectListView = false; // the Project window shows a list with columns instead of the grid
        float SnapMove = 0.5f, SnapRotate = 15.0f, SnapScale = 0.1f;
        // Shortcuts changed in Window > Shortcuts (id -> key chord).
        nlohmann::json Shortcuts = nlohmann::json::object();

        static EditorPreferences& Get();
        static Dimension::fs::path FilePath();
        void Load();
        void Save() const;
    };
}
