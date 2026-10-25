#pragma once
#include <Dimension/Core/Base.h>

namespace Editor
{
    // Per-user editor settings (Edit > Preferences), stored in %APPDATA%/Dimension/EditorPreferences.json.
    struct EditorPreferences
    {
        float UIScale = 1.0f;          // font/UI scale
        float CameraFlySpeed = 6.0f;   // scene camera speed when the editor starts
        float CameraFieldOfView = 60.0f;
        bool ShowGrid = true;
        bool ClearConsoleOnPlay = true;
        float AutosaveMinutes = 5.0f;  // 0 = off; backups go to <project>/.dimension/Autosave
        bool SaveSceneBeforePlay = false;
        int Theme = 0;                 // Dimension::ImGuiLayer::Theme: Dark, Light, Midnight
        uint32_t Accent = 0x3A79BB;    // 0xRRGGBB

        static EditorPreferences& Get();
        static Dimension::fs::path FilePath();
        void Load();
        void Save() const;
    };
}
