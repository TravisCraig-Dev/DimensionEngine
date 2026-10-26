#pragma once
#include "Dimension/ECS/Registry.h"
#include "Dimension/Scene/Components.h"
#include "Dimension/Scene/System.h"
#include <unordered_map>

namespace Dimension
{
    class Physics2DWorld;
    class Physics3DWorld;
    class AudioWorld;

    struct SceneSettings
    {
        glm::vec3 AmbientColor{ 0.55f, 0.6f, 0.7f };
        float AmbientIntensity = 0.25f;
        glm::vec2 Gravity2D{ 0.0f, -9.81f };
        glm::vec3 Gravity{ 0.0f, -9.81f, 0.0f };
        glm::vec3 SkyTop{ 0.25f, 0.45f, 0.80f };     // sRGB
        glm::vec3 SkyHorizon{ 0.70f, 0.80f, 0.92f };
        glm::vec3 SkyGround{ 0.33f, 0.31f, 0.29f };
        float Exposure = 1.0f;
        bool Tonemapping = false; // ACES filmic; off keeps 2D colors exactly as authored
        // Sky: 0 = procedural gradient, 1 = equirectangular texture (ideally .hdr) that also lights the scene.
        int32_t SkyMode = 0;
        AssetHandle SkyTexture = AssetHandle::None();
        float SkyIntensity = 1.0f;
        float SkyRotation = 0.0f;       // degrees around +Y
        float ReflectionIntensity = 1.0f; // sky reflections (texture mode); diffuse sky light uses AmbientIntensity
        bool Bloom = false;
        float BloomIntensity = 1.0f;
        float BloomThreshold = 1.0f; // HDR brightness where bloom starts
        float BloomKnee = 0.5f;      // soft transition below the threshold (fraction of it)
        float BloomRadius = 1.0f;    // upsample filter spread
        bool FXAA = false;
        int MSAA = 1; // samples per pixel: 1 (off), 2, 4 or 8
        float Vignette = 0.0f;
        // Screen-space ambient occlusion: darkens ambient and reflected light in creases and contact areas.
        bool SSAO = false;
        float SSAORadius = 0.5f;    // world units
        float SSAOIntensity = 1.5f; // exponent applied to the occlusion
        float SSAOBias = 0.025f;    // view-space depth bias against self-occlusion
        bool SSAODebug = false;     // show the occlusion buffer instead of the lit scene
        // Screen-space reflections: smooth surfaces reflect what is on screen, falling back to probes and the sky.
        bool SSR = false;
        float SSRIntensity = 1.0f;
        float SSRMaxDistance = 25.0f;  // view-space length of a reflection ray
        float SSRThickness = 0.3f;     // world units an object is assumed to extend behind its depth
        float SSRMaxRoughness = 0.6f;  // rougher surfaces keep only probe and sky reflections
        int SSRSteps = 48;
        bool UIKeyboardNavigation = true; // arrows/Enter/Space move and press the UI selection (off when gameplay uses them)
        // Depth of field: objects nearer or farther than the focus distance blur (perspective cameras).
        bool DepthOfField = false;
        float FocusDistance = 10.0f; // world units from the camera
        float FocusRange = 6.0f;     // distance from the focus at which blur reaches its maximum
        float BokehSize = 0.012f;    // maximum blur radius as a fraction of the image height
    };

    // A scene: an ECS registry plus hierarchy, identity (Guid) mapping and the
    // runtime systems (scripts, physics, audio) that run in play mode.
    class Scene
    {
    public:
        explicit Scene(std::string name = "Untitled");
        ~Scene();
        Scene(const Scene&) = delete;
        Scene& operator=(const Scene&) = delete;

        // Deep copy used for play mode. Entity handles are preserved.
        Ref<Scene> Clone() const;

        // ---- Entities
        Entity CreateEntity(const std::string& name = "GameObject", Entity parent = Entity::Null(), Guid id = Guid());
        void DestroyEntity(Entity e);         // immediate, includes children
        void DestroyEntityDeferred(Entity e); // safe during iteration; flushed at end of frame
        Entity DuplicateEntity(Entity e);
        bool IsValid(Entity e) const { return m_Registry.Valid(e); }

