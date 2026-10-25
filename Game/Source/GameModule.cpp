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

    // Drives a Vehicle: W/S or Up/Down (gamepad: right/left trigger) for throttle and reverse, A/D or Left/Right
    // (left stick) to steer, Space (gamepad B) for the hand brake. Optionally shows the speed on a UI Text.
    class CarDriver : public ScriptableEntity
    {
    public:
        Entity Speedometer;
        float SteerSpeed = 4.0f; // how quickly the wheel turns toward the input (keys snap otherwise)

        void OnUpdate(float dt) override
        {
            auto* car = TryGet<VehicleComponent>();
            if (!car) return;
            float throttle = 0.0f, steer = 0.0f;
            if (Input::IsKeyDown(Key::W) || Input::IsKeyDown(Key::Up)) throttle += 1.0f;
            if (Input::IsKeyDown(Key::S) || Input::IsKeyDown(Key::Down)) throttle -= 1.0f;
            if (Input::IsKeyDown(Key::A) || Input::IsKeyDown(Key::Left)) steer -= 1.0f;
            if (Input::IsKeyDown(Key::D) || Input::IsKeyDown(Key::Right)) steer += 1.0f;
            float pad = Input::GetGamepadAxis(GamepadAxis::RightTrigger) - Input::GetGamepadAxis(GamepadAxis::LeftTrigger);
            if (pad != 0.0f) throttle = pad;
            if (float stick = Input::GetGamepadAxis(GamepadAxis::LeftX); stick != 0.0f) steer = stick;
            car->Throttle = throttle;
            car->Steering += std::clamp(steer - car->Steering, -SteerSpeed * dt, SteerSpeed * dt);
            car->HandBrake = Input::IsKeyDown(Key::Space) || Input::IsGamepadButtonDown(GamepadButton::B) ? 1.0f : 0.0f;
            if (GetScene().IsValid(Speedometer))
                if (auto* text = GetScene().GetRegistry().TryGet<UITextComponent>(Speedometer))
                {
                    std::string gear = car->Gear < 0 ? "R" : car->Gear == 0 ? "N" : std::to_string(car->Gear);
                    text->Text = std::format("<b>{:.0f}</b> km/h   <color=#9ab>gear</color> <b>{}</b>   <color=#9ab>{:.0f} rpm</color>",
                                             std::abs(car->SpeedKmh), gear, car->EngineRPM);
                }
        }
    };

    // Chase camera: stays Distance behind and Height above the target's heading and looks at it, smoothly.
    class ChaseCamera : public ScriptableEntity
    {
    public:
        Entity Target;
        float Distance = 7.0f, Height = 2.5f, Smoothing = 4.0f;

        void OnUpdate(float dt) override
        {
            if (!GetScene().IsValid(Target)) return;
            auto& target = GetScene().GetRegistry().Get<TransformComponent>(Target);
            glm::vec3 focus = target.GetWorldPosition(), forward = target.Forward();
            forward.y = 0.0f;
            forward = glm::length(forward) > 1e-3f ? glm::normalize(forward) : glm::vec3(0, 0, -1);
            glm::vec3 goal = focus - forward * Distance + glm::vec3(0.0f, Height, 0.0f);
            auto& t = Transform();
            t.Position = glm::mix(t.Position, goal, 1.0f - std::exp(-Smoothing * dt));
            glm::vec3 look = focus + glm::vec3(0.0f, 0.8f, 0.0f) - t.Position;
            float yaw = std::atan2(-look.x, -look.z), pitch = std::atan2(look.y, std::sqrt(look.x * look.x + look.z * look.z));
            t.Rotation = { pitch, yaw, 0.0f };
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

    // UI Slider -> light intensity, with a live label ("Prefix: 2.2").
    class LightSlider : public ScriptableEntity
    {
    public:
        std::string Light = "Sun";
        std::string Label;
        std::string Prefix = "Intensity";

        void OnCreate() override
        {
            if (auto* s = TryGet<UISliderComponent>()) Apply(s->Value);
        }
        void OnUIValueChanged(float value) override { Apply(value); }

    private:
        void Apply(float value)
        {
            auto& reg = GetScene().GetRegistry();
            Entity light = Find(Light);
            if (light.IsNull()) return;
            if (auto* d = reg.TryGet<DirectionalLightComponent>(light)) d->Intensity = value;
            if (auto* p = reg.TryGet<PointLightComponent>(light)) p->Intensity = value;
            Entity label = Find(Label);
            if (auto* t = label.IsNull() ? nullptr : reg.TryGet<UITextComponent>(label))
            {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "%s: %.1f", Prefix.c_str(), value);
                t->Text = buf;
            }
        }
    };

    // UI Toggle -> enables/disables a script on another entity (like ticking a component's checkbox).
    class ScriptToggle : public ScriptableEntity
    {
    public:
        std::string Target;
        std::string Script = "Rotator";

        void OnCreate() override
        {
            if (auto* t = TryGet<UIToggleComponent>()) Apply(t->IsOn);
        }
        void OnUIValueChanged(float value) override { Apply(value != 0.0f); }

    private:
        void Apply(bool on)
        {
            Entity target = Find(Target);
            if (target.IsNull()) return;
            if (auto* sc = GetScene().GetRegistry().TryGet<ScriptComponent>(target))
                if (auto* s = sc->Find(Script)) s->Enabled = on;
        }
    };

    // Drives an Animator float parameter back and forth (for a blend tree) and reacts to animation events
    // named "Pulse" with a short squash-and-stretch.
    class BlendDriver : public ScriptableEntity
    {
    public:
        std::string Parameter = "Energy";
        float Period = 8.0f;

        void OnUpdate(float dt) override
        {
            m_Time += dt;
            if (auto* anim = TryGet<AnimatorComponent>())
                anim->SetFloat(Parameter, 0.5f - 0.5f * std::cos(m_Time * glm::two_pi<float>() / std::max(Period, 0.01f)));
            m_Pulse = std::max(0.0f, m_Pulse - dt * 3.0f);
            float s = 1.0f + 0.12f * m_Pulse;
            Transform().Scale = { s, 1.0f / s, s };
        }

        void OnAnimationEvent(const std::string& name) override
        {
            if (name != "Pulse") return;
            m_Pulse = 1.0f;
            Log::Trace("{}: animation event '{}'", Name(), name);
        }

    private:
        float m_Time = 0.0f, m_Pulse = 0.0f;
    };

    // UI Button that switches scenes (Scene: a name from File > Build Settings, or an Assets-relative path).
    class LoadSceneButton : public ScriptableEntity
    {
    public:
        std::string Scene;
        bool Async = false; // load in the background (the button keeps the current scene running meanwhile)
        void OnClick() override
        {
            if (!Async) SceneManager::LoadScene(Scene);
            else if (!m_Loading || m_Loading->IsDone) m_Loading = SceneManager::LoadSceneAsync(Scene);
        }

    private:
        std::shared_ptr<SceneLoadOperation> m_Loading;
    };

    // Loads another scene after a delay (splash screens, "game over" -> menu). Written as a coroutine.
    class LoadSceneAfterDelay : public ScriptableEntity
    {
    public:
        std::string Scene;
        float Delay = 3.0f;
        void OnCreate() override { StartCoroutine(LoadLater()); }

    private:
        Coroutine LoadLater()
        {
            co_await WaitForSeconds(Delay);
            SceneManager::LoadScene(Scene);
        }
    };

    // Moves the entity back and forth between its start position and start + Offset (moving platforms, elevators).
    // Give it a kinematic Rigidbody so Character Controllers standing on it ride along.
    class PlatformMover : public ScriptableEntity
    {
    public:
        glm::vec3 Offset{ 4.0f, 0.0f, 0.0f };
        float Speed = 1.5f;     // units per second
        float Pause = 0.5f;     // seconds at each end

        void OnCreate() override { m_Start = Get<TransformComponent>().Position; }
        void OnUpdate(float dt) override
        {
            float length = glm::length(Offset);
            if (length < 1e-4f || Speed <= 0.0f) return;
            float travel = length / Speed;
            m_Time = std::fmod(m_Time + dt, 2.0f * (travel + Pause));
            float t = m_Time < travel ? m_Time / travel : m_Time < travel + Pause ? 1.0f
                    : m_Time < 2.0f * travel + Pause ? 1.0f - (m_Time - travel - Pause) / travel : 0.0f;
            t = t * t * (3.0f - 2.0f * t); // ease in and out
            Get<TransformComponent>().Position = m_Start + Offset * t;
        }

    private:
        glm::vec3 m_Start{ 0.0f };
        float m_Time = 0.0f;
    };

    // Keeps this object alive across scene loads (DontDestroyOnLoad), as a singleton: when a scene that also contains
    // it is loaded again, the newcomer removes itself so music keeps playing without restarting or doubling.
    class KeepAcrossScenes : public ScriptableEntity
    {
    public:
        void OnCreate() override
        {
            Scene& scene = GetScene();
            const std::string& name = Get<TagComponent>().Name;
            for (const auto& loaded : scene.GetLoadedScenes())
                if (loaded.Name == Scene::kPersistentSceneName)
                    for (Entity e : loaded.Roots)
                        if (e != Self() && scene.IsValid(e) && scene.GetRegistry().Get<TagComponent>(e).Name == name)
                        {
                            Log::Info("'{}' already survives from an earlier scene; removing the duplicate", name);
                            Destroy(Self());
                            return;
                        }
            scene.DontDestroyOnLoad(Self());
        }
    };

    // UI Slider (0-1) -> Audio Mixer group volume, with an optional label ("Music: 80%").
    class MixerVolumeSlider : public ScriptableEntity
    {
    public:
        std::string Group = "Music";
        std::string Label;
        void OnCreate() override
        {
            if (auto* s = TryGet<UISliderComponent>()) Apply(s->Value);
        }
        void OnUIValueChanged(float value) override { Apply(value); }

    private:
        void Apply(float value)
        {
            AudioMixer::SetVolumeDb(Group, AudioMixer::LinearToDb(value));
            Entity label = Find(Label);
            if (auto* t = label.IsNull() ? nullptr : GetScene().GetRegistry().TryGet<UITextComponent>(label))
                t->Text = Group + " Volume: " + std::to_string((int)std::round(value * 100.0f)) + "%";
        }
    };

    // Character Controller movement: WASD/arrows relative to the camera's heading, Space to jump.
    class CharacterMover : public ScriptableEntity
    {
    public:
        float MoveSpeed = 5.0f;
        float JumpSpeed = 6.0f;
        float TurnSpeed = 12.0f; // how fast the body faces the move direction

        void OnUpdate(float dt) override
        {
            auto* cc = TryGet<CharacterControllerComponent>();
            if (!cc) return;
            glm::vec2 input(0.0f);
            if (Input::IsKeyDown(Key::W) || Input::IsKeyDown(Key::Up)) input.y += 1.0f;
            if (Input::IsKeyDown(Key::S) || Input::IsKeyDown(Key::Down)) input.y -= 1.0f;
            if (Input::IsKeyDown(Key::A) || Input::IsKeyDown(Key::Left)) input.x -= 1.0f;
            if (Input::IsKeyDown(Key::D) || Input::IsKeyDown(Key::Right)) input.x += 1.0f;
            // A gamepad's left stick works too (+Y is down on the stick).
            glm::vec2 stick(Input::GetGamepadAxis(GamepadAxis::LeftX), -Input::GetGamepadAxis(GamepadAxis::LeftY));
            if (glm::dot(stick, stick) > 0.0f) input = stick;
            if (glm::length(input) > 1.0f) input = glm::normalize(input);

            // Camera-relative: forward is the camera's view direction flattened onto the ground.
            glm::vec3 forward(0, 0, -1), right(1, 0, 0);
            Entity cam = GetScene().GetPrimaryCamera();
            if (!cam.IsNull())
            {
                auto& ct = GetScene().GetRegistry().Get<TransformComponent>(cam);
                glm::vec3 f = ct.Forward();
                f.y = 0.0f;
                if (glm::length(f) > 1e-3f)
                {
                    forward = glm::normalize(f);
                    right = glm::cross(forward, glm::vec3(0, 1, 0));
                }
            }
            glm::vec3 move = (forward * input.y + right * input.x) * MoveSpeed;
            if ((Input::IsKeyPressed(Key::Space) || Input::IsGamepadButtonPressed(GamepadButton::A)) && cc->IsGrounded) cc->VerticalSpeed = JumpSpeed;
            Physics3DWorld::SimpleMoveCharacter(GetScene(), Self(), move);

            if (glm::length(move) > 0.1f)
            {
                float target = std::atan2(-move.x, -move.z);
                float& yaw = Transform().Rotation.y;
                float delta = std::remainder(target - yaw, glm::two_pi<float>());
                yaw += delta * (1.0f - std::exp(-TurnSpeed * dt));
            }
        }
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

        ScriptRegistry::Register<LightSlider>("LightSlider")
            .Field("Light", &LightSlider::Light)
            .Field("Label", &LightSlider::Label)
            .Field("Prefix", &LightSlider::Prefix);
        ScriptRegistry::Register<ScriptToggle>("ScriptToggle")
            .Field("Target", &ScriptToggle::Target)
            .Field("Script", &ScriptToggle::Script);
        ScriptRegistry::Register<BlendDriver>("BlendDriver")
            .Field("Parameter", &BlendDriver::Parameter)
            .Field("Period", &BlendDriver::Period).Range(0.1f, 120.0f);
        ScriptRegistry::Register<LoadSceneButton>("LoadSceneButton").Field("Scene", &LoadSceneButton::Scene).Field("Async", &LoadSceneButton::Async);
        ScriptRegistry::Register<LoadSceneAfterDelay>("LoadSceneAfterDelay")
            .Field("Scene", &LoadSceneAfterDelay::Scene)
            .Field("Delay", &LoadSceneAfterDelay::Delay).Range(0.0f, 3600.0f);
        ScriptRegistry::Register<KeepAcrossScenes>("KeepAcrossScenes");
        ScriptRegistry::Register<PlatformMover>("PlatformMover")
            .Field("Offset", &PlatformMover::Offset)
            .Field("Speed", &PlatformMover::Speed).Range(0.0f, 100.0f).Speed(0.05f)
            .Field("Pause", &PlatformMover::Pause).Range(0.0f, 60.0f).Speed(0.05f);
        ScriptRegistry::Register<MixerVolumeSlider>("MixerVolumeSlider")
            .Field("Group", &MixerVolumeSlider::Group)
            .Field("Label", &MixerVolumeSlider::Label);
        ScriptRegistry::Register<CarDriver>("CarDriver")
            .Field("Speedometer", &CarDriver::Speedometer)
            .Field("Steer Speed", &CarDriver::SteerSpeed).Range(0.1f, 100.0f);
        ScriptRegistry::Register<ChaseCamera>("ChaseCamera")
            .Field("Target", &ChaseCamera::Target)
            .Field("Distance", &ChaseCamera::Distance).Range(0.0f, 100.0f)
            .Field("Height", &ChaseCamera::Height).Range(-100.0f, 100.0f)
            .Field("Smoothing", &ChaseCamera::Smoothing).Range(0.0f, 100.0f);
        ScriptRegistry::Register<CharacterMover>("CharacterMover")
            .Field("Move Speed", &CharacterMover::MoveSpeed).Range(0.0f, 100.0f)
            .Field("Jump Speed", &CharacterMover::JumpSpeed).Range(0.0f, 100.0f)
            .Field("Turn Speed", &CharacterMover::TurnSpeed).Range(0.0f, 100.0f);
        SystemRegistry::Register<PulseSystem>("Pulse");
        Log::Info("Sandbox game module registered");
    }
}

// Called by the engine after loading (and after every hot reload of) this DLL.
DIM_GAME_MODULE_ENTRY
{
    Game::RegisterGameModule();
}
