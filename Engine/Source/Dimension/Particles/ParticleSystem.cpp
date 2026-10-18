#include "Dimension/Particles/ParticleSystem.h"
#include "Dimension/Scene/Scene.h"
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

        void EmitOne(ParticleSystemComponent& ps, const glm::mat4& world)
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

            Particle p;
            float speed = Range(rng, ps.StartSpeed);
            if (ps.SimulationSpace == ParticleSpace::World)
            {
                p.Position = glm::vec3(world * glm::vec4(pos, 1.0f));
                p.Velocity = glm::normalize(glm::vec3(world * glm::vec4(dir, 0.0f))) * speed;
            }
            else
            {
                p.Position = pos;
                p.Velocity = dir * speed;
            }
            p.Color = glm::mix(ps.StartColorA, ps.StartColorB, Random01(rng));
            p.Age = 0.0f;
            p.Lifetime = std::max(0.01f, Range(rng, ps.StartLifetime));
            p.Size = Range(rng, ps.StartSize);
            p.Rotation = glm::radians(Range(rng, ps.StartRotation));
            p.AngularVelocity = glm::radians(Range(rng, ps.AngularVelocity));
            ps.Particles.push_back(p);
        }

        void Simulate(ParticleSystemComponent& ps, const glm::mat4& world, bool active, float dt)
        {
            if (!ps.Started && ps.Playing)
            {
                ps.Started = true;
                ps.Time = 0.0f;
                ps.EmitAccumulator = 0.0f;
                for (int i = 0; i < ps.BurstCount; ++i) EmitOne(ps, world);
            }

            if (ps.Playing && active)
            {
                ps.Time += dt;
                if (ps.Time >= ps.Duration)
                {
                    if (ps.Looping)
                    {
                        ps.Time = std::fmod(ps.Time, std::max(ps.Duration, 1e-3f));
                        for (int i = 0; i < ps.BurstCount; ++i) EmitOne(ps, world);
                    }
                    else
                        ps.Playing = false;
                }
                if (ps.Playing)
                {
                    ps.EmitAccumulator += ps.RateOverTime * dt;
                    int count = (int)ps.EmitAccumulator;
                    ps.EmitAccumulator -= (float)count;
                    for (int i = 0; i < count; ++i) EmitOne(ps, world);
                }
            }
            for (; ps.PendingEmit > 0; --ps.PendingEmit) EmitOne(ps, world);

            glm::vec3 gravity(0.0f, -9.81f * ps.GravityModifier, 0.0f);
            if (ps.SimulationSpace == ParticleSpace::Local) gravity = glm::vec3(glm::inverse(world) * glm::vec4(gravity, 0.0f));
            float damping = std::max(0.0f, 1.0f - ps.Drag * dt);
            for (auto& p : ps.Particles)
            {
                p.Age += dt;
                p.Velocity = (p.Velocity + gravity * dt) * damping;
                p.Position += p.Velocity * dt;
                p.Rotation += p.AngularVelocity * dt;
            }
            std::erase_if(ps.Particles, [](const Particle& p) { return p.Age >= p.Lifetime; });
        }
    }

    namespace ParticleSystems
    {
        void Update(Scene& scene, float dt)
        {
            for (auto [e, ps, t] : scene.GetRegistry().ViewOf<ParticleSystemComponent, TransformComponent>())
            {
                if (ps.RandomState == 0x9E3779B9u) ps.RandomState ^= (e.Index + 1) * 2654435761u; // distinct streams
                if (!ps.Started && !ps.Playing && ps.PlayOnAwake && ps.Time == 0.0f && ps.Particles.empty() && ps.PendingEmit == 0)
                    ps.Playing = true;
                Simulate(ps, t.World, t.ActiveInHierarchy, dt);
            }
        }

        void Restart(Scene& scene)
        {
            for (auto [e, ps] : scene.GetRegistry().ViewOf<ParticleSystemComponent>())
            {
                ps.Particles.clear();
                ps.Time = 0.0f;
                ps.EmitAccumulator = 0.0f;
                ps.Started = false;
                ps.Playing = ps.PlayOnAwake;
                ps.PendingEmit = 0;
            }
        }
    }
}
