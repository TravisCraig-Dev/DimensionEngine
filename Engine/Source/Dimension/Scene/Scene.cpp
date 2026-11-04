#include "Dimension/Scene/DebugDraw.h"
#include "Dimension/Scene/Scene.h"
#include "Dimension/Scene/SceneSerializer.h"
#include "Dimension/Scripting/ScriptableEntity.h"
#include "Dimension/Scripting/ScriptRegistry.h"
#include "Dimension/Physics/Physics2D.h"
#include "Dimension/Physics/Physics3D.h"
#include "Dimension/Physics/Water.h"
#include "Dimension/Navigation/NavMesh.h"
#include "Dimension/Core/Profiler.h"
#include "Dimension/Audio/AudioEngine.h"
#include "Dimension/Asset/AssetManager.h"
#include "Dimension/UI/UI.h"
#include "Dimension/Scene/Prefab.h"
#include "Dimension/Particles/ParticleSystem.h"
#include "Dimension/Animation/Animation.h"
#include "Dimension/Animation/AnimatorController.h"
#include "Dimension/Animation/Timeline.h"
#include "Dimension/Renderer/Mesh.h"
#include "Dimension/Core/Log.h"
#include "Dimension/Core/Input.h"
#include <glm/gtx/matrix_decompose.hpp>
#include "Dimension/Asset/DataAsset.h"
#include <algorithm>
#include <map>
#include <fstream>

namespace Dimension
{
    glm::mat4 CameraComponent::GetProjection(float aspect) const
    {
        glm::mat4 proj;
        if (Projection == ProjectionType::Perspective)
            proj = glm::perspective(glm::radians(FieldOfView), aspect, NearClip, FarClip);
        else
        {
            float h = OrthographicSize, w = h * aspect;
            proj = glm::ortho(-w, w, -h, h, NearClip, FarClip);
        }
        return proj;
    }

    Scene::Scene(std::string name) : m_Name(std::move(name)) {}

    namespace
    {
        Scene::EventHook s_EventHook = nullptr;
    }

    void Scene::SetEventHook(EventHook hook) { s_EventHook = hook; }

    Scene::~Scene()
    {
        EditModeScripts::Clear(this, true);
        if (m_Running) OnRuntimeStop();
    }

    Ref<Scene> Scene::Clone() const
    {
        auto copy = MakeRef<Scene>(m_Name);
        copy->m_Registry = m_Registry.Clone();
        copy->m_Roots = m_Roots;
        copy->m_GuidMap = m_GuidMap;
        copy->m_Settings = m_Settings;
        return copy;
    }

    // ---- Entities ---------------------------------------------------------------

    Entity Scene::CreateEntity(const std::string& name, Entity parent, Guid id)
    {
        Entity e = m_Registry.Create();
        m_Registry.Emplace<IDComponent>(e, id);
        m_Registry.Emplace<TagComponent>(e).Name = name;
        m_Registry.Emplace<TransformComponent>(e);
        m_Registry.Emplace<HierarchyComponent>(e);
        m_GuidMap[id] = e;
        if (!parent.IsNull() && IsValid(parent))
        {
            m_Registry.Get<HierarchyComponent>(e).Parent = parent;
            m_Registry.Get<HierarchyComponent>(parent).Children.push_back(e);
        }
        else
            m_Roots.push_back(e);
        return e;
    }

    void Scene::DestroyEntity(Entity e)
    {
        if (!IsValid(e)) return;
        auto children = m_Registry.Get<HierarchyComponent>(e).Children; // copy: recursion mutates it
        for (Entity c : children) DestroyEntity(c);

        if (m_Running)
        {
            if (auto* sc = m_Registry.TryGet<ScriptComponent>(e))
                for (auto& inst : sc->LiveInstances(false))
                {
                    if (inst->m_EnableNotified) { inst->m_EnableNotified = false; inst->OnDisable(); }
                    inst->OnDestroy();
                }
            if (m_Physics2D) m_Physics2D->OnEntityDestroyed(e);
            if (m_Physics3D) m_Physics3D->OnEntityDestroyed(e);
            if (m_Audio) m_Audio->OnEntityDestroyed(e);
        }
        DetachFromParent(e);
        std::erase(m_Roots, e);
        m_GuidMap.erase(m_Registry.Get<IDComponent>(e).ID);
        m_Registry.Destroy(e);
    }

    void Scene::DestroyEntityDeferred(Entity e)
    {
        if (IsValid(e) && std::find(m_PendingDestroy.begin(), m_PendingDestroy.end(), e) == m_PendingDestroy.end())
            m_PendingDestroy.push_back(e);
    }

    void Scene::FlushDeferredDestroys()
    {
        auto pending = std::move(m_PendingDestroy);
        m_PendingDestroy.clear();
        for (Entity e : pending) DestroyEntity(e);
    }

    Entity Scene::DuplicateEntity(Entity e)
    {
        if (!IsValid(e)) return Entity::Null();
        auto tree = SceneSerializer::SerializeEntityTree(*this, e);
        Entity parent = GetParent(e);
        Entity copy = SceneSerializer::DeserializeEntityTree(*this, tree, parent, true);
        // Place right after the original.
        auto& siblings = SiblingList(e);
        auto it = std::find(siblings.begin(), siblings.end(), e);
        SetSiblingIndex(copy, (size_t)std::distance(siblings.begin(), it) + 1);
        if (m_Running)
        {
            if (m_Physics2D) m_Physics2D->CreateBodiesFor(copy);
            if (m_Physics3D) m_Physics3D->CreateBodiesFor(copy);
            InstantiateScripts();
        }
        return copy;
    }

    Entity Scene::FindEntityByGuid(Guid id) const
    {
        auto it = m_GuidMap.find(id);
        return it != m_GuidMap.end() ? it->second : Entity::Null();
    }

    Entity Scene::FindEntityByName(const std::string& name) const
    {
        Entity found = Entity::Null();
        m_Registry.EachEntity([&](Entity e) {
            if (found.IsNull() && m_Registry.Get<TagComponent>(e).Name == name) found = e;
        });
        return found;
    }

    Guid Scene::GetGuid(Entity e) const
    {
        auto* id = m_Registry.TryGet<IDComponent>(e);
        return id ? id->ID : Guid::None();
    }

    // ---- Hierarchy ----------------------------------------------------------------

    const std::vector<Entity>& Scene::GetChildren(Entity e) const
    {
        static const std::vector<Entity> kEmpty;
        auto* h = m_Registry.TryGet<HierarchyComponent>(e);
        return h ? h->Children : kEmpty;
    }

    Entity Scene::GetParent(Entity e) const
    {
        auto* h = m_Registry.TryGet<HierarchyComponent>(e);
        return h ? h->Parent : Entity::Null();
    }

    bool Scene::IsDescendantOf(Entity e, Entity ancestor) const
    {
        for (Entity p = GetParent(e); !p.IsNull(); p = GetParent(p))
            if (p == ancestor) return true;
        return false;
    }

    Entity Scene::FindEntityByTag(const std::string& tag) const
    {
        Entity found = Entity::Null();
        m_Registry.EachEntity([&](Entity e) {
            if (found.IsNull() && m_Registry.Get<TagComponent>(e).Tag == tag) found = e;
        });
        return found;
    }

    std::vector<Entity> Scene::FindEntitiesByTag(const std::string& tag) const
    {
        std::vector<Entity> found;
        m_Registry.EachEntity([&](Entity e) {
            if (m_Registry.Get<TagComponent>(e).Tag == tag) found.push_back(e);
        });
        return found;
    }

    // ---- Active state ------------------------------------------------------------------

