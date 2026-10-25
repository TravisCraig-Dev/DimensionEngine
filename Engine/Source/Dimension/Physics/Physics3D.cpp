#include "Dimension/Physics/Physics3D.h"
#include "Dimension/Physics/ConvexDecomposition.h"
#include "Dimension/Physics/Cloth.h"
#include <Jolt/Jolt.h>
#include <Jolt/Physics/SoftBody/SoftBodySharedSettings.h>
#include <Jolt/Physics/SoftBody/SoftBodyCreationSettings.h>
#include <Jolt/Physics/SoftBody/SoftBodyMotionProperties.h>
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
#include <Jolt/Physics/Collision/Shape/OffsetCenterOfMassShape.h>
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
#include <Jolt/Physics/Vehicle/VehicleConstraint.h>
#include <Jolt/Physics/Vehicle/VehicleCollisionTester.h>
#include <Jolt/Physics/Vehicle/WheeledVehicleController.h>
#include <Jolt/Physics/Vehicle/MotorcycleController.h>
#include <Jolt/Physics/Vehicle/TrackedVehicleController.h>
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
        struct Joint
        {
            Entity A, B;
            JPH::Ref<JPH::Constraint> Constraint;
            JointType Type;
            float MinDistance = 0.0f, MaxDistance = 0.0f; // distance joints: current limits (a winch moves them)
        };
        std::vector<Joint> Joints;
        // Jointed bodies that must not collide share group 0 and get their own sub-group.
        static constexpr JPH::uint kMaxJointSubGroups = 4096;
        JPH::Ref<JPH::GroupFilterTable> JointFilter = new JPH::GroupFilterTable(kMaxJointSubGroups);
        std::unordered_map<JPH::uint32, JPH::uint> SubGroupOf; // body id -> sub-group
        JPH::uint NextSubGroup = 0;
        std::vector<Entity> Characters;
        struct Vehicle
        {
            Entity E;
            JPH::Ref<JPH::VehicleConstraint> Constraint;
        };
        std::vector<Vehicle> Vehicles;
        struct ClothBody
        {
            Entity E;
            JPH::BodyID Body;
            std::vector<glm::vec3> Rest;   // grid positions in the object's space
            std::vector<uint32_t> Pinned;
            std::vector<uint32_t> Indices;
            std::vector<Vertex> Template; // UVs and rest tangents for the rendered vertices
        };
        std::vector<ClothBody> Cloths;
        void RemoveCloth(size_t i)
        {
            auto& bi = System.GetBodyInterface();
            if (bi.IsAdded(Cloths[i].Body)) bi.RemoveBody(Cloths[i].Body);
            bi.DestroyBody(Cloths[i].Body);
            BodyToEntity.erase(Cloths[i].Body.GetIndexAndSequenceNumber());
            Cloths.erase(Cloths.begin() + i);
        }
        void RemoveVehicle(size_t i)
        {
            System.RemoveStepListener(Vehicles[i].Constraint);
            System.RemoveConstraint(Vehicles[i].Constraint);
            Vehicles.erase(Vehicles.begin() + i);
        }
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
        std::vector<Entity> vehicles;
        for (auto [e, v] : reg.ViewOf<VehicleComponent>()) vehicles.push_back(e);
        for (Entity e : vehicles) CreateVehicle(e);
        std::vector<Entity> cloths;
        for (auto [e, c] : reg.ViewOf<ClothComponent>()) cloths.push_back(e);
        for (Entity e : cloths) CreateCloth(e);
        m_Impl->System.OptimizeBroadPhase();
    }

    Physics3DWorld::~Physics3DWorld()
    {
        for (auto& j : m_Impl->Joints) m_Impl->System.RemoveConstraint(j.Constraint);
        m_Impl->Joints.clear();
        while (!m_Impl->Vehicles.empty()) m_Impl->RemoveVehicle(m_Impl->Vehicles.size() - 1);
        while (!m_Impl->Cloths.empty()) m_Impl->RemoveCloth(m_Impl->Cloths.size() - 1);
        for (auto [e, c] : m_Scene.GetRegistry().ViewOf<ClothComponent>())
        {
            c.SimulatedPositions.clear();
            c.SimulatedNormals.clear();
        }
        for (auto [e, cc] : m_Scene.GetRegistry().ViewOf<CharacterControllerComponent>()) cc.Runtime.reset();
        auto& bi = m_Impl->System.GetBodyInterface();
        for (auto [e, rb] : m_Scene.GetRegistry().ViewOf<RigidbodyComponent>())
        {
            if (rb.RuntimeBody == 0xFFFFFFFF) continue;
            JPH::BodyID id(rb.RuntimeBody);
            if (bi.IsAdded(id)) bi.RemoveBody(id);
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
        if (m_Scene.GetRegistry().Has<VehicleComponent>(e)) CreateVehicle(e);
        if (m_Scene.GetRegistry().Has<ClothComponent>(e)) CreateCloth(e);
        for (Entity c : m_Scene.GetChildren(e)) CreateJointsRecursive(c);
    }

    void Physics3DWorld::CreateCloth(Entity e)
    {
        auto& reg = m_Scene.GetRegistry();
        auto& cloth = reg.Get<ClothComponent>(e);
        for (auto& c : m_Impl->Cloths)
            if (c.E == e) return;
        auto& t = reg.Get<TransformComponent>(e);
        MeshData grid = Cloth::BuildGrid(cloth);
        Impl::ClothBody body;
        body.E = e;
        body.Pinned = Cloth::PinnedVertices(cloth);
        body.Indices = grid.Indices;
        body.Template = grid.Vertices;
        // Vertices in world space at the start (the soft body keeps them relative to its fixed position).
        JPH::Ref<JPH::SoftBodySharedSettings> shared = new JPH::SoftBodySharedSettings();
        float invMass = (float)grid.Vertices.size() / std::max(cloth.Mass, 1e-4f);
        std::vector<bool> pinned(grid.Vertices.size(), false);
        for (uint32_t p : body.Pinned) pinned[p] = true;
        glm::vec3 origin = t.GetWorldPosition();
        for (size_t i = 0; i < grid.Vertices.size(); ++i)
        {
            body.Rest.push_back(grid.Vertices[i].Position);
            glm::vec3 w = glm::vec3(t.World * glm::vec4(grid.Vertices[i].Position, 1.0f)) - origin;
            shared->mVertices.push_back(JPH::SoftBodySharedSettings::Vertex(JPH::Float3(w.x, w.y, w.z), JPH::Float3(0, 0, 0), pinned[i] ? 0.0f : invMass));
        }
        for (size_t k = 0; k + 2 < grid.Indices.size(); k += 3)
            shared->mFaces.push_back(JPH::SoftBodySharedSettings::Face(grid.Indices[k], grid.Indices[k + 1], grid.Indices[k + 2]));
        JPH::SoftBodySharedSettings::VertexAttributes attributes(std::max(cloth.StretchCompliance, 0.0f), std::max(cloth.StretchCompliance, 0.0f) * 10.0f,
                                                                std::max(cloth.BendCompliance, 0.0f));
        shared->CreateConstraints(&attributes, 1, JPH::SoftBodySharedSettings::EBendType::Distance);
        shared->Optimize();
        JPH::SoftBodyCreationSettings settings(shared, JPH::RVec3(ToJolt(origin)), JPH::Quat::sIdentity(),
                                               Layers::Make(reg.Get<TagComponent>(e).Layer, true));
        settings.mNumIterations = (JPH::uint32)std::clamp(cloth.Iterations, 1, 64);
        settings.mLinearDamping = std::max(cloth.Damping, 0.0f);
        settings.mFriction = std::clamp(cloth.Friction, 0.0f, 1.0f);
        settings.mVertexRadius = std::max(cloth.Thickness, 0.0f);
        settings.mUpdatePosition = false; // vertices stay relative to the fixed starting point
        settings.mMakeRotationIdentity = false;
        settings.mAllowSleeping = false;  // pinned vertices may move with the object at any time
        settings.mFacesDoubleSided = true;
        auto& bi = m_Impl->System.GetBodyInterface();
        body.Body = bi.CreateAndAddSoftBody(settings, JPH::EActivation::Activate);
        if (body.Body.IsInvalid())
        {
            Log::Error("Too many physics bodies (cloth '{}')", reg.Get<TagComponent>(e).Name);
            return;
        }
        m_Impl->BodyToEntity[body.Body.GetIndexAndSequenceNumber()] = e;
        m_Impl->Cloths.push_back(std::move(body));
    }

    void Physics3DWorld::UpdateCloths(float dt, bool afterStep)
    {
        auto& reg = m_Scene.GetRegistry();
        auto& lockInterface = m_Impl->System.GetBodyLockInterface();
        for (auto& c : m_Impl->Cloths)
        {
            auto* cloth = reg.TryGet<ClothComponent>(c.E);
            if (!cloth) continue;
            auto& t = reg.Get<TransformComponent>(c.E);
            JPH::BodyLockWrite lock(lockInterface, c.Body);
            if (!lock.Succeeded()) continue;
            JPH::Body& body = lock.GetBody();
            auto* mp = static_cast<JPH::SoftBodyMotionProperties*>(body.GetMotionProperties());
            glm::vec3 origin = FromJolt(JPH::Vec3(body.GetCenterOfMassPosition()));
            auto& vertices = mp->GetVertices();
            if (!afterStep)
            {
                // Pinned vertices follow the object: Jolt moves kinematic vertices by their velocity, so they get the
                // velocity that reaches the object's pose over this step. Wind pushes the others.
                for (uint32_t p : c.Pinned)
                {
                    glm::vec3 target = glm::vec3(t.World * glm::vec4(c.Rest[p], 1.0f)) - origin;
                    glm::vec3 now = FromJolt(vertices[p].mPosition);
                    if (dt > 0.0f) vertices[p].mVelocity = ToJolt((target - now) / dt);
                    else vertices[p].mPosition = ToJolt(target);
                }
                if (glm::dot(cloth->Wind, cloth->Wind) > 0.0f)
                    for (auto& v : vertices)
                        if (v.mInvMass > 0.0f) v.mVelocity += ToJolt(cloth->Wind * dt);
                continue;
            }
            // Simulated shape back into the object's space, for rendering.
            glm::mat4 toLocal = glm::inverse(t.World);
            std::vector<Vertex> shaped = c.Template;
            for (size_t i = 0; i < vertices.size() && i < shaped.size(); ++i)
                shaped[i].Position = glm::vec3(toLocal * glm::vec4(origin + FromJolt(vertices[i].mPosition), 1.0f));
            Cloth::RecomputeNormals(shaped, c.Indices);
            cloth->SimulatedPositions.resize(shaped.size());
            cloth->SimulatedNormals.resize(shaped.size());
            for (size_t i = 0; i < shaped.size(); ++i)
            {
                cloth->SimulatedPositions[i] = shaped[i].Position;
                cloth->SimulatedNormals[i] = shaped[i].Normal;
            }
        }
    }

    void Physics3DWorld::CreateVehicle(Entity e)
    {
        auto& reg = m_Scene.GetRegistry();
        auto& vc = reg.Get<VehicleComponent>(e);
        auto* rb = reg.TryGet<RigidbodyComponent>(e);
        for (auto& v : m_Impl->Vehicles)
            if (v.E == e) return;
        if (!rb || rb->RuntimeBody == 0xFFFFFFFF || rb->Type != BodyType::Dynamic)
        {
            Log::Warn("Vehicle '{}' needs a dynamic Rigidbody with a collider", reg.Get<TagComponent>(e).Name);
            return;
        }
        JPH::VehicleConstraintSettings settings;
        settings.mUp = JPH::Vec3(0, 1, 0);
        settings.mForward = JPH::Vec3(0, 0, -1);
        settings.mMaxPitchRollAngle = JPH::DegreesToRadians(60.0f);
        float hx = vc.TrackWidth * 0.5f, hz = vc.WheelBase * 0.5f;
        auto suspension = [&](JPH::WheelSettings& w, const glm::vec3& mount) {
            w.mPosition = ToJolt(mount);
            w.mSuspensionDirection = JPH::Vec3(0, -1, 0);
            w.mSteeringAxis = JPH::Vec3(0, 1, 0);
            w.mWheelUp = JPH::Vec3(0, 1, 0);
            w.mWheelForward = JPH::Vec3(0, 0, -1);
            w.mSuspensionMinLength = 0.05f;
            w.mSuspensionMaxLength = 0.05f + std::max(vc.SuspensionTravel, 0.01f);
            w.mSuspensionSpring.mFrequency = std::max(vc.SuspensionFrequency, 0.1f);
            w.mSuspensionSpring.mDamping = std::max(vc.SuspensionDamping, 0.0f);
            w.mRadius = std::max(vc.WheelRadius, 0.05f);
            w.mWidth = std::max(vc.WheelWidth, 0.02f);
        };
        auto engine = [&](JPH::VehicleEngineSettings& en) {
            en.mMaxTorque = std::max(vc.EngineTorque, 0.0f);
            en.mMaxRPM = std::max(vc.MaxRPM, 500.0f);
            en.mMinRPM = std::min(1000.0f, en.mMaxRPM * 0.5f);
        };
        if (vc.Type == VehicleType::Tank)
        {
            // Tracks: Track Wheels road wheels per side along the wheel base (left side first); the rear one drives.
            int perSide = std::clamp(vc.TrackWheels, 2, 16);
            auto* controller = new JPH::TrackedVehicleControllerSettings();
            engine(controller->mEngine);
            for (int side = 0; side < 2; ++side)
            {
                auto& track = controller->mTracks[side];
                track.mWheels.clear();
                for (int k = 0; k < perSide; ++k)
                {
                    auto* w = new JPH::WheelSettingsTV();
                    float z = -hz + vc.WheelBase * (float)k / (float)(perSide - 1);
                    suspension(*w, { side == 0 ? -hx : hx, vc.WheelHeight, z });
                    w->mLongitudinalFriction = 4.0f;
                    w->mLateralFriction = 2.0f;
                    track.mWheels.push_back((JPH::uint)settings.mWheels.size());
                    settings.mWheels.push_back(w);
                }
                track.mDrivenWheel = track.mWheels.back();
                track.mMaxBrakeTorque = std::max(vc.BrakeTorque, 0.0f);
            }
            settings.mController = controller;
        }
        else if (vc.Type == VehicleType::Motorcycle)
        {
            // Two wheels on the center line (front, rear). The front steers about an axis raked back by 25 degrees.
            for (int i = 0; i < 2; ++i)
            {
                auto* w = new JPH::WheelSettingsWV();
                suspension(*w, { 0.0f, vc.WheelHeight, i == 0 ? -hz : hz });
                if (i == 0)
                {
                    float caster = JPH::DegreesToRadians(25.0f);
                    w->mSteeringAxis = JPH::Vec3(0, std::cos(caster), std::sin(caster));
                    w->mSuspensionDirection = -w->mSteeringAxis;
                    w->mMaxSteerAngle = JPH::DegreesToRadians(std::clamp(vc.MaxSteerAngle, 0.0f, 80.0f));
                }
                w->mMaxBrakeTorque = std::max(vc.BrakeTorque, 0.0f);
                w->mMaxHandBrakeTorque = i == 0 ? 0.0f : std::max(vc.HandBrakeTorque, 0.0f);
                settings.mWheels.push_back(w);
            }
            // Gearing of a road bike (Jolt's motorcycle sample).
            auto* controller = new JPH::MotorcycleControllerSettings();
            engine(controller->mEngine);
            controller->mTransmission.mShiftDownRPM = 0.2f * controller->mEngine.mMaxRPM;
            controller->mTransmission.mShiftUpRPM = 0.8f * controller->mEngine.mMaxRPM;
            controller->mTransmission.mGearRatios = { 2.27f, 1.63f, 1.3f, 1.09f, 0.96f, 0.88f };
            controller->mTransmission.mReverseGearRatios = { -4.0f };
            controller->mTransmission.mClutchStrength = 2.0f;
            controller->mDifferentials.resize(1);
            controller->mDifferentials[0].mLeftWheel = -1; // the rear wheel alone
            controller->mDifferentials[0].mRightWheel = 1;
            controller->mDifferentials[0].mDifferentialRatio = 1.93f * 40.0f / 16.0f;
            settings.mController = controller;
        }
        else
        {
            // Wheels: front left, front right, rear left, rear right (the front is -Z, the right +X).
            const glm::vec3 mounts[4] = { { -hx, vc.WheelHeight, -hz }, { hx, vc.WheelHeight, -hz }, { -hx, vc.WheelHeight, hz }, { hx, vc.WheelHeight, hz } };
            for (int i = 0; i < 4; ++i)
            {
                bool front = i < 2;
                auto* w = new JPH::WheelSettingsWV();
                suspension(*w, mounts[i]);
                w->mMaxSteerAngle = front ? JPH::DegreesToRadians(std::clamp(vc.MaxSteerAngle, 0.0f, 80.0f)) : 0.0f;
                w->mMaxBrakeTorque = std::max(vc.BrakeTorque, 0.0f);
                w->mMaxHandBrakeTorque = front ? 0.0f : std::max(vc.HandBrakeTorque, 0.0f);
                settings.mWheels.push_back(w);
            }
            auto* controller = new JPH::WheeledVehicleControllerSettings();
            engine(controller->mEngine);
            controller->mDifferentials.clear();
            auto axle = [&](int left, int right, float share) {
                JPH::VehicleDifferentialSettings d;
                d.mLeftWheel = left;
                d.mRightWheel = right;
                d.mEngineTorqueRatio = share;
                controller->mDifferentials.push_back(d);
            };
            if (vc.Drive != VehicleDrive::RearWheel) axle(0, 1, vc.Drive == VehicleDrive::AllWheel ? 0.5f : 1.0f);
            if (vc.Drive != VehicleDrive::FrontWheel) axle(2, 3, vc.Drive == VehicleDrive::AllWheel ? 0.5f : 1.0f);
            settings.mController = controller;
        }

        JPH::BodyID id(rb->RuntimeBody);
        JPH::BodyLockWrite lock(m_Impl->System.GetBodyLockInterface(), id);
        if (!lock.Succeeded()) return;
        JPH::Body& body = lock.GetBody();
        auto* constraint = new JPH::VehicleConstraint(body, settings);
        // Wheels touch what the body's layer collides with, as cylinders (no snagging between ray hits).
        // (a motorcycle's are fully rounded, so they roll over their edge as it leans)
        constraint->SetVehicleCollisionTester(new JPH::VehicleCollisionTesterCastCylinder(body.GetObjectLayer(), vc.Type == VehicleType::Motorcycle ? 1.0f : 0.1f));
        m_Impl->System.AddConstraint(constraint);
        m_Impl->System.AddStepListener(constraint);
        m_Impl->Vehicles.push_back({ e, constraint });
    }

    void Physics3DWorld::UpdateVehicles(bool afterStep)
    {
        auto& reg = m_Scene.GetRegistry();
        auto& bi = m_Impl->System.GetBodyInterface();
        for (auto& v : m_Impl->Vehicles)
        {
            auto* vc = reg.TryGet<VehicleComponent>(v.E);
            auto* rb = reg.TryGet<RigidbodyComponent>(v.E);
            if (!vc || !rb || rb->RuntimeBody == 0xFFFFFFFF) continue;
            JPH::BodyID id(rb->RuntimeBody);
            // Cars and motorcycles (a MotorcycleController is a wheeled one); tanks use their tracked controller below.
            auto* controller = vc->Type == VehicleType::Tank ? nullptr : static_cast<JPH::WheeledVehicleController*>(v.Constraint->GetController());
            JPH::RVec3 pos;
            JPH::Quat rot;
            bi.GetPositionAndRotation(id, pos, rot);
            glm::quat q = FromJolt(rot);
            float forwardSpeed = glm::dot(FromJolt(bi.GetLinearVelocity(id)), q * glm::vec3(0, 0, -1));
            if (!afterStep)
            {
                // Throttle against the motion brakes first (then reverses or drives once nearly stopped).
                float throttle = std::clamp(vc->Throttle, -1.0f, 1.0f), brake = std::clamp(vc->Brake, 0.0f, 1.0f);
                if ((throttle > 0.0f && forwardSpeed < -1.0f) || (throttle < 0.0f && forwardSpeed > 1.0f))
                {
                    brake = std::max(brake, std::abs(throttle));
                    throttle = 0.0f;
                }
                float steer = std::clamp(vc->Steering, -1.0f, 1.0f);
                if (vc->Type == VehicleType::Tank)
                {
                    // Steering slows (and past half reverses) the inner track; standing still, the tracks counter-rotate
                    // to turn on the spot. Jolt needs non-zero ratios.
                    float left = steer > 0.0f ? 1.0f : 1.0f + 2.0f * steer, right = steer < 0.0f ? 1.0f : 1.0f - 2.0f * steer;
                    if (std::abs(throttle) < 0.05f && steer != 0.0f)
                    {
                        throttle = 0.5f * std::abs(steer);
                        left = steer > 0.0f ? 1.0f : -1.0f;
                        right = -left;
                    }
                    auto nonZero = [](float r) { return std::abs(r) < 0.05f ? (r < 0.0f ? -0.05f : 0.05f) : r; };
                    static_cast<JPH::TrackedVehicleController*>(v.Constraint->GetController())->SetDriverInput(throttle, nonZero(left), nonZero(right), brake);
                }
                else // (Jolt's motorcycle steers the other way round for the same input)
                    controller->SetDriverInput(throttle, vc->Type == VehicleType::Motorcycle ? -steer : steer, brake, std::clamp(vc->HandBrake, 0.0f, 1.0f));
                if (throttle != 0.0f || vc->Steering != 0.0f || brake != 0.0f) bi.ActivateBody(id);
                continue;
            }
            vc->SpeedKmh = forwardSpeed * 3.6f;
            if (vc->Type == VehicleType::Tank)
            {
                auto* tracked = static_cast<JPH::TrackedVehicleController*>(v.Constraint->GetController());
                vc->EngineRPM = tracked->GetEngine().GetCurrentRPM();
                vc->Gear = tracked->GetTransmission().GetCurrentGear();
            }
            else
            {
                vc->EngineRPM = controller->GetEngine().GetCurrentRPM();
                vc->Gear = controller->GetTransmission().GetCurrentGear();
            }
            // Wheel visuals (and grounded flags) for the four slots: the car's wheels; a motorcycle's front and rear in
            // the left slots; a tank's corner road wheels. The cylinder's Y axis is the axle (out of the vehicle).
            int count = (int)v.Constraint->GetWheels().size(), perSide = count / 2;
            std::array<int, 4> wheelOf = vc->Type == VehicleType::Motorcycle ? std::array<int, 4>{ 0, -1, 1, -1 }
                                       : vc->Type == VehicleType::Tank     ? std::array<int, 4>{ 0, perSide, perSide - 1, count - 1 }
                                                                           : std::array<int, 4>{ 0, 1, 2, 3 };
            const Entity visuals[4] = { vc->WheelFrontLeft, vc->WheelFrontRight, vc->WheelRearLeft, vc->WheelRearRight };
            auto& carT = reg.Get<TransformComponent>(v.E);
            glm::mat4 carWorld = glm::translate(glm::mat4(1.0f), FromJolt(JPH::Vec3(pos))) * glm::toMat4(q) * glm::scale(glm::mat4(1.0f), carT.Scale);
            for (int slot = 0; slot < 4; ++slot)
            {
                int i = wheelOf[slot];
                vc->WheelGrounded[slot] = i >= 0 && i < count && v.Constraint->GetWheel(i)->HasContact();
                Entity w = visuals[slot];
                if (i < 0 || i >= count || w.IsNull() || !m_Scene.IsValid(w) || w == v.E) continue;
                JPH::RMat44 m = v.Constraint->GetWheelWorldTransform(i, JPH::Vec3(0, slot % 2 == 0 ? -1.0f : 1.0f, 0), JPH::Vec3(1, 0, 0));
                glm::mat4 world(1.0f);
                for (int c = 0; c < 4; ++c)
                {
                    JPH::Vec4 col = c < 3 ? JPH::Vec4(m.GetColumn3(c), 0.0f) : JPH::Vec4(JPH::Vec3(m.GetTranslation()), 1.0f);
                    world[c] = glm::vec4(col.GetX(), col.GetY(), col.GetZ(), col.GetW());
                }
                Entity parent = m_Scene.GetParent(w);
                glm::mat4 parentWorld = parent.IsNull() ? glm::mat4(1.0f) : parent == v.E ? carWorld : reg.Get<TransformComponent>(parent).World;
                glm::mat4 local = glm::inverse(parentWorld) * world;
                auto& wt = reg.Get<TransformComponent>(w);
                wt.Position = glm::vec3(local[3]);
                glm::mat3 r(local);
                for (int c = 0; c < 3; ++c) r[c] = glm::normalize(r[c]);
                wt.Rotation = glm::eulerAngles(glm::quat_cast(r));
            }
        }
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
        if (j.Type == JointType::Distance)
        {
            auto* d = static_cast<JPH::DistanceConstraint*>(constraint);
            m_Impl->Joints.back().MinDistance = d->GetMinDistance();
            m_Impl->Joints.back().MaxDistance = d->GetMaxDistance();
            j.CurrentLength = d->GetMaxDistance();
        }
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
                if (!mc->Convex && !mc->ConvexDecomposition && rb.Type == BodyType::Dynamic)
                    Log::Warn("Mesh Collider on '{}': dynamic bodies use the convex hull (or Convex Decomposition)", reg.Get<TagComponent>(e).Name);
                auto parts = convex && mc->ConvexDecomposition ? ConvexDecomposition::Decompose(data, mc->MaxParts) : nullptr;
                if (parts && !parts->empty())
                {
                    for (const ConvexPart& part : *parts)
                    {
                        JPH::Array<JPH::Vec3> points;
                        for (const glm::vec3& p : part.Points) points.push_back(ToJolt(p * scale));
                        compound.AddShape(JPH::Vec3::sZero(), JPH::Quat::sIdentity(), new JPH::ConvexHullShapeSettings(points));
                    }
                }
                else if (convex)
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
        // Vehicles: a lower center of mass keeps them on their wheels.
        if (auto* vc = reg.TryGet<VehicleComponent>(e); vc && glm::dot(vc->CenterOfMassOffset, vc->CenterOfMassOffset) > 0.0f)
            if (auto offset = JPH::OffsetCenterOfMassShapeSettings(ToJolt(vc->CenterOfMassOffset), shapeResult.Get()).Create(); !offset.HasError())
                shapeResult = offset;

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
        if (!t.ActiveInHierarchy) bi.RemoveBody(id); // inactive objects have no collider until they are activated
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
        for (size_t i = 0; i < m_Impl->Vehicles.size(); ++i)
            if (m_Impl->Vehicles[i].E == e)
            {
                m_Impl->RemoveVehicle(i);
                break;
            }
        for (size_t i = 0; i < m_Impl->Cloths.size(); ++i)
            if (m_Impl->Cloths[i].E == e)
            {
                m_Impl->RemoveCloth(i);
                break;
            }
        auto* rb = m_Scene.GetRegistry().TryGet<RigidbodyComponent>(e);
        if (!rb || rb->RuntimeBody == 0xFFFFFFFF) return;
        auto& bi = m_Impl->System.GetBodyInterface();
        JPH::BodyID id(rb->RuntimeBody);
        if (bi.IsAdded(id)) bi.RemoveBody(id);
        bi.DestroyBody(id);
        m_Impl->BodyToEntity.erase(rb->RuntimeBody);
        m_Impl->Sensors.erase(rb->RuntimeBody);
        rb->RuntimeBody = 0xFFFFFFFF;
    }

    void Physics3DWorld::Step(float dt)
    {
        auto& reg = m_Scene.GetRegistry();
        auto& bi = m_Impl->System.GetBodyInterface();

        // Bodies of inactive objects leave the world and come back (where the object now is) when it is activated.
        for (auto [e, rb, t] : reg.ViewOf<RigidbodyComponent, TransformComponent>())
        {
            if (rb.RuntimeBody == 0xFFFFFFFF) continue;
            JPH::BodyID id(rb.RuntimeBody);
            bool added = bi.IsAdded(id);
            if (added && !t.ActiveInHierarchy) bi.RemoveBody(id);
            else if (!added && t.ActiveInHierarchy)
            {
                bi.SetPositionAndRotation(id, JPH::RVec3(ToJolt(t.GetWorldPosition())), ToJolt(t.GetRotationQuat()), JPH::EActivation::DontActivate);
                bi.AddBody(id, JPH::EActivation::Activate);
            }
        }

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
            if (jc && joint.Type == JointType::Distance && jc->UseMotor && jc->MotorSpeed != 0.0f)
            {
                // Winch: the joint's length changes by Motor Speed per second (both limits, so a rope keeps its
                // slack). Reeling in stalls while the joint already pulls harder than Motor Max Torque (as a force).
                auto* d = static_cast<JPH::DistanceConstraint*>(joint.Constraint.GetPtr());
                float pull = dt > 0.0f ? std::abs(d->GetTotalLambdaPosition()) / dt : 0.0f;
                if (jc->MotorSpeed > 0.0f || pull <= std::max(jc->MotorMaxTorque, 0.0f))
                {
                    float delta = jc->MotorSpeed * dt;
                    joint.MinDistance = std::max(joint.MinDistance + delta, 0.01f);
                    joint.MaxDistance = std::max(joint.MaxDistance + delta, joint.MinDistance);
                    d->SetDistance(joint.MinDistance, joint.MaxDistance);
                    bi.ActivateBody(JPH::BodyID(reg.Get<RigidbodyComponent>(joint.A).RuntimeBody));
                    if (auto* rbB = m_Scene.IsValid(joint.B) ? reg.TryGet<RigidbodyComponent>(joint.B) : nullptr) bi.ActivateBody(JPH::BodyID(rbB->RuntimeBody));
                }
                jc->CurrentLength = joint.MaxDistance;
                continue;
            }
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

        UpdateVehicles(false);
        UpdateCloths(dt, false);
        m_Impl->System.Update(dt, 1, m_Impl->TempAllocator.get(), m_Impl->JobSystem.get());
        UpdateCloths(dt, true);

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
        UpdateVehicles(true);

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

    std::vector<RaycastHit> Physics3DWorld::RaycastAll(Scene& scene, const glm::vec3& origin, const glm::vec3& direction, float maxDistance,
                                                       uint32_t layerMask)
    {
        std::vector<RaycastHit> out;
        Physics3DWorld* world = scene.GetPhysics3D();
        float len = glm::length(direction);
        if (!world || len <= 0.0f) return out;
        glm::vec3 dir = direction / len * maxDistance;
        auto& system = world->GetImpl()->System;
        JPH::RRayCast ray(JPH::RVec3(ToJolt(origin)), ToJolt(dir));
        JPH::AllHitCollisionCollector<JPH::CastRayCollector> collector;
        system.GetNarrowPhaseQuery().CastRay(ray, JPH::RayCastSettings(), collector, {}, LayerMaskFilter(layerMask));
        collector.Sort();
        for (const JPH::RayCastResult& hit : collector.mHits)
        {
            RaycastHit r;
            r.Hit = world->GetImpl()->Lookup(hit.mBodyID);
            if (r.Hit.IsNull() || (!out.empty() && out.back().Hit == r.Hit)) continue;
            r.Distance = hit.mFraction * maxDistance;
            r.Point = origin + dir * hit.mFraction;
            JPH::BodyLockRead lock(system.GetBodyLockInterface(), hit.mBodyID);
            if (lock.Succeeded())
                r.Normal = FromJolt(lock.GetBody().GetWorldSpaceSurfaceNormal(hit.mSubShapeID2, JPH::RVec3(ToJolt(r.Point))));
            out.push_back(r);
        }
        return out;
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
