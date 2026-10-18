#include "Dimension/Scene/SceneManager.h"
#include "Dimension/Scene/Scene.h"
#include "Dimension/Scene/SceneSerializer.h"
#include "Dimension/Scene/Prefab.h"
#include "Dimension/Asset/AssetManager.h"
#include "Dimension/Project/Project.h"
#include "Dimension/Core/Log.h"
#include <mutex>
#include <optional>

namespace Dimension
{
    namespace
    {
        struct Request
        {
            std::string Name;
            int Index = -1;
        };
        std::mutex s_Mutex;
        std::optional<Request> s_Pending;
    }

    void SceneManager::LoadScene(const std::string& name)
    {
        std::lock_guard lock(s_Mutex);
        s_Pending = Request{ name, -1 };
    }

    void SceneManager::LoadScene(int buildIndex)
    {
        std::lock_guard lock(s_Mutex);
        s_Pending = Request{ std::string(), buildIndex };
    }

    bool SceneManager::HasPendingLoad()
    {
        std::lock_guard lock(s_Mutex);
        return s_Pending.has_value();
    }

    void SceneManager::CancelPendingLoad()
    {
        std::lock_guard lock(s_Mutex);
        s_Pending.reset();
    }

    std::vector<AssetHandle> SceneManager::GetBuildScenes()
    {
        auto& config = Project::GetConfig();
        std::vector<AssetHandle> scenes;
        for (AssetHandle h : config.BuildScenes)
            if (h.IsValid()) scenes.push_back(h);
        if (scenes.empty() && config.StartScene.IsValid()) scenes.push_back(config.StartScene);
        return scenes;
    }

    int SceneManager::GetBuildIndex(const std::string& sceneName)
    {
        auto scenes = GetBuildScenes();
        for (size_t i = 0; i < scenes.size(); ++i)
            if (AssetManager::GetDisplayName(scenes[i]) == sceneName) return (int)i;
        return -1;
    }

    AssetHandle SceneManager::Resolve(const std::string& nameOrPath)
    {
        int index = GetBuildIndex(nameOrPath);
        if (index >= 0) return GetBuildScenes()[index];
        // Not in the build list: accept an Assets-relative path, with or without the extension.
        for (const std::string& candidate : { nameOrPath, nameOrPath + ".scene" })
        {
            AssetHandle h = AssetManager::GetHandle(candidate);
            if (auto* meta = AssetManager::GetMetadata(h); meta && meta->Type == AssetType::Scene) return h;
        }
        return AssetHandle::None();
    }

    Ref<Scene> SceneManager::LoadPending()
    {
        std::optional<Request> request;
        {
            std::lock_guard lock(s_Mutex);
            request.swap(s_Pending);
        }
        if (!request) return nullptr;

        AssetHandle handle = AssetHandle::None();
        if (request->Index >= 0)
        {
            auto scenes = GetBuildScenes();
            if (request->Index < (int)scenes.size()) handle = scenes[request->Index];
            else Log::Error("LoadScene: build index {} is out of range ({} scenes in the build)", request->Index, scenes.size());
        }
        else
        {
            handle = Resolve(request->Name);
            if (!handle.IsValid()) Log::Error("LoadScene: no scene named '{}' (add it in File > Build Settings)", request->Name);
        }
        if (!handle.IsValid()) return nullptr;

        auto scene = SceneSerializer::LoadFromFile(AssetManager::GetAbsolutePath(handle));
        if (!scene)
        {
            Log::Error("LoadScene: failed to load '{}'", AssetManager::GetDisplayName(handle));
            return nullptr;
        }
        Prefabs::SyncAll(*scene);
        Log::Info("Loaded scene '{}'", scene->GetName());
        return scene;
    }
}