    bool Scene::IsActiveInHierarchy(Entity e) const
    {
        for (Entity p = e; !p.IsNull(); p = GetParent(p))
            if (!m_Registry.Get<TagComponent>(p).Active) return false;
        return IsValid(e);
    }

    void Scene::SetActive(Entity e, bool active)
    {
        if (!IsValid(e)) return;
        m_Registry.Get<TagComponent>(e).Active = active;
        // The subtree's ActiveInHierarchy (and world matrices) now, so code asking right after sees the new state.
        Entity parent = GetParent(e);
        glm::mat4 parentWorld = parent.IsNull() ? glm::mat4(1.0f) : m_Registry.Get<TransformComponent>(parent).World;
        UpdateTransformRecursive(e, parentWorld, parent.IsNull() || IsActiveInHierarchy(parent));
        if (!m_Running) return;
        std::vector<Entity> subtree{ e };
        for (size_t i = 0; i < subtree.size(); ++i)
            for (Entity c : GetChildren(subtree[i])) subtree.push_back(c);
        for (Entity x : subtree)
            if (IsValid(x)) SyncScriptsEnabled(x); // a callback may destroy objects
    }

    void Scene::SetScriptEnabled(Entity e, size_t scriptIndex, bool enabled)
    {
        auto* sc = IsValid(e) ? m_Registry.TryGet<ScriptComponent>(e) : nullptr;
        if (!sc || scriptIndex >= sc->Scripts.size()) return;
        sc->Scripts[scriptIndex].Enabled = enabled;
        SyncScriptsEnabled(e);
    }

    void Scene::SyncScriptsEnabled(Entity e)
    {
        auto* sc = m_Registry.TryGet<ScriptComponent>(e);
        if (!sc) return;
        bool active = m_Running && IsActiveInHierarchy(e);
        // Copied first: the callbacks may add scripts or objects.
        std::vector<std::pair<std::shared_ptr<ScriptableEntity>, bool>> changes;
        for (auto& s : sc->Scripts)
            if (s.Instance && s.Instance->m_EnableNotified != (active && s.Enabled)) changes.emplace_back(s.Instance, active && s.Enabled);
        for (auto& [inst, on] : changes)
        {
            if (inst->m_EnableNotified == on) continue;
            inst->m_EnableNotified = on;
            if (on) inst->OnEnable();
            else inst->OnDisable();
        }
    }

    std::vector<Entity>& Scene::SiblingList(Entity e)
    {
        Entity parent = GetParent(e);
        return parent.IsNull() ? m_Roots : m_Registry.Get<HierarchyComponent>(parent).Children;
    }

    void Scene::DetachFromParent(Entity e)
    {
        auto& h = m_Registry.Get<HierarchyComponent>(e);
        if (!h.Parent.IsNull() && IsValid(h.Parent))
            std::erase(m_Registry.Get<HierarchyComponent>(h.Parent).Children, e);
        h.Parent = Entity::Null();
    }

    void Scene::SetParent(Entity child, Entity parent, bool keepWorld)
    {
        if (!IsValid(child) || child == parent) return;
        if (!parent.IsNull() && (!IsValid(parent) || IsDescendantOf(parent, child))) return;
        if (GetParent(child) == parent) return;

        UpdateTransforms();
        glm::mat4 childWorld = m_Registry.Get<TransformComponent>(child).World;

        if (GetParent(child).IsNull()) std::erase(m_Roots, child);
        else DetachFromParent(child);

        if (parent.IsNull())
            m_Roots.push_back(child);
        else
        {
            m_Registry.Get<HierarchyComponent>(child).Parent = parent;
            m_Registry.Get<HierarchyComponent>(parent).Children.push_back(child);
        }

        if (keepWorld)
        {
            glm::mat4 parentWorld = parent.IsNull() ? glm::mat4(1.0f) : m_Registry.Get<TransformComponent>(parent).World;
            glm::mat4 local = glm::inverse(parentWorld) * childWorld;
            glm::vec3 scale, translation, skew;
            glm::vec4 perspective;
            glm::quat rotation;
            if (glm::decompose(local, scale, rotation, translation, skew, perspective))
            {
                auto& t = m_Registry.Get<TransformComponent>(child);
                t.Position = translation;
                t.Rotation = glm::eulerAngles(rotation);
                t.Scale = scale;
            }
        }
        UpdateTransforms();
        // Moved under an inactive parent (or out from under one): its scripts are told at once.
        if (m_Running) SetActive(child, m_Registry.Get<TagComponent>(child).Active);
    }

    void Scene::SetSiblingIndex(Entity e, size_t index)
    {
        if (!IsValid(e)) return;
        auto& list = SiblingList(e);
        std::erase(list, e);
        index = std::min(index, list.size());
        list.insert(list.begin() + (ptrdiff_t)index, e);
    }

    // ---- Prefabs --------------------------------------------------------------------

    Entity Scene::InstantiatePrefab(AssetHandle prefab, const glm::vec3& position, Entity parent)
    {
        Entity root = Prefabs::Instantiate(*this, prefab, parent);
        if (root.IsNull()) return root;
        m_Registry.Get<TransformComponent>(root).Position = position;
        UpdateTransforms();
        if (m_Running)
        {
            if (m_Physics2D) m_Physics2D->CreateBodiesFor(root);
            if (m_Physics3D) m_Physics3D->CreateBodiesFor(root);
            if (m_Audio) m_Audio->CreateSourcesFor(root);
            InstantiateScripts();
        }
        return root;
    }

    // ---- Transforms -------------------------------------------------------------------

    void Scene::UpdateTransforms()
    {
        for (Entity root : m_Roots) UpdateTransformRecursive(root, glm::mat4(1.0f), true);
    }

    void Scene::UpdateTransformRecursive(Entity e, const glm::mat4& parentWorld, bool parentActive)
    {
        auto& t = m_Registry.Get<TransformComponent>(e);
        t.World = parentWorld * t.GetLocalMatrix();
        t.ActiveInHierarchy = parentActive && m_Registry.Get<TagComponent>(e).Active;
        glm::mat4 world = t.World;
        bool active = t.ActiveInHierarchy;
        for (Entity c : m_Registry.Get<HierarchyComponent>(e).Children) UpdateTransformRecursive(c, world, active);
    }

    Entity Scene::GetPrimaryCamera()
    {
        Entity fallback = Entity::Null();
        for (auto [e, cam, t] : m_Registry.ViewOf<CameraComponent, TransformComponent>())
        {
            if (!t.ActiveInHierarchy) continue;
            if (cam.Primary) return e;
            if (fallback.IsNull()) fallback = e;
        }
        return fallback;
    }

    // ---- Runtime -----------------------------------------------------------------------

