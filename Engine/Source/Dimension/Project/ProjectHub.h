#pragma once
#include "Dimension/Core/Base.h"
#include <string>
#include <vector>

namespace Dimension
{
    // The Project Hub (File > Project Hub..., like Unity Hub): the projects opened before, kept for the user in
    // %APPDATA%/Dimension/RecentProjects.json, and new projects made with a first scene from a scene template.
    namespace ProjectHub
    {
        struct RecentProject
        {
            fs::path Path;
            std::string Name;       // from its project file (else the folder's name)
            int64_t LastOpened = 0; // seconds since 1970
            bool Exists = true;     // its project file is still there
        };
        // Newest first.
        std::vector<RecentProject> Recent();
        // Puts a project first (opened now).
        void Remember(const fs::path& project);
        void Forget(const fs::path& project);
        // Where the list is kept (tests point it elsewhere).
        void SetRecentFile(const fs::path& file);

        bool IsProject(const fs::path& folder);
        // Makes a project in `folder` (created if missing) named `name`, whose first scene (Assets/Scenes/Main.scene,
        // from the scene template `sceneTemplate`) is its start scene; it is left open. False when `folder` already
        // holds a project.
        bool Create(const fs::path& folder, const std::string& name, const std::string& sceneTemplate);
    }
}
