#pragma once
#include "Dimension/Scene/Scene.h"
#include "Dimension/Physics/Physics2D.h"
#include "Dimension/Physics/Physics3D.h"

namespace Dimension
{
    // Base class for native gameplay scripts (the engine's MonoBehaviour).
    //
    //   class Rotator : public ScriptableEntity {
    //   public:
    //       float Speed = 90.0f;
    //       void OnUpdate(float dt) override { Transform().Rotation.y += glm::radians(Speed) * dt; }
    //   };
    //   ScriptRegistry::Register<Rotator>("Rotator").Field("Speed", &Rotator::Speed);
    class ScriptableEntity
    {
    public:
        virtual ~ScriptableEntity() = default;

        virtual void OnCreate() {}
        virtual void OnUpdate(float dt) {}
        virtual void OnFixedUpdate(float dt) {}
        virtual void OnDestroy() {}
        virtual void OnCollisionEnter(Entity other) {}
        virtual void OnCollisionExit(Entity other) {}
        virtual void OnTriggerEnter(Entity other) {}
        virtual void OnTriggerExit(Entity other) {}
        virtual void OnClick() {} // UI Button on this entity was clicked

        Entity Self() const { return m_Entity; }
        Scene& GetScene() const { return *m_Scene; }

        template <class T> T& Get() { return m_Scene->GetRegistry().Get<T>(m_Entity); }
        template <class T> T* TryGet() { return m_Scene->GetRegistry().TryGet<T>(m_Entity); }
        template <class T> bool Has() const { return m_Scene->GetRegistry().Has<T>(m_Entity); }
        template <class T, class... A> T& Add(A&&... args) { return m_Scene->GetRegistry().Emplace<T>(m_Entity, std::forward<A>(args)...); }

        TransformComponent& Transform() { return Get<TransformComponent>(); }

        // Another script of type T on this entity (or nullptr), like Unity's GetComponent<MyScript>().
        template <class T> T* GetScript()
        {
            if (auto* sc = TryGet<ScriptComponent>())
                for (auto& s : sc->Scripts)
                    if (auto* t = dynamic_cast<T*>(s.Instance.get())) return t;
            return nullptr;
        }
        const std::string& Name() { return Get<TagComponent>().Name; }

        Entity Find(const std::string& name) const { return m_Scene->FindEntityByName(name); }
        Entity Instantiate(AssetHandle prefab, const glm::vec3& position) { return m_Scene->InstantiatePrefab(prefab, position); }
        void Destroy(Entity e) { m_Scene->DestroyEntityDeferred(e); }
        RaycastHit Raycast(const glm::vec3& origin, const glm::vec3& dir, float maxDistance = 1000.0f)
        {
            return Physics3DWorld::Raycast(*m_Scene, origin, dir, maxDistance);
        }
        RaycastHit2D Raycast2D(const glm::vec2& origin, const glm::vec2& dir, float maxDistance = 1000.0f)
        {
            return Physics2DWorld::Raycast(*m_Scene, origin, dir, maxDistance);
        }

    private:
        friend class Scene;
        Entity m_Entity;
        Scene* m_Scene = nullptr;
    };
}
