#pragma once
#include "Dimension/Core/Guid.h"
#include "Dimension/Core/Curve.h"
#include "Dimension/ECS/Entity.h"
#include "Dimension/Particles/ParticleSystem.h"
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace Dimension
{
    class Scene;

    // A Visual Effect Graph (like Unity's VFX Graph): GPU particles made by a graph instead of fixed modules. The graph
    // has four contexts: Spawn (how many particles start: a rate and bursts), Initialize (blocks setting each new
    // particle's attributes), Update (blocks changing them every frame: forces, drag, noise, colors over life) and
    // Output (how they draw). Blocks take their values from operator nodes (math, random, noise, gradients, the
    // particle's attributes, exposed properties) linked into their inputs, or from the values set on the inputs.
    //
    // A graph compiles to a short program per context that one compute shader (VfxSim.comp) runs for every particle,
    // so effects need no shader compiler at runtime; the particles are drawn like GPU Particle Systems' (sorted, lit).

    // Values flowing through the graph are vec4s; a port's kind only says how its value is shown and edited.
    enum class VfxValue : int32_t { Float = 0, Vector = 1, Color = 2 };
    // Particle attributes (Get Attribute, Set / Add Attribute blocks).
    enum class VfxAttribute : int32_t { Position = 0, Velocity, Color, Size, Age, Lifetime, Rotation, AngularVelocity, Count };

    struct VfxNode
    {
        uint32_t Id = 0;
        std::string Type;         // a VisualEffects::NodeType's Name
        glm::vec2 Position{ 0.0f }; // in the graph (operators; blocks sit in their context)
        std::vector<glm::vec4> Inputs; // each input's value while nothing is linked to it
        int32_t Option = 0;       // the type's choice (an attribute, a component, a property's index)
        Gradient Colors = Gradient::Linear({ 1, 1, 1, 1 }, { 1, 1, 1, 0 }); // Sample Gradient, Color Over Life
        Curve Values = Curve::Linear(1.0f, 0.0f);                           // Sample Curve, Size Over Life
        bool Enabled = true;      // blocks: off = skipped
        AssetHandle Child = AssetHandle::None(); // Trigger On Die: the effect its events start
    };

    struct VfxLink
    {
        uint32_t From = 0;          // operator node (its output)
        uint32_t To = 0;            // node whose input it feeds
        int32_t Input = 0;
        bool operator==(const VfxLink&) const = default;
    };

    struct VfxProperty
    {
        std::string Name;
        VfxValue Type = VfxValue::Float;
        glm::vec4 Value{ 0.0f }; // default (a component may override it)
    };

    struct VfxBurst
    {
        float Time = 0.0f; // seconds into each loop
        int32_t Count = 10;
    };

    struct VisualEffectAsset
    {
        // Spawn
        int32_t Capacity = 10000;   // particles alive at most
        float Rate = 100.0f;        // per second
        std::vector<VfxBurst> Bursts;
        float Duration = 5.0f;      // a loop's length (bursts repeat every loop)
        bool Loop = true;           // else the spawn stops after one Duration
        // Output
        ParticleSpace Space = ParticleSpace::World;
        ParticleBlend Blend = ParticleBlend::Additive;
        AssetHandle Texture = AssetHandle::None(); // None = the soft round particle
        bool Sort = true;           // Alpha: back to front
        bool Lit = false;
        // Trails (strips): each particle also draws a ribbon back through where it was, narrowing and fading (up to
        // 262,144 particles).
        bool Trails = false;
        float TrailLifetime = 0.4f; // seconds of path the ribbon covers
        float TrailWidth = 1.0f;    // times the particle's size
        // Graph
        std::vector<VfxNode> Nodes;
        std::vector<VfxLink> Links;
        std::vector<uint32_t> Initialize, Update; // their blocks, in order
        std::vector<VfxProperty> Properties;
        uint32_t NextId = 1;

        VfxNode* Find(uint32_t id);
        const VfxNode* Find(uint32_t id) const;
        // A new node of a type (its inputs at their defaults); blocks are appended to their context (blocks that go in
        // either: Initialize, or Update with `toUpdate`).
        VfxNode& Add(const std::string& type, glm::vec2 position = glm::vec2(0.0f), bool toUpdate = false);
        void Remove(uint32_t id); // and its links
        // Links `from`'s output into `to`'s input (replacing what fed it). False if it would make a cycle.
        bool Link(uint32_t from, uint32_t to, int32_t input);
        const VfxLink* LinkInto(uint32_t to, int32_t input) const;

        nlohmann::json ToJson() const;
        static VisualEffectAsset FromJson(const nlohmann::json& j);
        static std::shared_ptr<VisualEffectAsset> Load(const std::filesystem::path& file);
        bool Save(const std::filesystem::path& file) const;
        // A new asset's graph: sparks from a sphere, flying up and out, falling and fading.
        static VisualEffectAsset Default();
    };

    // A compiled graph: one instruction list per context (uvec4s VfxSim.comp runs), its constants (properties first,
    // in the asset's order), and the gradients' and curves' tables.
    struct VfxProgram
    {
        static constexpr int kRegisters = 32;
        static constexpr int kLutSize = 32;
        static constexpr int kMaxLuts = 16;
        static constexpr int kMaxChildren = 4; // effects its events start (Trigger On Die), each an event slot
        static constexpr int kMaxEvents = 8192; // events per slot per frame
        std::vector<glm::uvec4> Code;
        std::vector<glm::vec4> Constants;
        std::vector<glm::vec4> Luts; // kLutSize per table
        uint32_t InitStart = 0, InitCount = 0, UpdateStart = 0, UpdateCount = 0;
        std::vector<AssetHandle> Children; // event slot -> the effect it starts
        std::vector<std::string> Errors;
        bool Valid() const { return Errors.empty(); }
    };

    // The VM's instructions (VfxSim.comp's switch: the same numbers). x = op, y = destination register, z = operand
    // registers a | b << 8 | c << 16, w = an immediate (constant index, attribute, built-in, table, component).
    enum class VfxOp : uint32_t
    {
        Const = 1, GetAttr, SetAttr, AddAttr, Input,
        Add, Sub, Mul, Div, Min, Max, Lerp, Sin, Cos, Abs, Fract, Floor, Length, Normalize, Dot, Cross,
        Random, RandomInSphere, RandomOnSphere, RandomInCircle, Noise, Gradient, Curve, Split, Combine, Step,
        Saturate, KillIf, Pow, ToSimPoint, ToSimVector, RandomCone, Bounce, Sqrt, TriggerDie
    };
    // Built-in inputs (VfxOp::Input).
    enum class VfxInput : uint32_t { AgeOverLifetime = 0, Time, DeltaTime, ParticleRandom, ParticleIndex, EmitterPosition, SourcePosition, SourceVelocity, SourceColor };

    namespace VisualEffects
    {
        // A node type: its inputs (names, kinds and defaults), whether it has an output, and where it may go.
        enum class Category : int32_t { Operator = 0, Initialize = 1, Update = 2, Both = 3 };
        struct Port
        {
            const char* Name;
            VfxValue Kind;
            glm::vec4 Default;
        };
        struct NodeType
        {
            const char* Name;
            const char* Menu;           // where the Add menu lists it ("Math/Add")
            Category Where;
            std::vector<Port> Inputs;
            bool HasOutput;
            VfxValue Output;
            std::vector<const char*> Options; // the Option dropdown's names (empty: none)
            const char* Help;
        };
        const std::vector<NodeType>& NodeTypes();
        const NodeType* FindType(const std::string& name);
        const char* AttributeName(VfxAttribute a);

        VfxProgram Compile(const VisualEffectAsset& asset);
        // A property's value as the program reads it (a Float in every component).
        glm::vec4 PropertyConstant(const VfxProperty& property, const glm::vec4& value);

        // The VM on the CPU (as VfxSim.comp runs it): tests and previews check graphs with it. A particle's attributes,
        // and what a program reads besides them.
        struct CpuParticle
        {
            glm::vec3 Position{ 0.0f }, Velocity{ 0.0f };
            glm::vec4 Color{ 1.0f };
            float Size = 0.1f, Age = 0.0f, Lifetime = 1.0f, Rotation = 0.0f, AngularVelocity = 0.0f;
            float Random = 0.0f; // per particle, constant
            uint32_t Index = 0;
            bool Alive() const { return Age < Lifetime; }
        };
        // An event a particle sent (Trigger On Die): which slot, and its attributes then (simulation space).
        struct CpuEvent
        {
            int Slot = 0;
            glm::vec3 Position{ 0.0f }, Velocity{ 0.0f };
            glm::vec4 Color{ 1.0f };
        };
        struct CpuContext
        {
            float Time = 0.0f, DeltaTime = 0.0f;
            glm::mat4 EmitterToSim{ 1.0f };
            uint32_t Seed = 1;
            const CpuEvent* Source = nullptr;  // Spawn: the event starting this particle (it starts where the event was)
            std::vector<CpuEvent>* Events = nullptr; // Step: where events go
        };
        // A new particle (its defaults, then Initialize), and a frame of an alive one (Update, then it moves on).
        void Spawn(const VfxProgram& program, CpuParticle& p, const CpuContext& ctx);
        void Step(const VfxProgram& program, CpuParticle& p, const CpuContext& ctx);

        // Spawning, every frame (play mode and edit-mode preview): how many particles each effect starts.
        void Update(Scene& scene, float dt);
        // Resets all effects and starts those with Play On Awake (play mode begins).
        void Restart(Scene& scene);
        // The compiled program of an asset (cached until the asset changes; null when it has none).
        std::shared_ptr<const VfxProgram> ProgramOf(AssetHandle asset);
        void Invalidate(AssetHandle asset);
    }

    // An exposed property's value set on one object.
    struct VfxOverride
    {
        std::string Name;
        glm::vec4 Value{ 0.0f };
    };

    // Plays a Visual Effect asset (like Unity's Visual Effect component).
    struct VisualEffectComponent
    {
        AssetHandle Asset = AssetHandle::None();
        bool PlayOnAwake = true;
        float PlayRate = 1.0f; // time scale
        // Exposed properties set on this object (by name; the rest keep the asset's values).
        std::vector<VfxOverride> Overrides;

        // Runtime
        bool Playing = false;
        float Time = 0.0f;       // seconds since Play
        float SpawnCarry = 0.0f; // fraction of a particle the rate owes
        int32_t PendingSpawn = 0; // particles to start next simulation
        float PendingDt = 0.0f;   // simulation time owed (the renderer catches up)
        float SinceSpawn = 1e9f;  // seconds since particles last started
        uint32_t Generation = 0;  // bumped to clear the particles
        uint32_t Seed = 0;
        uint32_t AliveCount = 0;  // last known (read back from the GPU a few frames late)

        void Play();
        void Stop() { Playing = false; }
        void Reinit();
        bool SetProperty(const std::string& name, const glm::vec4& value);
        bool GetProperty(const std::string& name, glm::vec4& value) const; // override, else the asset's default
    };
}
