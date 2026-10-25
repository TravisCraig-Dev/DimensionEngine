#include "Dimension/Scene/SceneManager.h"
#include "Dimension/Scene/Scene.h"
#include "Dimension/Scene/SceneSerializer.h"
#include "Dimension/Scene/Prefab.h"
#include "Dimension/Asset/AssetManager.h"
#include "Dimension/Project/Project.h"
#include "Dimension/Core/Log.h"
#include "Dimension/Asset/AssetPrefetch.h"
#include "Dimension/Renderer/Mesh.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Dimension
{
    struct SceneLoadOperation::State
    {
        AssetHandle Handle = AssetHandle::None();
        LoadSceneMode Mode = LoadSceneMode::Single;
        fs::path File;
        nlohmann::json Data;
        std::thread Worker;
        std::atomic<bool> WorkerDone{ false };
        // Main-thread snapshot of the asset database for the worker: GUID -> (absolute path, type, import settings,
        // already loaded).
        struct AssetInfo { fs::path Path; AssetType Type; nlohmann::json Settings; bool Cached; };
        std::unordered_map<uint64_t, AssetInfo> Assets;
        ~State() { if (Worker.joinable()) Worker.join(); }
    };

    namespace
    {
        std::mutex s_AsyncMutex;
        std::vector<std::shared_ptr<SceneLoadOperation>> s_Async;

        // Worker: parse the scene, follow references through prefabs/materials/controllers, decode the textures and
        // models that are not loaded yet into AssetPrefetch.
        void RunAsyncLoad(SceneLoadOperation* op)
        {
            auto& st = *op->Internal;
            try
            {
                st.Data = nlohmann::json::parse(std::ifstream(st.File));
            }
            catch (const std::exception& e)
            {
                Log::Error("LoadSceneAsync: failed to read '{}': {}", st.File.string(), e.what());
                op->Failed = true;
                st.WorkerDone = true;
                return;
            }
            op->Progress = 0.1f;
            std::vector<uint64_t> pending;
            std::unordered_set<uint64_t> seen;
            auto scan = [&](auto& self, const nlohmann::json& j) -> void {
                if (j.is_number_unsigned() || j.is_number_integer())
                {
                    uint64_t v = j.is_number_unsigned() ? j.get<uint64_t>() : (uint64_t)std::max<int64_t>(0, j.get<int64_t>());
                    if (v >= 1024 && st.Assets.contains(v) && seen.insert(v).second) pending.push_back(v);
                }
                else if (j.is_array() || j.is_object())
                    for (const auto& item : j) self(self, item);
            };
            scan(scan, st.Data);
            std::vector<uint64_t> toDecode;
            while (!pending.empty())
            {
                uint64_t g = pending.back();
                pending.pop_back();
                const auto& info = st.Assets.at(g);
                scan(scan, info.Settings);
                switch (info.Type)
                {
                case AssetType::Scene:
                case AssetType::Prefab:
                case AssetType::Material:
                case AssetType::AnimatorController:
                    try { scan(scan, nlohmann::json::parse(std::ifstream(info.Path))); }
                    catch (const std::exception&) {}
                    break;
                case AssetType::Texture:
                case AssetType::Mesh:
                    if (!info.Cached && !AssetPrefetch::Contains(info.Path)) toDecode.push_back(g);
                    break;
                default: break;
                }
            }
            for (size_t i = 0; i < toDecode.size(); ++i)
            {
                const auto& info = st.Assets.at(toDecode[i]);
                if (info.Type == AssetType::Texture)
                {
                    if (auto image = DecodedImage::Decode(info.Path)) AssetPrefetch::StoreImage(info.Path, std::move(*image));
                }
                else
                {
                    auto mesh = std::make_shared<MeshData>();
                    if (AssetManager::LoadMeshData(info.Path, *mesh)) AssetPrefetch::StoreMesh(info.Path, std::move(mesh));
                }
                op->Progress = 0.1f + 0.8f * (float)(i + 1) / (float)toDecode.size();
            }
            op->Progress = 0.9f;
            st.WorkerDone = true;
        }

        std::shared_ptr<SceneLoadOperation> StartAsync(AssetHandle handle, LoadSceneMode mode)
        {
            if (!handle.IsValid()) return nullptr;
            auto op = std::make_shared<SceneLoadOperation>();
            op->SceneName = AssetManager::GetDisplayName(handle);
            op->Internal = std::make_shared<SceneLoadOperation::State>();
            auto& st = *op->Internal;
            st.Handle = handle;
            st.Mode = mode;
            st.File = AssetManager::GetAbsolutePath(handle);
            for (int type = (int)AssetType::Texture; type <= (int)AssetType::AnimatorController; ++type)
                for (AssetHandle h : AssetManager::GetAssetsOfType((AssetType)type))
                    if (const AssetMetadata* m = AssetManager::GetMetadata(h))
                        st.Assets[(uint64_t)h] = { AssetManager::GetAbsolutePath(h), m->Type, m->ImportSettings, AssetManager::IsLoaded(h) };
            SceneLoadOperation* raw = op.get();
            st.Worker = std::thread([raw] { RunAsyncLoad(raw); });
            std::lock_guard lock(s_AsyncMutex);
            s_Async.push_back(op);
            return op;
        }

        bool AnyAsyncReady()
        {
            std::lock_guard lock(s_AsyncMutex);
            for (auto& op : s_Async)
                if (op->Internal->WorkerDone && (op->AllowSceneActivation || op->Failed)) return true;
            return false;
        }
    }

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

    std::shared_ptr<SceneLoadOperation> SceneManager::LoadSceneAsync(const std::string& name, LoadSceneMode mode)
    {
        Request r{ name, -1, false, mode };
        return StartAsync(ResolveRequest(r), mode);
    }

    std::shared_ptr<SceneLoadOperation> SceneManager::LoadSceneAsync(int buildIndex, LoadSceneMode mode)
    {
        Request r{ std::string(), buildIndex, false, mode };
        return StartAsync(ResolveRequest(r), mode);
    }

    void SceneManager::WaitForAsyncLoads()
    {
        std::vector<std::shared_ptr<SceneLoadOperation>> ops;
        {
            std::lock_guard lock(s_AsyncMutex);
            ops = s_Async;
        }
        for (auto& op : ops)
            if (op->Internal->Worker.joinable()) op->Internal->Worker.join();
    }

    bool SceneManager::HasPendingLoad()
    {
        {
            std::lock_guard lock(s_Mutex);
            if (!s_Queue.empty()) return true;
        }
        return AnyAsyncReady();
    }
    void SceneManager::CancelPendingLoad()
    {
        {
            std::lock_guard lock(s_Mutex);
            s_Queue.clear();
        }
        std::vector<std::shared_ptr<SceneLoadOperation>> ops;
        {
            std::lock_guard lock(s_AsyncMutex);
            ops.swap(s_Async);
        }
        for (auto& op : ops)
        {
            if (op->Internal->Worker.joinable()) op->Internal->Worker.join();
            op->Failed = true;
            op->IsDone = true;
        }
        AssetPrefetch::Clear();
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
        // Background loads whose worker has finished (and that may activate).
        std::vector<std::shared_ptr<SceneLoadOperation>> ready;
        {
            std::lock_guard lock(s_AsyncMutex);
            for (auto it = s_Async.begin(); it != s_Async.end();)
                if ((*it)->Internal->WorkerDone && ((*it)->AllowSceneActivation || (*it)->Failed))
                {
                    ready.push_back(*it);
                    it = s_Async.erase(it);
                }
                else
                    ++it;
        }
        for (auto& op : ready)
        {
            auto& st = *op->Internal;
            if (st.Worker.joinable()) st.Worker.join();
            if (!op->Failed)
            {
                auto loaded = SceneSerializer::Deserialize(st.Data);
                if (loaded)
                {
                    Prefabs::SyncAll(*loaded);
                    bool additive = st.Mode == LoadSceneMode::Additive;
                    running.LoadSceneData(SceneSerializer::Serialize(*loaded), additive);
                    Log::Info("Loaded scene '{}' in the background ({})", loaded->GetName(), additive ? "additive" : "single");
                    changed = true;
                }
                else
                    op->Failed = true;
            }
            op->Progress = 1.0f;
            op->IsDone = true;
        }
        AssetPrefetch::Trim(30.0f); // decoded data no scene asked for
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