    bool Scene::ReloadScriptModule(const std::string& module, const std::function<bool()>& reload)
    {
        EntityRefResolver resolver;
        resolver.ToGuid = [this](Entity x) { return IsValid(x) ? (uint64_t)GetGuid(x) : uint64_t(0); };
        resolver.FromGuid = [this](uint64_t g) { return FindEntityByGuid(Guid(g)); };
        struct Saved { Entity E; size_t Index; };
        std::vector<Saved> saved;
        for (auto [e, sc] : m_Registry.ViewOf<ScriptComponent>())
            for (size_t i = 0; i < sc.Scripts.size(); ++i)
            {
                auto& script = sc.Scripts[i];
                const ScriptInfo* info = script.Instance ? ScriptRegistry::Find(script.ClassName) : nullptr;
                if (!info || info->Module != module) continue;
                script.Instance->OnBeforeSerialize();
                void* obj = info->ToObject(script.Instance.get());
                for (auto& f : info->Fields) script.Fields[f.Name] = FieldToJson(f, obj, &resolver);
                saved.push_back({ e, i });
            }
        for (auto& s : saved) m_Registry.Get<ScriptComponent>(s.E).Scripts[s.Index].Instance.reset();
        bool ok = reload();

        std::vector<std::shared_ptr<ScriptableEntity>> created;
        for (auto& s : saved)
        {
            auto* sc = IsValid(s.E) ? m_Registry.TryGet<ScriptComponent>(s.E) : nullptr;
            if (!sc || s.Index >= sc->Scripts.size()) continue;
            auto& script = sc->Scripts[s.Index];
            const ScriptInfo* info = ScriptRegistry::Find(script.ClassName);
            if (!info)
            {
                Log::Warn("Script class '{}' is gone after the reload; it stays inactive until play restarts", script.ClassName);
                continue;
            }
            created.push_back(CreateScriptInstance(s.E, script, *info, resolver));
        }
        for (auto& inst : created) inst->OnScriptsReloaded();
        for (auto& s : saved)
            if (IsValid(s.E)) SyncScriptsEnabled(s.E); // OnEnable again (listeners started over)
        Log::Info("Reloaded {} live script(s) while playing", created.size());
        return ok;
    }

    std::shared_ptr<ScriptableEntity> Scene::CreateScriptInstance(Entity e, ScriptInstance& script, const ScriptInfo& info, const EntityRefResolver& resolver)
    {
        auto instance = CreateDetachedScriptInstance(e, script, info, resolver);
        script.Instance = instance;
        return instance;
    }

    std::shared_ptr<ScriptableEntity> Scene::CreateDetachedScriptInstance(Entity e, const ScriptInstance& script, const ScriptInfo& info,
                                                                          const EntityRefResolver& resolver)
    {
        auto instance = info.Create();
        instance->m_Entity = e;
        instance->m_Scene = this;
        void* obj = info.ToObject(instance.get());
        for (auto& f : info.Fields)
            if (script.Fields.is_object() && script.Fields.contains(f.Name)) FieldFromJson(f, obj, script.Fields[f.Name], &resolver);
        return instance;
    }

    void Scene::InstantiateScripts()
    {
        EntityRefResolver resolver;
        resolver.FromGuid = [this](uint64_t g) { return FindEntityByGuid(Guid(g)); };

        std::vector<std::shared_ptr<ScriptableEntity>> created;
        std::vector<Entity> owners;
        for (auto [e, sc] : m_Registry.ViewOf<ScriptComponent>())
        {
            for (auto& script : sc.Scripts)
            {
                if (script.Instance || script.ClassName.empty()) continue;
                const ScriptInfo* info = ScriptRegistry::Find(script.ClassName);
                if (!info)
                {
                    Log::Error("Script class '{}' is not registered", script.ClassName);
                    continue;
                }
                created.push_back(CreateScriptInstance(e, script, *info, resolver));
                if (owners.empty() || owners.back() != e) owners.push_back(e);
            }
        }
        // OnCreate may create entities/scripts, so call after the iteration (in attachment order, like Awake), then
        // OnEnable for the scripts of active objects.
        for (auto& inst : created) inst->OnCreate();
        for (Entity e : owners)
            if (IsValid(e)) SyncScriptsEnabled(e);
    }

    void Scene::OnRuntimeStart()
    {
        m_Running = true;
        m_FixedAccumulator = 0.0f;
        m_Time = 0.0f;
        m_TimeScale = 1.0f;
        m_UnscaledTime = 0.0f;
        Input::SetCursorMode(CursorMode::Normal);
        m_FrameCount = 0;
        UpdateTransforms();

        m_Physics2D = MakeScope<Physics2DWorld>(*this);
        m_Physics3D = MakeScope<Physics3DWorld>(*this);
        m_Audio = MakeScope<AudioWorld>(*this);
        m_LoadedScenes = { LoadedScene{ m_Name, m_Roots } };
        m_Persistent.clear();

        ParticleSystems::Restart(*this);
        Timeline::Restart(*this);
        TrailRenderers::ClearAll(*this);
        for (auto [e, anim] : m_Registry.ViewOf<AnimatorComponent>())
            if (anim.Controller.IsValid())
            {
                anim.Runtime = AnimatorRuntime();
                anim.Clip.clear();
                anim.FadeDuration = 0.0f;
            }
        for (auto& entry : SystemRegistry::All()) m_Systems.push_back(entry.Create());
        // Navigation meshes from the colliders as they start (before scripts send agents anywhere).
        for (auto [e, s] : m_Registry.ViewOf<NavMeshSurfaceComponent>()) s.Data.reset();
        for (auto [e, a] : m_Registry.ViewOf<NavMeshAgentComponent>())
        {
            a.Mesh.reset();
            a.Node = -1;
        }
        Navigation::BakeAll(*this);
        DataAssets::OnRuntimeStart(); // the scripts' data asset objects start from their files
        InstantiateScripts();
        for (auto& sys : m_Systems) sys->OnStart(*this);
        m_Audio->Start();
    }

    void Scene::StartQueryPhysics()
    {
        if (m_Running) return;
        UpdateTransforms();
        m_Physics3D = MakeScope<Physics3DWorld>(*this);
    }

    // ---- Several scenes at runtime -----------------------------------------------------

    void Scene::DontDestroyOnLoad(Entity e)
    {
        if (!IsValid(e)) return;
        while (!GetParent(e).IsNull()) e = GetParent(e);
        if (IsPersistent(e)) return;
        m_Persistent.push_back(e);
        for (auto& loaded : m_LoadedScenes) std::erase(loaded.Roots, e);
        auto it = std::find_if(m_LoadedScenes.begin(), m_LoadedScenes.end(), [](auto& l) { return l.Name == kPersistentSceneName; });
        if (it == m_LoadedScenes.end()) it = m_LoadedScenes.insert(m_LoadedScenes.end(), LoadedScene{ kPersistentSceneName, {} });
        it->Roots.push_back(e);
    }

    bool Scene::IsPersistent(Entity e) const
    {
        if (!IsValid(e)) return false;
        while (!GetParent(e).IsNull()) e = GetParent(e);
        return std::find(m_Persistent.begin(), m_Persistent.end(), e) != m_Persistent.end();
    }

    void Scene::LoadSceneData(const nlohmann::json& data, bool additive)
    {
        std::string name = data.value("name", std::string("Untitled"));
        FlushDeferredDestroys();
        std::erase_if(m_Persistent, [this](Entity e) { return !IsValid(e); });
        if (!additive)
        {
            std::vector<Entity> roots = m_Roots; // copy: destroying edits the list
            for (Entity r : roots)
                if (!IsPersistent(r)) DestroyEntity(r);
            std::erase_if(m_LoadedScenes, [](auto& l) { return l.Name != kPersistentSceneName; });
            m_Name = name;
            if (data.contains("settings")) SceneSerializer::ReadSettings(data["settings"], m_Settings);
            m_UIHovered = Entity::Null();
        }
        std::vector<Entity> roots = SceneSerializer::DeserializeInto(*this, data);
        UpdateTransforms();
        if (m_Running)
        {
            for (Entity r : roots)
            {
                if (m_Physics2D) m_Physics2D->CreateBodiesFor(r);
                if (m_Physics3D) m_Physics3D->CreateBodiesFor(r);
                if (m_Audio) m_Audio->CreateSourcesFor(r);
            }
            Navigation::BakeAll(*this, true); // surfaces the new scene brought
            InstantiateScripts();
        }
        // The active (single-mode) scene comes first, after any persistent objects' entry.
        LoadedScene entry{ name, roots };
        if (additive) m_LoadedScenes.push_back(std::move(entry));
        else m_LoadedScenes.insert(m_LoadedScenes.begin(), std::move(entry));
    }

