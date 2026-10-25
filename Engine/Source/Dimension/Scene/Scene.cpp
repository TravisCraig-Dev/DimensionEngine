#include "Dimension/Scene/Scene.h"
#include "Dimension/Scene/SceneSerializer.h"
#include "Dimension/Scripting/ScriptableEntity.h"
#include "Dimension/Scripting/ScriptRegistry.h"
#include "Dimension/Physics/Physics2D.h"
#include "Dimension/Physics/Physics3D.h"
#include "Dimension/Audio/AudioEngine.h"
#include "Dimension/Asset/AssetManager.h"
#include "Dimension/UI/UI.h"
#include "Dimension/Scene/Prefab.h"
#include "Dimension/Particles/ParticleSystem.h"
#include "Dimension/Animation/Animation.h"
#include "Dimension/Animation/AnimatorController.h"
#include "Dimension/Renderer/Mesh.h"
#include "Dimension/Core/Log.h"
#include "Dimension/Core/Input.h"
#include <glm/gtx/matrix_decompose.hpp>
#include <algorithm>
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

    Scene::~Scene()
    {
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
                for (auto& inst : sc->LiveInstances(false)) inst->OnDestroy();
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

    void Scene::InstantiateScripts()
    {
        EntityRefResolver resolver;
        resolver.FromGuid = [this](uint64_t g) { return FindEntityByGuid(Guid(g)); };

        std::vector<std::shared_ptr<ScriptableEntity>> created;
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
                auto instance = info->Create();
                instance->m_Entity = e;
                instance->m_Scene = this;
                void* obj = info->ToObject(instance.get());
                for (auto& f : info->Fields)
                    if (script.Fields.contains(f.Name)) FieldFromJson(f, obj, script.Fields[f.Name], &resolver);
                script.Instance = instance;
                created.push_back(instance);
            }
        }
        // OnCreate may create entities/scripts, so call after the iteration (in attachment order, like Awake).
        for (auto& inst : created) inst->OnCreate();
    }

    void Scene::OnRuntimeStart()
    {
        m_Running = true;
        m_FixedAccumulator = 0.0f;
        m_Time = 0.0f;
        m_FrameCount = 0;
        UpdateTransforms();

        m_Physics2D = MakeScope<Physics2DWorld>(*this);
        m_Physics3D = MakeScope<Physics3DWorld>(*this);
        m_Audio = MakeScope<AudioWorld>(*this);
        m_LoadedScenes = { LoadedScene{ m_Name, m_Roots } };
        m_Persistent.clear();

        ParticleSystems::Restart(*this);
        TrailRenderers::ClearAll(*this);
        for (auto [e, anim] : m_Registry.ViewOf<AnimatorComponent>())
            if (anim.Controller.IsValid())
            {
                anim.Runtime = AnimatorRuntime();
                anim.Clip.clear();
                anim.FadeDuration = 0.0f;
            }
        for (auto& entry : SystemRegistry::All()) m_Systems.push_back(entry.Create());
        InstantiateScripts();
        for (auto& sys : m_Systems) sys->OnStart(*this);
        m_Audio->Start();
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
            InstantiateScripts();
        }
        // The active (single-mode) scene comes first, after any persistent objects' entry.
        LoadedScene entry{ name, roots };
        if (additive) m_LoadedScenes.push_back(std::move(entry));
        else m_LoadedScenes.insert(m_LoadedScenes.begin(), std::move(entry));
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
        if (!m_Running) return;
        Input::SetKeyboardCapturedByUI(false);
        for (auto [e, sc] : m_Registry.ViewOf<ScriptComponent>())
            for (auto& inst : sc.LiveInstances(false)) inst->OnDestroy();
        for (auto [e, sc] : m_Registry.ViewOf<ScriptComponent>())
            for (auto& script : sc.Scripts) script.Instance.reset();
        for (auto& sys : m_Systems) sys->OnStop(*this);
        m_Systems.clear();
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
            pointer.Home = Input::IsKeyTyped(Key::Home);
            pointer.End = Input::IsKeyTyped(Key::End);
            pointer.Tab = Input::IsKeyTyped(Key::Tab);
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
            m_UIHovered = UI::Update(*this, pointer, m_UIScreen, dt);
            // A focused text field keeps the keys from gameplay (WASD while typing a name must not move the player).
            bool typing = false;
            for (auto [e, field] : m_Registry.ViewOf<UIInputFieldComponent>()) typing |= field.Focused;
            Input::SetKeyboardCapturedByUI(typing);
            m_UIScroll = 0.0f;
            m_UIWasDown = m_UIDown;
        }

        m_Registry.ViewOf<ScriptComponent>().Each([&](Entity e, ScriptComponent& sc) {
            if (!m_Registry.Get<TransformComponent>(e).ActiveInHierarchy) return;
            for (auto& inst : sc.LiveInstances(true))
            {
                inst->OnUpdate(dt);
                inst->TickTimers(dt);
            }
        });
        for (auto& sys : m_Systems) sys->OnUpdate(*this, dt);
        UpdateSpriteAnimators(dt);

        m_FixedAccumulator += dt;
        while (m_FixedAccumulator >= kFixedTimestep)
        {
            m_Registry.ViewOf<ScriptComponent>().Each([&](Entity e, ScriptComponent& sc) {
                if (!m_Registry.Get<TransformComponent>(e).ActiveInHierarchy) return;
                for (auto& inst : sc.LiveInstances(true)) inst->OnFixedUpdate(kFixedTimestep);
            });
            for (auto& sys : m_Systems) sys->OnFixedUpdate(*this, kFixedTimestep);
            UpdateTransforms();
            m_Physics2D->Step(kFixedTimestep);
            m_Physics3D->Step(kFixedTimestep);
            m_FixedAccumulator -= kFixedTimestep;
        }

        UpdateTransforms();
        UpdateAnimators(dt);
        ParticleSystems::Update(*this, dt);
        TrailRenderers::Update(*this, m_Time);
        AudioMixer::Update(dt);
        m_Audio->Update();
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
    }

    void Scene::UpdateAnimators(float dt)
    {
        Pose current, previous;
        std::vector<std::pair<Entity, std::vector<std::string>>> firedEvents;
        for (auto [e, anim, mr] : m_Registry.ViewOf<AnimatorComponent, MeshRendererComponent>())
        {
            Ref<Mesh> mesh = AssetManager::GetMesh(mr.Mesh);
            const Skeleton* skeleton = mesh ? mesh->GetSkeleton() : nullptr;
            if (!skeleton)
            {
                anim.SkinMatrices.clear();
                continue;
            }
            const auto& clips = mesh->GetClips();
            auto duration = [&](const std::string& name) {
                const AnimationClip* c = Animation::FindClip(clips, name);
                return c ? c->Duration : 0.0f;
            };
            Ref<AnimatorController> controller = AssetManager::GetAnimatorController(anim.Controller);
            if (controller) controller->LinkLayers();

            // Samples a 1D blend tree at a normalized time: the two weighted children, in sync.
            auto sampleTree = [&](const AnimatorController& machine, const AnimatorRuntime& runtime, const AnimState& st, float normalized, Pose& out) {
                std::vector<float> w;
                AnimatorStateMachine::BlendWeights(st, AnimatorStateMachine::GetParameter(machine, runtime, st.BlendParameter), w);
                float frac = st.Loop ? normalized - std::floor(normalized) : std::clamp(normalized, 0.0f, 1.0f);
                auto sampleChild = [&](int i, Pose& pose) {
                    const AnimationClip* c = Animation::FindClip(clips, st.Children[i].Clip);
                    if (c) Animation::Sample(*skeleton, *c, frac * c->Duration, pose);
                    else pose.SetBindPose(*skeleton);
                };
                int a = -1, b = -1;
                for (int i = 0; i < (int)w.size(); ++i)
                    if (w[i] > 0.0f) (a < 0 ? a : b) = i;
                if (a < 0) { out.SetBindPose(*skeleton); return; }
                sampleChild(a, out);
                if (b >= 0)
                {
                    Pose pb, blended;
                    sampleChild(b, pb);
                    Animation::Blend(out, pb, w[b], blended);
                    out = std::move(blended);
                }
            };

            // Runs one state machine (the base layer: `pb` is the AnimatorComponent itself; an extra layer: its playback)
            // and produces its pose, including the crossfade from the previous state.
            auto evaluate = [&](auto& pb, const AnimatorController* machine, bool baseLayer, Pose& out) {
                if (machine)
                {
                    AnimatorStateMachine::Update(*machine, pb.Runtime, dt * anim.Speed, duration, [&](const AnimState& state, float fade) {
                        int index = (int)(&state - machine->States.data());
                        bool isTree = state.Motion == AnimMotionType::BlendTree1D;
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
            evaluate(anim, controller.get(), true, current);

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
                    // Parameters are shared; a trigger consumed by this layer is consumed for the whole Animator.
                    pb.Runtime.Parameters = anim.Runtime.Parameters;
                    pb.Runtime.Triggers = anim.Runtime.Triggers;
                    evaluate(pb, &layer.Machine, false, layerPose);
                    for (auto it = anim.Runtime.Triggers.begin(); it != anim.Runtime.Triggers.end();)
                        it = pb.Runtime.Triggers.contains(*it) ? std::next(it) : anim.Runtime.Triggers.erase(it);
                    float weight = std::clamp(pb.Weight >= 0.0f ? pb.Weight : layer.Weight, 0.0f, 1.0f);
                    if (weight <= 0.0f || layer.Machine.States.empty()) continue;
                    Animation::BuildMask(*skeleton, layer.Mask, mask);
                    if (layer.Blending == AnimLayerBlending::Override)
                        Animation::LayerOverride(current, layerPose, mask, weight);
                    else
                    {
                        // Reference: the first frame of the layer's current motion.
                        const AnimState* st = pb.TreeState >= 0 && pb.TreeState < (int)layer.Machine.States.size() ? &layer.Machine.States[pb.TreeState] : nullptr;
                        if (st) sampleTree(layer.Machine, pb.Runtime, *st, 0.0f, reference);
                        else if (const AnimationClip* c = Animation::FindClip(clips, pb.Clip)) Animation::Sample(*skeleton, *c, 0.0f, reference);
                        else reference = layerPose;
                        Animation::LayerAdditive(current, layerPose, reference, mask, weight);
                    }
                }
            }
            Animation::ComputeSkinMatrices(*skeleton, current, anim.SkinMatrices);
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
    }

    void Scene::DispatchUIValueChanged(Entity e, float value)
    {
        if (auto* sc = m_Registry.TryGet<ScriptComponent>(e))
            for (auto& inst : sc->LiveInstances(true)) inst->OnUIValueChanged(value);
    }

    void Scene::DispatchUIText(Entity e, const std::string& text, bool submit)
    {
        if (auto* sc = m_Registry.TryGet<ScriptComponent>(e))
            for (auto& inst : sc->LiveInstances(true))
            {
                if (submit) inst->OnUISubmit(text);
                else inst->OnUITextChanged(text);
            }
    }

    void Scene::DispatchCollision(Entity a, Entity b, bool begin, bool trigger)
    {
        auto call = [&](Entity self, Entity other) {
            auto* sc = m_Registry.TryGet<ScriptComponent>(self);
            if (!sc) return;
            for (auto& inst : sc->LiveInstances(true))
            {
                if (trigger) begin ? inst->OnTriggerEnter(other) : inst->OnTriggerExit(other);
                else begin ? inst->OnCollisionEnter(other) : inst->OnCollisionExit(other);
            }
        };
        if (IsValid(a) && IsValid(b))
        {
            call(a, b);
            call(b, a);
        }
    }
}
