#pragma once
#include "Dimension/Core/Base.h"
#include "Dimension/ECS/Entity.h"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <vector>

namespace Dimension
{
    class Scene;
    struct RagdollComponent;

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
        // A Ragdoll's capsule nearest `point` takes the impulse there (a hit knocks the limb it lands on).
        static void AddRagdollImpulse(Scene& scene, Entity e, const glm::vec3& impulse, const glm::vec3& point);
        // Dents a Deformable object at a world point, pushing its surface along `direction` (world, into the object) by up
        // to `depth` meters at the point (less out to its Radius). Returns how many vertices moved.
        static int Dent(Scene& scene, Entity e, const glm::vec3& point, const glm::vec3& direction, float depth);
        // Snaps a rope in two between point `segment` and the next (as Break Force and Break Stretch do); once only.
        void SnapRope(Entity e, int segment);
        // Rebuilds a body's collider from its components (after its mesh, dents or terrain changed).
        void RefreshShape(Entity e);
        // Breaks a Breakable object into its pieces now, flying apart from `point` (world space). Returns the pieces.
        static std::vector<Entity> Break(Scene& scene, Entity e, const glm::vec3& point);
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
        // Rebuilds a body's shape from its colliders now (e.g. after its mesh collider's mesh was dented), keeping its
        // motion and joints.
        void CreateJoint(Entity e);
        void CreateCharacter(Entity e);
        void CreateVehicle(Entity e);
        void CreateCloth(Entity e);
        void UpdateBoats(float dt);
        void UpdateHitches(float dt);
        void UpdateWind(float dt);
        uint32_t AddRopeBody(Entity e, const glm::vec3& origin, const std::vector<glm::vec3>& points, bool pinStart, bool pinEnd, int brokenAt);
        void CreateRope(Entity e);
        void UpdateRopes(float dt, bool afterStep);
        void CreateSoftBody(Entity e);
        void CreateRagdoll(Entity e);
        void UpdateRagdolls(float dt, bool afterStep);
        std::vector<Entity> BreakNow(Entity e, const glm::vec3& point);
        void UpdateDebris(float dt);
        // Simulate turned off: aligns the object with the limp body (by its root joint) and starts blending the animation in.
        void BeginGetUp(Entity e, RagdollComponent& comp, const std::vector<glm::mat4>& bindGlobal, int rootJoint);
        void UpdateCloths(float dt, bool afterStep);
        void UpdateVehicles(bool afterStep);
        void UpdateWater(float dt); // buoyancy and drag from Water Volumes
        Scene& m_Scene;
        Scope<Impl> m_Impl;
    };
}
