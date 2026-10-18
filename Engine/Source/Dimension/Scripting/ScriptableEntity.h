#pragma once
#include "Dimension/Scene/Scene.h"
#include "Dimension/Physics/Physics2D.h"
#include "Dimension/Physics/Physics3D.h"
#include <algorithm>
#include <functional>

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
        virtual void OnUIValueChanged(float value) {}              // UI Slider value, or UI Toggle (1 = on)
        virtual void OnUITextChanged(const std::string& text) {}   // UI Input Field edited
        virtual void OnUISubmit(const std::string& text) {}        // UI Input Field: Enter pressed
        virtual void OnAnimationEvent(const std::string& name) {}  // Animator state event reached

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

        // Timers (like Unity's Invoke / InvokeRepeating). Callbacks run after OnUpdate while the script is enabled.
        // Returns an id for CancelInvoke(id).
        int Invoke(std::function<void()> fn, float delay) { return AddTimer(std::move(fn), delay, 0.0f); }
        int InvokeRepeating(std::function<void()> fn, float delay, float interval) { return AddTimer(std::move(fn), delay, std::max(interval, 1e-4f)); }
        void CancelInvoke(int id) { std::erase_if(m_Timers, [id](const Timer& t) { return t.Id == id; }); }
        void CancelInvoke() { m_Timers.clear(); }
        bool IsInvoking() const { return !m_Timers.empty(); }
        // Seconds since play mode started (scaled; stops while paused).
        float Time() const { return m_Scene->GetTime(); }

        // Engine-internal: advances timers.
        void TickTimers(float dt)
        {
            if (m_Timers.empty()) return;
            // Callbacks may add or cancel timers: work on a snapshot of the ids due now.
            std::vector<int> due;
            for (auto& t : m_Timers)
                if ((t.Remaining -= dt) <= 0.0f) due.push_back(t.Id);
            for (int id : due)
            {
                auto it = std::find_if(m_Timers.begin(), m_Timers.end(), [id](const Timer& t) { return t.Id == id; });
                if (it == m_Timers.end()) continue;
                auto fn = it->Fn;
                if (it->Interval > 0.0f) it->Remaining += it->Interval;
                else m_Timers.erase(it);
                fn();
            }
        }

    private:
        struct Timer
        {
            int Id;
            float Remaining, Interval;
            std::function<void()> Fn;
        };
        int AddTimer(std::function<void()> fn, float delay, float interval)
        {
            m_Timers.push_back({ ++m_NextTimer, delay, interval, std::move(fn) });
            return m_NextTimer;
        }

        friend class Scene;
        Entity m_Entity;
        Scene* m_Scene = nullptr;
        std::vector<Timer> m_Timers;
        int m_NextTimer = 0;
    };
}
