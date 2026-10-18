#pragma once
#include "Dimension/Core/Base.h"
#include "Dimension/Core/Guid.h"

namespace Dimension
{
    class Scene;

    // Runtime scene switching (like Unity's SceneManager.LoadScene). Requests are deferred: the host
    // (Player, or the editor in play mode) applies them at the end of the frame by calling LoadPending.
    class SceneManager
    {
    public:
        // `name` is a scene in the build list (file name without extension), or an Assets-relative path.
        static void LoadScene(const std::string& name);
        static void LoadScene(int buildIndex);
        static bool HasPendingLoad();
        static void CancelPendingLoad();

        // Resolves and loads the requested scene (prefab instances synced, not started). Clears the request.
        // Returns null when nothing is pending or the scene cannot be found.
        static Ref<Scene> LoadPending();

        // Scenes in build order: the project's build list, or just its start scene.
        static std::vector<AssetHandle> GetBuildScenes();
        static int GetBuildIndex(const std::string& sceneName); // -1 if not in the build list
        static AssetHandle Resolve(const std::string& nameOrPath);
    };
}
