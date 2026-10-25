#pragma once
#include "Dimension/Core/Base.h"
#include "Dimension/ECS/Entity.h"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <vector>

namespace Dimension
{
    class Scene;

    namespace Physics
    {
        // Layer mask used by queries when none is given: every layer except "Ignore Raycast" (layer 1).
        constexpr uint32_t kDefaultQueryMask = ~(1u << 1);
        constexpr uint32_t kAllLayers = 0xFFFFFFFFu;
    }

    struct RaycastHit
    {
        Entity Hit = Entity::Null();
        glm::vec3 Point{ 0.0f };
        glm::vec3 Normal{ 0.0f };
        float Distance = 0.0f;
        explicit operator bool() const { return !Hit.IsNull(); }
    };

    // Jolt Physics world bound to a playing scene. Bodies come from Rigidbody +
    // Box/Sphere/Capsule Collider components (colliders alone become static bodies);
    // Joint components become constraints. Bodies use the entity's Layer and the
    // project's layer collision matrix. Simulated entities should be hierarchy roots:
    // results are written to the local Position/Rotation.
    class Physics3DWorld
    {
    public:
        explicit Physics3DWorld(Scene& scene);
        ~Physics3DWorld();

        void Step(float dt);
        void CreateBodiesFor(Entity root); // bodies, then joints, for the subtree
        void OnEntityDestroyed(Entity e);

        static void SetLinearVelocity(Scene& scene, Entity e, const glm::vec3& v);
        static glm::vec3 GetLinearVelocity(Scene& scene, Entity e);
        static void AddForce(Scene& scene, Entity e, const glm::vec3& force);
        static void AddImpulse(Scene& scene, Entity e, const glm::vec3& impulse);
        // Closest hit along a ray (direction need not be normalized). Triggers are hit too.
        static RaycastHit Raycast(Scene& scene, const glm::vec3& origin, const glm::vec3& direction, float maxDistance,
                                  uint32_t layerMask = Physics::kDefaultQueryMask);
        // Every body along a ray, nearest first (one hit per body; a ray starting inside a body hits it at distance 0).
        static std::vector<RaycastHit> RaycastAll(Scene& scene, const glm::vec3& origin, const glm::vec3& direction, float maxDistance,
                                                  uint32_t layerMask = Physics::kDefaultQueryMask);
        // Entities whose colliders overlap a sphere / an oriented box.
        static std::vector<Entity> OverlapSphere(Scene& scene, const glm::vec3& center, float radius,
                                                 uint32_t layerMask = Physics::kDefaultQueryMask);
        static std::vector<Entity> OverlapBox(Scene& scene, const glm::vec3& center, const glm::vec3& halfExtents,
                                              const glm::quat& rotation = glm::quat(1, 0, 0, 0), uint32_t layerMask = Physics::kDefaultQueryMask);

        size_t GetJointCount() const;

        // Character Controller (moves immediately, using this frame's delta time):
        // Move: displacement for this frame (no gravity). SimpleMove: horizontal speed in units/s, gravity applied.
        static void MoveCharacter(Scene& scene, Entity e, const glm::vec3& motion);
        static void SimpleMoveCharacter(Scene& scene, Entity e, const glm::vec3& speed);

        struct Impl;
        Impl* GetImpl() { return m_Impl.get(); }

    private:
        void CreateBodiesRecursive(Entity e);
        void CreateJointsRecursive(Entity e);
        void CreateBody(Entity e);
        void CreateJoint(Entity e);
        void CreateCharacter(Entity e);
        void CreateVehicle(Entity e);
        void UpdateVehicles(bool afterStep);
        Scene& m_Scene;
        Scope<Impl> m_Impl;
    };
}