        Entity FindEntityByGuid(Guid id) const;
        Entity FindEntityByName(const std::string& name) const;
        Guid GetGuid(Entity e) const;
        size_t GetEntityCount() const { return m_Registry.AliveCount(); }

        // ---- Hierarchy
        void SetParent(Entity child, Entity parent, bool keepWorldTransform = true);
        Entity GetParent(Entity e) const;
        bool IsDescendantOf(Entity e, Entity ancestor) const;
        // Moves e to position `index` among its siblings (roots if no parent).
        void SetSiblingIndex(Entity e, size_t index);
        const std::vector<Entity>& GetRootEntities() const { return m_Roots; }
        const std::vector<Entity>& GetChildren(Entity e) const;

        // ---- Prefabs
        Entity InstantiatePrefab(AssetHandle prefab, const glm::vec3& position, Entity parent = Entity::Null());

        // ---- Lifecycle
        void OnRuntimeStart();
        void OnRuntimeStop();
        void OnUpdateRuntime(float dt);
        void OnUpdateEditor(float dt);
        bool IsRunning() const { return m_Running; }
        bool IsPaused() const { return m_Paused; }
        void SetPaused(bool paused) { m_Paused = paused; }
        void Step(int frames = 1) { m_StepFrames = frames; }

        void UpdateTransforms();
        Entity GetPrimaryCamera();

        Registry& GetRegistry() { return m_Registry; }
        const Registry& GetRegistry() const { return m_Registry; }
        SceneSettings& GetSettings() { return m_Settings; }
        Physics2DWorld* GetPhysics2D() { return m_Physics2D.get(); }
        Physics3DWorld* GetPhysics3D() { return m_Physics3D.get(); }
        AudioWorld* GetAudio() { return m_Audio.get(); }

        const std::string& GetName() const { return m_Name; }
        void SetName(std::string name) { m_Name = std::move(name); }

        // ---- Several scenes at runtime (driven by SceneManager) ----
        // The running scene holds the objects of every loaded scene. Each loaded scene remembers the root objects it
        // contributed so it can be unloaded; persistent objects (DontDestroyOnLoad) survive single-mode loads.
        struct LoadedScene
        {
            std::string Name;
            std::vector<Entity> Roots;
        };
        static constexpr const char* kPersistentSceneName = "DontDestroyOnLoad";
        const std::vector<LoadedScene>& GetLoadedScenes() const { return m_LoadedScenes; }
        // Marks the object's root (with all its children) to survive scene loads.
        void DontDestroyOnLoad(Entity e);
        bool IsPersistent(Entity e) const;
        // Adds a serialized scene's objects (and starts them when running). Single mode first destroys every
        // non-persistent object and takes over the new scene's name and settings; additive keeps both.
        void LoadSceneData(const nlohmann::json& data, bool additive);
        // Destroys the root objects a loaded scene contributed. False if no loaded scene has that name.
        bool UnloadScene(const std::string& name);

        // UI input for the current frame (pointer in render-target pixels). Applied in OnUpdateRuntime.
        void SetUIInput(const glm::vec2& pointer, bool down, const glm::vec2& screenSize, bool valid = true, float scroll = 0.0f);
        bool IsPointerOverUI() const { return !m_UIHovered.IsNull(); }
        // The UI widget selected for keyboard/gamepad navigation (Null = none).
        Entity GetUISelected() const { return m_UISelected; }
        void SetUISelected(Entity e) { m_UISelected = e; }
        // True after navigation input (arrows, D-pad) until the next mouse click: only then does Submit press the
        // selection and the highlight show, so Space for jumping does not click the last button the mouse used.
        bool IsUINavigating() const { return m_UINavigating; }
        void SetUINavigating(bool navigating) { m_UINavigating = navigating; }
        // Play-mode clock: seconds and frames since OnRuntimeStart (paused frames do not count).
        float GetTime() const { return m_Time; }
        float GetDeltaTime() const { return m_DeltaTime; } // this frame's (clamped) update step
        uint64_t GetFrameCount() const { return m_FrameCount; }
        // Extra UI keyboard input for this frame (tests / injected input); combined with the Input module.
        void InjectUIKeyboard(const std::string& text, bool backspace = false, bool enter = false);
        void DispatchUIClick(Entity e);
        void DispatchAnimationEvent(Entity e, const std::string& name);
        void DispatchJointBreak(Entity e, float force);
        void DispatchUIValueChanged(Entity e, float value);
        void DispatchUIText(Entity e, const std::string& text, bool submit);

