#pragma once
#include "Dimension/ECS/Entity.h"
#include <glm/glm.hpp>
#include <cstdint>
#include <vector>

namespace Dimension
{
    enum class Joint2DType : int32_t; // Components.h
}

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
        // layerMask: bit per physics layer (default: all but "Ignore Raycast").
        static RaycastHit2D Raycast(Scene& scene, const glm::vec2& origin, const glm::vec2& direction, float maxDistance,
                                    uint32_t layerMask = ~(1u << 1));
        static std::vector<Entity> OverlapCircle(Scene& scene, const glm::vec2& center, float radius, uint32_t layerMask = ~(1u << 1));
        static std::vector<Entity> OverlapBox(Scene& scene, const glm::vec2& center, const glm::vec2& halfExtents, float angle = 0.0f,
                                              uint32_t layerMask = ~(1u << 1));

        uint32_t GetWorldId() const { return m_World; }
        size_t GetJointCount() const { return m_Joints.size(); }

    private:
        void CreateBodiesRecursive(Entity e);
        void CreateJointsRecursive(Entity e);
        void CreateBody(Entity e);
        void CreateJoint(Entity e);
        void CreateTilemapBody(Entity e);

        Scene& m_Scene;
        uint32_t m_World = 0;  // b2WorldId packed
        uint64_t m_Ground = 0; // static body for joints attached to the world (b2BodyId packed)
        struct JointRecord
        {
            Entity Owner;
            uint64_t Id; // b2JointId packed
            Joint2DType Type;
        };
        std::vector<JointRecord> m_Joints;
        std::vector<uint64_t> m_TilemapBodies; // b2BodyId packed
    };
}
