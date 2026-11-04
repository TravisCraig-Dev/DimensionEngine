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
        // The object's own body made again from its components (after one was removed while playing).
        void RebuildBody(Entity e);
        // The object's colliders' settings (size, offset, trigger, friction, bounciness) applied to its body, keeping its
        // motion, mass and joints.
        void RefreshColliders(Entity e);
        // The object's joint made again from its component (another connected body, anchor, type, limits...; a broken
        // one is mended), or removed when the component is gone. Its bodies wake.
        void RebuildJoint(Entity e);
        // The Rigidbody's settings (Body Type, Mass, damping, Use Gravity, Constraints) applied to its body while
        // playing, keeping its motion and joints.
        void RefreshBody(Entity e);
        // Puts the body (and its Transform) at a world position and rotation (Euler radians): at once (teleport), or for
        // a kinematic body by moving there during the next step, pushing what it meets.
        void MoveBody(Entity e, const glm::vec3& position, const glm::vec3& rotation, bool teleport);

        // While playing (like Unity's Physics settings, for this world only; the project's are left as they are):
        // gravity, which layers collide (a copy of the project's Layer Collision Matrix), and pairs of objects that never
        // collide. Changes wake the bodies they concern, and touching pairs part at once.
        glm::vec3 GetGravity() const;
        void SetGravity(const glm::vec3& gravity);
        bool GetLayersCollide(int a, int b) const;
        void SetLayersCollide(int a, int b, bool collide);
        void SetIgnoreCollision(Entity a, Entity b, bool ignore);
        bool IsCollisionIgnored(Entity a, Entity b) const;

        static void SetLinearVelocity(Scene& scene, Entity e, const glm::vec3& v);
        static glm::vec3 GetLinearVelocity(Scene& scene, Entity e);
        static void AddForce(Scene& scene, Entity e, const glm::vec3& force);
        static void AddImpulse(Scene& scene, Entity e, const glm::vec3& impulse);
        // Spin (world axes, radians per second), torque (N*m), and a force or impulse at a world point (it also turns the
        // body when the point is off its center of mass).
        // Sleeping bodies (at rest for a while) are not simulated until something touches or moves them.
        static void WakeUp(Scene& scene, Entity e);
        static bool IsSleeping(Scene& scene, Entity e);
        static glm::vec3 GetAngularVelocity(Scene& scene, Entity e);
        static void SetAngularVelocity(Scene& scene, Entity e, const glm::vec3& radiansPerSecond);
        static void AddTorque(Scene& scene, Entity e, const glm::vec3& torque);
        static void AddForceAtPosition(Scene& scene, Entity e, const glm::vec3& force, const glm::vec3& point);
        static void AddImpulseAtPosition(Scene& scene, Entity e, const glm::vec3& impulse, const glm::vec3& point);
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
        // everySurface: each surface crossed instead (a mesh's floors one above the other each give a hit).
        static std::vector<RaycastHit> RaycastAll(Scene& scene, const glm::vec3& origin, const glm::vec3& direction, float maxDistance,
                                                  uint32_t layerMask = Physics::kDefaultQueryMask, bool everySurface = false);
        // The first collider a sphere / an oriented box meets when moved along `direction` up to maxDistance (like a thick
        // ray): Point is where they touch, Normal the surface's there, Distance how far the shape moved. A collider the
        // shape starts inside is hit at distance 0.
        static RaycastHit SphereCast(Scene& scene, const glm::vec3& origin, float radius, const glm::vec3& direction, float maxDistance,
                                     uint32_t layerMask = Physics::kDefaultQueryMask);
        static RaycastHit BoxCast(Scene& scene, const glm::vec3& center, const glm::vec3& halfExtents, const glm::quat& rotation,
                                  const glm::vec3& direction, float maxDistance, uint32_t layerMask = Physics::kDefaultQueryMask);
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