        // Script/physics glue (called by physics worlds).
        void DispatchCollision(Entity a, Entity b, bool begin, bool trigger);

        // Engine events for listeners outside the entity's own scripts (C# events such as UIButton.Clicked). Kinds:
        // 0 click, 1 value changed (value), 2 text changed, 3 submit (text), 4/5 collision enter/exit and 6/7 trigger
        // enter/exit (other), -1 play stopped (listeners of this scene end).
        enum class EventKind : int { Stopped = -1, Click = 0, ValueChanged, TextChanged, Submit, CollisionEnter, CollisionExit, TriggerEnter, TriggerExit };
        using EventHook = void (*)(Scene& scene, Entity e, EventKind kind, float value, Entity other, const char* text);
        static void SetEventHook(EventHook hook);

        // Reloads a script module while playing (C# hot reload): saves the live fields of that module's scripts,
        // destroys them (without OnDestroy), runs `reload`, then recreates them with the saved values and calls
        // OnScriptsReloaded instead of OnCreate. Scripts whose class is gone stay inactive. Returns reload's result.
        bool ReloadScriptModule(const std::string& module, const std::function<bool()>& reload);

        static constexpr float kFixedTimestep = 1.0f / 60.0f;

    private:
        void UpdateSpriteAnimators(float dt);
    public:
        // Advances Animator time by dt (0 = just evaluate the pose) and computes skinning matrices.
        void UpdateAnimators(float dt);
    private:
        void UpdateTransformRecursive(Entity e, const glm::mat4& parentWorld, bool parentActive);
        void DetachFromParent(Entity e);
        void InstantiateScripts();
        std::shared_ptr<ScriptableEntity> CreateScriptInstance(Entity e, ScriptInstance& script, const struct ScriptInfo& info, const struct EntityRefResolver& resolver);
        void FlushDeferredDestroys();
        std::vector<Entity>& SiblingList(Entity e);

        std::string m_Name;
        Registry m_Registry;
        std::vector<Entity> m_Roots;
        std::unordered_map<Guid, Entity> m_GuidMap;
        SceneSettings m_Settings;

        bool m_Running = false, m_Paused = false;
        int m_StepFrames = 0;
        float m_FixedAccumulator = 0.0f;
        std::vector<Entity> m_PendingDestroy;
        glm::vec2 m_UIPointer{ 0.0f }, m_UIScreen{ 0.0f };
        bool m_UIDown = false, m_UIWasDown = false, m_UIInputValid = false;
        float m_UIScroll = 0.0f;
        float m_Time = 0.0f, m_DeltaTime = 0.0f;
        uint64_t m_FrameCount = 0;
        std::string m_UIInjectedText;
        bool m_UIInjectedBackspace = false, m_UIInjectedEnter = false;
        Entity m_UISelected = Entity::Null();
        bool m_UINavigating = false;
        int m_UIStickDir = 0; // left stick direction last frame (navigation repeats only on a new push)
        Entity m_UIHovered = Entity::Null();
        std::vector<Scope<System>> m_Systems;
        std::vector<LoadedScene> m_LoadedScenes;
        std::vector<Entity> m_Persistent;
        Scope<Physics2DWorld> m_Physics2D;
        Scope<Physics3DWorld> m_Physics3D;
        Scope<AudioWorld> m_Audio;

        friend class SceneSerializer;
    };
}
