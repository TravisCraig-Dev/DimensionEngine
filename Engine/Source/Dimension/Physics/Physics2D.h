#pragma once
#include "Dimension/ECS/Entity.h"
#include <glm/glm.hpp>
#include <cstdint>

namespace Dimension
{
    class Scene;

    struct RaycastHit2D
    {
        Entity Hit = Entity::Null();
        glm::vec2 Point{ 0.0f };
        glm::vec2 Normal{ 0.0f };
        float Distance = 0.0f;
        explicit operator bool() const { return !Hit.IsNull(); }
    };

    // Box2D v3 world bound to a playing scene. Bodies are created from
    // Rigidbody2D + Box/CircleCollider2D components; colliders without a rigidbody
    // become static bodies. Simulation writes back Position.xy and Rotation.z.
    class Physics2DWorld
    {
    public:
        explicit Physics2DWorld(Scene& scene);
        ~Physics2DWorld();

        void Step(float dt);
        void CreateBodiesFor(Entity root); // includes children
        void OnEntityDestroyed(Entity e);

        // Script-facing helpers (no-ops for entities without a body).
        static void SetLinearVelocity(Scene& scene, Entity e, const glm::vec2& v);
        static glm::vec2 GetLinearVelocity(Scene& scene, Entity e);
        static void ApplyForce(Scene& scene, Entity e, const glm::vec2& force);
        static void ApplyImpulse(Scene& scene, Entity e, const glm::vec2& impulse);
        static RaycastHit2D Raycast(Scene& scene, const glm::vec2& origin, const glm::vec2& direction, float maxDistance);

        uint32_t GetWorldId() const { return m_World; }

    private:
        void CreateBody(Entity e);

        Scene& m_Scene;
        uint32_t m_World = 0; // b2WorldId packed
    };
}
