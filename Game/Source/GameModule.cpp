#include <Dimension/Dimension.h>
#include <Dimension/Scripting/GameModule.h>

using namespace Dimension;

namespace Game
{
    // Spins the entity around its local axes.
    class Rotator : public ScriptableEntity
    {
    public:
        glm::vec3 DegreesPerSecond{ 0.0f, 45.0f, 0.0f };

        void OnUpdate(float dt) override { Transform().Rotation += glm::radians(DegreesPerSecond) * dt; }
    };

    // Moves up and down on a sine wave.
    class Bobber : public ScriptableEntity
    {
    public:
        float Amplitude = 0.25f;
        float Frequency = 1.0f;

        void OnCreate() override { m_BaseY = Transform().Position.y; }
        void OnUpdate(float dt) override
        {
            m_Time += dt;
            Transform().Position.y = m_BaseY + std::sin(m_Time * Frequency * glm::two_pi<float>()) * Amplitude;
        }

    private:
        float m_BaseY = 0.0f, m_Time = 0.0f;
    };

    // 2D platformer controller: A/D or arrows to move, Space to jump. Needs a Rigidbody 2D.
    class PlayerController2D : public ScriptableEntity
    {
    public:
        float MoveSpeed = 6.0f;
        float JumpImpulse = 6.0f;

        void OnUpdate(float) override
        {
            Scene& scene = GetScene();
            glm::vec2 v = Physics2DWorld::GetLinearVelocity(scene, Self());
            float input = 0.0f;
            if (Input::IsKeyDown(Key::A) || Input::IsKeyDown(Key::Left)) input -= 1.0f;
            if (Input::IsKeyDown(Key::D) || Input::IsKeyDown(Key::Right)) input += 1.0f;
            v.x = input * MoveSpeed;
            Physics2DWorld::SetLinearVelocity(scene, Self(), v);

            if (Input::IsKeyPressed(Key::Space) && m_Grounded > 0)
                Physics2DWorld::ApplyImpulse(scene, Self(), { 0.0f, JumpImpulse });
            if (auto* sprite = TryGet<SpriteRendererComponent>(); sprite && input != 0.0f) sprite->FlipX = input < 0.0f;
        }

        void OnCollisionEnter(Entity) override { ++m_Grounded; }
        void OnCollisionExit(Entity) override { m_Grounded = std::max(0, m_Grounded - 1); }

    private:
        int m_Grounded = 0;
    };

    // 3D character: WASD moves a Rigidbody, Space jumps.
    class PlayerController3D : public ScriptableEntity
    {
    public:
        float MoveSpeed = 5.0f;
        float JumpSpeed = 5.0f;

        void OnUpdate(float) override
        {
            Scene& scene = GetScene();
            glm::vec3 v = Physics3DWorld::GetLinearVelocity(scene, Self());
            glm::vec3 dir(0.0f);
            if (Input::IsKeyDown(Key::W)) dir.z -= 1.0f;
            if (Input::IsKeyDown(Key::S)) dir.z += 1.0f;
            if (Input::IsKeyDown(Key::A)) dir.x -= 1.0f;
            if (Input::IsKeyDown(Key::D)) dir.x += 1.0f;
            if (glm::length(dir) > 0.0f) dir = glm::normalize(dir);
            v.x = dir.x * MoveSpeed;
            v.z = dir.z * MoveSpeed;
            if (Input::IsKeyPressed(Key::Space) && std::abs(v.y) < 0.05f) v.y = JumpSpeed;
            Physics3DWorld::SetLinearVelocity(scene, Self(), v);
        }
    };

    // Smoothly follows a target entity (assign it in the inspector).
    class CameraFollow : public ScriptableEntity
    {
    public:
        Entity Target;
        glm::vec3 Offset{ 0.0f, 0.0f, 10.0f };
        float Smoothing = 5.0f;

        void OnUpdate(float dt) override
        {
            if (!GetScene().IsValid(Target)) return;
            glm::vec3 goal = GetScene().GetRegistry().Get<TransformComponent>(Target).GetWorldPosition() + Offset;
            auto& t = Transform();
            t.Position = glm::mix(t.Position, goal, 1.0f - std::exp(-Smoothing * dt));
        }
    };

    // Periodically spawns a prefab at this entity's position.
    class Spawner : public ScriptableEntity
    {
    public:
        AssetHandle Prefab = AssetHandle::None();
        float Interval = 1.0f;
        int MaxSpawns = 20;

        void OnUpdate(float dt) override
        {
            m_Timer += dt;
            if (m_Timer < Interval || m_Count >= MaxSpawns || !Prefab.IsValid()) return;
            m_Timer = 0.0f;
            ++m_Count;
            glm::vec3 jitter((float)(m_Count % 5) * 0.1f - 0.2f, 0.0f, 0.0f);
            Instantiate(Prefab, Transform().GetWorldPosition() + jitter);
        }

    private:
        float m_Timer = 0.0f;
        int m_Count = 0;
    };

    // Destroys its entity after a delay.
    class Lifetime : public ScriptableEntity
    {
    public:
        float Seconds = 5.0f;
        void OnUpdate(float dt) override
        {
            Seconds -= dt;
            if (Seconds <= 0.0f) Destroy(Self());
        }
    };

