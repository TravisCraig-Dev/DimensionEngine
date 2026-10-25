#pragma once
#include "Dimension/Core/Base.h"
#include "Dimension/Core/Guid.h"
#include <atomic>
#include <memory>

namespace Dimension
{
    class Scene;

    enum class LoadSceneMode { Single, Additive };

    // A scene loading in the background (like Unity's AsyncOperation). A worker thread reads and parses the scene and
    // decodes the textures and models it uses; the scene then activates at the end of a frame on the main thread.
    struct SceneLoadOperation
    {
        std::string SceneName;
        std::atomic<float> Progress{ 0.0f }; // 0 to 0.9 while loading in the background, 1 once activated
        std::atomic<bool> IsDone{ false };   // activated, or failed
        std::atomic<bool> Failed{ false };
        // False holds the loaded scene at 0.9 until set to true (e.g. "press any key to continue").
        std::atomic<bool> AllowSceneActivation{ true };
        bool IsReady() const { return Progress.load() >= 0.9f; }

        struct State;
        std::shared_ptr<State> Internal;
    };

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
        // Loads in the background; returns null if the scene cannot be found. Activation follows the same rules as
        // LoadScene (single loads replace everything not marked DontDestroyOnLoad).
        static std::shared_ptr<SceneLoadOperation> LoadSceneAsync(const std::string& name, LoadSceneMode mode = LoadSceneMode::Single);
        static std::shared_ptr<SceneLoadOperation> LoadSceneAsync(int buildIndex, LoadSceneMode mode = LoadSceneMode::Single);
        // Blocks until every background load has finished its worker (tests, shutdown).
        static void WaitForAsyncLoads();
        static bool HasPendingLoad(); // queued loads, or a background load ready to activate
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
