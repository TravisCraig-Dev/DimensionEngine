#include "Dimension/Particles/ParticleSystem.h"
#include "Dimension/Scene/Scene.h"
#include "Dimension/Physics/Physics3D.h"
#include <glm/gtc/constants.hpp>
#include <algorithm>
#include <cmath>

namespace Dimension
{
    namespace
    {
        // xorshift32: small, fast and deterministic per system.
        float Random01(uint32_t& state)
        {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            return (state >> 8) * (1.0f / 16777216.0f);
        }
        float Range(uint32_t& state, const glm::vec2& r) { return r.x + (r.y - r.x) * Random01(state); }

        struct SubEmitRequest { Entity Target; glm::vec3 Position; int32_t Count; Entity Source = Entity::Null(); };

        // Emits one particle. `origin` (world space) overrides the emitter position (sub-emitter bursts).
        void EmitOne(ParticleSystemComponent& ps, const glm::mat4& world, const glm::vec3* origin, std::vector<SubEmitRequest>& subEmits)
        {
            if ((int32_t)ps.Particles.size() >= ps.MaxParticles) return;
            uint32_t& rng = ps.RandomState;
            glm::vec3 pos(0.0f), dir(0.0f, 1.0f, 0.0f);
            switch (ps.Shape)
            {
            case ParticleShape::Cone:
            {
                // Random point on the base disc, direction spread within the cone around +Y.
                float a = Random01(rng) * glm::two_pi<float>();
                float r = std::sqrt(Random01(rng)) * ps.ShapeRadius;
                pos = { std::cos(a) * r, 0.0f, std::sin(a) * r };
                float spread = glm::radians(ps.ShapeAngle) * std::sqrt(Random01(rng));
                float b = Random01(rng) * glm::two_pi<float>();
                dir = { std::sin(spread) * std::cos(b), std::cos(spread), std::sin(spread) * std::sin(b) };
                break;
            }
            case ParticleShape::Sphere:
            {
                float z = Random01(rng) * 2.0f - 1.0f, a = Random01(rng) * glm::two_pi<float>();
                float s = std::sqrt(std::max(0.0f, 1.0f - z * z));
                dir = { s * std::cos(a), z, s * std::sin(a) };
                pos = dir * ps.ShapeRadius * std::cbrt(Random01(rng));
                break;
            }
            case ParticleShape::Box:
                pos = (glm::vec3(Random01(rng), Random01(rng), Random01(rng)) - 0.5f) * ps.BoxSize;
                break;
            case ParticleShape::Point: break;
            }

            // The emitter frame: its transform, moved to the burst origin for sub-emitter bursts.
            glm::mat4 frame = world;
            if (origin) frame[3] = glm::vec4(*origin, 1.0f);

            Particle p;
            float speed = Range(rng, ps.StartSpeed);
            glm::vec3 worldPos = glm::vec3(frame * glm::vec4(pos, 1.0f));
            if (ps.SimulationSpace == ParticleSpace::World)
            {
                p.Position = worldPos;
                glm::vec3 d = glm::vec3(frame * glm::vec4(dir, 0.0f));
                p.Velocity = (glm::length(d) > 1e-6f ? glm::normalize(d) : dir) * speed;
            }
            else
            {
                p.Position = origin ? glm::vec3(glm::inverse(world) * glm::vec4(worldPos, 1.0f)) : pos;
                p.Velocity = dir * speed;
            }
            p.Color = glm::mix(ps.StartColorA, ps.StartColorB, Random01(rng));
            p.Age = 0.0f;
            p.Lifetime = std::max(0.01f, Range(rng, ps.StartLifetime));
            p.Size = Range(rng, ps.StartSize);
            p.Rotation = glm::radians(Range(rng, ps.StartRotation));
            p.AngularVelocity = glm::radians(Range(rng, ps.AngularVelocity));
            ps.Particles.push_back(p);
            if (ps.SubEmitterTrigger == ParticleSubEmitTrigger::Birth && !ps.SubEmitter.IsNull())
                subEmits.push_back({ ps.SubEmitter, worldPos, ps.SubEmitCount });
        }

