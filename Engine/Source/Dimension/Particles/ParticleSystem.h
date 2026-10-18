#pragma once
#include "Dimension/Core/Guid.h"
#include "Dimension/Core/Curve.h"
#include "Dimension/ECS/Entity.h"
#include <glm/glm.hpp>
#include <array>
#include <cstdint>
#include <vector>

namespace Dimension
{
    class Scene;

    enum class ParticleShape : int32_t { Cone = 0, Sphere = 1, Box = 2, Point = 3 };
    enum class ParticleSpace : int32_t { Local = 0, World = 1 };
    enum class ParticleBlend : int32_t { Alpha = 0, Additive = 1 };
    enum class ParticleCollision : int32_t { None = 0, Plane = 1, World = 2 };
    enum class ParticleSubEmitTrigger : int32_t { Birth = 0, Collision = 1, Death = 2 };

    struct Particle
    {
        static constexpr int kTrailPoints = 12;

        glm::vec3 Position;
        glm::vec3 Velocity;
        glm::vec4 Color;
        float Age;
        float Lifetime;
        float Size;
        float Rotation;        // radians
        float AngularVelocity; // radians per second
        // Trail history, newest first (same space as Position).
        std::array<glm::vec3, kTrailPoints> Trail;
        uint8_t TrailCount = 0;
        float TrailTimer = 0.0f;
    };

    // CPU particle emitter (a compact take on Unity's ParticleSystem modules).
    // Ranges are (min, max) and are sampled uniformly per particle.
    struct ParticleSystemComponent
    {
        // Main
        float Duration = 5.0f;
        bool Looping = true;
        bool PlayOnAwake = true;
        glm::vec2 StartLifetime{ 1.5f, 2.5f };
        glm::vec2 StartSpeed{ 1.5f, 3.0f };
        glm::vec2 StartSize{ 0.15f, 0.35f };
        glm::vec2 StartRotation{ 0.0f, 360.0f };   // degrees
        glm::vec2 AngularVelocity{ -45.0f, 45.0f }; // degrees per second
        glm::vec4 StartColorA{ 1.0f };
        glm::vec4 StartColorB{ 1.0f };
        float GravityModifier = 0.0f;               // multiples of 9.81 m/s^2 downward
        float Drag = 0.0f;                          // velocity damping per second
        int32_t MaxParticles = 1000;
        ParticleSpace SimulationSpace = ParticleSpace::World;

        // Emission
        float RateOverTime = 20.0f;
        int32_t BurstCount = 0;                     // emitted when the system starts (and each loop)

        // Shape
        ParticleShape Shape = ParticleShape::Cone;
        float ShapeAngle = 25.0f;                   // cone half angle, degrees
        float ShapeRadius = 0.25f;
        glm::vec3 BoxSize{ 1.0f };

        // Over lifetime (normalized age 0-1)
        Gradient ColorOverLifetime = Gradient::Linear(glm::vec4(1.0f), glm::vec4(1.0f, 1.0f, 1.0f, 0.0f)); // multiplies the start color
        Curve SizeOverLifetime = Curve::Constant(1.0f);  // multiplies the start size
        Curve SpeedOverLifetime = Curve::Constant(1.0f); // multiplies the velocity when moving

        // Collision
        ParticleCollision Collision = ParticleCollision::None;
        float CollisionPlaneHeight = 0.0f; // world Y of the plane (Plane mode)
        float Bounce = 0.5f;               // fraction of the normal velocity kept
        float CollisionDampen = 0.1f;      // fraction of the tangential velocity lost
        float LifetimeLoss = 0.0f;         // fraction of the lifetime lost per collision
        float CollisionRadius = 0.05f;

        // Sub emitter: another Particle System that emits a burst at this system's particles.
        Entity SubEmitter = Entity::Null();
        ParticleSubEmitTrigger SubEmitterTrigger = ParticleSubEmitTrigger::Death;
        int32_t SubEmitCount = 20;

        // Trails
        bool Trails = false;
        float TrailLifetime = 0.25f; // seconds of history
        float TrailWidth = 0.5f;     // multiplier of the particle size

        // Renderer
        AssetHandle Texture = AssetHandle::None();    // None = built-in soft particle
        ParticleBlend Blend = ParticleBlend::Alpha;
        int32_t SortingOrder = 0;

        // Runtime state (not serialized)
        struct Burst { glm::vec3 WorldPosition; int32_t Count; };
        std::vector<Particle> Particles;
        std::vector<Burst> PendingBursts; // sub-emitter requests
        float Time = 0.0f;
        float EmitAccumulator = 0.0f;
        uint32_t RandomState = 0x9E3779B9u;
        bool Playing = false;
        bool Started = false;
        int32_t PendingEmit = 0;

        void Play() { Playing = true; Started = false; Time = 0.0f; }
        void Stop() { Playing = false; }  // existing particles finish their lives
        void Clear() { Particles.clear(); }
        void Emit(int count) { PendingEmit += count; }
        void EmitAt(const glm::vec3& worldPosition, int count) { PendingBursts.push_back({ worldPosition, count }); }
        bool IsAlive() const { return Playing || !Particles.empty() || !PendingBursts.empty(); }
    };

    namespace ParticleSystems
    {
        // Simulates every particle system in the scene (also used for edit-mode preview).
        void Update(Scene& scene, float dt);
        // Resets all systems and starts those with PlayOnAwake (called when play mode begins).
        void Restart(Scene& scene);
    }
}
