#include "Dimension/Physics/Physics2D.h"
#include "Dimension/Scene/Scene.h"
#include <box2d/box2d.h>
#include <cstring>

namespace Dimension
{
    namespace
    {
        b2WorldId LoadWorld(uint32_t packed) { return b2LoadWorldId(packed); }

        void* PackEntity(Entity e) { return (void*)(((uintptr_t)e.Generation << 32) | e.Index); }
        Entity UnpackEntity(void* p)
        {
            uintptr_t v = (uintptr_t)p;
            return Entity{ (uint32_t)(v & 0xFFFFFFFFu), (uint32_t)(v >> 32) };
        }

        b2BodyId BodyOf(Scene& scene, Entity e)
        {
            auto* rb = scene.GetRegistry().TryGet<Rigidbody2DComponent>(e);
            if (!rb || !rb->RuntimeBody) return b2_nullBodyId;
            b2BodyId id = b2LoadBodyId(rb->RuntimeBody);
            return b2Body_IsValid(id) ? id : b2_nullBodyId;
        }
    }

    Physics2DWorld::Physics2DWorld(Scene& scene) : m_Scene(scene)
    {
        b2WorldDef def = b2DefaultWorldDef();
        auto g = scene.GetSettings().Gravity2D;
        def.gravity = { g.x, g.y };
        m_World = b2StoreWorldId(b2CreateWorld(&def));

        auto& reg = scene.GetRegistry();
        std::vector<Entity> entities;
        reg.EachEntity([&](Entity e) {
            if (reg.Has<Rigidbody2DComponent>(e) || reg.Has<BoxCollider2DComponent>(e) || reg.Has<CircleCollider2DComponent>(e))
                entities.push_back(e);
        });
        for (Entity e : entities) CreateBody(e);
    }

    Physics2DWorld::~Physics2DWorld()
    {
        for (auto [e, rb] : m_Scene.GetRegistry().ViewOf<Rigidbody2DComponent>()) rb.RuntimeBody = 0;
        b2DestroyWorld(LoadWorld(m_World));
    }

    void Physics2DWorld::CreateBodiesFor(Entity root)
    {
        auto& reg = m_Scene.GetRegistry();
        if (reg.Has<Rigidbody2DComponent>(root) || reg.Has<BoxCollider2DComponent>(root) || reg.Has<CircleCollider2DComponent>(root))
            CreateBody(root);
        for (Entity c : m_Scene.GetChildren(root)) CreateBodiesFor(c);
    }

    void Physics2DWorld::CreateBody(Entity e)
    {
        auto& reg = m_Scene.GetRegistry();
        auto& t = reg.Get<TransformComponent>(e);
        if (!reg.Has<Rigidbody2DComponent>(e)) reg.Emplace<Rigidbody2DComponent>(e).Type = BodyType::Static;
        auto& rb = reg.Get<Rigidbody2DComponent>(e);
        if (rb.RuntimeBody) return;

        glm::vec3 worldPos = t.GetWorldPosition();
        b2BodyDef bd = b2DefaultBodyDef();
        bd.type = rb.Type == BodyType::Dynamic ? b2_dynamicBody : rb.Type == BodyType::Kinematic ? b2_kinematicBody : b2_staticBody;
        bd.position = { worldPos.x, worldPos.y };
        bd.rotation = b2MakeRot(t.Rotation.z);
        bd.gravityScale = rb.GravityScale;
        bd.linearDamping = rb.LinearDamping;
        bd.fixedRotation = rb.FixedRotation;
        bd.userData = PackEntity(e);
        bd.isEnabled = t.ActiveInHierarchy;
        b2BodyId body = b2CreateBody(LoadWorld(m_World), &bd);
        rb.RuntimeBody = b2StoreBodyId(body);

        glm::vec2 scale = glm::abs(glm::vec2(t.Scale));
        if (auto* box = reg.TryGet<BoxCollider2DComponent>(e))
        {
            b2ShapeDef sd = b2DefaultShapeDef();
            sd.density = box->Density;
            sd.material.friction = box->Friction;
            sd.material.restitution = box->Restitution;
            sd.isSensor = box->IsTrigger;
            sd.enableSensorEvents = true;
            sd.enableContactEvents = true;
            sd.userData = PackEntity(e);
            b2Polygon poly = b2MakeOffsetBox(box->Size.x * 0.5f * scale.x, box->Size.y * 0.5f * scale.y,
                                             { box->Offset.x * scale.x, box->Offset.y * scale.y }, b2Rot_identity);
            b2CreatePolygonShape(body, &sd, &poly);
        }
        if (auto* circle = reg.TryGet<CircleCollider2DComponent>(e))
        {
            b2ShapeDef sd = b2DefaultShapeDef();
            sd.density = circle->Density;
            sd.material.friction = circle->Friction;
            sd.material.restitution = circle->Restitution;
            sd.isSensor = circle->IsTrigger;
            sd.enableSensorEvents = true;
            sd.enableContactEvents = true;
            sd.userData = PackEntity(e);
            b2Circle c{ { circle->Offset.x * scale.x, circle->Offset.y * scale.y }, circle->Radius * std::max(scale.x, scale.y) };
            b2CreateCircleShape(body, &sd, &c);
        }
    }

    void Physics2DWorld::OnEntityDestroyed(Entity e)
    {
        b2BodyId body = BodyOf(m_Scene, e);
        if (B2_IS_NON_NULL(body)) b2DestroyBody(body);
        if (auto* rb = m_Scene.GetRegistry().TryGet<Rigidbody2DComponent>(e)) rb->RuntimeBody = 0;
    }