        // Moves a particle and resolves collisions. Works in world space; local-space particles are converted.
        void Integrate(Scene& scene, ParticleSystemComponent& ps, Particle& p, const glm::mat4& world, const glm::mat4& invWorld,
                       const glm::vec3& gravity, float damping, float dt, std::vector<SubEmitRequest>& subEmits)
        {
            bool local = ps.SimulationSpace == ParticleSpace::Local;
            p.Age += dt;
            p.Velocity = (p.Velocity + gravity * dt) * damping;
            float speedScale = ps.SpeedOverLifetime.Evaluate(std::clamp(p.Age / p.Lifetime, 0.0f, 1.0f));
            glm::vec3 previous = p.Position;
            p.Position += p.Velocity * speedScale * dt;
            p.Rotation += p.AngularVelocity * dt;
            if (ps.Collision == ParticleCollision::None) return;

            glm::vec3 from = local ? glm::vec3(world * glm::vec4(previous, 1.0f)) : previous;
            glm::vec3 to = local ? glm::vec3(world * glm::vec4(p.Position, 1.0f)) : p.Position;
            glm::vec3 hitPoint, normal;
            bool hit = false;
            if (ps.Collision == ParticleCollision::Plane)
            {
                float plane = ps.CollisionPlaneHeight + ps.CollisionRadius;
                if (to.y < plane && from.y >= plane - 1e-4f)
                {
                    float t = (from.y - plane) / std::max(from.y - to.y, 1e-6f);
                    hitPoint = glm::mix(from, to, std::clamp(t, 0.0f, 1.0f));
                    hitPoint.y = plane;
                    normal = { 0, 1, 0 };
                    hit = true;
                }
                else if (to.y < plane) // started below (e.g. spawned inside): push out
                {
                    hitPoint = { to.x, plane, to.z };
                    normal = { 0, 1, 0 };
                    hit = true;
                }
            }
            else if (scene.GetPhysics3D())
            {
                glm::vec3 delta = to - from;
                float len = glm::length(delta);
                if (len > 1e-6f)
                    if (RaycastHit r = Physics3DWorld::Raycast(scene, from, delta, len + ps.CollisionRadius, Physics::kAllLayers))
                    {
                        hitPoint = r.Point + r.Normal * ps.CollisionRadius;
                        normal = r.Normal;
                        hit = true;
                    }
            }
            if (!hit) return;

            glm::vec3 v = local ? glm::vec3(world * glm::vec4(p.Velocity, 0.0f)) : p.Velocity;
            float vn = glm::dot(v, normal);
            if (vn < 0.0f)
            {
                glm::vec3 normalPart = normal * vn, tangent = v - normalPart;
                v = tangent * (1.0f - ps.CollisionDampen) - normalPart * ps.Bounce;
            }
            p.Velocity = local ? glm::vec3(invWorld * glm::vec4(v, 0.0f)) : v;
            p.Position = local ? glm::vec3(invWorld * glm::vec4(hitPoint, 1.0f)) : hitPoint;
            p.Lifetime -= p.Lifetime * std::clamp(ps.LifetimeLoss, 0.0f, 1.0f);
            if (ps.SubEmitterTrigger == ParticleSubEmitTrigger::Collision && !ps.SubEmitter.IsNull())
                subEmits.push_back({ ps.SubEmitter, hitPoint, ps.SubEmitCount });
        }