    void Scene::RegisterLoadedScene(std::string name, std::vector<Entity> roots)
    {
        for (LoadedScene& l : m_LoadedScenes)
            std::erase_if(l.Roots, [&](Entity e) { return std::find(roots.begin(), roots.end(), e) != roots.end(); });
        m_LoadedScenes.push_back({ std::move(name), std::move(roots) });
    }

    bool Scene::UnloadScene(const std::string& name)
    {
        auto it = std::find_if(m_LoadedScenes.begin(), m_LoadedScenes.end(), [&](auto& l) { return l.Name == name; });
        if (it == m_LoadedScenes.end()) return false;
        std::vector<Entity> roots = std::move(it->Roots);
        m_LoadedScenes.erase(it);
        for (Entity r : roots)
            if (IsValid(r)) DestroyEntity(r);
        std::erase_if(m_Persistent, [this](Entity e) { return !IsValid(e); });
        m_UIHovered = Entity::Null();
        return true;
    }

    void Scene::OnRuntimeStop()
    {
        DebugDraw::Clear();
        if (!m_Running) return;
        Input::SetKeyboardCapturedByUI(false);
        Input::SetCursorMode(CursorMode::Normal);
        for (auto [e, sc] : m_Registry.ViewOf<ScriptComponent>())
            for (auto& inst : sc.LiveInstances(false))
                if (inst->m_EnableNotified) { inst->m_EnableNotified = false; inst->OnDisable(); }
        for (auto [e, sc] : m_Registry.ViewOf<ScriptComponent>())
            for (auto& inst : sc.LiveInstances(false)) inst->OnDestroy();
        for (auto [e, sc] : m_Registry.ViewOf<ScriptComponent>())
            for (auto& script : sc.Scripts) script.Instance.reset();
        for (auto& sys : m_Systems) sys->OnStop(*this);
        m_Systems.clear();
        if (s_EventHook) s_EventHook(*this, Entity::Null(), EventKind::Stopped, 0.0f, Entity::Null(), nullptr);
        m_Audio.reset();
        m_Physics3D.reset();
        m_Physics2D.reset();
        m_Running = false;
    }

    void Scene::OnUpdateRuntime(float dt)
    {
        if (m_Paused)
        {
            if (m_StepFrames <= 0)
            {
                UpdateTransforms();
                return;
            }
            --m_StepFrames;
            dt = kFixedTimestep;
        }
        dt = std::min(dt, 0.1f); // avoid spiral of death after hitches
        DebugDraw::Tick(dt);     // the last update's debug lines go (timed ones count down)
        const float unscaledDt = dt;
        m_UnscaledDeltaTime = dt;
        m_UnscaledTime += dt;
        dt *= m_TimeScale;
        m_Time += dt;
        m_DeltaTime = dt;
        ++m_FrameCount;

        Input::SetKeyboardCapturedByUI(false); // the UI reads the keyboard first
        if (m_UIInputValid)
        {
            UI::PointerState pointer{ m_UIPointer, m_UIDown, m_UIDown && !m_UIWasDown, !m_UIDown && m_UIWasDown, m_UIScroll };
            pointer.Text = Input::GetTypedText() + m_UIInjectedText;
            pointer.Backspace = Input::IsKeyTyped(Key::Backspace) || m_UIInjectedBackspace;
            pointer.Enter = Input::IsKeyPressed(Key::Enter) || m_UIInjectedEnter;
            pointer.Delete = Input::IsKeyTyped(Key::Delete);
            pointer.Left = Input::IsKeyTyped(Key::Left);
            pointer.Right = Input::IsKeyTyped(Key::Right);
            pointer.LineUp = Input::IsKeyTyped(Key::Up);
            pointer.LineDown = Input::IsKeyTyped(Key::Down);
            pointer.Home = Input::IsKeyTyped(Key::Home);
            pointer.End = Input::IsKeyTyped(Key::End);
            pointer.Tab = Input::IsKeyTyped(Key::Tab);
            // Navigation: arrows (with key repeat), D-pad, and the left stick crossing half way (one step per push).
            glm::vec2 stick(Input::GetGamepadAxis(GamepadAxis::LeftX), Input::GetGamepadAxis(GamepadAxis::LeftY));
            int stickDir = std::abs(stick.x) > 0.5f || std::abs(stick.y) > 0.5f
                               ? (std::abs(stick.x) > std::abs(stick.y) ? (stick.x > 0 ? 2 : 1) : (stick.y > 0 ? 4 : 3)) : 0;
            bool stickPush = stickDir != 0 && stickDir != m_UIStickDir;
            m_UIStickDir = stickDir;
            bool keys = m_Settings.UIKeyboardNavigation;
            pointer.NavLeft = (keys && Input::IsKeyTyped(Key::Left)) || Input::IsGamepadButtonPressed(GamepadButton::DpadLeft) || (stickPush && stickDir == 1);
            pointer.NavRight = (keys && Input::IsKeyTyped(Key::Right)) || Input::IsGamepadButtonPressed(GamepadButton::DpadRight) || (stickPush && stickDir == 2);
            pointer.NavUp = (keys && Input::IsKeyTyped(Key::Up)) || Input::IsGamepadButtonPressed(GamepadButton::DpadUp) || (stickPush && stickDir == 3);
            pointer.NavDown = (keys && Input::IsKeyTyped(Key::Down)) || Input::IsGamepadButtonPressed(GamepadButton::DpadDown) || (stickPush && stickDir == 4);
            pointer.Submit = (keys && (Input::IsKeyPressed(Key::Enter) || Input::IsKeyPressed(Key::Space))) || Input::IsGamepadButtonPressed(GamepadButton::A);
            pointer.Cancel = Input::IsKeyPressed(Key::Escape) || Input::IsGamepadButtonPressed(GamepadButton::B);
            int mods = Input::GetModifiers();
            pointer.Shift = (mods & ModShift) != 0;
            pointer.Ctrl = (mods & ModControl) != 0;
            pointer.SelectAll = pointer.Ctrl && Input::IsKeyTyped(Key::A);
            pointer.Copy = pointer.Ctrl && Input::IsKeyTyped(Key::C);
            pointer.Cut = pointer.Ctrl && Input::IsKeyTyped(Key::X);
            pointer.Paste = pointer.Ctrl && Input::IsKeyTyped(Key::V);
            if (pointer.Paste) pointer.PasteText = Input::GetClipboardText();
            pointer.SetClipboard = [](const std::string& text) { Input::SetClipboardText(text); };
            m_UIInjectedText.clear();
            m_UIInjectedBackspace = m_UIInjectedEnter = false;
            m_UIHovered = UI::Update(*this, pointer, m_UIScreen, unscaledDt); // menus work while the game is frozen
            // A focused text field keeps the keys from gameplay (WASD while typing a name must not move the player).
            bool typing = false;
            for (auto [e, field] : m_Registry.ViewOf<UIInputFieldComponent>()) typing |= field.Focused;
            Input::SetKeyboardCapturedByUI(typing);
            m_UIScroll = 0.0f;
            m_UIWasDown = m_UIDown;
        }

        {
            DIM_PROFILE_SCOPE("Scripts");
            // OnEnable / OnDisable for changes made without SetActive / SetScriptEnabled (the Inspector's boxes).
            std::vector<Entity> scripted;
            for (auto [e, sc] : m_Registry.ViewOf<ScriptComponent>()) scripted.push_back(e);
            for (Entity e : scripted)
                if (IsValid(e)) SyncScriptsEnabled(e);
            m_Registry.ViewOf<ScriptComponent>().Each([&](Entity e, ScriptComponent& sc) {
                if (!m_Registry.Get<TransformComponent>(e).ActiveInHierarchy) return;
                for (auto& inst : sc.LiveInstances(true))
                {
                    inst->OnUpdate(dt);
                    inst->TickTimers(dt);
                    inst->TickCoroutines(dt);
                }
            });
        }
        {
            DIM_PROFILE_SCOPE("Systems");
            for (auto& sys : m_Systems) sys->OnUpdate(*this, dt);
        }
        {
            DIM_PROFILE_SCOPE("Navigation");
            Navigation::Update(*this, dt);
        }
        UpdateSpriteAnimators(dt);

        m_FixedAccumulator += dt;
        while (m_FixedAccumulator >= kFixedTimestep)
        {
            DIM_PROFILE_SCOPE("Fixed Update");
            m_Registry.ViewOf<ScriptComponent>().Each([&](Entity e, ScriptComponent& sc) {
                if (!m_Registry.Get<TransformComponent>(e).ActiveInHierarchy) return;
                for (auto& inst : sc.LiveInstances(true)) inst->OnFixedUpdate(kFixedTimestep);
            });
            for (auto& sys : m_Systems) sys->OnFixedUpdate(*this, kFixedTimestep);
            UpdateTransforms();
            {
                DIM_PROFILE_SCOPE("Physics");
                m_Physics2D->Step(kFixedTimestep);
                m_Physics3D->Step(kFixedTimestep);
            }
            m_FixedAccumulator -= kFixedTimestep;
        }

        Timeline::Update(*this, dt);
        UpdateTransforms();
        Navigation::FollowPlatforms(*this); // agents on Moving surfaces, where physics took them
        {
            DIM_PROFILE_SCOPE("Animation");
            UpdateAnimators(dt);
        }
        Water::Update(*this, m_Time);
        {
            DIM_PROFILE_SCOPE("Particles");
            ParticleSystems::Update(*this, dt);
            TrailRenderers::Update(*this, m_Time);
        }
        {
            DIM_PROFILE_SCOPE("Audio");
            AudioMixer::Update(dt);
            m_Audio->Update(dt);
        }
        FlushDeferredDestroys();
    }

