#include "Dimension/Particles/ParticleSystem.h"
#include "Dimension/Scene/Scene.h"
#include "Dimension/Physics/Physics3D.h"
#include "Dimension/Asset/AssetManager.h"
#include "Dimension/Renderer/Mesh.h"
#include <glm/gtc/constants.hpp>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <unordered_map>

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
            case ParticleShape::Mesh:
                // A spawn point on the surface, outward along its normal.
                if (const auto* samples = ParticleSystems::MeshSurfaceSamples(ps.EmitMesh))
                {
                    size_t k = std::min((size_t)(Random01(rng) * ParticleSystems::kMeshSamples), (size_t)ParticleSystems::kMeshSamples - 1);
                    pos = glm::vec3((*samples)[k * 2]);
                    dir = glm::vec3((*samples)[k * 2 + 1]);
                }
                break;
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
            p.SheetOffset = ps.SheetRandomStart ? Random01(rng) : 0.0f;
            ps.Particles.push_back(p);
            if (ps.SubEmitterTrigger == ParticleSubEmitTrigger::Birth && !ps.SubEmitter.IsNull())
                subEmits.push_back({ ps.SubEmitter, worldPos, ps.SubEmitCount });
        }

        // Moves a particle and resolves collisions. Works in world space; local-space particles are converted.
        void Integrate(Scene& scene, ParticleSystemComponent& ps, Particle& p, const glm::mat4& world, const glm::mat4& invWorld,
                       const glm::vec3& gravity, float damping, float dt, std::vector<SubEmitRequest>& subEmits)
        {
            bool local = ps.SimulationSpace == ParticleSpace::Local;
            // A module's vector given in `space`, expressed in the simulation space.
            auto toSim = [&](const glm::vec3& v, ParticleSpace space) {
                if (space == ps.SimulationSpace) return v;
                return space == ParticleSpace::Local ? glm::vec3(world * glm::vec4(v, 0.0f)) : glm::vec3(invWorld * glm::vec4(v, 0.0f));
            };
            p.Age += dt;
            float life = std::clamp(p.Age / p.Lifetime, 0.0f, 1.0f);
            glm::vec3 accel = gravity;
            if (ps.ForceOverLifetime) accel += toSim(ps.Force * ps.ForceCurve.Evaluate(life), ps.ForceSpace);
            p.Velocity = (p.Velocity + accel * dt) * damping;
            float speedScale = ps.SpeedOverLifetime.Evaluate(life);
            glm::vec3 previous = p.Position;
            p.Position += p.Velocity * speedScale * dt;
            if (ps.VelocityOverLifetime)
            {
                p.Position += toSim(ps.LifetimeVelocity * ps.VelocityCurve.Evaluate(life), ps.VelocitySpace) * dt;
                if (ps.OrbitalSpeed != 0.0f)
                {
                    // Orbit the emitter's origin around its local Y axis.
                    glm::vec3 lp = local ? p.Position : glm::vec3(invWorld * glm::vec4(p.Position, 1.0f));
                    float a = glm::radians(ps.OrbitalSpeed) * dt, c = std::cos(a), s = std::sin(a);
                    lp = glm::vec3(c * lp.x + s * lp.z, lp.y, -s * lp.x + c * lp.z);
                    p.Position = local ? lp : glm::vec3(world * glm::vec4(lp, 1.0f));
                }
            }
            p.Rotation += p.AngularVelocity * dt;
            if (ps.Noise && ps.NoiseStrength != 0.0f)
            {
                // Sampled in world space so local and world simulation swirl the same way; moves the particle
                // without accumulating into its velocity (like Unity's Noise module).
                glm::vec3 wp = local ? glm::vec3(world * glm::vec4(p.Position, 1.0f)) : p.Position;
                glm::vec3 drift = ParticleSystems::CurlNoise(wp * ps.NoiseFrequency + glm::vec3(0.0f, 0.0f, ps.NoiseTime * ps.NoiseScrollSpeed)) *
                                  ps.NoiseStrength;
                p.Position += (local ? glm::vec3(invWorld * glm::vec4(drift, 0.0f)) : drift) * dt;
            }
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

        // GPU systems only count what to create; the renderer's compute pass makes and moves the particles.
        void QueueGpuEmission(ParticleSystemComponent& ps, const glm::vec3* origin, int count)
        {
            if (count <= 0) return;
            auto& list = ps.GpuEmissions;
            bool atOrigin = origin != nullptr;
            glm::vec3 at = origin ? *origin : glm::vec3(0.0f);
            if (!list.empty() && list.back().AtOrigin == atOrigin && (!atOrigin || list.back().Origin == at))
                list.back().Count += count;
            else if ((int)list.size() < ParticleSystemComponent::kMaxGpuEmissions)
                list.push_back({ at, count, atOrigin });
            else
                list.back().Count += count; // out of segments: join the last one
            ps.GpuSinceEmit = 0.0f;
        }

        void Simulate(Scene& scene, ParticleSystemComponent& ps, const glm::mat4& world, bool active, float dt,
                      std::vector<SubEmitRequest>& subEmits)
        {
            auto emit = [&](const glm::vec3* origin) {
                if (ps.SimulateOnGpu) QueueGpuEmission(ps, origin, 1);
                else EmitOne(ps, world, origin, subEmits);
            };
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

            ps.NoiseTime += dt;
            if (ps.SimulateOnGpu)
            {
                ps.Particles.clear();
                ps.GpuPendingDt += dt;
                ps.GpuSinceEmit += dt;
                return;
            }
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

    int32_t ParticleSystemComponent::SheetFrame(const Particle& p) const
    {
        if (!TextureSheet) return 0;
        int32_t cells = std::max(1, SheetColumns) * std::max(1, SheetRows);
        int32_t frames = SheetFrames > 0 ? std::min(SheetFrames, cells) : cells;
        float life = std::clamp(p.Age / std::max(p.Lifetime, 1e-4f), 0.0f, 0.9999f);
        float t = life * std::max(SheetCycles, 0.0f) + p.SheetOffset;
        t -= std::floor(t);
        return std::min((int32_t)(t * frames), frames - 1);
    }

    void ParticleSystemComponent::SheetUV(int32_t frame, glm::vec2& uvMin, glm::vec2& uvMax) const
    {
        if (!TextureSheet)
        {
            uvMin = { 0.0f, 1.0f };
            uvMax = { 1.0f, 0.0f };
            return;
        }
        int32_t cols = std::max(1, SheetColumns), rows = std::max(1, SheetRows);
        float col = (float)(frame % cols), row = (float)(frame / cols);
        uvMin = { col / cols, (row + 1.0f) / rows };
        uvMax = { (col + 1.0f) / cols, row / rows };
    }

    namespace ParticleSystems
    {
        namespace
        {
            uint32_t HashCell(int32_t x, int32_t y, int32_t z)
            {
                uint32_t h = (uint32_t)x * 0x8da6b343u ^ (uint32_t)y * 0xd8163841u ^ (uint32_t)z * 0xcb1ab31fu;
                h ^= h >> 15;
                h *= 0x2c1b3c6du;
                h ^= h >> 12;
                h *= 0x297a2d39u;
                return h ^ (h >> 15);
            }

            float GradDot(uint32_t hash, const glm::vec3& d)
            {
                // One of the 12 cube-edge gradients (Perlin's improved noise).
                switch (hash % 12u)
                {
                case 0: return d.x + d.y;
                case 1: return -d.x + d.y;
                case 2: return d.x - d.y;
                case 3: return -d.x - d.y;
                case 4: return d.x + d.z;
                case 5: return -d.x + d.z;
                case 6: return d.x - d.z;
                case 7: return -d.x - d.z;
                case 8: return d.y + d.z;
                case 9: return -d.y + d.z;
                case 10: return d.y - d.z;
                default: return -d.y - d.z;
                }
            }
        }

        float GradientNoise(const glm::vec3& p)
        {
            glm::vec3 cell = glm::floor(p), f = p - cell;
            glm::vec3 u = f * f * f * (f * (f * 6.0f - 15.0f) + 10.0f); // quintic fade
            int32_t x = (int32_t)cell.x, y = (int32_t)cell.y, z = (int32_t)cell.z;
            auto corner = [&](int32_t i, int32_t j, int32_t k) { return GradDot(HashCell(x + i, y + j, z + k), f - glm::vec3(i, j, k)); };
            float x00 = glm::mix(corner(0, 0, 0), corner(1, 0, 0), u.x), x10 = glm::mix(corner(0, 1, 0), corner(1, 1, 0), u.x);
            float x01 = glm::mix(corner(0, 0, 1), corner(1, 0, 1), u.x), x11 = glm::mix(corner(0, 1, 1), corner(1, 1, 1), u.x);
            return glm::mix(glm::mix(x00, x10, u.y), glm::mix(x01, x11, u.y), u.z);
        }

        glm::vec3 CurlNoise(const glm::vec3& p)
        {
            // Curl of a noise vector potential (three decorrelated noise fields), by central differences.
            const float e = 0.05f;
            const glm::vec3 offB(31.416f, -47.853f, 12.793f), offC(-73.219f, 19.371f, 58.111f);
            auto psi = [&](const glm::vec3& q) { return glm::vec3(GradientNoise(q), GradientNoise(q + offB), GradientNoise(q + offC)); };
            glm::vec3 dx = (psi(p + glm::vec3(e, 0, 0)) - psi(p - glm::vec3(e, 0, 0))) / (2.0f * e);
            glm::vec3 dy = (psi(p + glm::vec3(0, e, 0)) - psi(p - glm::vec3(0, e, 0))) / (2.0f * e);
            glm::vec3 dz = (psi(p + glm::vec3(0, 0, e)) - psi(p - glm::vec3(0, 0, e))) / (2.0f * e);
            return glm::vec3(dy.z - dz.y, dz.x - dx.z, dx.y - dy.x) * 0.5f;
        }

        void Update(Scene& scene, float dt)
        {
            auto& reg = scene.GetRegistry();
            std::vector<SubEmitRequest> subEmits;
            for (auto [e, ps, t] : reg.ViewOf<ParticleSystemComponent, TransformComponent>())
            {
                if (ps.RandomState == 0x9E3779B9u) ps.RandomState ^= (e.Index + 1) * 2654435761u; // distinct streams
                if (ps.Shape == ParticleShape::Mesh)
                {
                    const auto* mr = reg.TryGet<MeshRendererComponent>(e);
                    ps.EmitMesh = ps.ShapeMesh.IsValid() ? ps.ShapeMesh : mr ? mr->Mesh : AssetHandle::None();
                }
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

        const std::vector<glm::vec4>* MeshSurfaceSamples(AssetHandle mesh)
        {
            static std::mutex mutex;
            static std::unordered_map<uint64_t, std::pair<std::weak_ptr<const MeshData>, std::vector<glm::vec4>>> cache;
            auto data = AssetManager::GetMeshData(mesh);
            if (!data || data->Indices.size() < 3) return nullptr;
            std::lock_guard lock(mutex);
            auto& entry = cache[(uint64_t)mesh];
            if (entry.first.lock() == data && !entry.second.empty()) return &entry.second;
            // Area-weighted: a running total of triangle areas, searched with uniform random numbers.
            std::vector<float> cumulative;
            float total = 0.0f;
            for (size_t i = 0; i + 2 < data->Indices.size(); i += 3)
            {
                const glm::vec3 &a = data->Vertices[data->Indices[i]].Position, &b = data->Vertices[data->Indices[i + 1]].Position,
                                &c = data->Vertices[data->Indices[i + 2]].Position;
                total += 0.5f * glm::length(glm::cross(b - a, c - a));
                cumulative.push_back(total);
            }
            if (total <= 0.0f) return nullptr;
            uint32_t rng = 0x2545F491u;
            std::vector<glm::vec4> samples;
            samples.reserve(kMeshSamples * 2);
            for (int s = 0; s < kMeshSamples; ++s)
            {
                size_t t = std::min((size_t)(std::lower_bound(cumulative.begin(), cumulative.end(), Random01(rng) * total) - cumulative.begin()), cumulative.size() - 1);
                const Vertex &a = data->Vertices[data->Indices[t * 3]], &b = data->Vertices[data->Indices[t * 3 + 1]], &c = data->Vertices[data->Indices[t * 3 + 2]];
                float u = Random01(rng), v = Random01(rng);
                if (u + v > 1.0f) { u = 1.0f - u; v = 1.0f - v; }
                glm::vec3 p = a.Position + (b.Position - a.Position) * u + (c.Position - a.Position) * v;
                glm::vec3 n = a.Normal * (1.0f - u - v) + b.Normal * u + c.Normal * v;
                if (glm::dot(n, n) < 1e-12f) n = glm::cross(b.Position - a.Position, c.Position - a.Position);
                samples.push_back(glm::vec4(p, 0.0f));
                samples.push_back(glm::vec4(glm::normalize(n), 0.0f));
            }
            entry = { data, std::move(samples) };
            return &entry.second;
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
                ps.GpuEmissions.clear();
                ps.GpuPendingDt = 0.0f;
                ps.GpuSinceEmit = 1e9f;
                ++ps.GpuGeneration;
            }
        }
    }
}