        void Simulate(Scene& scene, ParticleSystemComponent& ps, const glm::mat4& world, bool active, float dt,
                      std::vector<SubEmitRequest>& subEmits)
        {
            auto emit = [&](const glm::vec3* origin) { EmitOne(ps, world, origin, subEmits); };
            if (!ps.Started && ps.Playing)
            {
                ps.Started = true;
                ps.Time = 0.0f;
                ps.EmitAccumulator = 0.0f;
                for (int i = 0; i < ps.BurstCount; ++i) emit(nullptr);
            }

            if (ps.Playing && active)
            {
                ps.Time += dt;
                if (ps.Time >= ps.Duration)
                {
                    if (ps.Looping)
                    {
                        ps.Time = std::fmod(ps.Time, std::max(ps.Duration, 1e-3f));
                        for (int i = 0; i < ps.BurstCount; ++i) emit(nullptr);
                    }
                    else
                        ps.Playing = false;
                }
                if (ps.Playing)
                {
                    ps.EmitAccumulator += ps.RateOverTime * dt;
                    int count = (int)ps.EmitAccumulator;
                    ps.EmitAccumulator -= (float)count;
                    for (int i = 0; i < count; ++i) emit(nullptr);
                }
            }
            for (; ps.PendingEmit > 0; --ps.PendingEmit) emit(nullptr);
            for (auto& burst : ps.PendingBursts)
                for (int i = 0; i < burst.Count; ++i) emit(&burst.WorldPosition);
            ps.PendingBursts.clear();

            glm::mat4 invWorld = glm::inverse(world);
            glm::vec3 gravity(0.0f, -9.81f * ps.GravityModifier, 0.0f);
            if (ps.SimulationSpace == ParticleSpace::Local) gravity = glm::vec3(invWorld * glm::vec4(gravity, 0.0f));
            float damping = std::max(0.0f, 1.0f - ps.Drag * dt);
            float trailInterval = std::max(ps.TrailLifetime, 1e-3f) / Particle::kTrailPoints;
            for (auto& p : ps.Particles)
            {
                if (ps.Trails)
                {
                    p.TrailTimer += dt;
                    if (p.TrailTimer >= trailInterval || p.TrailCount == 0)
                    {
                        p.TrailTimer = 0.0f;
                        std::move_backward(p.Trail.begin(), p.Trail.end() - 1, p.Trail.end());
                        p.Trail[0] = p.Position;
                        p.TrailCount = (uint8_t)std::min<int>(p.TrailCount + 1, Particle::kTrailPoints);
                    }
                }
                Integrate(scene, ps, p, world, invWorld, gravity, damping, dt, subEmits);
            }
            bool deathEmits = ps.SubEmitterTrigger == ParticleSubEmitTrigger::Death && !ps.SubEmitter.IsNull();
            std::erase_if(ps.Particles, [&](const Particle& p) {
                if (p.Age < p.Lifetime) return false;
                if (deathEmits)
                {
                    glm::vec3 wp = ps.SimulationSpace == ParticleSpace::Local ? glm::vec3(world * glm::vec4(p.Position, 1.0f)) : p.Position;
                    subEmits.push_back({ ps.SubEmitter, wp, ps.SubEmitCount });
                }
                return true;
            });
        }
    }

    namespace ParticleSystems
    {
        void Update(Scene& scene, float dt)
        {
            auto& reg = scene.GetRegistry();
            std::vector<SubEmitRequest> subEmits;
            for (auto [e, ps, t] : reg.ViewOf<ParticleSystemComponent, TransformComponent>())
            {
                if (ps.RandomState == 0x9E3779B9u) ps.RandomState ^= (e.Index + 1) * 2654435761u; // distinct streams
                if (!ps.Started && !ps.Playing && ps.PlayOnAwake && ps.Time == 0.0f && ps.Particles.empty() && ps.PendingEmit == 0)
                    ps.Playing = true;
                size_t first = subEmits.size();
                Simulate(scene, ps, t.World, t.ActiveInHierarchy, dt, subEmits);
                for (size_t i = first; i < subEmits.size(); ++i) subEmits[i].Source = e;
            }
            // Queued for the target's next update (a system cannot feed itself).
            for (auto& s : subEmits)
                if (s.Target != s.Source && scene.IsValid(s.Target))
                    if (auto* target = reg.TryGet<ParticleSystemComponent>(s.Target)) target->EmitAt(s.Position, s.Count);
        }

        void Restart(Scene& scene)
        {
            for (auto [e, ps] : scene.GetRegistry().ViewOf<ParticleSystemComponent>())
            {
                ps.Particles.clear();
                ps.PendingBursts.clear();
                ps.Time = 0.0f;
                ps.EmitAccumulator = 0.0f;
                ps.Started = false;
                ps.Playing = ps.PlayOnAwake;
                ps.PendingEmit = 0;
            }
        }
    }
}
