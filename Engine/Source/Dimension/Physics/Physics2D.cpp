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

        b2Filter LayerFilter(Scene& scene, Entity e, const std::array<uint32_t, 32>& matrix)
        {
            int layer = std::clamp(scene.GetRegistry().Get<TagComponent>(e).Layer, 0, 31);
            b2Filter f = b2DefaultFilter();
            f.categoryBits = 1ull << layer;
            f.maskBits = matrix[layer];
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

        struct OverlapContext
        {
            std::vector<Entity> Found;
            bool HitTriggers = true;
        };

        bool CollectEntity(b2ShapeId shape, void* context)
        {
            auto& c = *static_cast<OverlapContext*>(context);
            if (!c.HitTriggers && b2Shape_IsSensor(shape)) return true;
            Entity e = UnpackEntity(b2Shape_GetUserData(shape));
            if (std::find(c.Found.begin(), c.Found.end(), e) == c.Found.end()) c.Found.push_back(e);
            return true;
        }

        // The nearest hit of a ray (proxy null) or a swept shape, skipping triggers unless they count.
        RaycastHit2D CastClosest(uint32_t worldId, const b2ShapeProxy* proxy, const glm::vec2& origin, const glm::vec2& direction, float maxDistance,
                                 uint32_t layerMask, bool hitTriggers)
        {
            struct Context { RaycastHit2D Hit; float Fraction = 2.0f; bool HitTriggers; } ctx{ {}, 2.0f, hitTriggers };
            RaycastHit2D none;
            float len = glm::length(direction);
            if (len <= 0.0f) return none;
            glm::vec2 translation = direction / len * maxDistance;
            auto closest = [](b2ShapeId shape, b2Vec2 point, b2Vec2 normal, float fraction, void* context) -> float {
                auto& c = *static_cast<Context*>(context);
                if (!c.HitTriggers && b2Shape_IsSensor(shape)) return -1.0f; // ignore it, go on
                if (fraction < c.Fraction)
                {
                    c.Fraction = fraction;
                    c.Hit = { UnpackEntity(b2Shape_GetUserData(shape)), { point.x, point.y }, { normal.x, normal.y }, 0.0f };
                }
                return fraction; // only nearer ones from here
            };
            b2WorldId world = LoadWorld(worldId);
            if (proxy) b2World_CastShape(world, proxy, { translation.x, translation.y }, MaskQuery(layerMask), closest, &ctx);
            else b2World_CastRay(world, { origin.x, origin.y }, { translation.x, translation.y }, MaskQuery(layerMask), closest, &ctx);
            if (ctx.Fraction > 1.0f) return none;
            ctx.Hit.Distance = ctx.Fraction * maxDistance;
            return ctx.Hit;
        }

        // A box's corners, turned `angle` about its center.
        void BoxCorners(const glm::vec2& center, const glm::vec2& halfExtents, float angle, b2Vec2 (&points)[4])
        {
            float c = std::cos(angle), s = std::sin(angle);
            const glm::vec2 corners[4] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, 1 } };
            for (int i = 0; i < 4; ++i)
            {
                glm::vec2 p = corners[i] * halfExtents;
                points[i] = { center.x + p.x * c - p.y * s, center.y + p.x * s + p.y * c };
            }
        }
    }

    Physics2DWorld::Physics2DWorld(Scene& scene) : m_Scene(scene)
    {
        b2WorldDef def = b2DefaultWorldDef();
        auto g = scene.GetSettings().Gravity2D;
        def.gravity = { g.x, g.y };
        m_World = b2StoreWorldId(b2CreateWorld(&def));
        m_Matrix = Project::GetConfig().LayerCollisionMatrix;
        b2World_SetCustomFilterCallback(LoadWorld(m_World), &Physics2DWorld::CustomFilter, this);

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
        sd.filter = LayerFilter(m_Scene, e, m_Matrix);
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
        bd.angularDamping = rb.AngularDamping;
        bd.fixedRotation = rb.FixedRotation;
        bd.userData = PackEntity(e);
        bd.isEnabled = t.ActiveInHierarchy;
        b2BodyId body = b2CreateBody(LoadWorld(m_World), &bd);
        rb.RuntimeBody = b2StoreBodyId(body);
        rb.SyncedPose = { worldPos.x, worldPos.y, t.Rotation.z };
        rb.Driven = false;
        AddShapes(e, rb.RuntimeBody);
    }

    void Physics2DWorld::AddShapes(Entity e, uint64_t bodyId)
    {
        auto& reg = m_Scene.GetRegistry();
        auto& t = reg.Get<TransformComponent>(e);
        b2BodyId body = b2LoadBodyId(bodyId);
        glm::vec2 scale = glm::abs(glm::vec2(t.Scale));
        if (auto* box = reg.TryGet<BoxCollider2DComponent>(e))
        {
            b2ShapeDef sd = b2DefaultShapeDef();
            sd.density = box->Density;
            sd.material.friction = box->Friction;
            sd.material.restitution = box->Restitution;
            sd.isSensor = box->IsTrigger;
            sd.filter = LayerFilter(m_Scene, e, m_Matrix);
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
            sd.filter = LayerFilter(m_Scene, e, m_Matrix);
            sd.enableSensorEvents = true;
            sd.enableContactEvents = true;
            sd.userData = PackEntity(e);
            b2Circle c{ { circle->Offset.x * scale.x, circle->Offset.y * scale.y }, circle->Radius * std::max(scale.x, scale.y) };
            b2CreateCircleShape(body, &sd, &c);
        }
    }

    void Physics2DWorld::RefreshColliders(Entity e)
    {
        b2BodyId body = BodyOf(m_Scene, e);
        if (B2_IS_NULL(body)) return;
        std::vector<b2ShapeId> shapes(b2Body_GetShapeCount(body));
        int n = b2Body_GetShapes(body, shapes.data(), (int)shapes.size());
        for (int i = 0; i < n; ++i) b2DestroyShape(shapes[i], false);
        AddShapes(e, b2StoreBodyId(body));
        b2Body_ApplyMassFromShapes(body);
        b2Body_SetAwake(body, true);
    }

    void Physics2DWorld::RefreshBody(Entity e)
    {
        b2BodyId body = BodyOf(m_Scene, e);
        if (B2_IS_NULL(body)) return;
        auto& rb = m_Scene.GetRegistry().Get<Rigidbody2DComponent>(e);
        b2BodyType type = rb.Type == BodyType::Dynamic ? b2_dynamicBody : rb.Type == BodyType::Kinematic ? b2_kinematicBody : b2_staticBody;
        if (b2Body_GetType(body) != type)
        {
            b2Body_SetType(body, type);
            if (type != b2_dynamicBody) // it stops where it is (as a 3D body does)
            {
                b2Body_SetLinearVelocity(body, { 0.0f, 0.0f });
                b2Body_SetAngularVelocity(body, 0.0f);
            }
        }
        b2Body_SetGravityScale(body, rb.GravityScale);
        b2Body_SetLinearDamping(body, std::max(rb.LinearDamping, 0.0f));
        b2Body_SetAngularDamping(body, std::max(rb.AngularDamping, 0.0f));
        b2Body_SetFixedRotation(body, rb.FixedRotation);
        rb.Driven = false;
        if (type != b2_staticBody) b2Body_SetAwake(body, true);
    }

    void Physics2DWorld::MoveBody(Entity e, const glm::vec2& position, float angle, bool teleport)
    {
        auto& reg = m_Scene.GetRegistry();
        auto& t = reg.Get<TransformComponent>(e);
        t.Position.x = position.x;
        t.Position.y = position.y;
        t.Rotation.z = angle;
        b2BodyId body = BodyOf(m_Scene, e);
        if (B2_IS_NULL(body)) return;
        auto& rb = reg.Get<Rigidbody2DComponent>(e);
        if (!teleport && rb.Type == BodyType::Kinematic) return; // the next step moves it there
        b2Body_SetTransform(body, { position.x, position.y }, b2MakeRot(angle));
        rb.SyncedPose = { position.x, position.y, angle };
        if (rb.Type != BodyType::Static) b2Body_SetAwake(body, true);
    }

    float Physics2DWorld::GetMass(Entity e) const
    {
        b2BodyId body = BodyOf(m_Scene, e);
        if (B2_IS_NULL(body)) return 0.0f;
        b2ShapeId shapes[16];
        int n = b2Body_GetShapes(body, shapes, 16);
        float mass = 0.0f;
        for (int i = 0; i < n; ++i) mass += b2Shape_GetMassData(shapes[i]).mass; // static bodies too
        return mass;
    }

    void Physics2DWorld::SetMass(Entity e, float mass)
    {
        float current = GetMass(e);
        if (current <= 0.0f || mass <= 0.0f) return;
        float k = mass / current;
        auto& reg = m_Scene.GetRegistry();
        if (auto* box = reg.TryGet<BoxCollider2DComponent>(e)) box->Density *= k;
        if (auto* circle = reg.TryGet<CircleCollider2DComponent>(e)) circle->Density *= k;
        RefreshColliders(e);
    }

    void Physics2DWorld::RebuildJoint(Entity e)
    {
        for (auto it = m_Joints.begin(); it != m_Joints.end();)
        {
            if (it->Owner != e) { ++it; continue; }
            b2JointId id = b2LoadJointId(it->Id);
            if (b2Joint_IsValid(id))
            {
                b2Joint_WakeBodies(id);
                b2DestroyJoint(id);
            }
            it = m_Joints.erase(it);
        }
        auto& reg = m_Scene.GetRegistry();
        if (!m_Scene.IsValid(e) || !reg.Has<Joint2DComponent>(e)) return;
        CreateJoint(e);
        if (!m_Joints.empty() && m_Joints.back().Owner == e) b2Joint_WakeBodies(b2LoadJointId(m_Joints.back().Id));
    }

    void Physics2DWorld::RefilterShapes(Entity e)
    {
        b2BodyId body = BodyOf(m_Scene, e);
        if (B2_IS_NULL(body)) return;
        b2Filter filter = LayerFilter(m_Scene, e, m_Matrix);
        b2ShapeId shapes[16];
        int n = b2Body_GetShapes(body, shapes, 16);
        for (int i = 0; i < n; ++i)
        {
            // A filter change drops the shape's contacts, so the next step checks its pairs again (custom filter too).
            b2Filter other = filter;
            other.groupIndex = filter.groupIndex == 0 ? 1 : 0;
            b2Shape_SetFilter(shapes[i], other);
            b2Shape_SetFilter(shapes[i], filter);
        }
        if (b2Body_GetType(body) != b2_staticBody) b2Body_SetAwake(body, true);
    }

    bool Physics2DWorld::CustomFilter(b2ShapeId a, b2ShapeId b, void* context)
    {
        auto* self = static_cast<Physics2DWorld*>(context);
        if (self->m_IgnoredPairs.empty()) return true;
        return !self->IsCollisionIgnored(UnpackEntity(b2Shape_GetUserData(a)), UnpackEntity(b2Shape_GetUserData(b)));
    }

    glm::vec2 Physics2DWorld::GetGravity() const
    {
        b2Vec2 g = b2World_GetGravity(LoadWorld(m_World));
        return { g.x, g.y };
    }

    void Physics2DWorld::SetGravity(const glm::vec2& gravity)
    {
        b2World_SetGravity(LoadWorld(m_World), { gravity.x, gravity.y });
        m_Scene.GetSettings().Gravity2D = gravity;
        for (auto [e, rb] : m_Scene.GetRegistry().ViewOf<Rigidbody2DComponent>())
            if (rb.RuntimeBody && rb.Type == BodyType::Dynamic) b2Body_SetAwake(b2LoadBodyId(rb.RuntimeBody), true);
    }

    bool Physics2DWorld::GetLayersCollide(int a, int b) const
    {
        return a >= 0 && b >= 0 && a < 32 && b < 32 && ((m_Matrix[a] >> b) & 1u);
    }

    void Physics2DWorld::SetLayersCollide(int a, int b, bool collide)
    {
        if (a < 0 || b < 0 || a >= 32 || b >= 32 || GetLayersCollide(a, b) == collide) return;
        if (collide) { m_Matrix[a] |= 1u << b; m_Matrix[b] |= 1u << a; }
        else { m_Matrix[a] &= ~(1u << b); m_Matrix[b] &= ~(1u << a); }
        auto& reg = m_Scene.GetRegistry();
        std::vector<Entity> affected;
        for (auto [e, rb] : reg.ViewOf<Rigidbody2DComponent>())
        {
            int layer = reg.Get<TagComponent>(e).Layer;
            if (rb.RuntimeBody && (layer == a || layer == b)) affected.push_back(e);
        }
        for (Entity e : affected) RefilterShapes(e);
        for (auto& body : m_TilemapBodies)
        {
            if (!body.Id || !m_Scene.IsValid(body.Owner)) continue;
            int layer = reg.Get<TagComponent>(body.Owner).Layer;
            if (layer != a && layer != b) continue;
            b2ShapeId shapes[64];
            int n = b2Body_GetShapes(b2LoadBodyId(body.Id), shapes, 64);
            for (int i = 0; i < n; ++i) b2Shape_SetFilter(shapes[i], LayerFilter(m_Scene, body.Owner, m_Matrix));
        }
    }

    void Physics2DWorld::SetIgnoreCollision(Entity a, Entity b, bool ignore)
    {
        if (a == b) return;
        bool ignored = IsCollisionIgnored(a, b);
        if (ignore == ignored) return;
        if (ignore) m_IgnoredPairs.push_back({ a, b });
        else std::erase_if(m_IgnoredPairs, [&](auto& p) { return (p.first == a && p.second == b) || (p.first == b && p.second == a); });
        RefilterShapes(a);
        RefilterShapes(b);
    }

    bool Physics2DWorld::IsCollisionIgnored(Entity a, Entity b) const
    {
        for (auto& p : m_IgnoredPairs)
            if ((p.first == a && p.second == b) || (p.first == b && p.second == a)) return true;
        return false;
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
            if (shapeCount > 0 && b2Shape_GetFilter(shapes[0]).categoryBits != LayerFilter(m_Scene, e, m_Matrix).categoryBits)
                for (int i = 0; i < shapeCount; ++i) b2Shape_SetFilter(shapes[i], LayerFilter(m_Scene, e, m_Matrix));
            if (b2Body_IsEnabled(id) == t.ActiveInHierarchy) continue;
            if (t.ActiveInHierarchy)
            {
                glm::vec3 p = t.GetWorldPosition();
                b2Body_SetTransform(id, { p.x, p.y }, b2MakeRot(t.Rotation.z));
                rb.SyncedPose = { p.x, p.y, t.Rotation.z };
                b2Body_Enable(id);
            }
            else
                b2Body_Disable(id);
        }

        // Static bodies follow their transforms (moved by scripts). A kinematic body whose Transform a script moved
        // gets the velocity that takes it there in this step (so it pushes and carries what it meets, as Unity's
        // MovePosition does) and stops after; one left alone keeps the velocity a script gave it.
        for (auto [e, rb, t] : reg.ViewOf<Rigidbody2DComponent, TransformComponent>())
        {
            if (!rb.RuntimeBody || rb.Type == BodyType::Dynamic) continue;
            b2BodyId id = b2LoadBodyId(rb.RuntimeBody);
            glm::vec3 p = t.GetWorldPosition();
            if (rb.Type == BodyType::Static)
            {
                b2Body_SetTransform(id, { p.x, p.y }, b2MakeRot(t.Rotation.z));
                continue;
            }
            glm::vec3 target(p.x, p.y, t.Rotation.z);
            if (glm::any(glm::greaterThan(glm::abs(target - rb.SyncedPose), glm::vec3(1e-5f))) && dt > 0.0f)
            {
                b2Transform to{ { target.x, target.y }, b2MakeRot(target.z) };
                b2Vec2 from = b2Body_GetWorldCenterOfMass(id);
                b2Vec2 center = b2TransformPoint(to, b2Body_GetLocalCenterOfMass(id));
                b2Body_SetLinearVelocity(id, { (center.x - from.x) / dt, (center.y - from.y) / dt });
                if (!rb.FixedRotation) b2Body_SetAngularVelocity(id, b2RelativeAngle(to.q, b2Body_GetRotation(id)) / dt);
                rb.Driven = true;
            }
            else if (rb.Driven)
            {
                b2Body_SetLinearVelocity(id, { 0.0f, 0.0f });
                b2Body_SetAngularVelocity(id, 0.0f);
                rb.Driven = false;
            }
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
            if (!rb.RuntimeBody || rb.Type == BodyType::Static) continue;
            b2BodyId id = b2LoadBodyId(rb.RuntimeBody);
            b2Vec2 p = b2Body_GetPosition(id);
            t.Position.x = p.x;
            t.Position.y = p.y;
            t.Rotation.z = b2Rot_GetAngle(b2Body_GetRotation(id));
            rb.SyncedPose = { p.x, p.y, t.Rotation.z };
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
        if (B2_IS_NON_NULL(id) && !b2Body_IsFixedRotation(id)) b2Body_SetAngularVelocity(id, w);
    }

    void Physics2DWorld::ApplyTorque(Scene& scene, Entity e, float torque)
    {
        b2BodyId id = BodyOf(scene, e);
        if (B2_IS_NON_NULL(id)) b2Body_ApplyTorque(id, torque, true);
    }

    std::vector<RaycastHit2D> Physics2DWorld::RaycastAll(Scene& scene, const glm::vec2& origin, const glm::vec2& direction, float maxDistance,
                                                         uint32_t layerMask, bool hitTriggers)
    {
        struct Context { std::vector<RaycastHit2D> Hits; float MaxDistance; bool HitTriggers; } ctx{ {}, maxDistance, hitTriggers };
        Physics2DWorld* world = scene.GetPhysics2D();
        float len = glm::length(direction);
        if (!world || len <= 0.0f) return {};
        glm::vec2 translation = direction / len * maxDistance;
        auto collect = [](b2ShapeId shape, b2Vec2 point, b2Vec2 normal, float fraction, void* context) -> float {
            auto& c = *static_cast<Context*>(context);
            if (!c.HitTriggers && b2Shape_IsSensor(shape)) return 1.0f;
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

    std::vector<Entity> Physics2DWorld::OverlapPoint(Scene& scene, const glm::vec2& point, uint32_t layerMask, bool hitTriggers)
    {
        OverlapContext ctx{ {}, hitTriggers };
        Physics2DWorld* world = scene.GetPhysics2D();
        if (!world) return {};
        b2Vec2 p{ point.x, point.y };
        b2ShapeProxy proxy = b2MakeProxy(&p, 1, 0.0f);
        b2World_OverlapShape(LoadWorld(world->m_World), &proxy, MaskQuery(layerMask), &CollectEntity, &ctx);
        return ctx.Found;
    }

    RaycastHit2D Physics2DWorld::Raycast(Scene& scene, const glm::vec2& origin, const glm::vec2& direction, float maxDistance, uint32_t layerMask,
                                         bool hitTriggers)
    {
        Physics2DWorld* world = scene.GetPhysics2D();
        if (!world) return {};
        return CastClosest(world->m_World, nullptr, origin, direction, maxDistance, layerMask, hitTriggers);
    }

    RaycastHit2D Physics2DWorld::CircleCast(Scene& scene, const glm::vec2& center, float radius, const glm::vec2& direction, float maxDistance,
                                            uint32_t layerMask, bool hitTriggers)
    {
        Physics2DWorld* world = scene.GetPhysics2D();
        if (!world) return {};
        b2Vec2 c{ center.x, center.y };
        b2ShapeProxy proxy = b2MakeProxy(&c, 1, std::max(radius, 1e-4f));
        return CastClosest(world->m_World, &proxy, center, direction, maxDistance, layerMask, hitTriggers);
    }

    RaycastHit2D Physics2DWorld::BoxCast(Scene& scene, const glm::vec2& center, const glm::vec2& halfExtents, float angle, const glm::vec2& direction,
                                         float maxDistance, uint32_t layerMask, bool hitTriggers)
    {
        Physics2DWorld* world = scene.GetPhysics2D();
        if (!world) return {};
        b2Vec2 points[4];
        BoxCorners(center, glm::max(halfExtents, glm::vec2(1e-4f)), angle, points);
        b2ShapeProxy proxy = b2MakeProxy(points, 4, 0.0f);
        return CastClosest(world->m_World, &proxy, center, direction, maxDistance, layerMask, hitTriggers);
    }

    std::vector<Entity> Physics2DWorld::OverlapCircle(Scene& scene, const glm::vec2& center, float radius, uint32_t layerMask, bool hitTriggers)
    {
        OverlapContext ctx{ {}, hitTriggers };
        Physics2DWorld* world = scene.GetPhysics2D();
        if (!world) return {};
        b2Vec2 c{ center.x, center.y };
        b2ShapeProxy proxy = b2MakeProxy(&c, 1, std::max(radius, 1e-4f));
        b2World_OverlapShape(LoadWorld(world->m_World), &proxy, MaskQuery(layerMask), &CollectEntity, &ctx);
        return ctx.Found;
    }

    std::vector<Entity> Physics2DWorld::OverlapBox(Scene& scene, const glm::vec2& center, const glm::vec2& halfExtents, float angle, uint32_t layerMask,
                                                   bool hitTriggers)
    {
        OverlapContext ctx{ {}, hitTriggers };
        Physics2DWorld* world = scene.GetPhysics2D();
        if (!world) return {};
        b2Vec2 points[4];
        BoxCorners(center, halfExtents, angle, points);
        b2ShapeProxy proxy = b2MakeProxy(points, 4, 0.0f);
        b2World_OverlapShape(LoadWorld(world->m_World), &proxy, MaskQuery(layerMask), &CollectEntity, &ctx);
        return ctx.Found;
    }

    bool Physics2DWorld::ClosestPoint(Scene& scene, Entity e, const glm::vec2& point, glm::vec2& closest)
    {
        b2BodyId body = BodyOf(scene, e);
        if (B2_IS_NULL(body)) return false;
        b2ShapeId shapes[16];
        int n = b2Body_GetShapes(body, shapes, 16);
        float best = std::numeric_limits<float>::max();
        for (int i = 0; i < n; ++i)
        {
            if (b2Shape_TestPoint(shapes[i], { point.x, point.y }))
            {
                closest = point; // inside: the point itself
                return true;
            }
            b2Vec2 p = b2Shape_GetClosestPoint(shapes[i], { point.x, point.y });
            float d = (p.x - point.x) * (p.x - point.x) + (p.y - point.y) * (p.y - point.y);
            if (d < best) { best = d; closest = { p.x, p.y }; }
        }
        return n > 0;
    }

    bool Physics2DWorld::GetBounds(Scene& scene, Entity e, glm::vec2& min, glm::vec2& max)
    {
        b2BodyId body = BodyOf(scene, e);
        if (B2_IS_NULL(body)) return false;
        b2ShapeId shapes[16];
        int n = b2Body_GetShapes(body, shapes, 16);
        b2Transform xf = b2Body_GetTransform(body);
        for (int i = 0; i < n; ++i)
        {
            // The exact box (b2Shape_GetAABB's carries a speculative margin).
            b2AABB box;
            switch (b2Shape_GetType(shapes[i]))
            {
            case b2_circleShape: { b2Circle c = b2Shape_GetCircle(shapes[i]); box = b2ComputeCircleAABB(&c, xf); break; }
            case b2_capsuleShape: { b2Capsule c = b2Shape_GetCapsule(shapes[i]); box = b2ComputeCapsuleAABB(&c, xf); break; }
            case b2_polygonShape: { b2Polygon p = b2Shape_GetPolygon(shapes[i]); box = b2ComputePolygonAABB(&p, xf); break; }
            case b2_segmentShape: { b2Segment g = b2Shape_GetSegment(shapes[i]); box = b2ComputeSegmentAABB(&g, xf); break; }
            default: box = b2Shape_GetAABB(shapes[i]); break;
            }
            glm::vec2 lo(box.lowerBound.x, box.lowerBound.y), hi(box.upperBound.x, box.upperBound.y);
            min = i == 0 ? lo : glm::min(min, lo);
            max = i == 0 ? hi : glm::max(max, hi);
        }
        return n > 0;
    }
}
