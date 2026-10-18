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

        const std::string& GetName() const { return m_Name; }
        void SetName(std::string name) { m_Name = std::move(name); }

        // UI input for the current frame (pointer in render-target pixels). Applied in OnUpdateRuntime.
        void SetUIInput(const glm::vec2& pointer, bool down, const glm::vec2& screenSize, bool valid = true);
        bool IsPointerOverUI() const { return !m_UIHovered.IsNull(); }
        void DispatchUIClick(Entity e);

        // Script/physics glue (called by physics worlds).
        void DispatchCollision(Entity a, Entity b, bool begin, bool trigger);

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
        Entity m_UIHovered = Entity::Null();
        std::vector<Scope<System>> m_Systems;
        Scope<Physics2DWorld> m_Physics2D;
        Scope<Physics3DWorld> m_Physics3D;
        Scope<AudioWorld> m_Audio;

        friend class SceneSerializer;
    };
}