    void Scene::UpdateSpriteAnimators(float dt)
    {
        for (auto [e, anim, sprite] : m_Registry.ViewOf<SpriteAnimatorComponent, SpriteRendererComponent>())
        {
            if (!anim.Playing || anim.FrameCount <= 0) continue;
            anim.Time += dt;
            int step = (int)(anim.Time * anim.FramesPerSecond);
            if (anim.Loop) step %= anim.FrameCount;
            else if (step >= anim.FrameCount)
            {
                step = anim.FrameCount - 1;
                anim.Playing = false;
            }
            sprite.Frame = anim.FirstFrame + step;
        }
    }

    void Scene::OnUpdateEditor(float dt)
    {
        UpdateTransforms();
        UpdateAnimators(0.0f);
        ParticleSystems::Update(*this, dt); // live preview while editing, like Unity's scene view
        m_EditorTime += dt;
        Water::Update(*this, m_EditorTime); // waves move in the editor too
    }

    namespace
    {
        // The clips an Animator plays: its mesh's own, or its Animation Source's retargeted onto the mesh's skeleton
        // (cached per source and target mesh, and rebuilt when either is reloaded).
        const std::vector<AnimationClip>& AnimatorClips(const AnimatorComponent& anim, const Ref<Mesh>& mesh)
        {
            if (!anim.ClipSource.IsValid()) return mesh->GetClips();
            Ref<Mesh> source = AssetManager::GetMesh(anim.ClipSource);
            if (!source || !source->GetSkeleton() || source == mesh) return mesh->GetClips();
            struct Entry
            {
                std::weak_ptr<Mesh> Source, Target;
                std::vector<AnimationClip> Clips;
            };
            static std::map<std::pair<const Mesh*, const Mesh*>, Entry> cache;
            Entry& entry = cache[{ source.get(), mesh.get() }];
            if (entry.Source.lock() != source || entry.Target.lock() != mesh)
            {
                entry.Source = source;
                entry.Target = mesh;
                entry.Clips.clear();
                for (const AnimationClip& clip : source->GetClips())
                    entry.Clips.push_back(Animation::RetargetClip(*source->GetSkeleton(), *mesh->GetSkeleton(), clip));
            }
            return entry.Clips;
        }

        // Blends two joint transforms: positions and scales linearly, rotations along the shortest arc.
        glm::mat4 BlendTransforms(const glm::mat4& a, const glm::mat4& b, float w)
        {
            glm::vec3 sa(glm::length(glm::vec3(a[0])), glm::length(glm::vec3(a[1])), glm::length(glm::vec3(a[2])));
            glm::vec3 sb(glm::length(glm::vec3(b[0])), glm::length(glm::vec3(b[1])), glm::length(glm::vec3(b[2])));
            glm::mat3 ra(a), rb(b);
            for (int c = 0; c < 3; ++c)
            {
                ra[c] /= std::max(sa[c], 1e-8f);
                rb[c] /= std::max(sb[c], 1e-8f);
            }
            glm::mat4 out = glm::mat4_cast(glm::slerp(glm::normalize(glm::quat_cast(ra)), glm::normalize(glm::quat_cast(rb)), w));
            glm::vec3 s = glm::mix(sa, sb, w);
            for (int c = 0; c < 3; ++c) out[c] *= s[c];
            out[3] = glm::vec4(glm::mix(glm::vec3(a[3]), glm::vec3(b[3]), w), 1.0f);
            return out;
        }
    }

