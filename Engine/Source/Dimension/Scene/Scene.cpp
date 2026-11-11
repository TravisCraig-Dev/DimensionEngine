#include "Dimension/Scene/DebugDraw.h"
#include "Dimension/Scene/Scene.h"
#include "Dimension/Animation/ClipAsset.h"
#include "Dimension/Video/VideoPlayer.h"
#include "Dimension/Scene/SceneSerializer.h"
#include "Dimension/Scripting/ScriptableEntity.h"
#include "Dimension/Scripting/ScriptRegistry.h"
#include "Dimension/Physics/Physics2D.h"
#include "Dimension/Physics/Physics3D.h"
#include "Dimension/Physics/Water.h"
#include "Dimension/Navigation/NavMesh.h"
#include "Dimension/Core/Profiler.h"
#include "Dimension/Core/AppRuntime.h"
#include "Dimension/Audio/AudioEngine.h"
#include "Dimension/Asset/AssetManager.h"
#include "Dimension/Renderer/Material.h"
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
#include "Dimension/Scene/CameraProjection.h"
#include "Dimension/Scene/ComponentRegistry.h"
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
        Scene::CoroutinePhaseHook s_CoroutinePhaseHook = nullptr;
        Scene::StateBehaviourHook s_StateBehaviourHook = nullptr;
    }

    void Scene::SetStateBehaviourHook(StateBehaviourHook hook) { s_StateBehaviourHook = hook; }

    void Scene::SetEventHook(EventHook hook) { s_EventHook = hook; }
    void Scene::SetCoroutinePhaseHook(CoroutinePhaseHook hook) { s_CoroutinePhaseHook = hook; }

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
        auto removals = std::move(m_PendingRemovals);
        m_PendingRemovals.clear();
        for (auto& r : removals)
        {
            if (!IsValid(r.E)) continue;
            if (r.Script)
            {
                auto* sc = m_Registry.TryGet<ScriptComponent>(r.E);
                auto it = sc ? std::find_if(sc->Scripts.begin(), sc->Scripts.end(), [&](auto& s) { return s.Instance.get() == r.Script; })
                             : std::vector<ScriptInstance>::iterator();
                if (!sc || it == sc->Scripts.end()) continue;
                auto instance = it->Instance; // kept alive through its callbacks
                sc->Scripts.erase(it);
                if (instance->m_EnableNotified) { instance->m_EnableNotified = false; instance->OnDisable(); }
                instance->OnDestroy();
                continue;
            }
            const ComponentInfo* info = ComponentRegistry::Find(r.Component);
            if (!info || !info->Has(*this, r.E)) continue;
            if (m_Audio && r.Component == "Audio Source") m_Audio->OnEntityDestroyed(r.E);
            info->Remove(*this, r.E);
            static const char* k3D[] = { "Rigidbody", "Box Collider", "Sphere Collider", "Capsule Collider", "Mesh Collider", "Character Controller" };
            static const char* k2D[] = { "Rigidbody 2D", "Box Collider 2D", "Circle Collider 2D" };
            for (const char* n : k3D)
                if (m_Physics3D && r.Component == n) m_Physics3D->RebuildBody(r.E);
            for (const char* n : k2D)
                if (m_Physics2D && r.Component == n) m_Physics2D->RebuildBody(r.E);
            if (r.Component == "Joint" || r.Component == "Joint 2D") RebuildJoint(r.E); // the joint lets go
        }
        auto pending = std::move(m_PendingDestroy);
        m_PendingDestroy.clear();
        for (Entity e : pending) DestroyEntity(e);
    }

    bool Scene::RemoveScriptDeferred(Entity e, const ScriptableEntity* script)
    {
        auto* sc = IsValid(e) && script ? m_Registry.TryGet<ScriptComponent>(e) : nullptr;
        if (!sc || std::none_of(sc->Scripts.begin(), sc->Scripts.end(), [&](auto& s) { return s.Instance.get() == script; })) return false;
        m_PendingRemovals.push_back({ e, script, {} });
        if (!m_Running) FlushDeferredDestroys();
        return true;
    }

    bool Scene::RemoveComponentDeferred(Entity e, const std::string& component)
    {
        static const char* kKept[] = { "Transform", "Tag", "ID", "Hierarchy", "Script" }; // part of every object (scripts go one by one)
        for (const char* k : kKept)
            if (component == k) return false;
        const ComponentInfo* info = ComponentRegistry::Find(component);
        if (!IsValid(e) || !info || !info->Has(*this, e)) return false;
        m_PendingRemovals.push_back({ e, nullptr, component });
        if (!m_Running) FlushDeferredDestroys();
        return true;
    }

    void Scene::SetWorldPose(Entity e, const glm::vec3& position, const glm::quat& rotation)
    {
        UpdateTransforms();
        auto& t = m_Registry.Get<TransformComponent>(e);
        glm::vec3 scale(glm::length(glm::vec3(t.World[0])), glm::length(glm::vec3(t.World[1])), glm::length(glm::vec3(t.World[2])));
        glm::mat4 world = glm::translate(glm::mat4(1.0f), position) * glm::toMat4(rotation) * glm::scale(glm::mat4(1.0f), scale);
        Entity parent = GetParent(e);
        glm::mat4 local = parent.IsNull() ? world : glm::inverse(m_Registry.Get<TransformComponent>(parent).World) * world;
        glm::vec3 s, p, skew;
        glm::vec4 perspective;
        glm::quat r;
        if (glm::decompose(local, s, r, p, skew, perspective))
        {
            t.Position = p;
            t.Rotation = glm::eulerAngles(r);
            t.Scale = s;
        }
        UpdateTransforms();
    }

    void Scene::StartAdded(Entity root)
    {
        if (!m_Running) return;
        UpdateTransforms();
        if (m_Physics2D) m_Physics2D->CreateBodiesFor(root);
        if (m_Physics3D) m_Physics3D->CreateBodiesFor(root);
        if (m_Audio) m_Audio->CreateSourcesFor(root);
        InstantiateScripts();
    }

    void Scene::CaptureScriptFields(Entity root)
    {
        EntityRefResolver resolver;
        resolver.ToGuid = [this](Entity x) { return IsValid(x) ? (uint64_t)GetGuid(x) : uint64_t(0); };
        resolver.FromGuid = [this](uint64_t g) { return FindEntityByGuid(Guid(g)); };
        std::vector<Entity> subtree{ root };
        for (size_t i = 0; i < subtree.size(); ++i)
            for (Entity c : GetChildren(subtree[i])) subtree.push_back(c);
        for (Entity e : subtree)
            if (auto* sc = m_Registry.TryGet<ScriptComponent>(e))
                for (auto& script : sc->Scripts)
                {
                    const ScriptInfo* info = script.Instance ? ScriptRegistry::Find(script.ClassName) : nullptr;
                    if (!info) continue;
                    script.Instance->OnBeforeSerialize();
                    void* obj = info->ToObject(script.Instance.get());
                    for (auto& f : info->Fields) script.Fields[f.Name] = FieldToJson(f, obj, &resolver);
                }
    }

    Entity Scene::Instantiate(Entity original, Entity parent, const glm::vec3* position, const glm::quat* rotation)
    {
        if (!IsValid(original) || (!parent.IsNull() && !IsValid(parent))) return Entity::Null();
        if (m_Running) CaptureScriptFields(original);
        UpdateTransforms();
        glm::vec3 scale, p, skew;
        glm::vec4 perspective;
        glm::quat r;
        glm::decompose(m_Registry.Get<TransformComponent>(original).World, scale, r, p, skew, perspective);
        auto tree = SceneSerializer::SerializeEntityTree(*this, original);
        Entity copy = SceneSerializer::DeserializeEntityTree(*this, tree, parent, true);
        if (copy.IsNull()) return copy;
        SetWorldPose(copy, position ? *position : p, rotation ? *rotation : r);
        StartAdded(copy);
        return copy;
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

    ScriptableEntity* Scene::AddScript(Entity e, const std::string& className)
    {
        const ScriptInfo* info = ScriptRegistry::Find(className);
        if (!IsValid(e) || !info) return nullptr;
        auto& sc = m_Registry.TryGet<ScriptComponent>(e) ? m_Registry.Get<ScriptComponent>(e) : m_Registry.Emplace<ScriptComponent>(e);
        ScriptInstance& script = sc.Add(className);
        if (!m_Running) return nullptr;
        EntityRefResolver resolver;
        resolver.FromGuid = [this](uint64_t g) { return FindEntityByGuid(Guid(g)); };
        auto instance = CreateScriptInstance(e, script, *info, resolver); // `script` may move once OnCreate adds more
        instance->OnBeforeCreate();
        instance->OnCreate();
        if (IsValid(e)) SyncScriptsEnabled(e);
        return instance.get();
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

    Entity Scene::InstantiatePrefab(AssetHandle prefab, const glm::vec3& position, const glm::quat& rotation, Entity parent)
    {
        Entity root = Prefabs::Instantiate(*this, prefab, parent);
        if (root.IsNull()) return root;
        SetWorldPose(root, position, rotation);
        StartAdded(root);
        return root;
    }

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
        Entity found = Entity::Null(), fallback = Entity::Null();
        for (auto [e, cam, t] : m_Registry.ViewOf<CameraComponent, TransformComponent>())
        {
            if (!t.ActiveInHierarchy) continue;
            if (cam.Primary) { found = e; break; }
            if (fallback.IsNull()) fallback = e;
        }
        if (found.IsNull()) found = fallback;
        if (found != m_LastPrimaryCamera)
        {
            if (!m_LastPrimaryCamera.IsNull()) ++m_CameraCuts; // the first camera shown is no cut
            m_LastPrimaryCamera = found;
        }
        return found;
    }

    void Scene::RebuildJoint(Entity e)
    {
        if (m_Physics3D) m_Physics3D->RebuildJoint(e);
        if (m_Physics2D) m_Physics2D->RebuildJoint(e);
    }

    void Scene::RefreshBody(Entity e)
    {
        if (!IsValid(e)) return;
        if (m_Physics3D) m_Physics3D->RefreshBody(e);
        if (m_Physics2D && m_Registry.Has<Rigidbody2DComponent>(e)) m_Physics2D->RefreshBody(e);
    }

    void Scene::MoveBody(Entity e, const glm::vec3& position, const glm::vec3& rotation, bool teleport)
    {
        if (!IsValid(e)) return;
        if (m_Registry.Has<Rigidbody2DComponent>(e))
        {
            if (m_Physics2D) m_Physics2D->MoveBody(e, position, rotation.z, teleport);
            else
            {
                auto& t = m_Registry.Get<TransformComponent>(e);
                t.Position = glm::vec3(position.x, position.y, t.Position.z);
                t.Rotation.z = rotation.z;
            }
            return;
        }
        if (m_Physics3D) m_Physics3D->MoveBody(e, position, rotation, teleport);
        else
        {
            auto& t = m_Registry.Get<TransformComponent>(e);
            t.Position = position;
            t.Rotation = rotation;
        }
    }

    void Scene::SetPrimaryCamera(Entity camera)
    {
        if (!IsValid(camera) || !m_Registry.Has<CameraComponent>(camera)) return;
        for (auto [e, cam] : m_Registry.ViewOf<CameraComponent>()) cam.Primary = e == camera;
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
        for (auto& inst : created) inst->OnBeforeCreate();
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
        for (auto& inst : created) inst->OnBeforeCreate();
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
        m_FixedTimestep = kFixedTimestep;
        m_FixedTime = 0.0f;
        m_MaxDeltaTime = 0.1f;
        m_LevelLoadTime = 0.0f;
        if (!m_WindowListener)
            m_WindowListener = AppRuntime::AddWindowListener([this](bool focused, bool focusChanged, bool minimized, bool minimizedChanged) {
                std::vector<Entity> scripted;
                for (auto [e, sc] : m_Registry.ViewOf<ScriptComponent>()) scripted.push_back(e);
                for (Entity e : scripted)
                {
                    auto* sc = IsValid(e) ? m_Registry.TryGet<ScriptComponent>(e) : nullptr;
                    if (!sc) continue;
                    for (auto& inst : sc->LiveInstances(true))
                    {
                        if (minimizedChanged) inst->OnApplicationPause(minimized);
                        if (focusChanged) inst->OnApplicationFocus(focused);
                    }
                }
            });
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
            m_LevelLoadTime = m_Time;
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
        AssetManager::ReleaseRuntimeMaterials(this); // the materials scripts made while it played
        AssetManager::ReleaseRuntimeTextures(this);  // and their textures
        if (m_WindowListener)
        {
            AppRuntime::RemoveWindowListener(m_WindowListener);
            m_WindowListener = 0;
        }
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
        Video::OnRuntimeStop(*this);
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
            dt = m_FixedTimestep;
        }
        dt = std::min(dt, m_MaxDeltaTime); // avoid spiral of death after hitches
        DebugDraw::Tick(dt);     // the last update's debug lines go (timed ones count down)
        const float unscaledDt = dt;
        m_UnscaledDeltaTime = dt;
        m_UnscaledTime += dt;
        dt *= m_TimeScale;
        m_Time += dt;
        m_DeltaTime = dt;
        ++m_FrameCount;

        Input::SetKeyboardCapturedByUI(false); // the UI reads the keyboard first
        if (m_UIInputValid || !m_UIRays.empty()) // script pointer rays work without a mouse (VR)
        {
            UI::PointerState pointer{ m_UIPointer, m_UIDown, m_UIDown && !m_UIWasDown, !m_UIDown && m_UIWasDown, m_UIScroll };
            pointer.Text = Input::GetTypedText() + m_UIInjectedText;
            pointer.Composition = Input::GetComposition();
            pointer.CompositionCursor = Input::GetCompositionCursor();
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
            // Keyboard / gamepad navigation goes to the canvas holding the selection: a World Space panel's own when it
            // is on one, the screen's otherwise.
            Entity navCanvas = CanvasOf(m_UISelected);
            if (!navCanvas.IsNull() && m_Registry.Get<CanvasComponent>(navCanvas).RenderMode != CanvasRenderMode::WorldSpace)
                navCanvas = Entity::Null();
            UI::UpdateListViews(*this); // list rows for the items in view
            UI::PullBindings(*this); // script values into bound widgets
            UI::PointerState screenPointer = pointer;
            if (!navCanvas.IsNull())
                screenPointer.NavUp = screenPointer.NavDown = screenPointer.NavLeft = screenPointer.NavRight = screenPointer.Submit =
                    screenPointer.Cancel = false;
            m_UIHovered = UI::Update(*this, screenPointer, m_UIScreen, unscaledDt); // menus work while the game is frozen
            // The focused field of a screen canvas tells the input method where its caret is.
            for (auto [fe, field] : m_Registry.ViewOf<UIInputFieldComponent>())
                if (field.Focused && field.CaretRect.z > 0.0f)
                {
                    Entity canvas = CanvasOf(fe);
                    if (canvas.IsNull() || m_Registry.Get<CanvasComponent>(canvas).RenderMode != CanvasRenderMode::WorldSpace)
                        Input::SetTextInputRect(field.CaretRect);
                }
            // World Space canvases: the pointer where the main camera's ray meets each panel, in its pixels (keys and
            // navigation stay with the screen's UI).
            std::vector<Entity> worldCanvases;
            for (auto [ce, canvas, ct] : m_Registry.ViewOf<CanvasComponent, TransformComponent>())
                if (ct.ActiveInHierarchy && canvas.RenderMode == CanvasRenderMode::WorldSpace) worldCanvases.push_back(ce);
            for (auto& [id, r] : m_UIRays)
            {
                r.Pressed = r.Down && !r.WasDown;
                r.Released = !r.Down && r.WasDown;
            }
            for (Entity ce : worldCanvases)
            {
                if (!IsValid(ce)) continue;
                glm::vec2 size = m_Registry.Get<CanvasComponent>(ce).PixelSize;
                bool navHere = ce == navCanvas;
                Entity selectedBefore = m_UISelected;
                // The pointers: each script ray with its own button, else the mouse through the camera. Keys and
                // navigation go with the first; later ones add their hovers to it.
                std::vector<std::pair<int, const UIPointerRay*>> sources;
                for (auto& [id, r] : m_UIRays) sources.emplace_back(id + 1, &r);
                if (sources.empty()) sources.emplace_back(0, nullptr);
                for (size_t i = 0; i < sources.size(); ++i)
                {
                    const UIPointerRay* ray = sources[i].second;
                    glm::vec2 pixel(-1e6f);
                    bool hit = ray ? UI::WorldCanvasRayPoint(*this, ce, ray->Origin, ray->Direction, pixel)
                                   : m_UIHovered.IsNull() && WorldCanvasPointer(ce, pixel);
                    UI::PointerState p = pointer;
                    if (ray)
                    {
                        p.Down = ray->Down;
                        p.Pressed = ray->Pressed;
                        p.Released = ray->Released;
                    }
                    p.Id = sources[i].first;
                    p.KeepHover = i > 0;
                    p.Position = hit ? pixel : glm::vec2(-1e6f);
                    p.Pressed = p.Pressed && hit;
                    p.Scroll = hit && !ray ? pointer.Scroll : 0.0f;
                    if (!navHere || i > 0) p.NavUp = p.NavDown = p.NavLeft = p.NavRight = p.Submit = p.Cancel = false;
                    if (i > 0)
                    {
                        p.Text.clear();
                        p.Backspace = p.Enter = p.Delete = p.Left = p.Right = p.LineUp = p.LineDown = p.Home = p.End = p.Tab = false;
                        p.SelectAll = p.Copy = p.Cut = p.Paste = false;
                    }
                    Entity over = UI::Update(*this, p, size, unscaledDt, ce);
                    if (m_UIHovered.IsNull() && hit) m_UIHovered = over;
                }
                // Nothing further this way on the panel: on to the nearest other World Space panel that way on screen.
                glm::vec2 dir = !navHere ? glm::vec2(0) : pointer.NavLeft ? glm::vec2(-1, 0) : pointer.NavRight ? glm::vec2(1, 0)
                                : pointer.NavUp ? glm::vec2(0, -1) : pointer.NavDown ? glm::vec2(0, 1) : glm::vec2(0);
                bool stays = navHere && dir != glm::vec2(0.0f) && m_UISelected == selectedBefore && !m_UISelected.IsNull();
                if (stays && dir.x != 0.0f && m_Registry.Has<UISliderComponent>(m_UISelected)) stays = false; // it stepped
                if (stays)
                    if (auto* dd = m_Registry.TryGet<UIDropdownComponent>(m_UISelected); dd && dd->Open) stays = false;
                if (stays)
                    for (auto [fe, field] : m_Registry.ViewOf<UIInputFieldComponent>()) stays &= !field.Focused;
                Entity camera = GetPrimaryCamera();
                if (stays && !camera.IsNull())
                {
                    glm::vec2 view = m_UIScreen.x > 0.0f ? m_UIScreen : glm::vec2(1280, 720);
                    auto onScreen = [&](Entity c, glm::vec2& at) {
                        glm::vec3 s = CameraProjection::WorldToScreen(*this, camera, glm::vec3(m_Registry.Get<TransformComponent>(c).World[3]), view);
                        at = glm::vec2(s);
                        return s.z > 0.0f; // in front of the camera
                    };
                    glm::vec2 from;
                    onScreen(ce, from);
                    Entity best = Entity::Null(), entry = Entity::Null();
                    float bestScore = 1e30f;
                    for (Entity other : worldCanvases)
                    {
                        glm::vec2 at;
                        if (other == ce || !IsValid(other) || !onScreen(other, at)) continue;
                        glm::vec2 d = at - from;
                        float along = glm::dot(d, dir), across = std::abs(d.x * dir.y - d.y * dir.x);
                        if (along <= 1.0f || across > along * 2.0f) continue;
                        float score = along + across * 2.0f;
                        if (score >= bestScore) continue;
                        if (Entity e = UI::NavigationEntry(*this, other, dir); !e.IsNull()) { bestScore = score; best = other; entry = e; }
                    }
                    if (!best.IsNull())
                    {
                        m_UISelected = entry;
                        navCanvas = Entity::Null(); // this frame's move is used up
                    }
                }
            }
            for (auto& [id, r] : m_UIRays) r.WasDown = r.Down;
            UI::PushBindings(*this); // widgets the player changed into their bound fields
            UpdatePointerEvents(m_UIPointer, m_UIDown, m_UIScroll);
            // A focused text field keeps the keys from gameplay (WASD while typing a name must not move the player).
            bool typing = false;
            for (auto [e, field] : m_Registry.ViewOf<UIInputFieldComponent>()) typing |= field.Focused;
            Input::SetKeyboardCapturedByUI(typing);
            m_UIScroll = 0.0f;
            m_UIWasDown = m_UIDown;
        }
        UpdateMouseEvents();

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
        m_FixedStepsThisFrame = 0;
        while (m_FixedAccumulator >= m_FixedTimestep)
        {
            DIM_PROFILE_SCOPE("Fixed Update");
            m_Registry.ViewOf<ScriptComponent>().Each([&](Entity e, ScriptComponent& sc) {
                if (!m_Registry.Get<TransformComponent>(e).ActiveInHierarchy) return;
                for (auto& inst : sc.LiveInstances(true)) inst->OnFixedUpdate(m_FixedTimestep);
            });
            for (auto& sys : m_Systems) sys->OnFixedUpdate(*this, m_FixedTimestep);
            UpdateTransforms();
            {
                DIM_PROFILE_SCOPE("Physics");
                m_Physics2D->Step(m_FixedTimestep);
                m_Physics3D->Step(m_FixedTimestep);
            }
            m_FixedAccumulator -= m_FixedTimestep;
            m_FixedTime += m_FixedTimestep;
            ++m_FixedStepsThisFrame;
            if (s_CoroutinePhaseHook) s_CoroutinePhaseHook(*this, 0); // WaitForFixedUpdate
        }
        // Interpolating bodies: their objects shown between the last two steps, as far along as the time is.
        {
            float alpha = m_FixedAccumulator / std::max(m_FixedTimestep, 1e-6f);
            m_Physics3D->Interpolate(alpha);
            m_Physics2D->Interpolate(alpha);
        }

        Timeline::Update(*this, dt);
        UpdateTransforms();
        Navigation::FollowPlatforms(*this); // agents on Moving surfaces, where physics took them
        {
            DIM_PROFILE_SCOPE("Animation");
            UpdateAnimators(dt);
        }
        {
            // LateUpdate (as Unity's): after animation, so scripts see this frame's poses (bones, root motion).
            DIM_PROFILE_SCOPE("Late Update");
            std::vector<Entity> late;
            for (auto [e, sc] : m_Registry.ViewOf<ScriptComponent>())
                for (auto& s : sc.Scripts)
                    if (s.Instance && s.Enabled && s.Instance->HandlesLateUpdate()) { late.push_back(e); break; }
            for (Entity e : late)
            {
                if (!IsValid(e) || !m_Registry.Get<TransformComponent>(e).ActiveInHierarchy) continue;
                if (auto* sc = m_Registry.TryGet<ScriptComponent>(e))
                    for (auto& inst : sc->LiveInstances(true))
                        if (inst->HandlesLateUpdate()) inst->OnLateUpdate(dt);
            }
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
        {
            DIM_PROFILE_SCOPE("Video");
            Video::Update(*this, m_UnscaledDeltaTime);
        }
        if (s_CoroutinePhaseHook) s_CoroutinePhaseHook(*this, 1); // WaitForEndOfFrame
        FlushDeferredDestroys();
    }

    namespace
    {
        // Shows the animator's frame for its Time (a run that does not loop stops on its last frame).
        void ShowSpriteFrame(SpriteAnimatorComponent& anim, SpriteRendererComponent& sprite)
        {
            if (anim.FrameCount <= 0) return;
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

    void Scene::UpdateSpriteAnimators(float dt)
    {
        for (auto [e, anim, sprite] : m_Registry.ViewOf<SpriteAnimatorComponent, SpriteRendererComponent>())
        {
            if (!anim.Playing || anim.FrameCount <= 0) continue;
            anim.Time += dt;
            ShowSpriteFrame(anim, sprite);
        }
    }

    void Scene::SetSpriteAnimatorTime(Entity entity, float seconds)
    {
        auto* anim = m_Registry.TryGet<SpriteAnimatorComponent>(entity);
        if (!anim) return;
        anim->Time = std::max(seconds, 0.0f);
        if (auto* sprite = m_Registry.TryGet<SpriteRendererComponent>(entity)) ShowSpriteFrame(*anim, *sprite);
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
        // The clips an Animator plays: its mesh's own, or its Animation Source's retargeted onto the mesh's skeleton, and
        // the .anim clips made for that model (one named as a model clip replaces it). Cached per source and target
        // mesh and retargeting, and made again when either is reloaded or any .anim changes.
        const std::vector<AnimationClip>& AnimatorClips(const AnimatorComponent& anim, AssetHandle meshHandle, const Ref<Mesh>& mesh)
        {
            Ref<Mesh> source = anim.ClipSource.IsValid() ? AssetManager::GetMesh(anim.ClipSource) : nullptr;
            if (source && (!source->GetSkeleton() || source == mesh)) source = nullptr;
            const std::vector<AssetHandle>& extra = AssetManager::ClipsFor(source ? anim.ClipSource : meshHandle);
            if (!source && extra.empty()) return mesh->GetClips();
            struct Entry
            {
                std::weak_ptr<Mesh> Source, Target;
                uint64_t Version = 0;
                std::vector<AnimationClip> Clips;
            };
            static std::map<std::tuple<const Mesh*, const Mesh*, AnimatorRetargeting>, Entry> cache;
            Entry& entry = cache[{ source.get(), mesh.get(), anim.Retargeting }];
            uint64_t version = AssetManager::ClipsVersion();
            if ((source && entry.Source.lock() != source) || entry.Target.lock() != mesh || entry.Version != version)
            {
                entry.Source = source;
                entry.Target = mesh;
                entry.Version = version;
                entry.Clips.clear();
                const Skeleton* from = source ? source->GetSkeleton() : mesh->GetSkeleton();
                auto add = [&](const AnimationClip& clip) {
                    AnimationClip made = !source ? clip
                                         : anim.Retargeting == AnimatorRetargeting::Humanoid
                                             ? Animation::RetargetHumanoid(*source->GetSkeleton(), *mesh->GetSkeleton(), clip)
                                             : Animation::RetargetClip(*source->GetSkeleton(), *mesh->GetSkeleton(), clip);
                    auto same = std::find_if(entry.Clips.begin(), entry.Clips.end(), [&](const AnimationClip& c) { return c.Name == made.Name; });
                    if (same != entry.Clips.end()) *same = std::move(made);
                    else entry.Clips.push_back(std::move(made));
                };
                for (const AnimationClip& clip : (source ? source : mesh)->GetClips()) add(clip);
                if (from)
                    for (AssetHandle h : extra)
                        if (auto asset = AssetManager::GetAnimationClip(h)) add(asset->ToClip(*from));
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

    void Scene::UpdateAnimators(float frameDt, Entity only)
    {
        Pose current, previous;
        std::vector<std::pair<Entity, std::vector<std::string>>> firedEvents;
        std::vector<Entity> animatorMoves; // objects whose scripts take the root motion (OnAnimatorMove)
        // State behaviour calls, sent after every Animator has updated.
        struct StateCall
        {
            Entity E;
            int Kind, Layer, State;
            std::string Name;
            AnimatorStateInfo Info;
            std::vector<std::string> Behaviours;
        };
        std::vector<StateCall> stateCalls;

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
            if (!only.IsNull() && e != only) continue;
            // This Animator's step: by its update mode while playing (Animator.Update gives its own; Manual animators
            // only pose at their time).
            float dt = frameDt;
            if (only.IsNull() && m_Running)
            {
                if (anim.UpdateMode == AnimatorUpdateMode::UnscaledTime) dt = m_UnscaledDeltaTime;
                else if (anim.UpdateMode == AnimatorUpdateMode::AnimatePhysics) dt = (float)m_FixedStepsThisFrame * m_FixedTimestep;
                else if (anim.UpdateMode == AnimatorUpdateMode::Manual) dt = 0.0f;
            }
            Ref<Mesh> mesh = AssetManager::GetMesh(mr.Mesh);
            const Skeleton* skeleton = mesh ? mesh->GetSkeleton() : nullptr;
            if (!skeleton)
            {
                anim.SkinMatrices.clear();
                continue;
            }
            // Cull Completely: nothing runs while the object's bounds (grown for poses past the bind pose) are outside
            // the main camera's view; Animator.Update still runs it.
            anim.Culled = false;
            if (m_Running && only.IsNull() && anim.CullingMode == AnimatorCullingMode::CullCompletely && !anim.SkinMatrices.empty())
            {
                Entity camera = GetPrimaryCamera();
                if (!camera.IsNull())
                {
                    glm::mat4 vp = CameraProjection::Projection(*this, camera, GetViewSize()) * CameraProjection::View(*this, camera);
                    const AABB& b = mesh->GetBounds();
                    glm::vec3 c = b.Center(), ext = b.Extents() * 1.5f + glm::vec3(0.1f);
                    glm::mat4 mvp = vp * m_Registry.Get<TransformComponent>(e).World;
                    int outside[6] = {};
                    for (int k = 0; k < 8; ++k)
                    {
                        glm::vec3 corner = c + ext * glm::vec3(k & 1 ? 1 : -1, k & 2 ? 1 : -1, k & 4 ? 1 : -1);
                        glm::vec4 p = mvp * glm::vec4(corner, 1.0f);
                        outside[0] += p.x < -p.w; outside[1] += p.x > p.w;
                        outside[2] += p.y < -p.w; outside[3] += p.y > p.w;
                        outside[4] += p.z < 0.0f;  outside[5] += p.z > p.w;
                    }
                    for (int k = 0; k < 6; ++k) anim.Culled |= outside[k] == 8;
                    if (anim.Culled) continue;
                }
            }
            const auto& clips = AnimatorClips(anim, mr.Mesh, mesh);
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
            auto evaluate = [&](auto& pb, const AnimatorController* machine, bool baseLayer, Pose& out, int layerIndex) {
                // Behaviour calls of this layer's states: queued here, their info filled in at the end.
                size_t callsBefore = stateCalls.size();
                auto queueCall = [&](int kind, int index) {
                    if (!m_Running || !machine || index < 0 || index >= (int)machine->States.size()) return;
                    const AnimState& st = machine->States[index];
                    if (!st.Behaviours.empty()) stateCalls.push_back({ e, kind, layerIndex, index, st.Name, {}, st.Behaviours });
                };
                if (machine)
                {
                    AnimatorStateMachine::Update(*machine, pb.Runtime, dt * anim.Speed, duration, [&](const AnimState& state, float fade) {
                        int index = (int)(&state - machine->States.data());
                        bool isTree = state.Motion != AnimMotionType::Clip;
                        bool hasCurrent = !pb.Clip.empty() || pb.TreeState >= 0;
                        // Leaving: a state still fading out is left now; the current one when its fade out ends (now
                        // without a fade).
                        if (pb.FadeDuration > 0.0f && pb.FadeElapsed < pb.FadeDuration) queueCall(2, pb.PreviousStateIndex);
                        if (!(fade > 0.0f && hasCurrent)) queueCall(2, pb.StateIndex);
                        queueCall(0, index);
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
                        pb.Time = isTree ? 0.0f : pb.Runtime.StateTime; // 0, or where Animator.Play started it
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
                if (pb.StartNormalized >= 0.0f && !machine && clip)
                {
                    pb.Time = pb.StartNormalized * clip->Duration;
                    if (pb.Loop && clip->Duration > 0.0f) pb.Time = std::fmod(pb.Time, clip->Duration);
                    pb.StartNormalized = -1.0f;
                }

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
                // How far the state has played, for scripts (Animator.NormalizedTime).
                pb.NormalizedTime = machine && pb.Runtime.State >= 0 ? AnimatorStateMachine::NormalizedTime(*machine, pb.Runtime, duration)
                                  : clip && clip->Duration > 0.0f ? pb.Time / clip->Duration : 0.0f;
                // State info for scripts: the current state, and while fading the one fading out (as of this update).
                {
                    const AnimState* st = machine && pb.Runtime.State >= 0 && pb.Runtime.State < (int)machine->States.size() ? &machine->States[pb.Runtime.State] : nullptr;
                    pb.Info = {};
                    pb.Info.Valid = st || clip;
                    pb.Info.NormalizedTime = pb.NormalizedTime;
                    pb.Info.Length = st ? AnimatorStateMachine::StateDuration(*machine, pb.Runtime, *st, duration) : clip ? clip->Duration : 0.0f;
                    pb.Info.Loop = st ? st->Loop : pb.Loop;
                    pb.Info.Speed = st ? st->Speed : 1.0f;
                    if (st) pb.Info.Tag = st->Tag;
                    pb.PreviousInfo = {};
                    if (pb.FadeDuration > 0.0f && pb.FadeElapsed < pb.FadeDuration)
                    {
                        const AnimState* prevState = treeOf(pb.PreviousStateIndex);
                        const AnimState* prevTree = treeOf(pb.PreviousTreeState);
                        const AnimationClip* prevClip = prevTree ? nullptr : Animation::FindClip(clips, pb.PreviousClip);
                        float length = prevTree ? AnimatorStateMachine::StateDuration(*machine, pb.Runtime, *prevTree, duration) : prevClip ? prevClip->Duration : 0.0f;
                        pb.PreviousInfo.Valid = prevTree || prevClip;
                        pb.PreviousInfo.Length = length;
                        pb.PreviousInfo.NormalizedTime = prevTree ? pb.PreviousTime : length > 0.0f ? pb.PreviousTime / length : 0.0f;
                        pb.PreviousInfo.Loop = prevState ? prevState->Loop : true;
                        pb.PreviousInfo.Speed = prevState ? prevState->Speed : 1.0f;
                        if (prevState) pb.PreviousInfo.Tag = prevState->Tag;
                    }
                }

                if (pb.FadeDuration > 0.0f && pb.FadeElapsed < pb.FadeDuration)
                {
                    pb.FadeElapsed += dt;
                    queueCall(pb.FadeElapsed >= pb.FadeDuration ? 2 : 1, pb.PreviousStateIndex); // its last frame: left
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
                // Updated every frame after the one it was entered in (as Unity's OnStateUpdate).
                bool enteredNow = false;
                for (size_t i = callsBefore; i < stateCalls.size(); ++i)
                    enteredNow |= stateCalls[i].Kind == 0 && stateCalls[i].State == pb.Runtime.State;
                if (!enteredNow) queueCall(1, pb.Runtime.State >= 0 && machine ? pb.Runtime.State : -1);
                // Each queued call gets its state's info as of this update (the state fading out: the earlier info).
                for (size_t i = callsBefore; i < stateCalls.size(); ++i)
                {
                    StateCall& call = stateCalls[i];
                    call.Info = call.State == pb.Runtime.State ? pb.Info : pb.PreviousInfo;
                    if (!call.Info.Valid)
                    {
                        const AnimState& st = machine->States[call.State];
                        call.Info.Loop = st.Loop;
                        call.Info.Speed = st.Speed;
                        call.Info.Tag = st.Tag;
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
            evaluate(anim, controller.get(), true, current, 0);

            // Root motion from the base layer (play mode only). A script with OnAnimatorMove takes it instead.
            anim.RootMotionDelta = glm::vec3(0.0f);
            anim.RootMotionRotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
            anim.ScriptRootMotion = false;
            if (auto* sc = m_Running ? m_Registry.TryGet<ScriptComponent>(e) : nullptr)
                for (auto& s : sc->Scripts) anim.ScriptRootMotion |= s.Instance && s.Enabled && s.Instance->HandlesAnimatorMove();
            if (anim.ScriptRootMotion) animatorMoves.push_back(e);
            if ((anim.ApplyRootMotion || anim.ScriptRootMotion) && dt > 0.0f && !current.Translations.empty())
            {
                int root = anim.RootMotionJoint.empty() ? (skeleton->Order.empty() ? 0 : skeleton->Order.front()) : skeleton->FindJoint(anim.RootMotionJoint);
                if (root >= 0 && root < (int)skeleton->Joints.size())
                {
                    glm::vec3 start = skeleton->Joints[root].Translation, end = start;
                    glm::quat startRot = skeleton->Joints[root].Rotation, endRot = startRot;
                    const AnimationClip* baseClip = anim.TreeState < 0 ? Animation::FindClip(clips, anim.Clip) : nullptr;
                    if (!baseClip && anim.TreeState < 0 && anim.Clip.empty() && !clips.empty()) baseClip = &clips.front();
                    float time = anim.TreeState >= 0 ? anim.Runtime.StateTime : anim.Time;
                    if (baseClip)
                    {
                        Pose p;
                        Animation::Sample(*skeleton, *baseClip, 0.0f, p);
                        start = p.Translations[root];
                        startRot = p.Rotations[root];
                        Animation::Sample(*skeleton, *baseClip, baseClip->Duration, p);
                        end = p.Translations[root];
                        endRot = p.Rotations[root];
                    }
                    std::string key = anim.TreeState >= 0 ? "#" + std::to_string(anim.TreeState) : anim.Clip;
                    float yaw = 0.0f;
                    glm::vec3 modelDelta = anim.ApplyRootRotation
                        ? Animation::ExtractRootMotion(*skeleton, root, current, anim.RootMotion, start, end, startRot, endRot, time, anim.Loop, key, yaw)
                        : Animation::ExtractRootMotion(*skeleton, root, current, anim.RootMotion, start, end, time, anim.Loop, key);
                    auto& t = m_Registry.Get<TransformComponent>(e);
                    glm::vec3 worldDelta = glm::mat3(t.World) * modelDelta;
                    anim.RootMotionDelta = worldDelta;
                    // The turn about the object's own up axis, in world space.
                    glm::vec3 up = glm::mat3(t.World) * glm::vec3(0, 1, 0);
                    if (yaw != 0.0f && glm::dot(up, up) > 1e-12f) anim.RootMotionRotation = glm::angleAxis(yaw, glm::normalize(up));
                    if (!anim.ScriptRootMotion) ApplyRootMotion(e, worldDelta, anim.RootMotionRotation);
                }
            }

            // MatchTarget: over its window, a share of the gap between the joint and the target each frame (the share
            // of the window's remaining time this frame took), all of it at the end.
            if (anim.Match.Active && m_Running)
            {
                AnimatorMatchTarget& m = anim.Match;
                int joint = skeleton->FindJoint(m.Joint);
                float tNow = anim.NormalizedTime;
                if (joint < 0 || anim.Runtime.State != m.State || (controller == nullptr && m.State >= 0)) m.Active = false;
                else if (tNow >= m.Start)
                {
                    float from = std::max(m.LastTime < 0.0f ? m.Start : m.LastTime, m.Start);
                    float share = tNow >= m.End ? 1.0f : (m.End - from > 1e-6f ? std::clamp((tNow - from) / (m.End - from), 0.0f, 1.0f) : 1.0f);
                    std::vector<glm::mat4> globals;
                    Animation::JointGlobals(*skeleton, current, globals);
                    auto& t = m_Registry.Get<TransformComponent>(e);
                    glm::vec3 at = glm::vec3(t.World * globals[joint][3]);
                    glm::vec3 gap = (m.Position - at) * m.Weight;
                    ApplyRootMotion(e, gap * share);
                    m.LastTime = tNow;
                    if (tNow >= m.End) m.Active = false;
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
                        evaluate(pb, &layer.Machine, false, layerPose, (int)li + 1);
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
        if (s_StateBehaviourHook)
            for (auto& call : stateCalls)
                if (IsValid(call.E)) s_StateBehaviourHook(*this, call.E, call.Kind, call.Layer, call.State, call.Name, call.Info, call.Behaviours);
        for (Entity e : animatorMoves)
            if (auto* sc = IsValid(e) ? m_Registry.TryGet<ScriptComponent>(e) : nullptr)
                for (auto& inst : sc->LiveInstances(true))
                    if (inst->HandlesAnimatorMove()) inst->OnAnimatorMove();
    }

    void Scene::ApplyRootMotion(Entity e, const glm::vec3& worldDelta, const glm::quat& worldTurn)
    {
        if (!IsValid(e)) return;
        Entity parent = GetParent(e);
        if (std::abs(worldTurn.w) < 1.0f - 1e-7f)
        {
            // Turn: the world rotation becomes worldTurn * world, kept in the parent's space.
            auto& t = m_Registry.Get<TransformComponent>(e);
            glm::quat parentRot(1.0f, 0.0f, 0.0f, 0.0f);
            if (!parent.IsNull())
            {
                glm::mat3 pm = glm::mat3(m_Registry.Get<TransformComponent>(parent).World);
                for (int c = 0; c < 3; ++c)
                    if (float len = glm::length(pm[c]); len > 1e-8f) pm[c] /= len;
                parentRot = glm::normalize(glm::quat_cast(pm));
            }
            t.Rotation = glm::eulerAngles(glm::normalize(glm::inverse(parentRot) * worldTurn * parentRot * t.GetRotationQuat()));
        }
        if (glm::dot(worldDelta, worldDelta) <= 0.0f) return;
        if (m_Registry.Has<CharacterControllerComponent>(e) && m_Physics3D)
        {
            Physics3DWorld::MoveCharacter(*this, e, worldDelta);
            return;
        }
        auto& t = m_Registry.Get<TransformComponent>(e);
        glm::mat3 toParent = parent.IsNull() ? glm::mat3(1.0f) : glm::inverse(glm::mat3(m_Registry.Get<TransformComponent>(parent).World));
        t.Position += toParent * worldDelta;
        t.World[3] += glm::vec4(worldDelta, 0.0f); // read before transforms update next (NavMesh Agents with Root Motion)
    }

    float Scene::SampleLightProbes(const glm::vec3& position, std::array<glm::vec3, 9>& out)
    {
        out.fill(glm::vec3(0.0f));
        const LightProbeVolumeComponent* best = nullptr;
        glm::vec3 bestCenter(0.0f);
        float bestVolume = 0.0f;
        for (auto [e, lpv, t] : m_Registry.ViewOf<LightProbeVolumeComponent, TransformComponent>())
        {
            if (!t.ActiveInHierarchy || !lpv.IsBaked()) continue;
            glm::vec3 center = t.GetWorldPosition(), half = glm::abs(lpv.Size) * 0.5f;
            glm::vec3 d = glm::abs(position - center);
            if (d.x > half.x || d.y > half.y || d.z > half.z) continue;
            float volume = half.x * half.y * half.z;
            if (!best || volume < bestVolume) { best = &lpv; bestCenter = center; bestVolume = volume; }
        }
        if (!best) return 0.0f;
        const LightProbeVolumeComponent& v = *best;
        glm::ivec3 n(std::max(v.ProbesX, 1), std::max(v.ProbesY, 1), std::max(v.ProbesZ, 1));
        glm::vec3 size = glm::max(glm::abs(v.Size), glm::vec3(1e-4f));
        // Grid coordinates (probes sit at the box's corners and evenly between; one probe on an axis sits in the middle).
        glm::vec3 g = (position - bestCenter) / size + 0.5f;
        glm::ivec3 i0(0), i1(0);
        glm::vec3 f(0.0f);
        for (int a = 0; a < 3; ++a)
        {
            if (n[a] <= 1) continue;
            float x = std::clamp(g[a], 0.0f, 1.0f) * (float)(n[a] - 1);
            i0[a] = std::min((int)std::floor(x), n[a] - 2);
            i1[a] = i0[a] + 1;
            f[a] = x - (float)i0[a];
        }
        for (int corner = 0; corner < 8; ++corner)
        {
            glm::ivec3 c(corner & 1 ? i1.x : i0.x, corner & 2 ? i1.y : i0.y, corner & 4 ? i1.z : i0.z);
            float w = (corner & 1 ? f.x : 1.0f - f.x) * (corner & 2 ? f.y : 1.0f - f.y) * (corner & 4 ? f.z : 1.0f - f.z);
            if (w <= 0.0f) continue;
            size_t probe = (size_t)(c.x + c.y * n.x + c.z * n.x * n.y);
            for (int k = 0; k < 9; ++k) out[k] += v.Baked[probe * 9 + k] * w;
        }
        for (auto& coefficient : out) coefficient *= v.Intensity;
        glm::vec3 half = glm::abs(v.Size) * 0.5f, inside = half - glm::abs(position - bestCenter);
        float edge = std::min(inside.x, std::min(inside.y, inside.z));
        return v.BlendDistance > 0.0f ? std::clamp(edge / v.BlendDistance, 0.0f, 1.0f) : 1.0f;
    }

    void Scene::RebindAnimator(Entity e)
    {
        auto* anim = IsValid(e) ? m_Registry.TryGet<AnimatorComponent>(e) : nullptr;
        if (!anim) return;
        anim->Runtime = {};
        anim->Layers.clear();
        anim->Time = 0.0f;
        anim->TreeState = anim->PreviousTreeState = -1;
        anim->StateIndex = anim->PreviousStateIndex = -1;
        anim->PreviousClip.clear();
        anim->FadeElapsed = anim->FadeDuration = 0.0f;
        anim->NormalizedTime = 0.0f;
        anim->CurrentState.clear();
        anim->StateSpeed = 1.0f;
        anim->RootMotion = {};
        anim->Match = {};
        anim->Info = anim->PreviousInfo = {};
        // With a controller the machine enters its default state at the next update and chooses the clip.
        if (AssetManager::GetAnimatorController(anim->Controller)) anim->Clip.clear();
    }

    bool Scene::PlayAnimatorState(Entity e, const std::string& name, int layer, float fadeSeconds, float offset, bool fixedTime)
    {
        auto* anim = IsValid(e) ? m_Registry.TryGet<AnimatorComponent>(e) : nullptr;
        if (!anim) return false;
        Ref<AnimatorController> controller = AssetManager::GetAnimatorController(anim->Controller);
        if (controller)
        {
            controller->LinkLayers();
            const AnimatorController* machine = controller->LayerMachine(layer);
            int index = machine ? machine->FindState(name) : -1;
            if (index < 0) return false;
            AnimatorRuntime* runtime = &anim->Runtime;
            if (layer > 0)
            {
                if ((int)anim->Layers.size() < layer) anim->Layers.resize(layer);
                runtime = &anim->Layers[layer - 1].Runtime;
            }
            runtime->RequestedState = index;
            runtime->RequestedFade = fadeSeconds;
            runtime->RequestedOffset = offset;
            runtime->RequestedFixedTime = fixedTime;
            return true;
        }
        if (layer != 0 || name.empty()) return false;
        // No controller: a clip of the model, from the offset (seconds become a fraction once the clip is known).
        anim->CrossFade(name, fadeSeconds);
        anim->Time = fixedTime ? std::max(offset, 0.0f) : 0.0f;
        anim->StartNormalized = fixedTime ? -1.0f : std::max(offset, 0.0f);
        return true;
    }

    AssetHandle Scene::GetRendererMaterialInstance(Entity e)
    {
        auto* mr = IsValid(e) ? m_Registry.TryGet<MeshRendererComponent>(e) : nullptr;
        if (!mr) return AssetHandle::None();
        if (mr->InstanceMaterial.IsValid() && mr->Material == mr->InstanceMaterial && AssetManager::IsRuntimeMaterial(mr->InstanceMaterial))
            return mr->InstanceMaterial;
        Material copy;
        if (auto m = AssetManager::GetMaterial(mr->Material)) copy = *m;
        else
        {
            // Its simple look as a material (as SceneRenderer draws it).
            copy.BaseColor = mr->Color;
            copy.BaseColorMap = mr->Texture;
            copy.Tiling = mr->Tiling;
            copy.Roughness = std::clamp(1.0f - mr->Smoothness, 0.02f, 1.0f);
            copy.EmissionColor = glm::vec3(mr->Color);
            copy.EmissionStrength = mr->Emission;
            copy.CastShadows = mr->CastShadows;
        }
        mr->InstanceMaterial = mr->Material = AssetManager::CreateRuntimeMaterial(copy, this);
        return mr->InstanceMaterial;
    }

    void Scene::NotifyApplicationQuit()
    {
        if (!m_Running) return;
        std::vector<Entity> scripted;
        for (auto [e, sc] : m_Registry.ViewOf<ScriptComponent>()) scripted.push_back(e);
        for (Entity e : scripted)
            if (auto* sc = IsValid(e) ? m_Registry.TryGet<ScriptComponent>(e) : nullptr)
                for (auto& inst : sc->LiveInstances(true)) inst->OnApplicationQuit();
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

    Entity Scene::PickObject(const glm::vec2& viewPoint)
    {
        Entity camera = GetPrimaryCamera();
        if (camera.IsNull()) return Entity::Null();
        glm::vec3 origin, direction;
        CameraProjection::ScreenPointToRay(*this, camera, viewPoint, GetViewSize(), origin, direction);
        float farClip = m_Registry.Get<CameraComponent>(camera).FarClip;
        Entity found = Entity::Null();
        float nearest = farClip;
        if (m_Physics3D)
            if (RaycastHit hit = Physics3DWorld::Raycast(*this, origin, direction, farClip); !hit.Hit.IsNull())
            {
                found = hit.Hit;
                nearest = hit.Distance;
            }
        // 2D colliders lie in the z = 0 plane.
        if (m_Physics2D && std::abs(direction.z) > 1e-6f)
        {
            float t = -origin.z / direction.z;
            if (t >= 0.0f && t < nearest)
            {
                glm::vec3 p = origin + direction * t;
                std::vector<Entity> under = Physics2DWorld::OverlapPoint(*this, { p.x, p.y });
                if (!under.empty()) found = under.front();
            }
        }
        return found;
    }

    void Scene::UpdateMouseEvents()
    {
        DIM_PROFILE_SCOPE("Mouse Events");
        Entity over = m_UIInputValid && m_UIHovered.IsNull() ? PickObject(m_UIPointer) : Entity::Null();
        // The scripts of an active object (copied first: callbacks may change the scene).
        auto send = [&](Entity e, auto&& call) {
            auto* sc = IsValid(e) && IsActiveInHierarchy(e) ? m_Registry.TryGet<ScriptComponent>(e) : nullptr;
            if (!sc) return;
            for (auto& inst : sc->LiveInstances(true)) call(*inst);
        };
        if (over != m_MouseOver)
        {
            Entity left = m_MouseOver;
            m_MouseOver = over;
            send(left, [](ScriptableEntity& s) { s.OnMouseExit(); });
            send(over, [](ScriptableEntity& s) { s.OnMouseEnter(); });
        }
        send(over, [](ScriptableEntity& s) { s.OnMouseOver(); });

        bool down = m_UIInputValid && m_UIDown;
        if (down && !m_MouseWasDown)
        {
            m_MousePressed = over;
            send(over, [](ScriptableEntity& s) { s.OnMouseDown(); });
        }
        else if (down)
            send(m_MousePressed, [](ScriptableEntity& s) { s.OnMouseDrag(); });
        if (!down && m_MouseWasDown && !m_MousePressed.IsNull())
        {
            Entity pressed = m_MousePressed;
            m_MousePressed = Entity::Null();
            send(pressed, [](ScriptableEntity& s) { s.OnMouseUp(); });
            if (pressed == over) send(pressed, [](ScriptableEntity& s) { s.OnMouseUpAsButton(); });
        }
        m_MouseWasDown = down;
    }

    Entity Scene::CanvasOf(Entity e)
    {
        for (; IsValid(e); e = GetParent(e))
            if (m_Registry.Has<CanvasComponent>(e)) return e;
        return Entity::Null();
    }

    bool Scene::WorldCanvasPointer(Entity canvas, glm::vec2& pixel)
    {
        return m_UIInputValid && UI::WorldCanvasPoint(*this, canvas, m_UIPointer, m_UIScreen, pixel);
    }

    std::vector<glm::vec2> Scene::UIPointerCursors(Entity canvas)
    {
        std::vector<glm::vec2> out;
        for (auto& [id, r] : m_UIRays)
        {
            glm::vec2 pixel;
            if (r.Cursor && UI::WorldCanvasRayPoint(*this, canvas, r.Origin, r.Direction, pixel)) out.push_back(pixel);
        }
        return out;
    }

    uint32_t Scene::PointerHandlers(Entity e)
    {
        if (!IsValid(e) || !IsActiveInHierarchy(e)) return 0;
        auto* sc = m_Registry.TryGet<ScriptComponent>(e);
        if (!sc) return 0;
        uint32_t mask = 0;
        for (auto& s : sc->Scripts)
            if (s.Instance && s.Enabled) mask |= s.Instance->PointerEventMask();
        return mask;
    }

    Entity Scene::PointerHandler(Entity from, PointerEventKind kind)
    {
        for (Entity e = from; IsValid(e); e = GetParent(e))
            if (PointerHandlers(e) & (1u << (int)kind)) return e;
        return Entity::Null();
    }

    void Scene::SendPointerEvent(Entity e, PointerEventKind kind, const PointerEventData& data)
    {
        if (!(PointerHandlers(e) & (1u << (int)kind))) return;
        for (auto& inst : m_Registry.Get<ScriptComponent>(e).LiveInstances(true))
            if (inst->PointerEventMask() & (1u << (int)kind)) inst->OnPointerEvent(kind, data);
    }

    Entity Scene::PointerEventTarget(const glm::vec2& point)
    {
        auto under = UI::ElementsAt(*this, point, m_UIScreen, [this](Entity e) { return PointerHandlers(e) != 0; });
        return under.empty() ? Entity::Null() : under.front();
    }

    // Unity's EventSystem pointer events (see PointerEventKind). Events go to the target element or bubble to the
    // nearest parent handling them; Enter / Exit go to each object the pointer came over or left (its parents too).
    void Scene::UpdatePointerEvents(glm::vec2 position, bool down, float scroll)
    {
        DIM_PROFILE_SCOPE("Pointer Events");
        bool any = false;
        for (auto [e, sc] : m_Registry.ViewOf<ScriptComponent>())
        {
            for (auto& s : sc.Scripts) any |= s.Instance && s.Instance->PointerEventMask() != 0;
            if (any) break;
        }
        if (!any)
        {
            m_Pointer = PointerTracking{};
            m_Pointer.Selected = m_UISelected;
            m_Pointer.WasDown = down;
            m_RayPointers.clear();
            return;
        }
        PointerEventsFor(m_Pointer, 0, nullptr, position, down, scroll);
        for (auto& [id, r] : m_UIRays) PointerEventsFor(m_RayPointers[id], id + 1, &r, glm::vec2(0.0f), r.Down, 0.0f);
        // A ray cleared: what it was over is left (and a press on it let go).
        for (auto it = m_RayPointers.begin(); it != m_RayPointers.end();)
        {
            if (m_UIRays.count(it->first)) { ++it; continue; }
            static const UIPointerRay gone{ glm::vec3(0.0f), glm::vec3(0.0f) };
            PointerEventsFor(it->second, it->first + 1, &gone, it->second.LastPosition, false, 0.0f);
            it = m_RayPointers.erase(it);
        }
        // Select / Deselect as the UI selection changes (clicks by any pointer, navigation, EventSystem.SetSelected).
        if (m_UISelected != m_Pointer.Selected)
        {
            Entity before = m_Pointer.Selected;
            m_Pointer.Selected = m_UISelected;
            SendPointerEvent(before, PointerEventKind::Deselect, m_PointerData);
            SendPointerEvent(m_Pointer.Selected, PointerEventKind::Select, m_PointerData);
        }
    }

    // One pointer's events: id 0 the mouse (screen canvases, then World Space ones through the camera when no script
    // rays point), 1 + n script ray n (World Space canvases only).
    void Scene::PointerEventsFor(PointerTracking& p, int id, const UIPointerRay* ray, glm::vec2 position, bool down, float scroll)
    {

        // While dragging, what is under the pointer beneath the dragged object (which usually follows the pointer), so
        // slots see it come and Drop goes to them.
        auto inDragged = [&](Entity e) {
            for (; IsValid(e); e = GetParent(e))
                if (e == p.DragTarget) return true;
            return false;
        };
        auto takes = [this](Entity e) { return PointerHandlers(e) != 0; };
        Entity over = Entity::Null();
        bool gone = ray && ray->Direction == glm::vec3(0.0f);
        if (!ray)
            for (Entity e : UI::ElementsAt(*this, position, m_UIScreen, takes))
                if (!p.Dragging || !inDragged(e)) { over = e; break; }
        // World Space canvases (the script's ray, else the mouse through the camera): positions in the panel's pixels.
        if (over.IsNull() && !gone && (ray || m_UIRays.empty()))
            for (auto [ce, canvas, ct] : m_Registry.ViewOf<CanvasComponent, TransformComponent>())
            {
                glm::vec2 pixel;
                if (!ct.ActiveInHierarchy || canvas.RenderMode != CanvasRenderMode::WorldSpace) continue;
                if (ray ? !UI::WorldCanvasRayPoint(*this, ce, ray->Origin, ray->Direction, pixel) : !WorldCanvasPointer(ce, pixel)) continue;
                for (Entity e : UI::ElementsAt(*this, pixel, canvas.PixelSize, takes, ce))
                    if (!p.Dragging || !inDragged(e)) { over = e; break; }
                if (!over.IsNull() || p.Dragging)
                {
                    position = pixel;
                    break;
                }
            }
        if (ray && over.IsNull() && !p.Dragging) position = p.LastPosition; // off every panel: no position of its own
        PointerEventData data;
        data.PointerId = id - 1;
        data.Position = position;
        data.Delta = p.Started ? position - p.LastPosition : glm::vec2(0.0f);
        data.PressPosition = p.PressPosition;
        data.Scroll = scroll;
        data.Over = over;
        data.Press = p.Press;
        data.Drag = p.DragTarget;
        data.Dragging = p.Dragging;
        data.ClickCount = p.ClickCount;
        p.Started = true;

        // Enter / Exit: the objects under the pointer are the target and its parents.
        std::vector<Entity> chain;
        for (Entity e = data.Over; IsValid(e); e = GetParent(e)) chain.push_back(e);
        std::vector<Entity> left, entered;
        for (Entity e : p.Hovered)
            if (std::find(chain.begin(), chain.end(), e) == chain.end()) left.push_back(e);
        for (Entity e : chain)
            if (std::find(p.Hovered.begin(), p.Hovered.end(), e) == p.Hovered.end()) entered.push_back(e);
        p.Hovered = chain;
        for (Entity e : left) SendPointerEvent(e, PointerEventKind::Exit, data);
        for (auto it = entered.rbegin(); it != entered.rend(); ++it) SendPointerEvent(*it, PointerEventKind::Enter, data);

        if (down && !p.WasDown)
        {
            // A second press on the same thing within 0.3 s counts as a double click.
            Entity clickable = PointerHandler(data.Over, PointerEventKind::Click);
            bool again = m_UnscaledTime - p.LastClickTime < 0.3f && !p.LastClick.IsNull() && clickable == p.LastClick;
            p.ClickCount = again ? p.ClickCount + 1 : 1;
            p.PressPosition = position;
            p.ClickTarget = clickable;
            p.Press = PointerHandler(data.Over, PointerEventKind::Down);
            if (p.Press.IsNull()) p.Press = PointerHandler(data.Over, PointerEventKind::Up);
            p.DragTarget = PointerHandler(data.Over, PointerEventKind::Drag);
            if (p.DragTarget.IsNull()) p.DragTarget = PointerHandler(data.Over, PointerEventKind::BeginDrag);
            p.Dragging = false;
            data.PressPosition = position;
            data.Press = p.Press;
            data.Drag = p.DragTarget;
            data.ClickCount = p.ClickCount;
            SendPointerEvent(p.Press, PointerEventKind::Down, data);
        }
        if (down && IsValid(p.DragTarget))
        {
            if (!p.Dragging && glm::length(position - p.PressPosition) >= kDragThreshold)
            {
                // A drag begins: what was pressed (unless it is the dragged object) gets its Up, and there is no click.
                p.Dragging = true;
                data.Dragging = true;
                if (p.Press != p.DragTarget)
                {
                    SendPointerEvent(p.Press, PointerEventKind::Up, data);
                    p.Press = Entity::Null();
                }
                p.ClickTarget = Entity::Null();
                data.Press = p.Press;
                SendPointerEvent(p.DragTarget, PointerEventKind::BeginDrag, data);
            }
            if (p.Dragging && data.Delta != glm::vec2(0.0f)) SendPointerEvent(p.DragTarget, PointerEventKind::Drag, data);
        }
        if (!down && p.WasDown)
        {
            SendPointerEvent(p.Press, PointerEventKind::Up, data);
            if (!p.ClickTarget.IsNull() && PointerHandler(data.Over, PointerEventKind::Click) == p.ClickTarget)
            {
                SendPointerEvent(p.ClickTarget, PointerEventKind::Click, data);
                p.LastClick = p.ClickTarget;
                p.LastClickTime = m_UnscaledTime;
            }
            if (p.Dragging)
            {
                SendPointerEvent(PointerHandler(data.Over, PointerEventKind::Drop), PointerEventKind::Drop, data);
                data.Dragging = false;
                SendPointerEvent(p.DragTarget, PointerEventKind::EndDrag, data);
            }
            p.Press = p.ClickTarget = p.DragTarget = Entity::Null();
            p.Dragging = false;
        }
        if (scroll != 0.0f) SendPointerEvent(PointerHandler(data.Over, PointerEventKind::Scroll), PointerEventKind::Scroll, data);

        if (id == 0) m_PointerData = data; // for Select / Deselect after every pointer's events
        p.WasDown = down;
        p.LastPosition = position;
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

    void Scene::DispatchTimelineEvent(Entity e, EventKind kind)
    {
        if (s_EventHook && IsValid(e)) s_EventHook(*this, e, kind, 0.0f, Entity::Null(), nullptr);
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
        bool begin = phase == CollisionPhase::Enter;
        auto call = [&](Entity self, const Collision& c) {
            auto* sc = IsValid(self) ? m_Registry.TryGet<ScriptComponent>(self) : nullptr;
            if (!sc || !IsValid(c.Other)) return;
            for (auto& inst : sc->LiveInstances(true))
            {
                if (trigger)
                {
                    if (phase == CollisionPhase::Enter) inst->OnTriggerEnter(c.Other);
                    else if (phase == CollisionPhase::Stay) inst->OnTriggerStay(c.Other);
                    else inst->OnTriggerExit(c.Other);
                }
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