    void Physics2DWorld::Step(float dt)
    {
        auto& reg = m_Scene.GetRegistry();
        b2WorldId world = LoadWorld(m_World);

        // Kinematic/static bodies follow their transforms (moved by scripts).
        for (auto [e, rb, t] : reg.ViewOf<Rigidbody2DComponent, TransformComponent>())
        {
            if (!rb.RuntimeBody || rb.Type == BodyType::Dynamic) continue;
            b2BodyId id = b2LoadBodyId(rb.RuntimeBody);
            glm::vec3 p = t.GetWorldPosition();
            b2Body_SetTransform(id, { p.x, p.y }, b2MakeRot(t.Rotation.z));
        }

        b2World_Step(world, dt, 4);

        for (auto [e, rb, t] : reg.ViewOf<Rigidbody2DComponent, TransformComponent>())
        {
            if (!rb.RuntimeBody || rb.Type != BodyType::Dynamic) continue;
            b2BodyId id = b2LoadBodyId(rb.RuntimeBody);
            b2Vec2 p = b2Body_GetPosition(id);
            t.Position.x = p.x;
            t.Position.y = p.y;
            t.Rotation.z = b2Rot_GetAngle(b2Body_GetRotation(id));
        }

        // Dispatch events after the step (collect first: callbacks may destroy bodies).
        struct Event { Entity A, B; bool Begin, Trigger; };
        std::vector<Event> events;
        b2ContactEvents contacts = b2World_GetContactEvents(world);
        for (int i = 0; i < contacts.beginCount; ++i)
            events.push_back({ UnpackEntity(b2Shape_GetUserData(contacts.beginEvents[i].shapeIdA)),
                               UnpackEntity(b2Shape_GetUserData(contacts.beginEvents[i].shapeIdB)), true, false });
        for (int i = 0; i < contacts.endCount; ++i)
        {
            auto& ev = contacts.endEvents[i];
            if (!b2Shape_IsValid(ev.shapeIdA) || !b2Shape_IsValid(ev.shapeIdB)) continue;
            events.push_back({ UnpackEntity(b2Shape_GetUserData(ev.shapeIdA)), UnpackEntity(b2Shape_GetUserData(ev.shapeIdB)), false, false });
        }
        b2SensorEvents sensors = b2World_GetSensorEvents(world);
        for (int i = 0; i < sensors.beginCount; ++i)
            events.push_back({ UnpackEntity(b2Shape_GetUserData(sensors.beginEvents[i].sensorShapeId)),
                               UnpackEntity(b2Shape_GetUserData(sensors.beginEvents[i].visitorShapeId)), true, true });
        for (int i = 0; i < sensors.endCount; ++i)
        {
            auto& ev = sensors.endEvents[i];
            if (!b2Shape_IsValid(ev.sensorShapeId) || !b2Shape_IsValid(ev.visitorShapeId)) continue;
            events.push_back({ UnpackEntity(b2Shape_GetUserData(ev.sensorShapeId)), UnpackEntity(b2Shape_GetUserData(ev.visitorShapeId)), false, true });
        }
        for (auto& ev : events) m_Scene.DispatchCollision(ev.A, ev.B, ev.Begin, ev.Trigger);
    }

    void Physics2DWorld::SetLinearVelocity(Scene& scene, Entity e, const glm::vec2& v)
    {
        b2BodyId id = BodyOf(scene, e);
        if (B2_IS_NON_NULL(id)) b2Body_SetLinearVelocity(id, { v.x, v.y });
    }

    glm::vec2 Physics2DWorld::GetLinearVelocity(Scene& scene, Entity e)
    {
        b2BodyId id = BodyOf(scene, e);
        if (B2_IS_NULL(id)) return glm::vec2(0.0f);
        b2Vec2 v = b2Body_GetLinearVelocity(id);
        return { v.x, v.y };
    }

    void Physics2DWorld::ApplyForce(Scene& scene, Entity e, const glm::vec2& f)
    {
        b2BodyId id = BodyOf(scene, e);
        if (B2_IS_NON_NULL(id)) b2Body_ApplyForceToCenter(id, { f.x, f.y }, true);
    }

    void Physics2DWorld::ApplyImpulse(Scene& scene, Entity e, const glm::vec2& i)
    {
        b2BodyId id = BodyOf(scene, e);
        if (B2_IS_NON_NULL(id)) b2Body_ApplyLinearImpulseToCenter(id, { i.x, i.y }, true);
    }

    RaycastHit2D Physics2DWorld::Raycast(Scene& scene, const glm::vec2& origin, const glm::vec2& direction, float maxDistance)
    {
        RaycastHit2D result;
        Physics2DWorld* world = scene.GetPhysics2D();
        float len = glm::length(direction);
        if (!world || len <= 0.0f) return result;
        glm::vec2 translation = direction / len * maxDistance;
        b2RayResult hit = b2World_CastRayClosest(LoadWorld(world->m_World), { origin.x, origin.y }, { translation.x, translation.y },
                                                 b2DefaultQueryFilter());
        if (!hit.hit) return result;
        result.Hit = UnpackEntity(b2Shape_GetUserData(hit.shapeId));
        result.Point = { hit.point.x, hit.point.y };
        result.Normal = { hit.normal.x, hit.normal.y };
        result.Distance = hit.fraction * maxDistance;
        return result;
    }
}