    // Instantiates a prefab when the UI Button on this entity is clicked.
    class SpawnOnClick : public ScriptableEntity
    {
    public:
        AssetHandle Prefab = AssetHandle::None();
        glm::vec3 SpawnPosition{ 0.0f, 7.0f, -1.0f };

        void OnClick() override
        {
            if (Prefab.IsValid()) Instantiate(Prefab, SpawnPosition);
        }
    };

    // Writes the number of entities named Name into this entity's UI Text.
    class EntityCounterText : public ScriptableEntity
    {
    public:
        std::string Name = "Ball";
        std::string Label = "Balls";

        void OnUpdate(float) override
        {
            auto* text = TryGet<UITextComponent>();
            if (!text) return;
            int count = 0;
            for (auto [e, tag] : GetScene().GetRegistry().ViewOf<TagComponent>())
                if (tag.Name == Name) ++count;
            text->Text = Label + ": " + std::to_string(count);
        }
    };

    // Alternates the Animator between two clips with a crossfade (or on Space when Interval is 0).
    class AnimationCycler : public ScriptableEntity
    {
    public:
        std::string ClipA = "Wave";
        std::string ClipB = "Curl";
        float Interval = 3.0f;
        float FadeSeconds = 0.4f;

        void OnUpdate(float dt) override
        {
            auto* anim = TryGet<AnimatorComponent>();
            if (!anim) return;
            m_Timer += dt;
            bool flip = Interval > 0.0f ? m_Timer >= Interval : Input::IsKeyPressed(Key::Space);
            if (!flip) return;
            m_Timer = 0.0f;
            anim->CrossFade(anim->Clip == ClipA ? ClipB : ClipA, FadeSeconds);
        }

    private:
        float m_Timer = 0.0f;
    };

    // Example ECS system (no script needed): pulses the emission of every mesh
    // whose entity is tagged "Pulse".
    class PulseSystem : public System
    {
    public:
        void OnUpdate(Scene& scene, float dt) override
        {
            m_Time += dt;
            for (auto [e, tag, mesh] : scene.GetRegistry().ViewOf<TagComponent, MeshRendererComponent>())
                if (tag.Tag == "Pulse") mesh.Emission = 0.5f + 0.5f * std::sin(m_Time * 3.0f);
        }

    private:
        float m_Time = 0.0f;
    };

    void RegisterGameModule()
    {
        ScriptRegistry::Register<Rotator>("Rotator").Field("Degrees Per Second", &Rotator::DegreesPerSecond);
        ScriptRegistry::Register<Bobber>("Bobber")
            .Field("Amplitude", &Bobber::Amplitude).Speed(0.01f)
            .Field("Frequency", &Bobber::Frequency).Speed(0.01f);
        ScriptRegistry::Register<PlayerController2D>("PlayerController2D")
            .Field("Move Speed", &PlayerController2D::MoveSpeed)
            .Field("Jump Impulse", &PlayerController2D::JumpImpulse);
        ScriptRegistry::Register<PlayerController3D>("PlayerController3D")
            .Field("Move Speed", &PlayerController3D::MoveSpeed)
            .Field("Jump Speed", &PlayerController3D::JumpSpeed);
        ScriptRegistry::Register<CameraFollow>("CameraFollow")
            .Field("Target", &CameraFollow::Target)
            .Field("Offset", &CameraFollow::Offset)
            .Field("Smoothing", &CameraFollow::Smoothing);
        ScriptRegistry::Register<Spawner>("Spawner")
            .Field("Prefab", &Spawner::Prefab).As(FieldType::Prefab)
            .Field("Interval", &Spawner::Interval).Range(0.01f, 100.0f).Speed(0.01f)
            .Field("Max Spawns", &Spawner::MaxSpawns);
        ScriptRegistry::Register<Lifetime>("Lifetime").Field("Seconds", &Lifetime::Seconds);

        ScriptRegistry::Register<SpawnOnClick>("SpawnOnClick")
            .Field("Prefab", &SpawnOnClick::Prefab).As(FieldType::Prefab)
            .Field("Spawn Position", &SpawnOnClick::SpawnPosition);
        ScriptRegistry::Register<EntityCounterText>("EntityCounterText")
            .Field("Name", &EntityCounterText::Name)
            .Field("Label", &EntityCounterText::Label);

        ScriptRegistry::Register<AnimationCycler>("AnimationCycler")
            .Field("Clip A", &AnimationCycler::ClipA)
            .Field("Clip B", &AnimationCycler::ClipB)
            .Field("Interval", &AnimationCycler::Interval).Range(0.0f, 100.0f).Speed(0.05f)
            .Field("Fade Seconds", &AnimationCycler::FadeSeconds).Range(0.0f, 10.0f).Speed(0.01f);

        SystemRegistry::Register<PulseSystem>("Pulse");
        Log::Info("Sandbox game module registered");
    }
}

// Called by the engine after loading (and after every hot reload of) this DLL.
DIM_GAME_MODULE_ENTRY
{
    Game::RegisterGameModule();
}
