#pragma once
#include "Dimension/Core/Base.h"
#include "Dimension/ECS/Entity.h"
#include <glm/glm.hpp>

namespace Dimension
{
    class Scene;

    struct RaycastHit
    {
        Entity Hit = Entity::Null();
        glm::vec3 Point{ 0.0f };
        glm::vec3 Normal{ 0.0f };
        float Distance = 0.0f;
        explicit operator bool() const { return !Hit.IsNull(); }
    };

    // Jolt Physics world bound to a playing scene. Bodies come from Rigidbody +
    // Box/SphereCollider components (colliders alone become static bodies).
    // Simulated entities should be hierarchy roots: results are written to the
    // local Position/Rotation.
    class Physics3DWorld
    {
    public:
        explicit Physics3DWorld(Scene& scene);
        ~Physics3DWorld();

        void Step(float dt);
        void CreateBodiesFor(Entity root);
        void OnEntityDestroyed(Entity e);

        static void SetLinearVelocity(Scene& scene, Entity e, const glm::vec3& v);
        static glm::vec3 GetLinearVelocity(Scene& scene, Entity e);
        static void AddForce(Scene& scene, Entity e, const glm::vec3& force);
        static void AddImpulse(Scene& scene, Entity e, const glm::vec3& impulse);
        // Closest hit along a ray (direction need not be normalized).
        static RaycastHit Raycast(Scene& scene, const glm::vec3& origin, const glm::vec3& direction, float maxDistance);

        struct Impl;
        Impl* GetImpl() { return m_Impl.get(); }

    private:
        void CreateBody(Entity e);
        Scene& m_Scene;
        Scope<Impl> m_Impl;
    };
}
