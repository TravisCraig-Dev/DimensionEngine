#pragma once
#include "Dimension/ECS/Registry.h"
#include "Dimension/Scene/Components.h"
#include "Dimension/Scene/System.h"
#include "Dimension/Physics/Collision.h"
#include <algorithm>
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
        // Wind (m/s, world space) that sways terrain grass; Gusts adds stronger waves moving through it.
        glm::vec3 Wind{ 1.5f, 0.0f, 0.8f };
        float WindGusts = 0.5f;
        // Volumetric clouds in the sky: a layer from Cloud Base to Cloud Top (meters) of drifting noise (moved by the
        // Wind), Coverage of the sky, lit by the sun through them; features about Cloud Scale meters across.
        bool Clouds = false;
        float CloudCoverage = 0.45f;
        float CloudDensity = 0.004f; // extinction per meter
        float CloudBase = 1500.0f, CloudTop = 3500.0f;
        float CloudScale = 3000.0f;
        int CloudSteps = 48;
        bool CloudShadows = true; // the clouds shade the sunlight on the ground
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
        // Screen-space global illumination: light bouncing off what is on screen onto nearby surfaces (a red wall
        // tints the floor beside it), on top of the ambient, sky and probe light (perspective cameras).
        bool SSGI = false;
        float SSGIIntensity = 1.0f;
        float SSGIDistance = 3.0f;   // view-space ray length
        float SSGIThickness = 0.5f;  // world units an object is assumed to extend behind its depth
        int SSGIRays = 4;            // per pixel and frame (TAA averages them over frames)
        // Order-independent transparency: transparent meshes blend by a weighted average instead of being sorted
        // per object, so intersecting and nested see-through objects look right in any order.
        bool OIT = false;
        // Occlusion culling: meshes hidden behind others (in the depth an earlier frame drew) are not drawn while the
        // camera holds nearly still.
        bool OcclusionCulling = true;
        bool UIKeyboardNavigation = true; // arrows/Enter/Space move and press the UI selection (off when gameplay uses them)
        // Depth of field: objects nearer or farther than the focus distance blur (perspective cameras).
        bool DepthOfField = false;
        float FocusDistance = 10.0f; // world units from the camera
        float FocusRange = 6.0f;     // distance from the focus at which blur reaches its maximum
        float BokehSize = 0.012f;    // maximum blur radius as a fraction of the image height
        // Temporal anti-aliasing (perspective game views): jittered frames blended over time.
        bool TAA = false;
        float TAAFeedback = 0.1f;    // weight of each new frame: lower is smoother but trails more
        // Volumetric fog: height fog lit by the ambient Fog Color and the sun (with light shafts through its shadows).
        bool Fog = false;
        glm::vec3 FogColor{ 0.55f, 0.6f, 0.7f }; // sRGB, the light the fog itself gives off (sky and bounce)
        float FogIntensity = 0.6f;
        float FogDensity = 0.02f;        // extinction per meter at the base height
        float FogHeight = 0.0f;          // base height: full density below
        float FogHeightFalloff = 0.15f;  // per meter above it
        float FogMaxDistance = 150.0f;
        float FogSunScattering = 1.0f;   // sunlight in the fog (0 = none)
        float FogAnisotropy = 0.6f;      // > 0: brighter looking toward the sun
        int FogSteps = 24;
    };

    // UI pointer events for scripts (like Unity's IPointerEnterHandler and friends), sent to UI elements: Enter / Exit
    // as the pointer comes over an element or its children and leaves; Down, Up and Click (released over what was
    // pressed); BeginDrag, Drag (each frame it moves) and EndDrag once it moved kDragThreshold pixels while held, with
    // Drop on what it is released over (while dragging, the pointer is over what lies beneath the dragged object);
    // Select / Deselect as the UI selection changes; Scroll for the wheel.
    enum class PointerEventKind : int { Enter, Exit, Down, Up, Click, BeginDrag, Drag, EndDrag, Drop, Select, Deselect, Scroll, Count };
    struct PointerEventData
    {
        glm::vec2 Position{ 0.0f };      // render-target pixels, top-left origin
        glm::vec2 Delta{ 0.0f };         // moved since last frame
        glm::vec2 PressPosition{ 0.0f }; // where the button went down
        float Scroll = 0.0f;             // wheel notches (positive = up)
        int Button = 0;                  // 0 left
        int ClickCount = 0;              // 2 for a double click (Click, and Down of the second press)
        bool Dragging = false;
        Entity Over = Entity::Null();    // the topmost UI element under the pointer
        Entity Press = Entity::Null();   // what handled Down
        Entity Drag = Entity::Null();    // what is being dragged (Drop: what was dropped)
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
        // A copy of an object with its children (like Unity's Instantiate): under `parent` (Null: a root object), at the
        // original's world position and rotation unless they are given (world space). While playing, its scripts start
        // with the original's live field values, and its bodies, sounds and scripts start at once. References inside
        // the copied tree point to the copies.
        Entity Instantiate(Entity original, Entity parent = Entity::Null(), const glm::vec3* position = nullptr,
                           const glm::quat* rotation = nullptr);
        // At the end of the frame, one script or an engine component (by its registry name) leaves an object: a script
        // gets OnDisable and OnDestroy, a physics component's body is made again without it. False for what cannot go
        // (Transform, Tag and the like) or is not there.
        bool RemoveScriptDeferred(Entity e, const ScriptableEntity* script);
        bool RemoveComponentDeferred(Entity e, const std::string& component);
        bool IsValid(Entity e) const { return m_Registry.Valid(e); }

        Entity FindEntityByGuid(Guid id) const;
        Entity FindEntityByName(const std::string& name) const;
        // The first entity / every entity with a tag (in creation order).
        Entity FindEntityByTag(const std::string& tag) const;
        std::vector<Entity> FindEntitiesByTag(const std::string& tag) const;
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

        // ---- Active state
        // Turns an object on or off (its Active box). While playing, the scripts of the objects it switches get
        // OnEnable / OnDisable at once; their physics bodies follow at the next step.
        void SetActive(Entity e, bool active);
        // Whether e and every parent are active (computed now, unlike TransformComponent::ActiveInHierarchy).
        bool IsActiveInHierarchy(Entity e) const;
        // Adds a script to an object; while playing it starts at once (OnCreate, OnEnable). Null when the class is unknown.
        ScriptableEntity* AddScript(Entity e, const std::string& className);
        // Turns one script on or off (its checkbox), with OnEnable / OnDisable while playing.
        void SetScriptEnabled(Entity e, size_t scriptIndex, bool enabled);
        // Gives e's scripts OnEnable / OnDisable where their state (playing, object active, script enabled) differs
        // from the last one they were told. Also run every frame, so changes made directly to the fields count too.
        void SyncScriptsEnabled(Entity e);

        // ---- Prefabs
        Entity InstantiatePrefab(AssetHandle prefab, const glm::vec3& position, Entity parent = Entity::Null());
        // At a world position and rotation (under `parent`).
        Entity InstantiatePrefab(AssetHandle prefab, const glm::vec3& position, const glm::quat& rotation, Entity parent);

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
        // The camera the game shows: the first active one marked Primary, else the first active one.
        Entity GetPrimaryCamera();
        // Makes a camera the one the game shows (Primary, and no other camera Primary).
        void SetPrimaryCamera(Entity camera);
        // While playing: the object's 3D and 2D joints made again from their components (see Physics3DWorld::RebuildJoint).
        void RebuildJoint(Entity e);
        // While playing: the object's Rigidbody (3D) or Rigidbody 2D settings applied to its body (see RefreshBody).
        void RefreshBody(Entity e);
        // Puts the object and its body at a world position and rotation (Euler radians; 2D uses z's angle): at once
        // (teleport), or a kinematic body moves there in the next physics step, pushing what it meets.
        void MoveBody(Entity e, const glm::vec3& position, const glm::vec3& rotation, bool teleport);
        // Counts the times the camera the game shows changed (a cut): the renderer starts its temporal history over.
        uint32_t GetCameraCuts() const { return m_CameraCuts; }
        // A 3D physics world for queries only (raycasts against the colliders; nothing is simulated), on a scene that
        // is not playing: the navigation bake uses one on a copy of the scene being edited.
        void StartQueryPhysics();

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
        // Records objects already in the scene as a loaded scene's (the editor's added scenes when play starts); they
        // leave the other loaded scenes' lists.
        void RegisterLoadedScene(std::string name, std::vector<Entity> roots);

        // UI input for the current frame (pointer in render-target pixels). Applied in OnUpdateRuntime.
        void SetUIInput(const glm::vec2& pointer, bool down, const glm::vec2& screenSize, bool valid = true, float scroll = 0.0f);
        bool IsPointerOverUI() const { return !m_UIHovered.IsNull(); }
        // The topmost UI element under the pointer this frame (Null when none).
        Entity GetUIHovered() const { return m_UIHovered; }
        // Pixels the pointer must move while held before a drag begins (Unity's EventSystem drag threshold).
        static constexpr float kDragThreshold = 10.0f;
        // The UI element that pointer events go to under a point: the topmost one taking the pointer or handling them.
        Entity PointerEventTarget(const glm::vec2& point);
        // The object whose collider is under the pointer (through the primary camera; not when over UI), as the
        // OnMouse* script events see it; Null when none.
        Entity GetObjectUnderPointer() const { return m_MouseOver; }
        // The nearest 3D or 2D collider under a view point (pixels, top-left origin), seen through the primary camera.
        Entity PickObject(const glm::vec2& viewPoint);
        // Moves a Sprite Animator to `seconds` into its frames and shows that frame at once (also while it is stopped);
        // past the end of a run that does not loop, it shows the last frame and stops.
        void SetSpriteAnimatorTime(Entity entity, float seconds);
        // The UI widget selected for keyboard/gamepad navigation (Null = none).
        Entity GetUISelected() const { return m_UISelected; }
        void SetUISelected(Entity e) { m_UISelected = e; }
        // True after navigation input (arrows, D-pad) until the next mouse click: only then does Submit press the
        // selection and the highlight show, so Space for jumping does not click the last button the mouse used.
        bool IsUINavigating() const { return m_UINavigating; }
        void SetUINavigating(bool navigating) { m_UINavigating = navigating; }
        // Play-mode clock: seconds and frames since OnRuntimeStart (paused frames do not count).
        float GetTime() const { return m_Time; }
        // Play time while running, otherwise the editor's preview clock (waves move in the editor too).
        float GetDisplayTime() const { return m_Running ? m_Time : m_EditorTime; }
        float GetDeltaTime() const { return m_DeltaTime; } // this frame's (clamped) update step
        // Game speed (like Unity's Time.timeScale; 1 = normal, 0 = frozen, up to 100): each update step is scaled, so
        // physics, animation, particles, script time and coroutines follow; UI animation keeps real time. Play
        // starts at 1.
        void SetTimeScale(float scale) { m_TimeScale = std::clamp(scale, 0.0f, 100.0f); }
        float GetTimeScale() const { return m_TimeScale; }
        float GetUnscaledDeltaTime() const { return m_UnscaledDeltaTime; } // this frame's step before the time scale
        float GetUnscaledTime() const { return m_UnscaledTime; }           // real seconds since play started
        // The fixed step (OnFixedUpdate, physics; Unity's Time.fixedDeltaTime, 1/60 by default when play starts), the
        // time fixed steps have reached (Time.fixedTime), the longest step a frame may take (Time.maximumDeltaTime,
        // 0.1 s: a hitch slows the game rather than jumping it), and the time since the last single scene load.
        void SetFixedTimestep(float seconds) { m_FixedTimestep = std::clamp(seconds, 0.0001f, 1.0f); }
        float GetFixedTimestep() const { return m_FixedTimestep; }
        float GetFixedTime() const { return m_FixedTime; }
        void SetMaximumDeltaTime(float seconds) { m_MaxDeltaTime = std::clamp(seconds, 0.001f, 1.0f); }
        float GetMaximumDeltaTime() const { return m_MaxDeltaTime; }
        float GetTimeSinceLevelLoad() const { return m_Time - m_LevelLoadTime; }
        // The game is closing (or play stopping in the editor): OnApplicationQuit on every running script.
        void NotifyApplicationQuit();
        // A Mesh Renderer's own copy of its material (Unity's renderer.material): made the first time (from its material,
        // or its simple look without one), owned by this scene (released when play stops). None without a renderer.
        AssetHandle GetRendererMaterialInstance(Entity e);
        // The view the scene is shown in (from SetUIInput): its size in pixels (a 1280x720 stand-in before the first
        // frame), and the pointer in it (top-left origin).
        glm::vec2 GetViewSize() const { return m_UIScreen.x >= 1.0f && m_UIScreen.y >= 1.0f ? m_UIScreen : glm::vec2(1280.0f, 720.0f); }
        glm::vec2 GetViewPointer() const { return m_UIPointer; }
        uint64_t GetFrameCount() const { return m_FrameCount; }
        // Extra UI keyboard input for this frame (tests / injected input); combined with the Input module.
        void InjectUIKeyboard(const std::string& text, bool backspace = false, bool enter = false);
        void DispatchUIClick(Entity e);
        void DispatchAnimationEvent(Entity e, const std::string& name);
        // Moves an animated object by a world-space root motion step as the Animator does (through its Character
        // Controller when it has one).
        void ApplyRootMotion(Entity e, const glm::vec3& worldDelta, const glm::quat& worldTurn = glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
        // Asks an Animator's layer (0 base) to play a state of its controller (or, without one, a clip on the base
        // layer): fading over `fadeSeconds`, from `offset` (normalized, or seconds when fixedTime). False when there is
        // no such state or clip name.
        bool PlayAnimatorState(Entity e, const std::string& name, int layer, float fadeSeconds, float offset, bool fixedTime);
        void DispatchJointBreak(Entity e, float force);
        void DispatchUIValueChanged(Entity e, float value);
        void DispatchUIText(Entity e, const std::string& text, bool submit);

        // Script/physics glue (called by physics worlds): `fromA` is the collision as `a` sees it (b flips it).
        void DispatchCollision(Entity a, Entity b, CollisionPhase phase, bool trigger, const Collision* fromA = nullptr);

        // Engine events for listeners outside the entity's own scripts (C# events such as UIButton.Clicked). Kinds:
        // 0 click, 1 value changed (value), 2 text changed, 3 submit (text), 4/5 collision enter/exit and 6/7 trigger
        // enter/exit (other), -1 play stopped (listeners of this scene end).
        enum class EventKind : int { Stopped = -1, Click = 0, ValueChanged, TextChanged, Submit, CollisionEnter, CollisionExit, TriggerEnter, TriggerExit };
        using EventHook = void (*)(Scene& scene, Entity e, EventKind kind, float value, Entity other, const char* text);
        static void SetEventHook(EventHook hook);
        // Animator states' behaviours (StateMachineBehaviour): kind 0 entered, 1 updated (every frame it plays, also
        // while fading out), 2 left (when its fade out ends); the state's index in its layer's machine and its info.
        using StateBehaviourHook = void (*)(Scene& scene, Entity e, int kind, int layer, int state, const std::string& stateName,
                                            const AnimatorStateInfo& info, const std::vector<std::string>& behaviours);
        static void SetStateBehaviourHook(StateBehaviourHook hook);
        // Points in a frame scripts' coroutines can wait for (C#'s WaitForFixedUpdate / WaitForEndOfFrame): 0 after each
        // physics step, 1 when the frame's update is done (after animation, particles and audio; before it is drawn).
        using CoroutinePhaseHook = void (*)(Scene& scene, int phase);
        static void SetCoroutinePhaseHook(CoroutinePhaseHook hook);

        // Reloads a script module while playing (C# hot reload): saves the live fields of that module's scripts,
        // destroys them (without OnDestroy), runs `reload`, then recreates them with the saved values and calls
        // OnScriptsReloaded instead of OnCreate. Scripts whose class is gone stay inactive. Returns reload's result.
        bool ReloadScriptModule(const std::string& module, const std::function<bool()>& reload);

        static constexpr float kFixedTimestep = 1.0f / 60.0f; // the default fixed step

    private:
        void UpdateSpriteAnimators(float dt);
        void UpdateMouseEvents();
        void UpdatePointerEvents(const glm::vec2& position, bool down, float scroll);
        uint32_t PointerHandlers(Entity e);                        // the event kinds e's enabled scripts handle (bits)
        Entity PointerHandler(Entity from, PointerEventKind kind); // from or the nearest parent handling `kind`
        void SendPointerEvent(Entity e, PointerEventKind kind, const PointerEventData& data);
    public:
        // Advances Animator time by dt (0 = just evaluate the pose) and computes skinning matrices.
        // `only`: just that Animator, by exactly dt (Animator.Update), whatever its update mode.
        void UpdateAnimators(float dt, Entity only = Entity::Null());
    private:
        void UpdateTransformRecursive(Entity e, const glm::mat4& parentWorld, bool parentActive);
        void DetachFromParent(Entity e);
        void InstantiateScripts();
        std::shared_ptr<ScriptableEntity> CreateScriptInstance(Entity e, ScriptInstance& script, const struct ScriptInfo& info, const struct EntityRefResolver& resolver);
    public:
        // A script instance on `e` with the script's saved values, not attached to it (edit-mode scripts).
        std::shared_ptr<ScriptableEntity> CreateDetachedScriptInstance(Entity e, const ScriptInstance& script, const struct ScriptInfo& info,
                                                                       const struct EntityRefResolver& resolver);
    private:
        void FlushDeferredDestroys();
        // An object's world position and rotation (keeping its world scale), as local values under its parent.
        void SetWorldPose(Entity e, const glm::vec3& position, const glm::quat& rotation);
        // Bodies, sounds and scripts of a subtree added while playing.
        void StartAdded(Entity root);
        // The live values of a subtree's scripts' fields into what they save (before copying it while playing).
        void CaptureScriptFields(Entity root);
        struct PendingRemoval { Entity E; const ScriptableEntity* Script; std::string Component; };
        std::vector<PendingRemoval> m_PendingRemovals;
        std::vector<Entity>& SiblingList(Entity e);

        std::string m_Name;
        Registry m_Registry;
        std::vector<Entity> m_Roots;
        std::unordered_map<Guid, Entity> m_GuidMap;
        SceneSettings m_Settings;

        bool m_Running = false, m_Paused = false;
        int m_StepFrames = 0;
        float m_FixedAccumulator = 0.0f;
        int m_FixedStepsThisFrame = 0; // physics steps taken this frame (Animate Physics animators)
        std::vector<Entity> m_PendingDestroy;
        glm::vec2 m_UIPointer{ 0.0f }, m_UIScreen{ 0.0f };
        bool m_UIDown = false, m_UIWasDown = false, m_UIInputValid = false;
        float m_UIScroll = 0.0f;
        float m_Time = 0.0f, m_DeltaTime = 0.0f;
        float m_TimeScale = 1.0f, m_UnscaledDeltaTime = 0.0f, m_UnscaledTime = 0.0f;
        float m_FixedTimestep = kFixedTimestep, m_FixedTime = 0.0f, m_MaxDeltaTime = 0.1f, m_LevelLoadTime = 0.0f;
        int m_WindowListener = 0; // AppRuntime listener while running (OnApplicationFocus / Pause)
        float m_EditorTime = 0.0f; // waves and other previews while editing
        uint64_t m_FrameCount = 0;
        std::string m_UIInjectedText;
        bool m_UIInjectedBackspace = false, m_UIInjectedEnter = false;
        Entity m_UISelected = Entity::Null();
        bool m_UINavigating = false;
        int m_UIStickDir = 0; // left stick direction last frame (navigation repeats only on a new push)
        Entity m_UIHovered = Entity::Null();
        Entity m_MouseOver = Entity::Null(), m_MousePressed = Entity::Null(); // OnMouse* events
        Entity m_LastPrimaryCamera = Entity::Null();
        uint32_t m_CameraCuts = 0;
        bool m_MouseWasDown = false;
        struct PointerTracking
        {
            std::vector<Entity> Hovered; // the element under the pointer and its parents
            Entity Press, ClickTarget, DragTarget, LastClick, Selected;
            glm::vec2 PressPosition{ 0.0f }, LastPosition{ 0.0f };
            bool WasDown = false, Dragging = false, Started = false;
            float LastClickTime = -1.0f;
            int ClickCount = 0;
        } m_Pointer;
        std::vector<Scope<System>> m_Systems;
        std::vector<LoadedScene> m_LoadedScenes;
        std::vector<Entity> m_Persistent;
        Scope<Physics2DWorld> m_Physics2D;
        Scope<Physics3DWorld> m_Physics3D;
        Scope<AudioWorld> m_Audio;

        friend class SceneSerializer;
    };
}
