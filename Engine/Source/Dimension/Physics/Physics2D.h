#pragma once
#include "Dimension/ECS/Entity.h"
#include <glm/glm.hpp>
#include <array>
#include <cstdint>
#include <vector>

struct b2ShapeId;

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
        // The object's own body made again from its components (after one was removed while playing).
        void RebuildBody(Entity e);
        // The object's colliders' settings (size, offset, trigger, friction, bounciness, density) applied to its body,
        // keeping its motion.
        void RefreshColliders(Entity e);
        // The object's joint made again from its component (another connected body, anchor, type, limits...; a broken
        // one is mended), or removed when the component is gone. Its bodies wake.
        void RebuildJoint(Entity e);
        // The Rigidbody 2D's settings (Body Type, Gravity Scale, damping, Fixed Rotation) applied to its body while
        // playing, keeping its motion and joints.
        void RefreshBody(Entity e);
        // Puts the body (and its Transform) at a world position and angle (radians): at once (teleport), or for a
        // kinematic body by moving there during the next step, pushing and carrying what it meets.
        void MoveBody(Entity e, const glm::vec2& position, float angle, bool teleport);
        // Interpolating bodies' objects placed between their last two physics poses (alpha 0..1 of the way there).
        void Interpolate(float alpha);
        // The velocity of a world point moving with the body; its center of mass (world, and in its own space, set
        // until its colliders change); its rotational inertia (kg m^2), set likewise; sleeping at once.
        static glm::vec2 GetPointVelocity(Scene& scene, Entity e, const glm::vec2& point);
        static glm::vec2 GetWorldCenterOfMass(Scene& scene, Entity e);
        static glm::vec2 GetLocalCenterOfMass(Scene& scene, Entity e);
        static void SetLocalCenterOfMass(Scene& scene, Entity e, const glm::vec2& center);
        static float GetInertia(Scene& scene, Entity e);
        static void SetInertia(Scene& scene, Entity e, float inertia);
        static void Sleep(Scene& scene, Entity e);
        static void WakeUp(Scene& scene, Entity e);
        // The body's mass from its colliders' densities, and a new one by scaling those densities.
        float GetMass(Entity e) const;
        void SetMass(Entity e, float mass);

        // While playing (like Unity's Physics settings, for this world only; the project's are left as they are):
        // gravity, which layers collide (a copy of the project's Layer Collision Matrix), and pairs of objects that never
        // collide. Changes wake the bodies they concern, and touching pairs part at once.
        glm::vec2 GetGravity() const;
        void SetGravity(const glm::vec2& gravity);
        bool GetLayersCollide(int a, int b) const;
        void SetLayersCollide(int a, int b, bool collide);
        void SetIgnoreCollision(Entity a, Entity b, bool ignore);
        bool IsCollisionIgnored(Entity a, Entity b) const;

        // Script-facing helpers (no-ops for entities without a body).
        static void SetLinearVelocity(Scene& scene, Entity e, const glm::vec2& v);
        static glm::vec2 GetLinearVelocity(Scene& scene, Entity e);
        static void ApplyForce(Scene& scene, Entity e, const glm::vec2& force);
        static void ApplyImpulse(Scene& scene, Entity e, const glm::vec2& impulse);
        // Spin in radians per second (counterclockwise), and torque in N*m.
        static float GetAngularVelocity(Scene& scene, Entity e);
        static void SetAngularVelocity(Scene& scene, Entity e, float radiansPerSecond);
        static void ApplyTorque(Scene& scene, Entity e, float torque);
        // layerMask: bit per physics layer (default: all but "Ignore Raycast"); hitTriggers false skips trigger colliders.
        static RaycastHit2D Raycast(Scene& scene, const glm::vec2& origin, const glm::vec2& direction, float maxDistance,
                                    uint32_t layerMask = ~(1u << 1), bool hitTriggers = true);
        // Every collider along the ray, nearest first (one hit per object: its nearest).
        static std::vector<RaycastHit2D> RaycastAll(Scene& scene, const glm::vec2& origin, const glm::vec2& direction, float maxDistance,
                                                    uint32_t layerMask = ~(1u << 1), bool hitTriggers = true);
        // The first collider a circle / a box (half extents, turned `angle` radians) meets when moved along `direction` up
        // to maxDistance: Point is where they touch, Distance how far the shape moved (0 for one it starts in).
        static RaycastHit2D CircleCast(Scene& scene, const glm::vec2& center, float radius, const glm::vec2& direction, float maxDistance,
                                       uint32_t layerMask = ~(1u << 1), bool hitTriggers = true);
        static RaycastHit2D BoxCast(Scene& scene, const glm::vec2& center, const glm::vec2& halfExtents, float angle, const glm::vec2& direction,
                                    float maxDistance, uint32_t layerMask = ~(1u << 1), bool hitTriggers = true);
        // The objects whose colliders contain a point (e.g. under the mouse, from ScreenToWorld).
        static std::vector<Entity> OverlapPoint(Scene& scene, const glm::vec2& point, uint32_t layerMask = ~(1u << 1), bool hitTriggers = true);
        static std::vector<Entity> OverlapCircle(Scene& scene, const glm::vec2& center, float radius, uint32_t layerMask = ~(1u << 1),
                                                 bool hitTriggers = true);
        static std::vector<Entity> OverlapBox(Scene& scene, const glm::vec2& center, const glm::vec2& halfExtents, float angle = 0.0f,
                                              uint32_t layerMask = ~(1u << 1), bool hitTriggers = true);
        // The point on the object's colliders nearest to `point` (the point itself when inside one); false without a body.
        static bool ClosestPoint(Scene& scene, Entity e, const glm::vec2& point, glm::vec2& closest);
        // The world box around the object's colliders now; false without a body.
        static bool GetBounds(Scene& scene, Entity e, glm::vec2& min, glm::vec2& max);

        uint32_t GetWorldId() const { return m_World; }
        size_t GetJointCount() const { return m_Joints.size(); }

    private:
        void CreateBodiesRecursive(Entity e);
        void CreateJointsRecursive(Entity e);
        void CreateBody(Entity e);
        void CreateJoint(Entity e);
        void CreateTilemapBody(Entity e);
        void AddShapes(Entity e, uint64_t body);
        void RefilterShapes(Entity e); // filters from the layer matrix again, which also drops its contacts for re-checking
        static bool CustomFilter(b2ShapeId a, b2ShapeId b, void* context);

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
        std::array<uint32_t, 32> m_Matrix{};                   // layer collision matrix while playing
        std::vector<std::pair<Entity, Entity>> m_IgnoredPairs; // never collide
        struct TilemapBody
        {
            Entity Owner;
            uint64_t Id = 0;       // b2BodyId packed (0: no tiles, so no body)
            uint32_t Revision = 0; // the tiles it was made from
        };
        std::vector<TilemapBody> m_TilemapBodies;
        void SyncTilemaps(); // remakes the bodies of tilemaps whose tiles changed (or that went away)
        std::vector<std::pair<uint64_t, uint64_t>> m_Touching; // shape pairs touching now (b2ShapeId packed), for Stay
        std::vector<std::pair<uint64_t, uint64_t>> m_InSensor; // sensor shape, visitor shape: overlapping now
    };
}
