#include "Dimension/Physics/Physics3D.h"
#include "Dimension/Scene/Scene.h"
#include "Dimension/Core/Log.h"

#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyActivationListener.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/PointConstraint.h>
#include <Jolt/Physics/Constraints/DistanceConstraint.h>
#include <Jolt/Physics/Constraints/SliderConstraint.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/GroupFilterTable.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include "Dimension/Asset/AssetManager.h"
#include "Dimension/Renderer/Mesh.h"
#include "Dimension/Project/Project.h"

#include <cstdarg>
#include <mutex>
#include <algorithm>
#include <array>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace Dimension
{
    namespace
    {
        // Object layer = (user layer << 1) | moving. Broad phase layers: 0 non-moving, 1 moving.
        namespace Layers
        {
            constexpr JPH::ObjectLayer NonMoving = 0;
            constexpr JPH::ObjectLayer Moving = 1;
            JPH::ObjectLayer Make(int userLayer, bool moving) { return (JPH::ObjectLayer)((std::clamp(userLayer, 0, 31) << 1) | (moving ? 1 : 0)); }
        }

        class ObjectLayerPairFilterImpl final : public JPH::ObjectLayerPairFilter
        {
        public:
            std::array<uint32_t, 32> Matrix = ProjectConfig::MakeDefaultMatrix();
            bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override
            {
                if (!(a & 1) && !(b & 1)) return false; // static vs static
                return (Matrix[a >> 1] >> (b >> 1)) & 1u;
            }
        };

        // Query filter: accepts bodies whose user layer is in the mask.
        class LayerMaskFilter final : public JPH::ObjectLayerFilter
        {
        public:
            explicit LayerMaskFilter(uint32_t mask) : m_Mask(mask) {}
            bool ShouldCollide(JPH::ObjectLayer layer) const override { return (m_Mask >> (layer >> 1)) & 1u; }

        private:
            uint32_t m_Mask;
        };

        class BroadPhaseLayerInterfaceImpl final : public JPH::BroadPhaseLayerInterface
        {
        public:
            JPH::uint GetNumBroadPhaseLayers() const override { return 2; }
            JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override { return JPH::BroadPhaseLayer((JPH::uint8)(layer & 1)); }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
            const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override
            {
                return (JPH::BroadPhaseLayer::Type)layer == 0 ? "NonMoving" : "Moving";
            }
#endif
        };

        class ObjectVsBroadPhaseLayerFilterImpl final : public JPH::ObjectVsBroadPhaseLayerFilter
        {
        public:
            bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer bp) const override
            {
                return (layer & 1) || (JPH::BroadPhaseLayer::Type)bp == Layers::Moving;
            }
        };

        void JoltTrace(const char* fmt, ...)
        {
            va_list args;
            va_start(args, fmt);
            char buffer[1024];
            vsnprintf(buffer, sizeof(buffer), fmt, args);
            va_end(args);
            Log::Trace("[Jolt] {}", buffer);
        }

        void EnsureJoltInitialized()
        {
            static bool initialized = false;
            if (initialized) return;
            JPH::RegisterDefaultAllocator();
            JPH::Trace = JoltTrace;
            JPH::Factory::sInstance = new JPH::Factory();
            JPH::RegisterTypes();
            initialized = true;
        }

        JPH::Vec3 ToJolt(const glm::vec3& v) { return JPH::Vec3(v.x, v.y, v.z); }
        glm::vec3 FromJolt(const JPH::Vec3& v) { return { v.GetX(), v.GetY(), v.GetZ() }; }
        JPH::Quat ToJolt(const glm::quat& q) { return JPH::Quat(q.x, q.y, q.z, q.w); }
        glm::quat FromJolt(const JPH::Quat& q) { return glm::quat(q.GetW(), q.GetX(), q.GetY(), q.GetZ()); }
    }

    struct Physics3DWorld::Impl : public JPH::ContactListener
    {
        BroadPhaseLayerInterfaceImpl BroadPhase;
        ObjectVsBroadPhaseLayerFilterImpl ObjectVsBroadPhase;
        ObjectLayerPairFilterImpl ObjectPair;
        Scope<JPH::TempAllocatorImpl> TempAllocator;
        Scope<JPH::JobSystemThreadPool> JobSystem;
        JPH::PhysicsSystem System;

        std::unordered_map<JPH::uint32, Entity> BodyToEntity; // read-only during Update
        std::unordered_set<JPH::uint32> Sensors;               // read-only during Update
        struct Joint { Entity A, B; JPH::Ref<JPH::Constraint> Constraint; JointType Type; };
        std::vector<Joint> Joints;
        // Jointed bodies that must not collide share group 0 and get their own sub-group.
        static constexpr JPH::uint kMaxJointSubGroups = 4096;
        JPH::Ref<JPH::GroupFilterTable> JointFilter = new JPH::GroupFilterTable(kMaxJointSubGroups);
        std::unordered_map<JPH::uint32, JPH::uint> SubGroupOf; // body id -> sub-group
        JPH::uint NextSubGroup = 0;
        std::vector<Entity> Characters;
        struct Event { Entity A, B; bool Begin, Trigger; };
        std::mutex EventMutex;
        std::vector<Event> Events;

        Entity Lookup(const JPH::BodyID& id) const
        {
            auto it = BodyToEntity.find(id.GetIndexAndSequenceNumber());
            return it != BodyToEntity.end() ? it->second : Entity::Null();
        }

        void OnContactAdded(const JPH::Body& a, const JPH::Body& b, const JPH::ContactManifold&, JPH::ContactSettings&) override
        {
            std::lock_guard lock(EventMutex);
            Events.push_back({ Lookup(a.GetID()), Lookup(b.GetID()), true, a.IsSensor() || b.IsSensor() });
        }

        void OnContactRemoved(const JPH::SubShapeIDPair& pair) override
        {
            bool trigger = Sensors.contains(pair.GetBody1ID().GetIndexAndSequenceNumber()) ||
                           Sensors.contains(pair.GetBody2ID().GetIndexAndSequenceNumber());
            std::lock_guard lock(EventMutex);
            Events.push_back({ Lookup(pair.GetBody1ID()), Lookup(pair.GetBody2ID()), false, trigger });
        }
    };

    Physics3DWorld::Physics3DWorld(Scene& scene) : m_Scene(scene), m_Impl((EnsureJoltInitialized(), MakeScope<Impl>())) // Impl allocates through Jolt
    {
        EnsureJoltInitialized();
        m_Impl->ObjectPair.Matrix = Project::GetConfig().LayerCollisionMatrix;
        m_Impl->TempAllocator = MakeScope<JPH::TempAllocatorImpl>(16 * 1024 * 1024);
        int threads = std::max(1, (int)std::thread::hardware_concurrency() - 1);
        m_Impl->JobSystem = MakeScope<JPH::JobSystemThreadPool>(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, threads);
        m_Impl->System.Init(65536, 0, 65536, 10240, m_Impl->BroadPhase, m_Impl->ObjectVsBroadPhase, m_Impl->ObjectPair);
        m_Impl->System.SetGravity(ToJolt(scene.GetSettings().Gravity));
        m_Impl->System.SetContactListener(m_Impl.get());

        auto& reg = scene.GetRegistry();
        std::vector<Entity> entities;
        reg.EachEntity([&](Entity e) {
            if (reg.Has<RigidbodyComponent>(e) || reg.Has<BoxColliderComponent>(e) || reg.Has<SphereColliderComponent>(e) ||
                reg.Has<CapsuleColliderComponent>(e) || reg.Has<MeshColliderComponent>(e) || reg.Has<CharacterControllerComponent>(e))
                entities.push_back(e);
        });
        for (Entity e : entities) CreateBody(e);
        std::vector<Entity> jointed;
        for (auto [e, j] : reg.ViewOf<JointComponent>()) jointed.push_back(e);
        for (Entity e : jointed) CreateJoint(e);
        m_Impl->System.OptimizeBroadPhase();
    }

    Physics3DWorld::~Physics3DWorld()
    {
        for (auto& j : m_Impl->Joints) m_Impl->System.RemoveConstraint(j.Constraint);
        m_Impl->Joints.clear();
        for (auto [e, cc] : m_Scene.GetRegistry().ViewOf<CharacterControllerComponent>()) cc.Runtime.reset();
        auto& bi = m_Impl->System.GetBodyInterface();
        for (auto [e, rb] : m_Scene.GetRegistry().ViewOf<RigidbodyComponent>())
        {
            if (rb.RuntimeBody == 0xFFFFFFFF) continue;
            JPH::BodyID id(rb.RuntimeBody);
            bi.RemoveBody(id);
            bi.DestroyBody(id);
            rb.RuntimeBody = 0xFFFFFFFF;
        }
    }

    void Physics3DWorld::CreateBodiesFor(Entity root)
    {
        CreateBodiesRecursive(root);
        CreateJointsRecursive(root);
    }

    void Physics3DWorld::CreateBodiesRecursive(Entity e)
    {
        auto& reg = m_Scene.GetRegistry();
        if (reg.Has<RigidbodyComponent>(e) || reg.Has<BoxColliderComponent>(e) || reg.Has<SphereColliderComponent>(e) ||
            reg.Has<CapsuleColliderComponent>(e) || reg.Has<MeshColliderComponent>(e) || reg.Has<CharacterControllerComponent>(e))
            CreateBody(e);
        for (Entity c : m_Scene.GetChildren(e)) CreateBodiesRecursive(c);
    }

    void Physics3DWorld::CreateJointsRecursive(Entity e)
    {
        if (m_Scene.GetRegistry().Has<JointComponent>(e)) CreateJoint(e);
        for (Entity c : m_Scene.GetChildren(e)) CreateJointsRecursive(c);
    }

    size_t Physics3DWorld::GetJointCount() const { return m_Impl->Joints.size(); }

    void Physics3DWorld::CreateJoint(Entity e)
    {
        auto& reg = m_Scene.GetRegistry();
        auto& j = reg.Get<JointComponent>(e);
        auto* rbA = reg.TryGet<RigidbodyComponent>(e);
        if (!rbA || rbA->RuntimeBody == 0xFFFFFFFF)
        {
            Log::Warn("Joint on '{}' needs a Rigidbody with a collider", reg.Get<TagComponent>(e).Name);
            return;
        }
        JPH::BodyID idA(rbA->RuntimeBody), idB; // invalid B = attached to the world
        Entity other = Entity::Null();
        const TransformComponent* tb = nullptr;
        if (m_Scene.IsValid(j.ConnectedBody) && j.ConnectedBody != e)
        {
            auto* rbB = reg.TryGet<RigidbodyComponent>(j.ConnectedBody);
            if (!rbB || rbB->RuntimeBody == 0xFFFFFFFF)
            {
                Log::Warn("Joint on '{}': connected body has no physics body", reg.Get<TagComponent>(e).Name);
                return;
            }
            idB = JPH::BodyID(rbB->RuntimeBody);
            other = j.ConnectedBody;
            tb = &reg.Get<TransformComponent>(other);
        }
        const auto& ta = reg.Get<TransformComponent>(e);
        glm::vec3 anchor = glm::vec3(ta.World * glm::vec4(j.Anchor, 1.0f));
        glm::vec3 axis = glm::mat3(ta.World) * j.Axis;
        axis = glm::length(axis) > 1e-6f ? glm::normalize(axis) : glm::vec3(0, 0, 1);

        JPH::Ref<JPH::TwoBodyConstraintSettings> settings;
        switch (j.Type)
        {
        case JointType::Fixed:
        {
            auto* s = new JPH::FixedConstraintSettings();
            s->mAutoDetectPoint = true;
            settings = s;
            break;
        }
        case JointType::Hinge:
        {
            auto* s = new JPH::HingeConstraintSettings();
            s->mPoint1 = s->mPoint2 = JPH::RVec3(ToJolt(anchor));
            s->mHingeAxis1 = s->mHingeAxis2 = ToJolt(axis);
            glm::vec3 helper = std::abs(axis.y) < 0.9f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
            s->mNormalAxis1 = s->mNormalAxis2 = ToJolt(glm::normalize(glm::cross(axis, helper)));
            if (j.UseLimits)
            {
                s->mLimitsMin = glm::radians(std::clamp(j.MinAngle, -180.0f, 0.0f));
                s->mLimitsMax = glm::radians(std::clamp(j.MaxAngle, 0.0f, 180.0f));
            }
            // Always valid motor settings, so the motor can be switched on while playing.
            s->mMotorSettings = JPH::MotorSettings(2.0f, 1.0f, 0.0f, std::max(j.MotorMaxTorque, 0.0f));
            settings = s;
            break;
        }
        case JointType::Point:
        {
            auto* s = new JPH::PointConstraintSettings();
            s->mPoint1 = s->mPoint2 = JPH::RVec3(ToJolt(anchor));
            settings = s;
            break;
        }
        case JointType::Distance:
        {
            auto* s = new JPH::DistanceConstraintSettings();
            glm::vec3 pointB = tb ? glm::vec3(tb->World * glm::vec4(j.ConnectedAnchor, 1.0f)) : j.ConnectedAnchor;
            s->mPoint1 = JPH::RVec3(ToJolt(anchor));
            s->mPoint2 = JPH::RVec3(ToJolt(pointB));
            float initial = glm::distance(anchor, pointB);
            s->mMinDistance = j.MinDistance > 0.0f ? j.MinDistance : initial;
            s->mMaxDistance = j.MaxDistance > 0.0f ? std::max(j.MaxDistance, s->mMinDistance) : std::max(initial, s->mMinDistance);
            // A spring: the limits pull back softly instead of rigidly.
            if (j.SpringFrequency > 0.0f)
                s->mLimitsSpringSettings = JPH::SpringSettings(JPH::ESpringMode::FrequencyAndDamping, j.SpringFrequency, std::max(j.SpringDamping, 0.0f));
            settings = s;
            break;
        }
        case JointType::Slider:
        {
            // Jolt measures the connected body's position relative to this one, so this entity's travel is negated.
            auto* s = new JPH::SliderConstraintSettings();
            s->mPoint1 = s->mPoint2 = JPH::RVec3(ToJolt(anchor));
            s->mSliderAxis1 = s->mSliderAxis2 = ToJolt(axis);
            glm::vec3 helper = std::abs(axis.y) < 0.9f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
            s->mNormalAxis1 = s->mNormalAxis2 = ToJolt(glm::normalize(glm::cross(axis, helper)));
            if (j.UseLimits)
            {
                s->mLimitsMin = -std::max(j.SliderMax, 0.0f);
                s->mLimitsMax = -std::min(j.SliderMin, 0.0f);
            }
            s->mMotorSettings = JPH::MotorSettings(2.0f, 1.0f, std::max(j.MotorMaxTorque, 0.0f), 0.0f);
            settings = s;
            break;
        }
        }

        auto& bi = m_Impl->System.GetBodyInterface();
        JPH::TwoBodyConstraint* constraint = bi.CreateConstraint(settings, idA, idB);
        if (!constraint) return;
        m_Impl->System.AddConstraint(constraint);
        bi.ActivateConstraint(constraint);
        m_Impl->Joints.push_back({ e, other, constraint, j.Type });
        j.Broken = false;
        j.CurrentForce = j.CurrentTorque = 0.0f;

        if (!j.EnableCollision && !idB.IsInvalid())
        {
            auto subGroup = [&](JPH::BodyID id) -> JPH::uint {
                auto it = m_Impl->SubGroupOf.find(id.GetIndexAndSequenceNumber());
                if (it != m_Impl->SubGroupOf.end()) return it->second;
                if (m_Impl->NextSubGroup >= Impl::kMaxJointSubGroups) return ~0u;
                JPH::uint sg = m_Impl->NextSubGroup++;
                m_Impl->SubGroupOf[id.GetIndexAndSequenceNumber()] = sg;
                JPH::BodyLockWrite lock(m_Impl->System.GetBodyLockInterface(), id);
                if (lock.Succeeded()) lock.GetBody().SetCollisionGroup(JPH::CollisionGroup(m_Impl->JointFilter, 0, sg));
                return sg;
            };
            JPH::uint a = subGroup(idA), b = subGroup(idB);
            if (a != ~0u && b != ~0u) m_Impl->JointFilter->DisableCollision(a, b);
            else Log::Warn("Too many jointed bodies: '{}' keeps colliding with its connected body", reg.Get<TagComponent>(e).Name);
        }
    }

    void Physics3DWorld::CreateCharacter(Entity e)
    {
        auto& reg = m_Scene.GetRegistry();
        auto& cc = reg.Get<CharacterControllerComponent>(e);
        if (cc.Runtime) return;
        auto& t = reg.Get<TransformComponent>(e);
        float radius = std::max(0.01f, cc.Radius);
        float halfCylinder = std::max(0.01f, cc.Height * 0.5f - radius);
        JPH::RotatedTranslatedShapeSettings shapeSettings(ToJolt(cc.Center), JPH::Quat::sIdentity(), new JPH::CapsuleShapeSettings(halfCylinder, radius));
        auto shape = shapeSettings.Create();
        if (shape.HasError()) return;

        JPH::Ref<JPH::CharacterVirtualSettings> settings = new JPH::CharacterVirtualSettings();
        settings->mShape = shape.Get();
        settings->mMaxSlopeAngle = glm::radians(std::clamp(cc.SlopeLimit, 0.0f, 89.0f));
        settings->mMass = cc.Mass;
        settings->mMaxStrength = cc.PushStrength;
        settings->mCharacterPadding = cc.SkinWidth;
        // Contacts on the lower hemisphere can support the character.
        settings->mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), halfCylinder - cc.Center.y);
        settings->mInnerBodyShape = shape.Get(); // lets raycasts, triggers and rigidbodies see the character
        settings->mInnerBodyLayer = Layers::Make(reg.Get<TagComponent>(e).Layer, true);
        auto* character = new JPH::CharacterVirtual(settings, JPH::RVec3(ToJolt(t.GetWorldPosition())), JPH::Quat::sIdentity(), 0, &m_Impl->System);
        cc.Runtime = std::shared_ptr<void>(new JPH::Ref<JPH::CharacterVirtual>(character),
                                           [](void* p) { delete static_cast<JPH::Ref<JPH::CharacterVirtual>*>(p); });
        cc.IsGrounded = false;
        cc.VerticalSpeed = 0.0f;
        cc.Velocity = glm::vec3(0.0f);
        if (!character->GetInnerBodyID().IsInvalid()) m_Impl->BodyToEntity[character->GetInnerBodyID().GetIndexAndSequenceNumber()] = e;
        m_Impl->Characters.push_back(e);
    }

    void Physics3DWorld::CreateBody(Entity e)
    {
        auto& reg = m_Scene.GetRegistry();
        if (reg.Has<CharacterControllerComponent>(e))
        {
            CreateCharacter(e); // characters are not rigid bodies (their colliders are ignored)
            return;
        }
        if (!reg.Has<RigidbodyComponent>(e)) reg.Emplace<RigidbodyComponent>(e).Type = BodyType::Static;
        auto& rb = reg.Get<RigidbodyComponent>(e);
        if (rb.RuntimeBody != 0xFFFFFFFF) return;
        auto& t = reg.Get<TransformComponent>(e);
        glm::vec3 scale = glm::abs(t.Scale);

        JPH::StaticCompoundShapeSettings compound;
        float friction = 0.6f, restitution = 0.0f;
        int shapeCount = 0;
        bool sensor = false;
        if (auto* box = reg.TryGet<BoxColliderComponent>(e))
        {
            glm::vec3 half = glm::max(box->Size * 0.5f * scale, glm::vec3(0.001f));
            float convex = std::min(JPH::cDefaultConvexRadius, std::min(half.x, std::min(half.y, half.z)) * 0.5f);
            compound.AddShape(ToJolt(box->Offset * scale), JPH::Quat::sIdentity(), new JPH::BoxShapeSettings(ToJolt(half), convex));
            friction = box->Friction;
            restitution = box->Restitution;
            sensor |= box->IsTrigger;
            ++shapeCount;
        }
        if (auto* sphere = reg.TryGet<SphereColliderComponent>(e))
        {
            float radius = std::max(0.001f, sphere->Radius * std::max(scale.x, std::max(scale.y, scale.z)));
            compound.AddShape(ToJolt(sphere->Offset * scale), JPH::Quat::sIdentity(), new JPH::SphereShapeSettings(radius));
            friction = sphere->Friction;
            restitution = sphere->Restitution;
            sensor |= sphere->IsTrigger;
            ++shapeCount;
        }
        if (auto* cap = reg.TryGet<CapsuleColliderComponent>(e))
        {
            float radius = std::max(0.001f, cap->Radius * std::max(scale.x, scale.z));
            float halfCylinder = std::max(0.001f, cap->Height * 0.5f * scale.y - radius);
            compound.AddShape(ToJolt(cap->Offset * scale), JPH::Quat::sIdentity(), new JPH::CapsuleShapeSettings(halfCylinder, radius));
            friction = cap->Friction;
            restitution = cap->Restitution;
            sensor |= cap->IsTrigger;
            ++shapeCount;
        }
        if (auto* mc = reg.TryGet<MeshColliderComponent>(e))
        {
            auto* mr = reg.TryGet<MeshRendererComponent>(e);
            AssetHandle meshHandle = mc->Mesh.IsValid() ? mc->Mesh : mr ? mr->Mesh : AssetHandle::None();
            auto data = AssetManager::GetMeshData(meshHandle);
            if (!data || data->Vertices.empty() || data->Indices.size() < 3)
                Log::Warn("Mesh Collider on '{}' has no mesh", reg.Get<TagComponent>(e).Name);
            else
            {
                bool convex = mc->Convex || rb.Type == BodyType::Dynamic;
                if (!mc->Convex && rb.Type == BodyType::Dynamic)
                    Log::Warn("Mesh Collider on '{}': dynamic bodies use the convex hull", reg.Get<TagComponent>(e).Name);
                if (convex)
                {
                    // The hull builder only needs a representative point cloud.
                    JPH::Array<JPH::Vec3> points;
                    size_t stride = std::max<size_t>(1, data->Vertices.size() / 2000);
                    for (size_t i = 0; i < data->Vertices.size(); i += stride) points.push_back(ToJolt(data->Vertices[i].Position * scale));
                    compound.AddShape(JPH::Vec3::sZero(), JPH::Quat::sIdentity(), new JPH::ConvexHullShapeSettings(points));
                }
                else
                {
                    JPH::VertexList vertices;
                    vertices.reserve(data->Vertices.size());
                    for (auto& v : data->Vertices)
                    {
                        glm::vec3 p = v.Position * scale;
                        vertices.push_back(JPH::Float3(p.x, p.y, p.z));
                    }
                    JPH::IndexedTriangleList triangles;
                    triangles.reserve(data->Indices.size() / 3);
                    for (size_t i = 0; i + 2 < data->Indices.size(); i += 3)
                        triangles.push_back(JPH::IndexedTriangle(data->Indices[i], data->Indices[i + 1], data->Indices[i + 2], 0));
                    compound.AddShape(JPH::Vec3::sZero(), JPH::Quat::sIdentity(), new JPH::MeshShapeSettings(std::move(vertices), std::move(triangles)));
                }
                friction = mc->Friction;
                restitution = mc->Restitution;
                sensor |= mc->IsTrigger;
                ++shapeCount;
            }
        }
        if (shapeCount == 0) return; // rigidbody without collider: nothing to simulate

        auto shapeResult = compound.Create();
        if (shapeResult.HasError())
        {
            Log::Error("Physics shape error: {}", shapeResult.GetError().c_str());
            return;
        }

        JPH::EMotionType motion = rb.Type == BodyType::Dynamic ? JPH::EMotionType::Dynamic
                                : rb.Type == BodyType::Kinematic ? JPH::EMotionType::Kinematic
                                                                 : JPH::EMotionType::Static;
        JPH::BodyCreationSettings settings(shapeResult.Get(), JPH::RVec3(ToJolt(t.GetWorldPosition())), ToJolt(t.GetRotationQuat()),
                                           motion, Layers::Make(reg.Get<TagComponent>(e).Layer, motion != JPH::EMotionType::Static));
        settings.mIsSensor = sensor;
        settings.mCollideKinematicVsNonDynamic = sensor; // triggers also see kinematic bodies (e.g. Character Controllers)
        settings.mFriction = friction;
        settings.mRestitution = restitution;
        settings.mLinearDamping = rb.LinearDamping;
        settings.mAngularDamping = rb.AngularDamping;
        settings.mGravityFactor = rb.UseGravity ? 1.0f : 0.0f;
        settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
        settings.mMassPropertiesOverride.mMass = rb.Mass;
        if (motion != JPH::EMotionType::Dynamic)
        {
            // Mesh shapes have no mass properties; non-dynamic bodies do not need real ones.
            settings.mOverrideMassProperties = JPH::EOverrideMassProperties::MassAndInertiaProvided;
            settings.mMassPropertiesOverride.mInertia = JPH::Mat44::sIdentity();
        }

        auto& bi = m_Impl->System.GetBodyInterface();
        JPH::BodyID id = bi.CreateAndAddBody(settings, JPH::EActivation::Activate);
        if (id.IsInvalid())
        {
            Log::Error("Too many physics bodies");
            return;
        }
        rb.RuntimeBody = id.GetIndexAndSequenceNumber();
        m_Impl->BodyToEntity[rb.RuntimeBody] = e;
        if (sensor) m_Impl->Sensors.insert(rb.RuntimeBody);
    }

    void Physics3DWorld::OnEntityDestroyed(Entity e)
    {
        // Constraints must go before their bodies.
        auto& joints = m_Impl->Joints;
        for (auto it = joints.begin(); it != joints.end();)
            if (it->A == e || it->B == e)
            {
                m_Impl->System.RemoveConstraint(it->Constraint);
                it = joints.erase(it);
            }
            else
                ++it;
        if (auto* cc = m_Scene.GetRegistry().TryGet<CharacterControllerComponent>(e); cc && cc->Runtime)
        {
            auto& character = *static_cast<JPH::Ref<JPH::CharacterVirtual>*>(cc->Runtime.get());
            m_Impl->BodyToEntity.erase(character->GetInnerBodyID().GetIndexAndSequenceNumber());
            cc->Runtime.reset();
            std::erase(m_Impl->Characters, e);
        }
        auto* rb = m_Scene.GetRegistry().TryGet<RigidbodyComponent>(e);
        if (!rb || rb->RuntimeBody == 0xFFFFFFFF) return;
        auto& bi = m_Impl->System.GetBodyInterface();
        JPH::BodyID id(rb->RuntimeBody);
        bi.RemoveBody(id);
        bi.DestroyBody(id);
        m_Impl->BodyToEntity.erase(rb->RuntimeBody);
        m_Impl->Sensors.erase(rb->RuntimeBody);
        rb->RuntimeBody = 0xFFFFFFFF;
    }

    void Physics3DWorld::Step(float dt)
    {
        auto& reg = m_Scene.GetRegistry();
        auto& bi = m_Impl->System.GetBodyInterface();

        for (auto [e, rb, t] : reg.ViewOf<RigidbodyComponent, TransformComponent>())
        {
            if (rb.RuntimeBody == 0xFFFFFFFF || rb.Type == BodyType::Dynamic) continue;
            JPH::BodyID id(rb.RuntimeBody);
            JPH::RVec3 pos(ToJolt(t.GetWorldPosition()));
            if (rb.Type == BodyType::Kinematic) bi.MoveKinematic(id, pos, ToJolt(t.GetRotationQuat()), dt);
            else bi.SetPositionAndRotation(id, pos, ToJolt(t.GetRotationQuat()), JPH::EActivation::DontActivate);
        }

        // Hinge motors follow their components (scripts may change Use Motor / Motor Speed while playing).
        for (auto& joint : m_Impl->Joints)
        {
            auto* jc = reg.TryGet<JointComponent>(joint.A);
            if (!jc || (joint.Type != JointType::Hinge && joint.Type != JointType::Slider)) continue;
            // Jolt drives the connected body relative to this one; positive speed should turn *this* entity
            // counter-clockwise around its axis (right-hand rule), or move it along +Axis, hence the signs.
            if (joint.Type == JointType::Hinge)
            {
                auto* hinge = static_cast<JPH::HingeConstraint*>(joint.Constraint.GetPtr());
                hinge->GetMotorSettings().SetTorqueLimit(std::max(jc->MotorMaxTorque, 0.0f));
                hinge->SetMotorState(jc->UseMotor ? JPH::EMotorState::Velocity : JPH::EMotorState::Off);
                hinge->SetTargetAngularVelocity(-glm::radians(jc->MotorSpeed));
            }
            else
            {
                auto* slider = static_cast<JPH::SliderConstraint*>(joint.Constraint.GetPtr());
                slider->GetMotorSettings().SetForceLimit(std::max(jc->MotorMaxTorque, 0.0f));
                slider->SetMotorState(jc->UseMotor ? JPH::EMotorState::Velocity : JPH::EMotorState::Off);
                slider->SetTargetVelocity(-jc->MotorSpeed);
            }
            if (jc->UseMotor)
            {
                bi.ActivateBody(JPH::BodyID(reg.Get<RigidbodyComponent>(joint.A).RuntimeBody));
                if (auto* rbB = m_Scene.IsValid(joint.B) ? reg.TryGet<RigidbodyComponent>(joint.B) : nullptr) bi.ActivateBody(JPH::BodyID(rbB->RuntimeBody));
            }
        }

        m_Impl->System.Update(dt, 1, m_Impl->TempAllocator.get(), m_Impl->JobSystem.get());

        // Joint reactions (impulses / dt); joints over their break force or torque are removed.
        std::vector<std::pair<Entity, float>> broken;
        for (auto it = m_Impl->Joints.begin(); it != m_Impl->Joints.end();)
        {
            auto* jc = reg.TryGet<JointComponent>(it->A);
            JPH::Constraint* c = it->Constraint.GetPtr();
            float force = 0.0f, torque = 0.0f;
            switch (it->Type)
            {
            case JointType::Fixed:
            {
                auto* f = static_cast<JPH::FixedConstraint*>(c);
                force = f->GetTotalLambdaPosition().Length();
                torque = f->GetTotalLambdaRotation().Length();
                break;
            }
            case JointType::Hinge:
            {
                auto* h = static_cast<JPH::HingeConstraint*>(c);
                JPH::Vector<2> r = h->GetTotalLambdaRotation();
                force = h->GetTotalLambdaPosition().Length();
                torque = std::sqrt(r[0] * r[0] + r[1] * r[1] + h->GetTotalLambdaRotationLimits() * h->GetTotalLambdaRotationLimits());
                break;
            }
            case JointType::Point: force = static_cast<JPH::PointConstraint*>(c)->GetTotalLambdaPosition().Length(); break;
            case JointType::Distance: force = std::abs(static_cast<JPH::DistanceConstraint*>(c)->GetTotalLambdaPosition()); break;
            case JointType::Slider:
            {
                // Perpendicular and limit forces; the slider's own motor force does not count.
                auto* s = static_cast<JPH::SliderConstraint*>(c);
                JPH::Vector<2> p = s->GetTotalLambdaPosition();
                float limit = s->GetTotalLambdaPositionLimits();
                force = std::sqrt(p[0] * p[0] + p[1] * p[1] + limit * limit);
                torque = s->GetTotalLambdaRotation().Length();
                break;
            }
            }
            force /= dt;
            torque /= dt;
            if (jc)
            {
                jc->CurrentForce = force;
                jc->CurrentTorque = torque;
                bool breaks = (jc->BreakForce > 0.0f && force > jc->BreakForce) || (jc->BreakTorque > 0.0f && torque > jc->BreakTorque);
                if (breaks)
                {
                    jc->Broken = true;
                    m_Impl->System.RemoveConstraint(c);
                    broken.emplace_back(it->A, force);
                    it = m_Impl->Joints.erase(it);
                    continue;
                }
            }
            ++it;
        }

        for (auto [e, rb, t] : reg.ViewOf<RigidbodyComponent, TransformComponent>())
        {
            if (rb.RuntimeBody == 0xFFFFFFFF || rb.Type != BodyType::Dynamic) continue;
            JPH::BodyID id(rb.RuntimeBody);
            JPH::RVec3 pos;
            JPH::Quat rot;
            bi.GetPositionAndRotation(id, pos, rot);
            t.Position = FromJolt(JPH::Vec3(pos));
            t.Rotation = glm::eulerAngles(FromJolt(rot));
        }

        std::vector<Impl::Event> events;
        {
            std::lock_guard lock(m_Impl->EventMutex);
            events.swap(m_Impl->Events);
        }
        for (auto& ev : events) m_Scene.DispatchCollision(ev.A, ev.B, ev.Begin, ev.Trigger);
        for (auto& [e, force] : broken) m_Scene.DispatchJointBreak(e, force);
    }

    namespace
    {
        bool BodyFor(Scene& scene, Entity e, JPH::BodyInterface*& bi, JPH::BodyID& id)
        {
            Physics3DWorld* world = scene.GetPhysics3D();
            auto* rb = scene.GetRegistry().TryGet<RigidbodyComponent>(e);
            if (!world || !rb || rb->RuntimeBody == 0xFFFFFFFF) return false;
            bi = &world->GetImpl()->System.GetBodyInterface();
            id = JPH::BodyID(rb->RuntimeBody);
            return true;
        }
    }

    void Physics3DWorld::SetLinearVelocity(Scene& scene, Entity e, const glm::vec3& v)
    {
        JPH::BodyInterface* bi;
        JPH::BodyID id;
        if (BodyFor(scene, e, bi, id)) bi->SetLinearVelocity(id, ToJolt(v));
    }

    glm::vec3 Physics3DWorld::GetLinearVelocity(Scene& scene, Entity e)
    {
        JPH::BodyInterface* bi;
        JPH::BodyID id;
        return BodyFor(scene, e, bi, id) ? FromJolt(bi->GetLinearVelocity(id)) : glm::vec3(0.0f);
    }

    void Physics3DWorld::AddForce(Scene& scene, Entity e, const glm::vec3& f)
    {
        JPH::BodyInterface* bi;
        JPH::BodyID id;
        if (BodyFor(scene, e, bi, id)) bi->AddForce(id, ToJolt(f));
    }

    void Physics3DWorld::AddImpulse(Scene& scene, Entity e, const glm::vec3& i)
    {
        JPH::BodyInterface* bi;
        JPH::BodyID id;
        if (BodyFor(scene, e, bi, id)) bi->AddImpulse(id, ToJolt(i));
    }

    RaycastHit Physics3DWorld::Raycast(Scene& scene, const glm::vec3& origin, const glm::vec3& direction, float maxDistance, uint32_t layerMask)
    {
        RaycastHit result;
        Physics3DWorld* world = scene.GetPhysics3D();
        float len = glm::length(direction);
        if (!world || len <= 0.0f) return result;
        glm::vec3 dir = direction / len * maxDistance;
        auto& system = world->GetImpl()->System;
        JPH::RRayCast ray(JPH::RVec3(ToJolt(origin)), ToJolt(dir));
        JPH::RayCastResult hit;
        if (!system.GetNarrowPhaseQuery().CastRay(ray, hit, {}, LayerMaskFilter(layerMask))) return result;
        result.Hit = world->GetImpl()->Lookup(hit.mBodyID);
        result.Distance = hit.mFraction * maxDistance;
        result.Point = origin + dir * hit.mFraction;
        JPH::BodyLockRead lock(system.GetBodyLockInterface(), hit.mBodyID);
        if (lock.Succeeded())
            result.Normal = FromJolt(lock.GetBody().GetWorldSpaceSurfaceNormal(hit.mSubShapeID2, JPH::RVec3(ToJolt(result.Point))));
        return result;
    }

    namespace
    {
        std::vector<Entity> CollideWith(Scene& scene, const JPH::Shape& shape, const JPH::RMat44& transform, uint32_t layerMask)
        {
            std::vector<Entity> out;
            Physics3DWorld* world = scene.GetPhysics3D();
            if (!world) return out;
            auto* impl = world->GetImpl();
            JPH::CollideShapeSettings settings;
            JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
            impl->System.GetNarrowPhaseQuery().CollideShape(&shape, JPH::Vec3::sReplicate(1.0f), transform, settings, JPH::RVec3::sZero(),
                                                            collector, {}, LayerMaskFilter(layerMask));
            for (auto& hit : collector.mHits)
            {
                Entity e = impl->Lookup(hit.mBodyID2);
                if (!e.IsNull() && std::find(out.begin(), out.end(), e) == out.end()) out.push_back(e);
            }
            return out;
        }
    }

    std::vector<Entity> Physics3DWorld::OverlapSphere(Scene& scene, const glm::vec3& center, float radius, uint32_t layerMask)
    {
        JPH::Ref<JPH::SphereShape> sphere = new JPH::SphereShape(std::max(radius, 1e-4f));
        return CollideWith(scene, *sphere, JPH::RMat44::sTranslation(JPH::RVec3(ToJolt(center))), layerMask);
    }

    std::vector<Entity> Physics3DWorld::OverlapBox(Scene& scene, const glm::vec3& center, const glm::vec3& halfExtents, const glm::quat& rotation,
                                                   uint32_t layerMask)
    {
        glm::vec3 half = glm::max(halfExtents, glm::vec3(1e-3f));
        float convex = std::min(JPH::cDefaultConvexRadius, std::min(half.x, std::min(half.y, half.z)) * 0.5f);
        JPH::Ref<JPH::BoxShape> box = new JPH::BoxShape(ToJolt(half), convex);
        return CollideWith(scene, *box, JPH::RMat44::sRotationTranslation(ToJolt(rotation), JPH::RVec3(ToJolt(center))), layerMask);
    }

    namespace
    {
        void UpdateCharacter(Scene& scene, Entity e, const glm::vec3& velocity, float dt)
        {
            Physics3DWorld* world = scene.GetPhysics3D();
            auto* cc = scene.GetRegistry().TryGet<CharacterControllerComponent>(e);
            if (!world || !cc || !cc->Runtime || dt <= 0.0f) return;
            auto& character = *static_cast<JPH::Ref<JPH::CharacterVirtual>*>(cc->Runtime.get());
            auto& t = scene.GetRegistry().Get<TransformComponent>(e);
            auto* impl = world->GetImpl();

            // Follow teleports done through the transform.
            glm::vec3 start = t.GetWorldPosition();
            if (glm::distance(start, FromJolt(JPH::Vec3(character->GetPosition()))) > 1e-3f) character->SetPosition(JPH::RVec3(ToJolt(start)));

            // Moving platforms: while grounded, the ground's velocity at the contact point is added.
            glm::vec3 total = velocity;
            if (cc->RideMovingPlatforms && character->GetGroundState() == JPH::CharacterBase::EGroundState::OnGround)
            {
                character->UpdateGroundVelocity();
                glm::vec3 ground = FromJolt(character->GetGroundVelocity());
                total += ground;
                cc->GroundVelocity = ground;
            }
            else
                cc->GroundVelocity = glm::vec3(0.0f);
            character->SetLinearVelocity(ToJolt(total));
            JPH::CharacterVirtual::ExtendedUpdateSettings settings;
            settings.mWalkStairsStepUp = JPH::Vec3(0.0f, cc->StepOffset, 0.0f);
            settings.mStickToFloorStepDown = JPH::Vec3(0.0f, -cc->StepOffset, 0.0f);
            JPH::ObjectLayer layer = Layers::Make(scene.GetRegistry().Get<TagComponent>(e).Layer, true);
            JPH::IgnoreSingleBodyFilter ignoreSelf(character->GetInnerBodyID());
            character->ExtendedUpdate(dt, impl->System.GetGravity(), settings, impl->System.GetDefaultBroadPhaseLayerFilter(layer),
                                      impl->System.GetDefaultLayerFilter(layer), ignoreSelf, {}, *impl->TempAllocator);

            glm::vec3 end = FromJolt(JPH::Vec3(character->GetPosition()));
            t.Position += end - start; // entities with characters should be roots; this keeps parented ones consistent too
            cc->Velocity = (end - start) / dt;
            cc->IsGrounded = character->GetGroundState() == JPH::CharacterBase::EGroundState::OnGround;
            scene.UpdateTransforms();
        }
    }

    void Physics3DWorld::MoveCharacter(Scene& scene, Entity e, const glm::vec3& motion)
    {
        float dt = scene.GetDeltaTime();
        if (dt > 0.0f) UpdateCharacter(scene, e, motion / dt, dt);
    }

    void Physics3DWorld::SimpleMoveCharacter(Scene& scene, Entity e, const glm::vec3& speed)
    {
        auto* cc = scene.GetRegistry().TryGet<CharacterControllerComponent>(e);
        Physics3DWorld* world = scene.GetPhysics3D();
        float dt = scene.GetDeltaTime();
        if (!cc || !world || dt <= 0.0f) return;
        float gravity = world->GetImpl()->System.GetGravity().GetY();
        if (cc->IsGrounded && cc->VerticalSpeed <= 0.0f) cc->VerticalSpeed = -1.0f; // keep a little contact with the ground
        else cc->VerticalSpeed += gravity * dt;
        UpdateCharacter(scene, e, glm::vec3(speed.x, cc->VerticalSpeed, speed.z), dt);
        if (cc->IsGrounded && cc->VerticalSpeed < 0.0f) cc->VerticalSpeed = -1.0f;
        else if (!cc->IsGrounded && cc->Velocity.y > -1e-3f && cc->VerticalSpeed > 0.0f && cc->Velocity.y < cc->VerticalSpeed * 0.5f)
            cc->VerticalSpeed = 0.0f; // hit a ceiling
    }
}