    void Scene::UpdateAnimators(float dt)
    {
        Pose current, previous;
        std::vector<std::pair<Entity, std::vector<std::string>>> firedEvents;

        // IK targets, by the Animator they pull on (positions are this frame's world transforms).
        struct IKGoal
        {
            Entity Animator;
            const IKTargetComponent* Target;
            glm::vec3 Position, HintPosition;
            bool HasHint;
        };
        std::vector<IKGoal> ikGoals;
        for (auto [e, ik, t] : m_Registry.ViewOf<IKTargetComponent, TransformComponent>())
        {
            if (!t.ActiveInHierarchy || ik.Weight <= 0.0f || ik.Tip.empty()) continue;
            Entity owner = ik.Animator;
            if (owner.IsNull() || !IsValid(owner))
            {
                owner = Entity::Null();
                for (Entity p = GetParent(e); !p.IsNull(); p = GetParent(p))
                    if (m_Registry.Has<AnimatorComponent>(p)) { owner = p; break; }
            }
            if (owner.IsNull() || !m_Registry.Has<AnimatorComponent>(owner)) continue;
            bool hasHint = !ik.Hint.IsNull() && IsValid(ik.Hint) && m_Registry.Has<TransformComponent>(ik.Hint);
            ikGoals.push_back({ owner, &ik, glm::vec3(t.World[3]), hasHint ? glm::vec3(m_Registry.Get<TransformComponent>(ik.Hint).World[3]) : glm::vec3(0.0f), hasHint });
        }

        for (auto [e, anim, mr] : m_Registry.ViewOf<AnimatorComponent, MeshRendererComponent>())
        {
            Ref<Mesh> mesh = AssetManager::GetMesh(mr.Mesh);
            const Skeleton* skeleton = mesh ? mesh->GetSkeleton() : nullptr;
            if (!skeleton)
            {
                anim.SkinMatrices.clear();
                continue;
            }
            const auto& clips = AnimatorClips(anim, mesh);
            auto duration = [&](const std::string& name) {
                const AnimationClip* c = Animation::FindClip(clips, name);
                return c ? c->Duration : 0.0f;
            };
            Ref<AnimatorController> controller = AssetManager::GetAnimatorController(anim.Controller);
            if (controller) controller->LinkLayers();

            // IK goals of this Animator for one layer (-1: the final pass, also taking layers past the last one).
            int layerCount = controller ? (int)controller->Layers.size() : 0;
            auto applyIK = [&](int layer, float layerWeight) {
                bool any = false;
                for (auto& g : ikGoals) any |= g.Animator == e;
                if (!any || layerWeight <= 0.0f) return;
                glm::mat4 toModel = glm::inverse(m_Registry.Get<TransformComponent>(e).World);
                for (auto& g : ikGoals)
                {
                    if (g.Animator != e) continue;
                    int at = g.Target->Layer < 0 || g.Target->Layer > layerCount ? -1 : g.Target->Layer;
                    if (at != layer) continue;
                    glm::vec3 hint = glm::vec3(toModel * glm::vec4(g.HintPosition, 1.0f)), target = glm::vec3(toModel * glm::vec4(g.Position, 1.0f));
                    float weight = std::clamp(g.Target->Weight, 0.0f, 1.0f) * layerWeight;
                    int tip = skeleton->FindJoint(g.Target->Tip);
                    if (g.Target->Mode == IKMode::LookAt)
                        Animation::SolveLookAt(*skeleton, current, tip, g.Target->AimAxis, g.Target->ChainLength, target, g.Target->MaxAngle, weight);
                    else
                        Animation::SolveIK(*skeleton, current, tip, g.Target->ChainLength, target, g.HasHint ? &hint : nullptr, weight);
                }
            };

            // Samples a blend tree at a normalized time: every weighted child, in sync (1D trees have at most two,
            // 2D trees any number). Children are folded in one at a time, each at its share of the weight so far.
            auto sampleTree = [&](const AnimatorController& machine, const AnimatorRuntime& runtime, const AnimState& st, float normalized, Pose& out) {
                std::vector<float> w;
                AnimatorStateMachine::TreeWeights(machine, runtime, st, w);
                float frac = st.Loop ? normalized - std::floor(normalized) : std::clamp(normalized, 0.0f, 1.0f);
                auto sampleChild = [&](int i, Pose& pose) {
                    const AnimationClip* c = Animation::FindClip(clips, st.Children[i].Clip);
                    if (c) Animation::Sample(*skeleton, *c, frac * c->Duration, pose);
                    else pose.SetBindPose(*skeleton);
                };
                float total = 0.0f;
                for (int i = 0; i < (int)w.size(); ++i)
                {
                    if (w[i] <= 1e-4f) continue;
                    if (total == 0.0f) sampleChild(i, out);
                    else
                    {
                        Pose child, blended;
                        sampleChild(i, child);
                        Animation::Blend(out, child, w[i] / (total + w[i]), blended);
                        out = std::move(blended);
                    }
                    total += w[i];
                }
                if (total == 0.0f) out.SetBindPose(*skeleton);
            };

            // Runs one state machine (the base layer: `pb` is the AnimatorComponent itself; an extra layer: its playback)
            // and produces its pose, including the crossfade from the previous state.
            auto evaluate = [&](auto& pb, const AnimatorController* machine, bool baseLayer, Pose& out) {
                if (machine)
                {
                    AnimatorStateMachine::Update(*machine, pb.Runtime, dt * anim.Speed, duration, [&](const AnimState& state, float fade) {
                        int index = (int)(&state - machine->States.data());
                        bool isTree = state.Motion != AnimMotionType::Clip;
                        bool hasCurrent = !pb.Clip.empty() || pb.TreeState >= 0;
                        if (fade > 0.0f && hasCurrent)
                        {
                            pb.PreviousClip = pb.Clip;
                            pb.PreviousTreeState = pb.TreeState;
                            pb.PreviousTime = pb.Time;
                            pb.FadeElapsed = 0.0f;
                            pb.FadeDuration = fade;
                        }
                        else
                            pb.FadeDuration = 0.0f;
                        pb.PreviousStateIndex = pb.StateIndex;
                        pb.StateIndex = index;
                        pb.TreeState = isTree ? index : -1;
                        pb.Clip = isTree ? std::string() : state.Clip;
                        pb.Time = 0.0f;
                        pb.Loop = state.Loop;
                        pb.StateSpeed = state.Speed;
                        pb.CurrentState = state.Name;
                        pb.Playing = true;
                    });
                    if (!pb.Runtime.FiredEvents.empty()) firedEvents.emplace_back(e, pb.Runtime.FiredEvents);
                }
                else
                {
                    pb.StateSpeed = 1.0f;
                    pb.TreeState = pb.PreviousTreeState = -1;
                }
                auto treeOf = [&](int index) -> const AnimState* {
                    return machine && index >= 0 && index < (int)machine->States.size() ? &machine->States[index] : nullptr;
                };
                const AnimState* tree = treeOf(pb.TreeState);
                const AnimationClip* clip = tree ? nullptr : Animation::FindClip(clips, pb.Clip);
                if (baseLayer && !tree && !clip && !clips.empty() && pb.Clip.empty()) clip = &clips.front(); // default to the first clip

                auto advance = [&](float& time, const AnimationClip* c) {
                    if (!c || !pb.Playing) return;
                    time += dt * anim.Speed * pb.StateSpeed;
                    if (c->Duration <= 0.0f) { time = 0.0f; return; }
                    if (pb.Loop) time = std::fmod(std::fmod(time, c->Duration) + c->Duration, c->Duration);
                    else time = std::clamp(time, 0.0f, c->Duration);
                };
                if (tree) sampleTree(*machine, pb.Runtime, *tree, pb.Runtime.StateTime, out);
                else
                {
                    advance(pb.Time, clip);
                    if (clip) Animation::Sample(*skeleton, *clip, pb.Time, out);
                    else out.SetBindPose(*skeleton);
                }

                if (pb.FadeDuration > 0.0f && pb.FadeElapsed < pb.FadeDuration)
                {
                    pb.FadeElapsed += dt;
                    if (const AnimState* prevTree = treeOf(pb.PreviousTreeState))
                    {
                        float d = AnimatorStateMachine::StateDuration(*machine, pb.Runtime, *prevTree, duration);
                        if (pb.Playing && d > 0.0f) pb.PreviousTime += dt * anim.Speed * prevTree->Speed / d;
                        sampleTree(*machine, pb.Runtime, *prevTree, pb.PreviousTime, previous);
                        Pose blended;
                        Animation::Blend(previous, out, std::clamp(pb.FadeElapsed / pb.FadeDuration, 0.0f, 1.0f), blended);
                        out = std::move(blended);
                    }
                    else if (const AnimationClip* prev = Animation::FindClip(clips, pb.PreviousClip))
                    {
                        advance(pb.PreviousTime, prev);
                        Animation::Sample(*skeleton, *prev, pb.PreviousTime, previous);
                        Pose blended;
                        Animation::Blend(previous, out, std::clamp(pb.FadeElapsed / pb.FadeDuration, 0.0f, 1.0f), blended);
                        out = std::move(blended);
                    }
                }
            };
            // A partial ragdoll's limp joints follow its capsules over the animation; a ragdoll getting up blends the
            // animated pose in from the limp one over its Get Up Time.
            auto blendGetUp = [&]() {
                auto* r = m_Registry.TryGet<RagdollComponent>(e);
                size_t n = skeleton->Joints.size();
                if (r && r->Driving && r->Limp.size() == n && r->Pose.size() == n && anim.SkinMatrices.size() == n)
                {
                    for (size_t j = 0; j < n; ++j)
                        if (r->Limp[j]) anim.SkinMatrices[j] = r->Pose[j] * skeleton->Joints[j].InverseBind;
                    return;
                }
                if (!r || r->Driving || r->BlendTime >= r->BlendDuration || r->BlendFrom.size() != n || anim.SkinMatrices.size() != n) return;
                r->BlendTime += dt;
                float w = r->GetUpProgress();
                w = w * w * (3.0f - 2.0f * w);
                for (size_t j = 0; j < n; ++j)
                {
                    const glm::mat4& inverseBind = skeleton->Joints[j].InverseBind;
                    glm::mat4 animated = anim.SkinMatrices[j] * glm::inverse(inverseBind);
                    anim.SkinMatrices[j] = BlendTransforms(r->BlendFrom[j], animated, w) * inverseBind;
                }
            };
            if (auto* ragdoll = m_Registry.TryGet<RagdollComponent>(e); ragdoll && ragdoll->Driving && ragdoll->Limp.empty() && ragdoll->Pose.size() == skeleton->Joints.size())
            {
                // A simulating ragdoll poses the skeleton from its capsules.
                anim.SkinMatrices.resize(skeleton->Joints.size());
                for (size_t j = 0; j < skeleton->Joints.size(); ++j) anim.SkinMatrices[j] = ragdoll->Pose[j] * skeleton->Joints[j].InverseBind;
                continue;
            }
            if (anim.TimelineDriven)
            {
                // A timeline's animation track plays this Animator (its own clip, controller and layers wait).
                auto sampleAt = [&](const std::string& name, float time, bool loop, Pose& out) {
                    const AnimationClip* c = Animation::FindClip(clips, name);
                    if (!c) { out.SetBindPose(*skeleton); return; }
                    float d = c->Duration;
                    float t = d <= 0.0f ? 0.0f : loop ? std::fmod(std::fmod(time, d) + d, d) : std::clamp(time, 0.0f, d);
                    Animation::Sample(*skeleton, *c, t, out);
                };
                sampleAt(anim.TimelineClip, anim.TimelineTime, anim.TimelineLoop, current);
                if (!anim.TimelinePrevClip.empty() && anim.TimelineWeight < 1.0f)
                {
                    sampleAt(anim.TimelinePrevClip, anim.TimelinePrevTime, anim.TimelinePrevLoop, previous);
                    Pose blended;
                    Animation::Blend(previous, current, anim.TimelineWeight, blended);
                    current = std::move(blended);
                }
                applyIK(0, 1.0f);
                applyIK(-1, 1.0f);
                Animation::ComputeSkinMatrices(*skeleton, current, anim.SkinMatrices);
                blendGetUp();
                continue;
            }
            evaluate(anim, controller.get(), true, current);

            // Root motion from the base layer (play mode only).
            anim.RootMotionDelta = glm::vec3(0.0f);
            if (anim.ApplyRootMotion && dt > 0.0f && !current.Translations.empty())
            {
                int root = anim.RootMotionJoint.empty() ? (skeleton->Order.empty() ? 0 : skeleton->Order.front()) : skeleton->FindJoint(anim.RootMotionJoint);
                if (root >= 0 && root < (int)skeleton->Joints.size())
                {
                    glm::vec3 start = skeleton->Joints[root].Translation, end = start;
                    const AnimationClip* baseClip = anim.TreeState < 0 ? Animation::FindClip(clips, anim.Clip) : nullptr;
                    if (!baseClip && anim.TreeState < 0 && anim.Clip.empty() && !clips.empty()) baseClip = &clips.front();
                    float time = anim.TreeState >= 0 ? anim.Runtime.StateTime : anim.Time;
                    if (baseClip)
                    {
                        Pose p;
                        Animation::Sample(*skeleton, *baseClip, 0.0f, p);
                        start = p.Translations[root];
                        Animation::Sample(*skeleton, *baseClip, baseClip->Duration, p);
                        end = p.Translations[root];
                    }
                    std::string key = anim.TreeState >= 0 ? "#" + std::to_string(anim.TreeState) : anim.Clip;
                    glm::vec3 modelDelta = Animation::ExtractRootMotion(*skeleton, root, current, anim.RootMotion, start, end, time, anim.Loop, key);
                    auto& t = m_Registry.Get<TransformComponent>(e);
                    glm::vec3 worldDelta = glm::mat3(t.World) * modelDelta;
                    anim.RootMotionDelta = worldDelta;
                    if (glm::dot(worldDelta, worldDelta) > 0.0f)
                    {
                        if (m_Registry.Has<CharacterControllerComponent>(e) && m_Physics3D) Physics3DWorld::MoveCharacter(*this, e, worldDelta);
                        else
                        {
                            Entity parent = GetParent(e);
                            glm::mat3 toParent = parent.IsNull() ? glm::mat3(1.0f) : glm::inverse(glm::mat3(m_Registry.Get<TransformComponent>(parent).World));
                            t.Position += toParent * worldDelta;
                            t.World[3] += glm::vec4(worldDelta, 0.0f); // read before transforms update next (NavMesh Agents with Root Motion)
                        }
                    }
                }
            }

            applyIK(0, 1.0f);

            // Samples a state's motion (clip or blend tree) at a normalized time.
            auto sampleState = [&](const AnimatorController& machine, const AnimatorRuntime& runtime, const AnimState& st, float normalized, Pose& out) {
                if (st.Motion != AnimMotionType::Clip) { sampleTree(machine, runtime, st, normalized, out); return; }
                const AnimationClip* c = Animation::FindClip(clips, st.Clip);
                if (!c) { out.SetBindPose(*skeleton); return; }
                float frac = st.Loop ? normalized - std::floor(normalized) : std::clamp(normalized, 0.0f, 1.0f);
                Animation::Sample(*skeleton, *c, frac * c->Duration, out);
            };
            // A synced layer: the source layer's current (and crossfading previous) state, each played with this
            // layer's motion for it, at the source's normalized times.
            auto evaluateSynced = [&](auto& src, const AnimatorController& srcMachine, const AnimLayer& layer, AnimatorLayerPlayback& pb, Pose& out) {
                auto motionFor = [&](int state) -> const AnimState* {
                    if (state < 0 || state >= (int)srcMachine.States.size()) return nullptr;
                    if (state < (int)layer.Machine.States.size() && HasOwnMotion(layer.Machine.States[state])) return &layer.Machine.States[state];
                    return &srcMachine.States[state];
                };
                auto normalizedOf = [&](int tree, const std::string& clip, float time) {
                    if (tree >= 0) return time;
                    float d = duration(clip);
                    return d > 0.0f ? time / d : 0.0f;
                };
                pb.CurrentState = src.CurrentState;
                pb.Runtime.State = src.Runtime.State;
                pb.Runtime.StateTime = src.Runtime.StateTime;
                pb.FadeElapsed = src.FadeElapsed;
                pb.FadeDuration = src.FadeDuration;
                pb.StateIndex = src.StateIndex;
                pb.PreviousStateIndex = src.PreviousStateIndex;
                const AnimState* st = motionFor(src.StateIndex);
                if (!st) { out = current; return; } // nothing playing yet: leave the pose alone
                sampleState(layer.Machine, src.Runtime, *st, normalizedOf(src.TreeState, src.Clip, src.TreeState >= 0 ? src.Runtime.StateTime : src.Time), out);
                if (src.FadeDuration > 0.0f && src.FadeElapsed < src.FadeDuration)
                    if (const AnimState* prev = motionFor(src.PreviousStateIndex))
                    {
                        sampleState(layer.Machine, src.Runtime, *prev, normalizedOf(src.PreviousTreeState, src.PreviousClip, src.PreviousTime), previous);
                        Pose blended;
                        Animation::Blend(previous, out, std::clamp(src.FadeElapsed / src.FadeDuration, 0.0f, 1.0f), blended);
                        out = std::move(blended);
                    }
            };

            // Extra layers, in order, on top of the base pose.
            if (controller && !controller->Layers.empty())
            {
                if (anim.Layers.size() < controller->Layers.size()) anim.Layers.resize(controller->Layers.size());
                Pose layerPose, reference;
                std::vector<float> mask;
                for (size_t li = 0; li < controller->Layers.size(); ++li)
                {
                    const AnimLayer& layer = controller->Layers[li];
                    auto& pb = anim.Layers[li];
                    float weight = std::clamp(pb.Weight >= 0.0f ? pb.Weight : layer.Weight, 0.0f, 1.0f);
                    bool synced = layer.SyncedLayer >= 0 && layer.SyncedLayer <= (int)li;
                    if (synced)
                    {
                        if (layer.SyncedLayer == 0) evaluateSynced(anim, *controller, layer, pb, layerPose);
                        else evaluateSynced(anim.Layers[layer.SyncedLayer - 1], controller->Layers[layer.SyncedLayer - 1].Machine, layer, pb, layerPose);
                    }
                    else
                    {
                        // Parameters are shared; a trigger consumed by this layer is consumed for the whole Animator.
                        pb.Runtime.Parameters = anim.Runtime.Parameters;
                        pb.Runtime.Triggers = anim.Runtime.Triggers;
                        evaluate(pb, &layer.Machine, false, layerPose);
                        for (auto it = anim.Runtime.Triggers.begin(); it != anim.Runtime.Triggers.end();)
                            it = pb.Runtime.Triggers.contains(*it) ? std::next(it) : anim.Runtime.Triggers.erase(it);
                    }
                    if (weight > 0.0f && !layer.Machine.States.empty())
                    {
                        Animation::BuildMask(*skeleton, layer.Mask, mask);
                        if (layer.Blending == AnimLayerBlending::Override)
                            Animation::LayerOverride(current, layerPose, mask, weight);
                        else
                        {
                            // Reference: the first frame of the layer's current motion.
                            const AnimState* st = nullptr;
                            if (synced)
                            {
                                int s = pb.StateIndex;
                                if (s >= 0 && s < (int)layer.Machine.States.size()) st = HasOwnMotion(layer.Machine.States[s]) ? &layer.Machine.States[s] : nullptr;
                                const AnimatorController* src = controller->LayerMachine(layer.SyncedLayer);
                                if (!st && src && s >= 0 && s < (int)src->States.size()) st = &src->States[s];
                                if (st) sampleState(layer.Machine, pb.Runtime, *st, 0.0f, reference);
                                else reference = layerPose;
                            }
                            else
                            {
                                st = pb.TreeState >= 0 && pb.TreeState < (int)layer.Machine.States.size() ? &layer.Machine.States[pb.TreeState] : nullptr;
                                if (st) sampleTree(layer.Machine, pb.Runtime, *st, 0.0f, reference);
                                else if (const AnimationClip* c = Animation::FindClip(clips, pb.Clip)) Animation::Sample(*skeleton, *c, 0.0f, reference);
                                else reference = layerPose;
                            }
                            Animation::LayerAdditive(current, layerPose, reference, mask, weight);
                        }
                    }
                    applyIK((int)li + 1, weight);
                }
            }
            applyIK(-1, 1.0f);
            Animation::ComputeSkinMatrices(*skeleton, current, anim.SkinMatrices);
            blendGetUp();
        }
        // After iteration: scripts may change the registry.
        for (auto& [e, names] : firedEvents)
            for (auto& name : names) DispatchAnimationEvent(e, name);
    }

