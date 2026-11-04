#include "Dimension/Physics/Physics2D.h"
#include "Dimension/Scene/Scene.h"
#include "Dimension/Project/Project.h"
#include "Dimension/Core/Log.h"
#include <box2d/box2d.h>
#include <algorithm>
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

        b2Filter LayerFilter(Scene& scene, Entity e)
        {
            int layer = std::clamp(scene.GetRegistry().Get<TagComponent>(e).Layer, 0, 31);
            b2Filter f = b2DefaultFilter();
            f.categoryBits = 1ull << layer;
            f.maskBits = Project::GetConfig().LayerCollisionMatrix[layer];
            return f;
        }

        b2QueryFilter MaskQuery(uint32_t layerMask)
        {
            b2QueryFilter q = b2DefaultQueryFilter();
            q.categoryBits = 0xFFFFFFFFull; // a query collides with every shape mask
            q.maskBits = layerMask;
            return q;
        }

        // The collision as shape A's object sees it, from the contact between the two shapes after the step; `begin` is the
        // manifold recorded when they started touching, used when the contact has none now.
        Collision ContactBetween(b2ShapeId a, b2ShapeId b, const b2Manifold* begin)
        {
            Collision c;
            b2Manifold manifold{};
            bool found = false, flip = false;
            int capacity = b2Shape_GetContactCapacity(a);
            if (capacity > 0)
            {
                std::vector<b2ContactData> data(capacity);
                int n = b2Shape_GetContactData(a, data.data(), capacity);
                for (int i = 0; i < n && !found; ++i)
                {
                    bool ab = B2_ID_EQUALS(data[i].shapeIdA, a) && B2_ID_EQUALS(data[i].shapeIdB, b);
                    bool ba = B2_ID_EQUALS(data[i].shapeIdA, b) && B2_ID_EQUALS(data[i].shapeIdB, a);
                    if ((ab || ba) && data[i].manifold.pointCount > 0) { manifold = data[i].manifold; found = true; flip = ba; }
                }
            }
            if (!found && begin) manifold = *begin;
            // Box2D's normal points from its shape A to B: toward our A it is the other way.
            glm::vec2 n(manifold.normal.x, manifold.normal.y);
            glm::vec2 towardA = flip ? n : -n;
            float closing = 0.0f; // pre-solve speed of A toward B along the normal
            for (int i = 0; i < manifold.pointCount; ++i)
            {
                const b2ManifoldPoint& p = manifold.points[i];
                c.Contacts.push_back({ { p.point.x, p.point.y, 0.0f }, { towardA, 0.0f }, p.separation });
                closing = std::max(closing, -p.normalVelocity);
            }
            c.Normal = { towardA, 0.0f };
            // Now: A's velocity minus B's, with its part along the normal as it was before the solver pushed them apart.
            b2Vec2 va = b2Body_GetLinearVelocity(b2Shape_GetBody(a)), vb = b2Body_GetLinearVelocity(b2Shape_GetBody(b));
            glm::vec2 rel(va.x - vb.x, va.y - vb.y), alongAB = -towardA;
            rel += alongAB * (closing - glm::dot(rel, alongAB));
            c.RelativeVelocity = { rel, 0.0f };
            // An inelastic hit's impulse, as in 3D: the closing speed times the pair's reduced mass (Box2D's own totals
            // count every substep and the push out of overlap).
            auto inverseMass = [](b2BodyId body) {
                return b2Body_GetType(body) == b2_dynamicBody && b2Body_GetMass(body) > 0.0f ? 1.0f / b2Body_GetMass(body) : 0.0f;
            };
            float inv = inverseMass(b2Shape_GetBody(a)) + inverseMass(b2Shape_GetBody(b));
            c.Impulse = inv > 0.0f ? closing / inv : 0.0f;
            return c;
        }

        bool CollectEntity(b2ShapeId shape, void* context)
        {
            auto* out = static_cast<std::vector<Entity>*>(context);
            Entity e = UnpackEntity(b2Shape_GetUserData(shape));
            if (std::find(out->begin(), out->end(), e) == out->end()) out->push_back(e);
            return true;
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
        std::vector<Entity> tilemaps;
        for (auto [e, tm] : reg.ViewOf<TilemapComponent>())
            if (tm.Collider) tilemaps.push_back(e);
        for (Entity e : tilemaps) CreateTilemapBody(e);
        std::vector<Entity> jointed;
        for (auto [e, j] : reg.ViewOf<Joint2DComponent>()) jointed.push_back(e);
        for (Entity e : jointed) CreateJoint(e);
    }

    Physics2DWorld::~Physics2DWorld()
    {
        for (auto [e, rb] : m_Scene.GetRegistry().ViewOf<Rigidbody2DComponent>()) rb.RuntimeBody = 0;
        b2DestroyWorld(LoadWorld(m_World));
    }

    void Physics2DWorld::CreateBodiesFor(Entity root)
    {
        CreateBodiesRecursive(root);
        CreateJointsRecursive(root);
    }

    void Physics2DWorld::CreateBodiesRecursive(Entity e)
    {
        auto& reg = m_Scene.GetRegistry();
        if (auto* tm = reg.TryGet<TilemapComponent>(e); tm && tm->Collider) CreateTilemapBody(e);
        if (reg.Has<Rigidbody2DComponent>(e) || reg.Has<BoxCollider2DComponent>(e) || reg.Has<CircleCollider2DComponent>(e))
            CreateBody(e);
        for (Entity c : m_Scene.GetChildren(e)) CreateBodiesRecursive(c);
    }

    void Physics2DWorld::CreateJointsRecursive(Entity e)
    {
        if (m_Scene.GetRegistry().Has<Joint2DComponent>(e)) CreateJoint(e);
        for (Entity c : m_Scene.GetChildren(e)) CreateJointsRecursive(c);
    }

    void Physics2DWorld::CreateTilemapBody(Entity e)
    {
        auto& reg = m_Scene.GetRegistry();
        auto& tm = reg.Get<TilemapComponent>(e);
        auto& t = reg.Get<TransformComponent>(e);
        std::erase_if(m_TilemapBodies, [&](const TilemapBody& b) {
            if (b.Owner != e) return false;
            if (b.Id) b2DestroyBody(b2LoadBodyId(b.Id));
            return true;
        });
        TilemapBody record{ e, 0, tm.Revision() };
        auto boxes = tm.ColliderBoxes();
        if (boxes.empty())
        {
            m_TilemapBodies.push_back(record); // painted later: SyncTilemaps makes it then
            return;
        }
        glm::vec3 p = t.GetWorldPosition();
        glm::vec2 scale = glm::abs(glm::vec2(t.Scale));
        b2BodyDef bd = b2DefaultBodyDef();
        bd.type = b2_staticBody;
        bd.position = { p.x, p.y };
        bd.rotation = b2MakeRot(t.Rotation.z);
        bd.userData = PackEntity(e);
        b2BodyId body = b2CreateBody(LoadWorld(m_World), &bd);
        b2ShapeDef sd = b2DefaultShapeDef();
        sd.material.friction = tm.Friction;
        sd.filter = LayerFilter(m_Scene, e);
        sd.enableContactEvents = true;
        sd.userData = PackEntity(e);
        for (auto& b : boxes)
        {
            glm::vec2 half = glm::vec2(b.z, b.w) * 0.5f * scale;
            glm::vec2 center = (glm::vec2(b.x, b.y) + glm::vec2(b.z, b.w) * 0.5f) * scale;
            b2Polygon poly = b2MakeOffsetBox(half.x, half.y, { center.x, center.y }, b2Rot_identity);
            b2CreatePolygonShape(body, &sd, &poly);
        }
        record.Id = b2StoreBodyId(body);
        m_TilemapBodies.push_back(record);
    }

    void Physics2DWorld::SyncTilemaps()
    {
        auto& reg = m_Scene.GetRegistry();
        std::vector<Entity> changed;
        for (auto it = m_TilemapBodies.begin(); it != m_TilemapBodies.end();)
        {
            auto* tm = m_Scene.IsValid(it->Owner) ? reg.TryGet<TilemapComponent>(it->Owner) : nullptr;
            if (!tm || !tm->Collider)
            {
                if (it->Id) b2DestroyBody(b2LoadBodyId(it->Id));
                it = m_TilemapBodies.erase(it);
                continue;
            }
            if (tm->Revision() != it->Revision) changed.push_back(it->Owner);
            ++it;
        }
        for (Entity e : changed) CreateTilemapBody(e);
    }

    void Physics2DWorld::CreateJoint(Entity e)
    {
        auto& reg = m_Scene.GetRegistry();
        auto& j = reg.Get<Joint2DComponent>(e);
        b2BodyId a = BodyOf(m_Scene, e);
        if (B2_IS_NULL(a))
        {
            Log::Warn("Joint 2D on '{}' needs a Rigidbody 2D with a collider", reg.Get<TagComponent>(e).Name);
            return;
        }
        b2BodyId b = b2_nullBodyId;
        Entity other = j.ConnectedBody;
        bool connected = m_Scene.IsValid(other) && other != e;
        if (connected)
        {
            b = BodyOf(m_Scene, other);
            if (B2_IS_NULL(b))
            {
                Log::Warn("Joint 2D on '{}': connected body has no physics body", reg.Get<TagComponent>(e).Name);
                return;
            }
        }
        else
        {
            if (!m_Ground)
            {
                b2BodyDef gd = b2DefaultBodyDef();
                gd.userData = PackEntity(Entity::Null());
                m_Ground = b2StoreBodyId(b2CreateBody(LoadWorld(m_World), &gd));
            }
            b = b2LoadBodyId(m_Ground);
        }

        const auto& ta = reg.Get<TransformComponent>(e);
        glm::vec2 anchor = glm::vec2(ta.World * glm::vec4(j.Anchor, 0.0f, 1.0f));
        b2Vec2 aw{ anchor.x, anchor.y };
        float referenceAngle = b2Rot_GetAngle(b2Body_GetRotation(b)) - b2Rot_GetAngle(b2Body_GetRotation(a));
        b2WorldId world = LoadWorld(m_World);
        b2JointId created = b2_nullJointId;
        switch (j.Type)
        {
        case Joint2DType::Fixed:
        {
            b2WeldJointDef d = b2DefaultWeldJointDef();
            d.bodyIdA = a;
            d.bodyIdB = b;
            d.localAnchorA = b2Body_GetLocalPoint(a, aw);
            d.localAnchorB = b2Body_GetLocalPoint(b, aw);
            d.referenceAngle = referenceAngle;
            d.collideConnected = j.CollideConnected;
            created = b2CreateWeldJoint(world, &d);
            break;
        }
        case Joint2DType::Hinge:
        {
            b2RevoluteJointDef d = b2DefaultRevoluteJointDef();
            d.bodyIdA = a;
            d.bodyIdB = b;
            d.localAnchorA = b2Body_GetLocalPoint(a, aw);
            d.localAnchorB = b2Body_GetLocalPoint(b, aw);
            d.referenceAngle = referenceAngle;
            d.enableLimit = j.UseLimits;
            d.lowerAngle = glm::radians(std::min(j.LowerAngle, j.UpperAngle));
            d.upperAngle = glm::radians(std::max(j.LowerAngle, j.UpperAngle));
            d.collideConnected = j.CollideConnected;
            d.enableMotor = j.UseMotor;
            d.motorSpeed = -glm::radians(j.MotorSpeed); // Box2D drives B relative to A; positive turns this entity counter-clockwise
            d.maxMotorTorque = std::max(j.MotorMaxTorque, 0.0f);
            created = b2CreateRevoluteJoint(world, &d);
            break;
        }
        case Joint2DType::Distance:
        {
            glm::vec2 pointB = connected ? glm::vec2(reg.Get<TransformComponent>(other).World * glm::vec4(j.ConnectedAnchor, 0.0f, 1.0f))
                                         : j.ConnectedAnchor;
            b2DistanceJointDef d = b2DefaultDistanceJointDef();
            d.bodyIdA = a;
            d.bodyIdB = b;
            d.localAnchorA = b2Body_GetLocalPoint(a, aw);
            d.localAnchorB = b2Body_GetLocalPoint(b, { pointB.x, pointB.y });
            d.length = j.Distance > 0.0f ? j.Distance : std::max(glm::distance(anchor, pointB), 0.01f);
            d.enableSpring = j.SpringFrequency > 0.0f;
            d.hertz = j.SpringFrequency;
            d.dampingRatio = std::max(j.SpringDamping, 0.0f);
            d.enableMotor = j.UseMotor && d.enableSpring; // a rigid rod has nothing to drive
            d.motorSpeed = j.MotorSpeed;
            d.maxMotorForce = std::max(j.MotorMaxTorque, 0.0f);
            d.collideConnected = j.CollideConnected;
            created = b2CreateDistanceJoint(world, &d);
            break;
        }
        case Joint2DType::Slider:
        {
            // Box2D measures the connected body's translation relative to this one, so this entity's travel is negated.
            glm::vec2 axis = glm::vec2(ta.World * glm::vec4(j.Axis, 0.0f, 0.0f));
            axis = glm::length(axis) > 1e-6f ? glm::normalize(axis) : glm::vec2(1.0f, 0.0f);
            b2PrismaticJointDef d = b2DefaultPrismaticJointDef();
            d.bodyIdA = a;
            d.bodyIdB = b;
            d.localAnchorA = b2Body_GetLocalPoint(a, aw);
            d.localAnchorB = b2Body_GetLocalPoint(b, aw);
            d.localAxisA = b2Body_GetLocalVector(a, { axis.x, axis.y });
            d.referenceAngle = referenceAngle;
            d.enableLimit = j.UseLimits;
            d.lowerTranslation = -std::max(j.UpperTranslation, 0.0f);
            d.upperTranslation = -std::min(j.LowerTranslation, 0.0f);
            d.enableMotor = j.UseMotor;
            d.motorSpeed = -j.MotorSpeed;
            d.maxMotorForce = std::max(j.MotorMaxTorque, 0.0f);
            d.collideConnected = j.CollideConnected;
            created = b2CreatePrismaticJoint(world, &d);
            break;
        }
        }
        if (B2_IS_NULL(created)) return;
        m_Joints.push_back({ e, b2StoreJointId(created), j.Type });
        j.Broken = false;
        j.CurrentForce = j.CurrentTorque = 0.0f;
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
            sd.filter = LayerFilter(m_Scene, e);
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
            sd.filter = LayerFilter(m_Scene, e);
            sd.enableSensorEvents = true;
            sd.enableContactEvents = true;
            sd.userData = PackEntity(e);
            b2Circle c{ { circle->Offset.x * scale.x, circle->Offset.y * scale.y }, circle->Radius * std::max(scale.x, scale.y) };
            b2CreateCircleShape(body, &sd, &c);
        }
    }

    void Physics2DWorld::RebuildBody(Entity e)
    {
        OnEntityDestroyed(e);
        auto& reg = m_Scene.GetRegistry();
        if (reg.Has<Rigidbody2DComponent>(e) || reg.Has<BoxCollider2DComponent>(e) || reg.Has<CircleCollider2DComponent>(e)) CreateBody(e);
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
        SyncTilemaps(); // tiles painted or cleared while playing

        // Bodies of inactive objects are disabled, and enabled again (where the object now is) when it is activated.
        for (auto [e, rb, t] : reg.ViewOf<Rigidbody2DComponent, TransformComponent>())
        {
            if (!rb.RuntimeBody) continue;
            b2BodyId id = b2LoadBodyId(rb.RuntimeBody);
            // A layer changed while playing (a script's Layer): every shape takes the new filter.
            b2ShapeId shapes[16];
            int shapeCount = b2Body_GetShapes(id, shapes, 16);
            if (shapeCount > 0 && b2Shape_GetFilter(shapes[0]).categoryBits != LayerFilter(m_Scene, e).categoryBits)
                for (int i = 0; i < shapeCount; ++i) b2Shape_SetFilter(shapes[i], LayerFilter(m_Scene, e));
            if (b2Body_IsEnabled(id) == t.ActiveInHierarchy) continue;
            if (t.ActiveInHierarchy)
            {
                glm::vec3 p = t.GetWorldPosition();
                b2Body_SetTransform(id, { p.x, p.y }, b2MakeRot(t.Rotation.z));
                b2Body_Enable(id);
            }
            else
                b2Body_Disable(id);
        }

        // Kinematic/static bodies follow their transforms (moved by scripts).
        for (auto [e, rb, t] : reg.ViewOf<Rigidbody2DComponent, TransformComponent>())
        {
            if (!rb.RuntimeBody || rb.Type == BodyType::Dynamic) continue;
            b2BodyId id = b2LoadBodyId(rb.RuntimeBody);
            glm::vec3 p = t.GetWorldPosition();
            b2Body_SetTransform(id, { p.x, p.y }, b2MakeRot(t.Rotation.z));
        }

        // Hinge motors follow their components (scripts may change them while playing).
        std::erase_if(m_Joints, [](const JointRecord& r) { return !b2Joint_IsValid(b2LoadJointId(r.Id)); }); // removed with a body
        for (auto& r : m_Joints)
        {
            auto* jc = reg.TryGet<Joint2DComponent>(r.Owner);
            if (!jc) continue;
            b2JointId id = b2LoadJointId(r.Id);
            if (r.Type == Joint2DType::Hinge)
            {
                b2RevoluteJoint_EnableMotor(id, jc->UseMotor);
                b2RevoluteJoint_SetMotorSpeed(id, -glm::radians(jc->MotorSpeed));
                b2RevoluteJoint_SetMaxMotorTorque(id, std::max(jc->MotorMaxTorque, 0.0f));
            }
            else if (r.Type == Joint2DType::Slider)
            {
                b2PrismaticJoint_EnableMotor(id, jc->UseMotor);
                b2PrismaticJoint_SetMotorSpeed(id, -jc->MotorSpeed);
                b2PrismaticJoint_SetMaxMotorForce(id, std::max(jc->MotorMaxTorque, 0.0f));
            }
            else if (r.Type == Joint2DType::Distance && jc->SpringFrequency > 0.0f)
            {
                b2DistanceJoint_EnableMotor(id, jc->UseMotor);
                b2DistanceJoint_SetMotorSpeed(id, jc->MotorSpeed);
                b2DistanceJoint_SetMaxMotorForce(id, std::max(jc->MotorMaxTorque, 0.0f));
            }
            else continue;
            if (jc->UseMotor) b2Joint_WakeBodies(id);
        }

        b2World_Step(world, dt, 4);

        // Joint reactions; joints over their break force or torque (excluding the motor's own torque) are destroyed.
        std::vector<std::pair<Entity, float>> broken;
        for (auto it = m_Joints.begin(); it != m_Joints.end();)
        {
            b2JointId id = b2LoadJointId(it->Id);
            auto* jc = reg.TryGet<Joint2DComponent>(it->Owner);
            if (!jc || !b2Joint_IsValid(id))
            {
                ++it;
                continue;
            }
            b2Vec2 f = b2Joint_GetConstraintForce(id);
            float torque = b2Joint_GetConstraintTorque(id);
            if (it->Type == Joint2DType::Hinge) torque -= b2RevoluteJoint_GetMotorTorque(id);
            if (it->Type == Joint2DType::Slider)
            {
                // Remove the motor's own push along the axis.
                b2Vec2 axis = b2Body_GetWorldVector(b2Joint_GetBodyA(id), b2Joint_GetLocalAxisA(id));
                float motor = b2PrismaticJoint_GetMotorForce(id);
                f.x -= axis.x * motor;
                f.y -= axis.y * motor;
            }
            jc->CurrentForce = std::sqrt(f.x * f.x + f.y * f.y);
            jc->CurrentTorque = std::abs(torque);
            if ((jc->BreakForce > 0.0f && jc->CurrentForce > jc->BreakForce) || (jc->BreakTorque > 0.0f && jc->CurrentTorque > jc->BreakTorque))
            {
                jc->Broken = true;
                broken.emplace_back(it->Owner, jc->CurrentForce);
                b2DestroyJoint(id);
                it = m_Joints.erase(it);
                continue;
            }
            ++it;
        }

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
        struct Event { Entity A, B; CollisionPhase Phase; bool Trigger; Collision C; };
        std::vector<Event> events;
        b2ContactEvents contacts = b2World_GetContactEvents(world);
        auto key = [](b2ShapeId a, b2ShapeId b) { return std::make_pair(b2StoreShapeId(a), b2StoreShapeId(b)); };
        auto touching = [&](uint64_t a, uint64_t b) {
            return std::find_if(m_Touching.begin(), m_Touching.end(), [&](auto& p) { return (p.first == a && p.second == b) || (p.first == b && p.second == a); });
        };
        std::vector<std::pair<uint64_t, uint64_t>> begun;
        for (int i = 0; i < contacts.beginCount; ++i)
        {
            auto& ev = contacts.beginEvents[i];
            if (!b2Shape_IsValid(ev.shapeIdA) || !b2Shape_IsValid(ev.shapeIdB)) continue;
            events.push_back({ UnpackEntity(b2Shape_GetUserData(ev.shapeIdA)), UnpackEntity(b2Shape_GetUserData(ev.shapeIdB)), CollisionPhase::Enter,
                               false, ContactBetween(ev.shapeIdA, ev.shapeIdB, &ev.manifold) });
            begun.push_back(key(ev.shapeIdA, ev.shapeIdB));
        }
        for (int i = 0; i < contacts.endCount; ++i)
        {
            auto& ev = contacts.endEvents[i];
            if (auto it = touching(b2StoreShapeId(ev.shapeIdA), b2StoreShapeId(ev.shapeIdB)); it != m_Touching.end()) m_Touching.erase(it);
            if (!b2Shape_IsValid(ev.shapeIdA) || !b2Shape_IsValid(ev.shapeIdB)) continue;
            events.push_back({ UnpackEntity(b2Shape_GetUserData(ev.shapeIdA)), UnpackEntity(b2Shape_GetUserData(ev.shapeIdB)), CollisionPhase::Exit,
                               false, {} });
        }
        // Pairs still touching from before this step: Stay, for objects with scripts.
        std::erase_if(m_Touching, [](auto& p) { return !b2Shape_IsValid(b2LoadShapeId(p.first)) || !b2Shape_IsValid(b2LoadShapeId(p.second)); });
        for (auto& [ka, kb] : m_Touching)
        {
            b2ShapeId a = b2LoadShapeId(ka), b = b2LoadShapeId(kb);
            Entity ea = UnpackEntity(b2Shape_GetUserData(a)), eb = UnpackEntity(b2Shape_GetUserData(b));
            bool scripted = (reg.Valid(ea) && reg.Has<ScriptComponent>(ea)) || (reg.Valid(eb) && reg.Has<ScriptComponent>(eb));
            bool awake = b2Body_IsAwake(b2Shape_GetBody(a)) || b2Body_IsAwake(b2Shape_GetBody(b)); // as in 3D: none while asleep
            if (scripted && awake) events.push_back({ ea, eb, CollisionPhase::Stay, false, ContactBetween(a, b, nullptr) });
        }
        for (auto& p : begun)
            if (touching(p.first, p.second) == m_Touching.end()) m_Touching.push_back(p);
        b2SensorEvents sensors = b2World_GetSensorEvents(world);
        auto inSensor = [&](uint64_t s, uint64_t v) {
            return std::find_if(m_InSensor.begin(), m_InSensor.end(), [&](auto& p) { return p.first == s && p.second == v; });
        };
        // Overlaps from before this step: Stay, for objects with scripts (as for contacts, none while both sleep).
        std::erase_if(m_InSensor, [](auto& p) { return !b2Shape_IsValid(b2LoadShapeId(p.first)) || !b2Shape_IsValid(b2LoadShapeId(p.second)); });
        for (int i = 0; i < sensors.endCount; ++i)
            if (auto it = inSensor(b2StoreShapeId(sensors.endEvents[i].sensorShapeId), b2StoreShapeId(sensors.endEvents[i].visitorShapeId));
                it != m_InSensor.end())
                m_InSensor.erase(it);
        for (auto& [ks, kv] : m_InSensor)
        {
            b2ShapeId s = b2LoadShapeId(ks), v = b2LoadShapeId(kv);
            Entity es = UnpackEntity(b2Shape_GetUserData(s)), ev = UnpackEntity(b2Shape_GetUserData(v));
            bool scripted = (reg.Valid(es) && reg.Has<ScriptComponent>(es)) || (reg.Valid(ev) && reg.Has<ScriptComponent>(ev));
            bool awake = b2Body_IsAwake(b2Shape_GetBody(s)) || b2Body_IsAwake(b2Shape_GetBody(v));
            if (scripted && awake) events.push_back({ es, ev, CollisionPhase::Stay, true, {} });
        }
        for (int i = 0; i < sensors.beginCount; ++i)
        {
            auto& ev = sensors.beginEvents[i];
            events.push_back({ UnpackEntity(b2Shape_GetUserData(ev.sensorShapeId)), UnpackEntity(b2Shape_GetUserData(ev.visitorShapeId)),
                               CollisionPhase::Enter, true, {} });
            if (b2Shape_IsValid(ev.sensorShapeId) && b2Shape_IsValid(ev.visitorShapeId) &&
                inSensor(b2StoreShapeId(ev.sensorShapeId), b2StoreShapeId(ev.visitorShapeId)) == m_InSensor.end())
                m_InSensor.push_back({ b2StoreShapeId(ev.sensorShapeId), b2StoreShapeId(ev.visitorShapeId) });
        }
        for (int i = 0; i < sensors.endCount; ++i)
        {
            auto& ev = sensors.endEvents[i];
            if (!b2Shape_IsValid(ev.sensorShapeId) || !b2Shape_IsValid(ev.visitorShapeId)) continue;
            events.push_back({ UnpackEntity(b2Shape_GetUserData(ev.sensorShapeId)), UnpackEntity(b2Shape_GetUserData(ev.visitorShapeId)),
                               CollisionPhase::Exit, true, {} });
        }
        for (auto& ev : events) m_Scene.DispatchCollision(ev.A, ev.B, ev.Phase, ev.Trigger, &ev.C);
        for (auto& [e, force] : broken) m_Scene.DispatchJointBreak(e, force);
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

    float Physics2DWorld::GetAngularVelocity(Scene& scene, Entity e)
    {
        b2BodyId id = BodyOf(scene, e);
        return B2_IS_NON_NULL(id) ? b2Body_GetAngularVelocity(id) : 0.0f;
    }

    void Physics2DWorld::SetAngularVelocity(Scene& scene, Entity e, float w)
    {
        b2BodyId id = BodyOf(scene, e);
        if (B2_IS_NON_NULL(id)) b2Body_SetAngularVelocity(id, w);
    }

    void Physics2DWorld::ApplyTorque(Scene& scene, Entity e, float torque)
    {
        b2BodyId id = BodyOf(scene, e);
        if (B2_IS_NON_NULL(id)) b2Body_ApplyTorque(id, torque, true);
    }

    std::vector<RaycastHit2D> Physics2DWorld::RaycastAll(Scene& scene, const glm::vec2& origin, const glm::vec2& direction, float maxDistance,
                                                         uint32_t layerMask)
    {
        struct Context { std::vector<RaycastHit2D> Hits; float MaxDistance; } ctx{ {}, maxDistance };
        Physics2DWorld* world = scene.GetPhysics2D();
        float len = glm::length(direction);
        if (!world || len <= 0.0f) return {};
        glm::vec2 translation = direction / len * maxDistance;
        auto collect = [](b2ShapeId shape, b2Vec2 point, b2Vec2 normal, float fraction, void* context) -> float {
            auto& c = *static_cast<Context*>(context);
            RaycastHit2D hit{ UnpackEntity(b2Shape_GetUserData(shape)), { point.x, point.y }, { normal.x, normal.y }, fraction * c.MaxDistance };
            auto same = std::find_if(c.Hits.begin(), c.Hits.end(), [&](const RaycastHit2D& h) { return h.Hit == hit.Hit; });
            if (same == c.Hits.end()) c.Hits.push_back(hit);
            else if (hit.Distance < same->Distance) *same = hit;
            return 1.0f; // keep going: every shape along the ray
        };
        b2World_CastRay(LoadWorld(world->m_World), { origin.x, origin.y }, { translation.x, translation.y }, MaskQuery(layerMask), collect, &ctx);
        std::sort(ctx.Hits.begin(), ctx.Hits.end(), [](const RaycastHit2D& a, const RaycastHit2D& b) { return a.Distance < b.Distance; });
        return ctx.Hits;
    }

    std::vector<Entity> Physics2DWorld::OverlapPoint(Scene& scene, const glm::vec2& point, uint32_t layerMask)
    {
        std::vector<Entity> out;
        Physics2DWorld* world = scene.GetPhysics2D();
        if (!world) return out;
        b2Vec2 p{ point.x, point.y };
        b2ShapeProxy proxy = b2MakeProxy(&p, 1, 0.0f);
        b2World_OverlapShape(LoadWorld(world->m_World), &proxy, MaskQuery(layerMask), &CollectEntity, &out);
        return out;
    }

    RaycastHit2D Physics2DWorld::Raycast(Scene& scene, const glm::vec2& origin, const glm::vec2& direction, float maxDistance, uint32_t layerMask)
    {
        RaycastHit2D result;
        Physics2DWorld* world = scene.GetPhysics2D();
        float len = glm::length(direction);
        if (!world || len <= 0.0f) return result;
        glm::vec2 translation = direction / len * maxDistance;
        b2RayResult hit = b2World_CastRayClosest(LoadWorld(world->m_World), { origin.x, origin.y }, { translation.x, translation.y },
                                                 MaskQuery(layerMask));
        if (!hit.hit) return result;
        result.Hit = UnpackEntity(b2Shape_GetUserData(hit.shapeId));
        result.Point = { hit.point.x, hit.point.y };
        result.Normal = { hit.normal.x, hit.normal.y };
        result.Distance = hit.fraction * maxDistance;
        return result;
    }

    std::vector<Entity> Physics2DWorld::OverlapCircle(Scene& scene, const glm::vec2& center, float radius, uint32_t layerMask)
    {
        std::vector<Entity> out;
        Physics2DWorld* world = scene.GetPhysics2D();
        if (!world) return out;
        b2Vec2 c{ center.x, center.y };
        b2ShapeProxy proxy = b2MakeProxy(&c, 1, std::max(radius, 1e-4f));
        b2World_OverlapShape(LoadWorld(world->m_World), &proxy, MaskQuery(layerMask), &CollectEntity, &out);
        return out;
    }

    std::vector<Entity> Physics2DWorld::OverlapBox(Scene& scene, const glm::vec2& center, const glm::vec2& halfExtents, float angle, uint32_t layerMask)
    {
        std::vector<Entity> out;
        Physics2DWorld* world = scene.GetPhysics2D();
        if (!world) return out;
        float c = std::cos(angle), s = std::sin(angle);
        b2Vec2 points[4];
        const glm::vec2 corners[4] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, 1 } };
        for (int i = 0; i < 4; ++i)
        {
            glm::vec2 p = corners[i] * halfExtents;
            points[i] = { center.x + p.x * c - p.y * s, center.y + p.x * s + p.y * c };
        }
        b2ShapeProxy proxy = b2MakeProxy(points, 4, 0.0f);
        b2World_OverlapShape(LoadWorld(world->m_World), &proxy, MaskQuery(layerMask), &CollectEntity, &out);
        return out;
    }
}
