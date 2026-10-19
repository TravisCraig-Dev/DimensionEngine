#pragma once
#include "Dimension/Core/Base.h"
#include "Dimension/Core/Guid.h"

namespace Dimension
{
    class Scene;

    enum class LoadSceneMode { Single, Additive };

    // Runtime scene loading (like Unity's SceneManager). Requests are queued: the host (Player, or the editor in
    // play mode) applies them at the end of the frame with ApplyPending, which loads into the running scene so
    // objects marked DontDestroyOnLoad keep their state.
    class SceneManager
    {
    public:
        // `name` is a scene in the build list (file name without extension), or an Assets-relative path.
        // A single-mode load supersedes requests queued before it.
        static void LoadScene(const std::string& name, LoadSceneMode mode = LoadSceneMode::Single);
        static void LoadScene(int buildIndex, LoadSceneMode mode = LoadSceneMode::Single);
        // Removes the objects of a loaded scene (by name) from the running scene.
        static void UnloadScene(const std::string& name);
        static bool HasPendingLoad();
        static void CancelPendingLoad();

        // Applies every queued request to the running scene. Returns true if something was loaded or unloaded.
        static bool ApplyPending(Scene& running);

        // Takes the first queued load and returns that scene on its own (prefab instances synced, not started).
        // Null when nothing is pending or the scene cannot be found. Used where scenes are swapped wholesale.
        static Ref<Scene> LoadPending();

        // Scenes in build order: the project's build list, or just its start scene.
        static std::vector<AssetHandle> GetBuildScenes();
        static int GetBuildIndex(const std::string& sceneName); // -1 if not in the build list
        static AssetHandle Resolve(const std::string& nameOrPath);
    };
}