    void Scene::DispatchJointBreak(Entity e, float force)
    {
        if (!IsValid(e)) return;
        if (auto* sc = m_Registry.TryGet<ScriptComponent>(e))
            for (auto& inst : sc->LiveInstances(true)) inst->OnJointBreak(force);
    }

    void Scene::DispatchAnimationEvent(Entity e, const std::string& name)
    {
        if (!IsValid(e)) return;
        if (auto* sc = m_Registry.TryGet<ScriptComponent>(e))
            for (auto& inst : sc->LiveInstances(true)) inst->OnAnimationEvent(name);
    }

    void Scene::SetUIInput(const glm::vec2& pointer, bool down, const glm::vec2& screenSize, bool valid, float scroll)
    {
        m_UIScroll = scroll;
        m_UIPointer = pointer;
        m_UIDown = down;
        m_UIScreen = screenSize;
        m_UIInputValid = valid;
        if (!valid) m_UIHovered = Entity::Null();
    }

    void Scene::InjectUIKeyboard(const std::string& text, bool backspace, bool enter)
    {
        m_UIInjectedText += text;
        m_UIInjectedBackspace |= backspace;
        m_UIInjectedEnter |= enter;
    }

    void Scene::DispatchUIClick(Entity e)
    {
        if (auto* sc = m_Registry.TryGet<ScriptComponent>(e))
            for (auto& inst : sc->LiveInstances(true)) inst->OnClick();
        if (s_EventHook && IsValid(e)) s_EventHook(*this, e, EventKind::Click, 0.0f, Entity::Null(), nullptr);
    }

