#include "Dimension/Scene/SceneManager.h"
#include "Dimension/Scene/Scene.h"
#include "Dimension/Scene/SceneSerializer.h"
#include "Dimension/Scene/Prefab.h"
#include "Dimension/Asset/AssetManager.h"
#include "Dimension/Project/Project.h"
#include "Dimension/Core/Log.h"
#include <mutex>
#include <vector>

namespace Dimension
{
    namespace
    {
        struct Request
        {
            std::string Name;
            int Index = -1;
            bool Unload = false;
            LoadSceneMode Mode = LoadSceneMode::Single;
        };
        std::mutex s_Mutex;
        std::vector<Request> s_Queue;

        void Enqueue(Request r)
        {
            std::lock_guard lock(s_Mutex);
            if (!r.Unload && r.Mode == LoadSceneMode::Single) s_Queue.clear(); // everything before it would be destroyed anyway
            s_Queue.push_back(std::move(r));
        }

        std::vector<Request> TakeAll()
        {
            std::lock_guard lock(s_Mutex);
            std::vector<Request> all;
            all.swap(s_Queue);
            return all;
        }

        AssetHandle ResolveRequest(const Request& r)
        {
            AssetHandle handle = AssetHandle::None();
            if (r.Index >= 0)
            {
                auto scenes = SceneManager::GetBuildScenes();
                if (r.Index < (int)scenes.size()) handle = scenes[r.Index];
                else Log::Error("LoadScene: build index {} is out of range ({} scenes in the build)", r.Index, scenes.size());
            }
            else
            {
                handle = SceneManager::Resolve(r.Name);
                if (!handle.IsValid()) Log::Error("LoadScene: no scene named '{}' (add it in File > Build Settings)", r.Name);
            }
            return handle;
        }

        // The scene file with its prefab instances brought up to date.
        Ref<Scene> LoadSceneAsset(AssetHandle handle)
        {
            auto scene = SceneSerializer::LoadFromFile(AssetManager::GetAbsolutePath(handle));
            if (!scene)
            {
                Log::Error("LoadScene: failed to load '{}'", AssetManager::GetDisplayName(handle));
                return nullptr;
            }
            Prefabs::SyncAll(*scene);
            return scene;
        }
    }

    void SceneManager::LoadScene(const std::string& name, LoadSceneMode mode) { Enqueue({ name, -1, false, mode }); }
    void SceneManager::LoadScene(int buildIndex, LoadSceneMode mode) { Enqueue({ std::string(), buildIndex, false, mode }); }
    void SceneManager::UnloadScene(const std::string& name) { Enqueue({ name, -1, true, LoadSceneMode::Additive }); }

    bool SceneManager::HasPendingLoad()
    {
        std::lock_guard lock(s_Mutex);
        return !s_Queue.empty();
    }

    void SceneManager::CancelPendingLoad()
    {
        std::lock_guard lock(s_Mutex);
        s_Queue.clear();
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

    bool SceneManager::ApplyPending(Scene& running)
    {
        bool changed = false;
        for (const Request& r : TakeAll())
        {
            if (r.Unload)
            {
                if (running.UnloadScene(r.Name))
                {
                    Log::Info("Unloaded scene '{}'", r.Name);
                    changed = true;
                }
                else
                    Log::Warn("UnloadScene: '{}' is not loaded", r.Name);
                continue;
            }
            AssetHandle handle = ResolveRequest(r);
            if (!handle.IsValid()) continue;
            auto loaded = LoadSceneAsset(handle);
            if (!loaded) continue;
            bool additive = r.Mode == LoadSceneMode::Additive;
            running.LoadSceneData(SceneSerializer::Serialize(*loaded), additive);
            size_t kept = 0;
            for (auto& l : running.GetLoadedScenes())
                if (l.Name == Scene::kPersistentSceneName) kept = l.Roots.size();
            Log::Info("Loaded scene '{}' ({}{})", loaded->GetName(), additive ? "additive" : "single",
                      !additive && kept ? ", " + std::to_string(kept) + " persistent object(s) kept" : std::string());
            changed = true;
        }
        return changed;
    }

    Ref<Scene> SceneManager::LoadPending()
    {
        std::vector<Request> all = TakeAll();
        for (const Request& r : all)
        {
            if (r.Unload) continue;
            AssetHandle handle = ResolveRequest(r);
            if (!handle.IsValid()) return nullptr;
            auto scene = LoadSceneAsset(handle);
            if (scene) Log::Info("Loaded scene '{}'", scene->GetName());
            return scene;
        }
        return nullptr;
    }
}