    void Scene::DispatchUIValueChanged(Entity e, float value)
    {
        if (auto* sc = m_Registry.TryGet<ScriptComponent>(e))
            for (auto& inst : sc->LiveInstances(true)) inst->OnUIValueChanged(value);
        if (s_EventHook && IsValid(e)) s_EventHook(*this, e, EventKind::ValueChanged, value, Entity::Null(), nullptr);
    }

    void Scene::DispatchUIText(Entity e, const std::string& text, bool submit)
    {
        if (auto* sc = m_Registry.TryGet<ScriptComponent>(e))
            for (auto& inst : sc->LiveInstances(true))
            {
                if (submit) inst->OnUISubmit(text);
                else inst->OnUITextChanged(text);
            }
        if (s_EventHook && IsValid(e)) s_EventHook(*this, e, submit ? EventKind::Submit : EventKind::TextChanged, 0.0f, Entity::Null(), text.c_str());
    }

    void Scene::DispatchCollision(Entity a, Entity b, CollisionPhase phase, bool trigger, const Collision* fromA)
    {
        if (trigger && phase == CollisionPhase::Stay) return;
        bool begin = phase == CollisionPhase::Enter;
        auto call = [&](Entity self, const Collision& c) {
            auto* sc = IsValid(self) ? m_Registry.TryGet<ScriptComponent>(self) : nullptr;
            if (!sc || !IsValid(c.Other)) return;
            for (auto& inst : sc->LiveInstances(true))
            {
                if (trigger) begin ? inst->OnTriggerEnter(c.Other) : inst->OnTriggerExit(c.Other);
                else if (phase == CollisionPhase::Enter) inst->OnCollisionEnter(c);
                else if (phase == CollisionPhase::Stay) inst->OnCollisionStay(c);
                else inst->OnCollisionExit(c);
            }
        };
        if (IsValid(a) && IsValid(b))
        {
            Collision seenByA;
            if (fromA) seenByA = *fromA;
            seenByA.Other = b;
            Collision seenByB = seenByA.Flipped(a);
            call(a, seenByA);
            call(b, seenByB);
            if (s_EventHook && phase != CollisionPhase::Stay)
            {
                EventKind kind = trigger ? (begin ? EventKind::TriggerEnter : EventKind::TriggerExit) : (begin ? EventKind::CollisionEnter : EventKind::CollisionExit);
                if (IsValid(a) && IsValid(b)) s_EventHook(*this, a, kind, 0.0f, b, nullptr);
                if (IsValid(a) && IsValid(b)) s_EventHook(*this, b, kind, 0.0f, a, nullptr);
            }
        }
    }
}
