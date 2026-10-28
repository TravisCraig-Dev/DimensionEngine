#include "Dimension/Scripting/CSharpRuntime.h"
#include "Dimension/Scripting/ScriptRegistry.h"
#include "Dimension/Scripting/ScriptableEntity.h"
#include "Dimension/Scene/ComponentRegistry.h"
#include "Dimension/Physics/Physics2D.h"
#include "Dimension/Physics/Physics3D.h"
#include "Dimension/Scene/SceneManager.h"
#include <unordered_map>
#include "Dimension/Audio/AudioEngine.h"
#include "Dimension/Audio/Microphone.h"
#include "Dimension/Terrain/Terrain.h"
#include "Dimension/Navigation/NavMesh.h"
#include "Dimension/Asset/AssetManager.h"
#include "Dimension/Core/Input.h"
#include "Dimension/Core/Log.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstring>
#include <deque>
#include <new>

#ifdef DIM_DOTNET
#include <windows.h>
#include <chrono>
#include <thread>
#include <nethost.h>
#include <hostfxr.h>
#include <coreclr_delegates.h>
#endif

namespace Dimension
{
#ifdef DIM_DOTNET
    namespace
    {
        // ---- Function tables (must match Interop.cs) --------------------------------------------
        struct NativeApi
        {
            void (*Log)(int, const char*);
            void (*GetTransform)(void*, uint64_t, float*, float*, float*);
            void (*SetTransform)(void*, uint64_t, const float*, const float*, const float*);
            void (*GetWorldPosition)(void*, uint64_t, float*);
            int (*HasComponent)(void*, uint64_t, const char*);
            void (*AddComponent)(void*, uint64_t, const char*);
            int (*IsValid)(void*, uint64_t);
            uint64_t (*FindEntity)(void*, const char*);
            void (*Destroy)(void*, uint64_t);
            uint64_t (*Instantiate)(void*, uint64_t, const float*);
            const char* (*GetName)(void*, uint64_t);
            void (*SetName)(void*, uint64_t, const char*);
            int (*IsKeyDown)(int);
            int (*IsKeyPressed)(int);
            int (*IsMouseDown)(int);
            void (*GetMousePosition)(float*);
            void (*GetVelocity2D)(void*, uint64_t, float*);
            void (*SetVelocity2D)(void*, uint64_t, const float*);
            void (*ApplyImpulse2D)(void*, uint64_t, const float*);
            void (*ApplyForce2D)(void*, uint64_t, const float*);
            void (*GetVelocity3D)(void*, uint64_t, float*);
            void (*SetVelocity3D)(void*, uint64_t, const float*);
            void (*AddImpulse3D)(void*, uint64_t, const float*);
            void (*AddForce3D)(void*, uint64_t, const float*);
            int (*Raycast3D)(void*, const float*, const float*, float, uint64_t*, float*, float*, float*);
            void (*SetUIText)(void*, uint64_t, const char*);
            const char* (*GetUIText)(void*, uint64_t);
            void (*AnimatorCrossFade)(void*, uint64_t, const char*, float);
            void (*ParticlesEmit)(void*, uint64_t, int);
            void (*ParticlesPlay)(void*, uint64_t, int);
            void (*AudioPlay)(void*, uint64_t, int);
            int (*GetColor)(void*, uint64_t, float*);
            void (*SetColor)(void*, uint64_t, const float*);
            void (*RegisterScriptClass)(const char*);
            float (*GetDeltaTime)();
            void (*AnimatorSetParameter)(void*, uint64_t, const char*, float, int);
            const char* (*AnimatorGetState)(void*, uint64_t);
            float (*GetUIValue)(void*, uint64_t);
            void (*SetUIValue)(void*, uint64_t, float);
            int (*OverlapSphere)(void*, const float*, float, uint32_t, uint64_t*, int);
            int (*Raycast3DMask)(void*, const float*, const float*, float, uint32_t, uint64_t*, float*, float*, float*);
            void (*LoadSceneByName)(const char*);
            void (*LoadSceneByIndex)(int);
            const char* (*GetSceneName)(void*);
            int (*GetSceneCount)();
            float (*GetTime)(void*);
            uint64_t (*GetFrameCount)(void*);
            void (*GetWorldMatrix)(void*, uint64_t, float*);
            int (*AudioIsPlaying)(void*, uint64_t);
            void (*AudioPlayOneShot)(void*, uint64_t, uint64_t, float);
            void (*MixerSetVolume)(const char*, float);
            float (*MixerGetVolume)(const char*);
            void (*CharacterMove)(void*, uint64_t, const float*, int);
            int (*CharacterState)(void*, uint64_t, float*, float*);
            void (*CharacterSetVerticalSpeed)(void*, uint64_t, float);
            void (*LineSetPositions)(void*, uint64_t, const float*, int);
            int (*LineGetPositions)(void*, uint64_t, float*, int);
            void (*TrailClear)(void*, uint64_t);
            void (*TrailSetEmitting)(void*, uint64_t, int);
            int (*TrailIsEmitting)(void*, uint64_t);
            const char* (*DropdownGetOptions)(void*, uint64_t);
            void (*DropdownSetOptions)(void*, uint64_t, const char*);
            void (*LoadSceneWithMode)(const char*, int, int);
            void (*UnloadScene)(const char*);
            void (*DontDestroyOnLoad)(void*, uint64_t);
            int (*GetLoadedSceneCount)(void*);
            const char* (*GetLoadedSceneName)(void*, int);
            void (*SetJointMotor)(void*, uint64_t, int, float);
            int (*GetJointState)(void*, uint64_t, float*);
            int (*MixerTransition)(const char*, float);
            int (*GamepadButton)(int, int, int); // button, pad, 0 = held / 1 = pressed this frame
            float (*GamepadAxis)(int, int);
            int (*LoadSceneAsync)(const char*, int, int); // operation id (0 = scene not found)
            int (*AsyncState)(int, float*);              // progress out; returns 1 when done, 0 while loading, -1 unknown id
            void (*AsyncAllowActivation)(int, int);
            void (*GamepadVibrate)(float, float, float, int); // low, high, seconds (0 = until changed), pad
            // Reflected component fields (as in the Inspector) as floats: bools 0/1, ints and enums by value, vectors
            // and colors by component. Get returns the float count (0 = no such component or field).
            int (*GetComponentField)(void*, uint64_t, const char*, const char*, float*);
            int (*SetComponentField)(void*, uint64_t, const char*, const char*, const float*, int);
            float (*VehicleState)(void*, uint64_t, int); // 0 km/h, 1 RPM, 2 gear, 3-6 wheel grounded
            float (*TimelineControl)(void*, uint64_t, int, float); // 0 play, 1 stop, 2 get time, 3 set time, 4 is playing
            int (*MicrophoneStart)(int, float, int);             // device, seconds, loop
            float (*MicrophoneState)(int);                       // 0 stop, 1 recording, 2 level, 3 peak, 4 position, 5 rate
            int (*MicrophoneSave)(const char*);
            const char* (*MicrophoneDevices)();                  // names, one per line
            void (*RagdollImpulse)(void*, uint64_t, const float*, const float*); // impulse, world point
            float (*RagdollQuery)(void*, uint64_t, int);                       // 0 face up (1/0), 1 get-up progress
            int (*BreakableBreak)(void*, uint64_t, const float*);              // world point; returns the piece count
            void (*RagdollSetPartial)(void*, uint64_t, const char*);           // bone name ("" = whole body)
            int (*TerrainCrater)(void*, const float*, float, float, int);      // world point, radius, depth, paint layer
            // NavMesh Agent: 0 set destination (v), 1 velocity (out v), 2 remaining distance, 3 has path, 4 path status,
            // 5 is stopped, 6 set stopped (v[0]), 7 reset path, 8 warp (v), 9 destination (out v), 10 on an off-mesh link
            float (*NavAgent)(void*, uint64_t, int, float*);
            // 0 sample position (a within maxDistance -> out), 1 path a -> b (corners into out, up to maxOut; returns the
            // count, negative when partial), 2 bake every surface again, 3 update the area of box a (center) by b (size)
            int (*NavQuery)(void*, int, const float*, const float*, float, float*, int);
        };

        // Background scene loads started from C#, by id.
        std::unordered_map<int, std::shared_ptr<SceneLoadOperation>> s_AsyncOps;
        int s_NextAsyncId = 1;

        struct ManagedApi
        {
            int (*LoadScripts)(const char*);
            void (*UnloadScripts)();
            intptr_t (*CreateInstance)(const char*, void*, uint64_t);
            void (*SetField)(intptr_t, int, const void*);
            void (*Invoke)(intptr_t, int, float);
            void (*InvokeEntity)(intptr_t, int, uint64_t);
            void (*DestroyInstance)(intptr_t);
            void (*InvokeString)(intptr_t, int, const char*);
            void (*RaiseEvent)(void*, uint64_t, int, float, uint64_t, const char*);
            const char* (*GetFields)(intptr_t); // JSON array of the field values (entities as packed ids)
            int (*Debugger)(int);                // 0 attached?, 1 launch, 2 break
        };

        bool s_Available = false;
        ManagedApi s_Managed{};
        NativeApi s_Native{};
        fs::path s_ScriptsPath;
        fs::file_time_type s_ScriptsTime{};
        float s_DeltaTime = 0.0f;
        thread_local std::string s_StringReturn; // backing store for strings returned to C#

        uint64_t Pack(Entity e) { return (uint64_t(e.Generation) << 32) | e.Index; }
        Entity Unpack(uint64_t v) { return Entity{ uint32_t(v & 0xFFFFFFFFu), uint32_t(v >> 32) }; }
        Scene& S(void* scene) { return *static_cast<Scene*>(scene); }

        // A reflected field of a component on an entity (null when the entity, component or field is missing).
        void* ReflectedField(void* scene, uint64_t e, const char* component, const char* field, const FieldInfo*& info)
        {
            if (!scene || !component || !field) return nullptr;
            Entity entity = Unpack(e);
            if (!S(scene).IsValid(entity)) return nullptr;
            const ComponentInfo* c = ComponentRegistry::Find(component);
            if (!c || !c->Has(S(scene), entity)) return nullptr;
            for (const FieldInfo& f : c->Fields)
                if (f.Name == field)
                {
                    info = &f;
                    return f.Access(c->Get(S(scene), entity));
                }
            return nullptr;
        }

        template <class T> T* Comp(void* scene, uint64_t e) { return scene ? S(scene).GetRegistry().TryGet<T>(Unpack(e)) : nullptr; }

        // ---- Managed class proxies ------------------------------------------------------------------
        enum class Slot : uint8_t { Float, Int, Bool, String, Vec2, Vec3, Vec4, Color4, Entity };
        constexpr size_t kSlotSize = 64;

        struct ManagedClass
        {
            std::string Name;
            std::vector<Slot> Slots;
            std::vector<FieldInfo> Fields;
            std::vector<nlohmann::json> Defaults;
        };
        std::deque<ManagedClass> s_Classes; // stable addresses (captured by ScriptInfo lambdas)

        // Script instance whose behaviour lives in C#. Inspector-visible field values are stored
        // natively (one slot per field) and pushed to the managed object when it is created.
        class ManagedScript final : public ScriptableEntity
        {
        public:
            explicit ManagedScript(const ManagedClass* cls) : m_Class(cls)
            {
                size_t size = std::max<size_t>(1, cls->Slots.size()) * kSlotSize;
                m_Storage = static_cast<uint8_t*>(::operator new(size, std::align_val_t(16)));
                std::memset(m_Storage, 0, size);
                for (size_t i = 0; i < cls->Slots.size(); ++i)
                {
                    if (cls->Slots[i] == Slot::String) new (Slot_(i)) std::string();
                    if (cls->Slots[i] == Slot::Entity) new (Slot_(i)) Entity();
                    FieldFromJson(cls->Fields[i], m_Storage, cls->Defaults[i], nullptr);
                }
            }

            ~ManagedScript() override
            {
                if (m_Handle && s_Available) s_Managed.DestroyInstance(m_Handle);
                for (size_t i = 0; i < m_Class->Slots.size(); ++i)
                    if (m_Class->Slots[i] == Slot::String) static_cast<std::string*>(Slot_(i))->~basic_string();
                ::operator delete(m_Storage, std::align_val_t(16));
            }

            void* Storage() { return m_Storage; }

            void OnCreate() override { CreateManaged(0); }
            void OnScriptsReloaded() override { CreateManaged(7); }

            // Live values of the managed object's fields into the native slots (for saving across a reload).
            void OnBeforeSerialize() override
            {
                if (!m_Handle || !s_Managed.GetFields) return;
                const char* text = s_Managed.GetFields(m_Handle);
                if (!text) return;
                try
                {
                    auto values = nlohmann::json::parse(text);
                    for (size_t i = 0; i < m_Class->Slots.size() && i < values.size(); ++i)
                    {
                        if (m_Class->Slots[i] == Slot::Entity)
                            *static_cast<Entity*>(Slot_(i)) = values[i].is_number_unsigned() ? Unpack(values[i].get<uint64_t>()) : Entity::Null();
                        else
                            FieldFromJson(m_Class->Fields[i], m_Storage, values[i], nullptr);
                    }
                }
                catch (const std::exception& e)
                {
                    Log::Error("Reading C# fields of {} failed: {}", m_Class->Name, e.what());
                }
            }

            // Creates the managed object, pushes the native field values to it and calls `method` (0 OnCreate,
            // 7 OnReloaded).
            void CreateManaged(int method)
            {
                m_Handle = s_Managed.CreateInstance(m_Class->Name.c_str(), &GetScene(), Pack(Self()));
                if (!m_Handle) return;
                for (size_t i = 0; i < m_Class->Slots.size(); ++i)
                {
                    switch (m_Class->Slots[i])
                    {
                    case Slot::String: s_Managed.SetField(m_Handle, (int)i, static_cast<std::string*>(Slot_(i))->c_str()); break;
                    case Slot::Entity:
                    {
                        uint64_t packed = Pack(*static_cast<Entity*>(Slot_(i)));
                        s_Managed.SetField(m_Handle, (int)i, &packed);
                        break;
                    }
                    default: s_Managed.SetField(m_Handle, (int)i, Slot_(i)); break;
                    }
                }
                s_Managed.Invoke(m_Handle, method, 0.0f);
            }
            void OnUpdate(float dt) override { s_DeltaTime = dt; if (m_Handle) s_Managed.Invoke(m_Handle, 1, dt); }
            void OnFixedUpdate(float dt) override { if (m_Handle) s_Managed.Invoke(m_Handle, 2, dt); }
            void OnDestroy() override { if (m_Handle) s_Managed.Invoke(m_Handle, 3, 0.0f); }
            void OnClick() override { if (m_Handle) s_Managed.Invoke(m_Handle, 4, 0.0f); }
            void OnUIValueChanged(float v) override { if (m_Handle) s_Managed.Invoke(m_Handle, 5, v); }
            void OnJointBreak(float force) override { if (m_Handle) s_Managed.Invoke(m_Handle, 6, force); }
            void OnUITextChanged(const std::string& t) override { if (m_Handle) s_Managed.InvokeString(m_Handle, 0, t.c_str()); }
            void OnUISubmit(const std::string& t) override { if (m_Handle) s_Managed.InvokeString(m_Handle, 1, t.c_str()); }
            void OnAnimationEvent(const std::string& n) override { if (m_Handle) s_Managed.InvokeString(m_Handle, 2, n.c_str()); }
            void OnCollisionEnter(Entity o) override { if (m_Handle) s_Managed.InvokeEntity(m_Handle, 0, Pack(o)); }
            void OnCollisionExit(Entity o) override { if (m_Handle) s_Managed.InvokeEntity(m_Handle, 1, Pack(o)); }
            void OnTriggerEnter(Entity o) override { if (m_Handle) s_Managed.InvokeEntity(m_Handle, 2, Pack(o)); }
            void OnTriggerExit(Entity o) override { if (m_Handle) s_Managed.InvokeEntity(m_Handle, 3, Pack(o)); }

        private:
            void* Slot_(size_t i) const { return m_Storage + i * kSlotSize; }
            const ManagedClass* m_Class;
            uint8_t* m_Storage = nullptr;
            intptr_t m_Handle = 0;
        };

        void RegisterScriptClass(const char* jsonText)
        {
            try
            {
                auto j = nlohmann::json::parse(jsonText);
                ManagedClass& cls = s_Classes.emplace_back();
                cls.Name = j.at("name").get<std::string>();
                for (auto& f : j.at("fields"))
                {
                    std::string type = f.at("type").get<std::string>();
                    Slot slot = type == "Float" ? Slot::Float : type == "Int" ? Slot::Int : type == "Bool" ? Slot::Bool
                              : type == "String" ? Slot::String : type == "Vec2" ? Slot::Vec2 : type == "Vec3" ? Slot::Vec3
                              : type == "Vec4" ? Slot::Vec4 : type == "Color4" ? Slot::Color4 : Slot::Entity;
                    FieldInfo info;
                    info.Name = f.at("name").get<std::string>();
                    info.Type = slot == Slot::Float ? FieldType::Float : slot == Slot::Int ? FieldType::Int : slot == Slot::Bool ? FieldType::Bool
                              : slot == Slot::String ? FieldType::String : slot == Slot::Vec2 ? FieldType::Vec2 : slot == Slot::Vec3 ? FieldType::Vec3
                              : slot == Slot::Vec4 ? FieldType::Vec4 : slot == Slot::Color4 ? FieldType::Color4 : FieldType::EntityRef;
                    info.Speed = slot == Slot::Int ? 1.0f : 0.05f;
                    // Inspector attributes: [Range], [Tooltip], [Header], [HideInInspector].
                    if (f.contains("min") && f.contains("max"))
                    {
                        info.Min = f["min"].get<float>();
                        info.Max = f["max"].get<float>();
                        if (slot == Slot::Float && info.Max > info.Min) info.Speed = (info.Max - info.Min) / 200.0f;
                    }
                    info.Tooltip = f.value("tooltip", std::string());
                    info.Header = f.value("header", std::string());
                    info.Hidden = f.value("hidden", false);
                    size_t offset = cls.Slots.size() * kSlotSize;
                    info.Access = [offset](void* storage) -> void* { return static_cast<uint8_t*>(storage) + offset; };
                    cls.Slots.push_back(slot);
                    cls.Fields.push_back(std::move(info));
                    cls.Defaults.push_back(f.value("default", nlohmann::json()));
                }

                ScriptInfo& info = ScriptRegistry::RegisterDynamic(cls.Name, "csharp");
                const ManagedClass* ptr = &cls;
                info.Create = [ptr] { return std::make_shared<ManagedScript>(ptr); };
                info.ToObject = [](ScriptableEntity* base) -> void* { return static_cast<ManagedScript*>(base)->Storage(); };
                info.Fields = cls.Fields;
                info.Menu = j.value("menu", std::string());
                for (auto& r : j.value("requires", nlohmann::json::array()))
                    if (r.is_string()) info.Requires.push_back(r.get<std::string>());
            }
            catch (const std::exception& e)
            {
                Log::Error("Invalid C# script metadata: {}", e.what());
            }
        }

        void FillNativeApi(NativeApi& a)
        {
            a.Log = [](int level, const char* msg) { Log::Write((LogLevel)std::clamp(level, 0, 3), std::string("[C#] ") + msg); };
            a.GetTransform = [](void* s, uint64_t e, float* p, float* r, float* sc) {
                if (auto* t = Comp<TransformComponent>(s, e)) { std::memcpy(p, &t->Position, 12); std::memcpy(r, &t->Rotation, 12); std::memcpy(sc, &t->Scale, 12); }
            };
            a.SetTransform = [](void* s, uint64_t e, const float* p, const float* r, const float* sc) {
                if (auto* t = Comp<TransformComponent>(s, e)) { std::memcpy(&t->Position, p, 12); std::memcpy(&t->Rotation, r, 12); std::memcpy(&t->Scale, sc, 12); }
            };
            a.GetWorldPosition = [](void* s, uint64_t e, float* out) {
                if (auto* t = Comp<TransformComponent>(s, e)) { glm::vec3 p = t->GetWorldPosition(); std::memcpy(out, &p, 12); }
            };
            a.HasComponent = [](void* s, uint64_t e, const char* name) -> int {
                auto* info = ComponentRegistry::Find(name);
                return info && s && info->Has(S(s), Unpack(e)) ? 1 : 0;
            };
            a.AddComponent = [](void* s, uint64_t e, const char* name) {
                if (auto* info = ComponentRegistry::Find(name); info && s && S(s).IsValid(Unpack(e))) info->Add(S(s), Unpack(e));
            };
            a.IsValid = [](void* s, uint64_t e) -> int { return s && S(s).IsValid(Unpack(e)) ? 1 : 0; };
            a.FindEntity = [](void* s, const char* name) -> uint64_t {
                if (!s) return UINT64_MAX;
                Entity e = S(s).FindEntityByName(name);
                return e.IsNull() ? UINT64_MAX : Pack(e);
            };
            a.Destroy = [](void* s, uint64_t e) { if (s) S(s).DestroyEntityDeferred(Unpack(e)); };
            a.Instantiate = [](void* s, uint64_t prefab, const float* pos) -> uint64_t {
                if (!s) return UINT64_MAX;
                Entity e = S(s).InstantiatePrefab(Guid(prefab), glm::vec3(pos[0], pos[1], pos[2]));
                return e.IsNull() ? UINT64_MAX : Pack(e);
            };
            a.GetName = [](void* s, uint64_t e) -> const char* {
                auto* tag = Comp<TagComponent>(s, e);
                s_StringReturn = tag ? tag->Name : std::string();
                return s_StringReturn.c_str();
            };
            a.SetName = [](void* s, uint64_t e, const char* name) { if (auto* tag = Comp<TagComponent>(s, e)) tag->Name = name; };
            a.IsKeyDown = [](int k) -> int { return Input::IsKeyDown((Key)k) ? 1 : 0; };
            a.IsKeyPressed = [](int k) -> int { return Input::IsKeyPressed((Key)k) ? 1 : 0; };
            a.IsMouseDown = [](int b) -> int { return Input::IsMouseDown((MouseButton)b) ? 1 : 0; };
            a.GetMousePosition = [](float* out) { glm::vec2 p = Input::GetMousePosition(); std::memcpy(out, &p, 8); };
            a.GetVelocity2D = [](void* s, uint64_t e, float* out) { glm::vec2 v = Physics2DWorld::GetLinearVelocity(S(s), Unpack(e)); std::memcpy(out, &v, 8); };
            a.SetVelocity2D = [](void* s, uint64_t e, const float* v) { Physics2DWorld::SetLinearVelocity(S(s), Unpack(e), { v[0], v[1] }); };
            a.ApplyImpulse2D = [](void* s, uint64_t e, const float* v) { Physics2DWorld::ApplyImpulse(S(s), Unpack(e), { v[0], v[1] }); };
            a.ApplyForce2D = [](void* s, uint64_t e, const float* v) { Physics2DWorld::ApplyForce(S(s), Unpack(e), { v[0], v[1] }); };
            a.GetVelocity3D = [](void* s, uint64_t e, float* out) { glm::vec3 v = Physics3DWorld::GetLinearVelocity(S(s), Unpack(e)); std::memcpy(out, &v, 12); };
            a.SetVelocity3D = [](void* s, uint64_t e, const float* v) { Physics3DWorld::SetLinearVelocity(S(s), Unpack(e), { v[0], v[1], v[2] }); };
            a.AddImpulse3D = [](void* s, uint64_t e, const float* v) { Physics3DWorld::AddImpulse(S(s), Unpack(e), { v[0], v[1], v[2] }); };
            a.AddForce3D = [](void* s, uint64_t e, const float* v) { Physics3DWorld::AddForce(S(s), Unpack(e), { v[0], v[1], v[2] }); };
            a.Raycast3D = [](void* s, const float* o, const float* d, float maxDist, uint64_t* hitEntity, float* point, float* normal, float* distance) -> int {
                if (!s) return 0;
                RaycastHit hit = Physics3DWorld::Raycast(S(s), { o[0], o[1], o[2] }, { d[0], d[1], d[2] }, maxDist);
                if (!hit) return 0;
                *hitEntity = Pack(hit.Hit);
                std::memcpy(point, &hit.Point, 12);
                std::memcpy(normal, &hit.Normal, 12);
                *distance = hit.Distance;
                return 1;
            };
            a.SetUIText = [](void* s, uint64_t e, const char* text) {
                if (auto* t = Comp<UITextComponent>(s, e)) t->Text = text;
                else if (auto* f = Comp<UIInputFieldComponent>(s, e)) f->Text = text;
            };
            a.GetUIText = [](void* s, uint64_t e) -> const char* {
                if (auto* t = Comp<UITextComponent>(s, e)) s_StringReturn = t->Text;
                else if (auto* f = Comp<UIInputFieldComponent>(s, e)) s_StringReturn = f->Text;
                else if (auto* d = Comp<UIDropdownComponent>(s, e); d && d->Value >= 0 && d->Value < (int)d->Options.size())
                    s_StringReturn = d->Options[d->Value];
                else s_StringReturn.clear();
                return s_StringReturn.c_str();
            };
            a.AnimatorCrossFade = [](void* s, uint64_t e, const char* clip, float fade) { if (auto* an = Comp<AnimatorComponent>(s, e)) an->CrossFade(clip, fade); };
            a.ParticlesEmit = [](void* s, uint64_t e, int n) { if (auto* ps = Comp<ParticleSystemComponent>(s, e)) ps->Emit(n); };
            a.ParticlesPlay = [](void* s, uint64_t e, int play) {
                if (auto* ps = Comp<ParticleSystemComponent>(s, e)) { if (play) ps->Play(); else ps->Stop(); }
            };
            a.AudioPlay = [](void* s, uint64_t e, int play) {
                if (!s) return;
                if (play) AudioWorld::Play(S(s), Unpack(e));
                else AudioWorld::Stop(S(s), Unpack(e));
            };
            a.GetColor = [](void* s, uint64_t e, float* out) -> int {
                if (auto* mr = Comp<MeshRendererComponent>(s, e)) { std::memcpy(out, &mr->Color, 16); return 1; }
                if (auto* sr = Comp<SpriteRendererComponent>(s, e)) { std::memcpy(out, &sr->Color, 16); return 1; }
                return 0;
            };
            a.SetColor = [](void* s, uint64_t e, const float* c) {
                if (auto* mr = Comp<MeshRendererComponent>(s, e)) std::memcpy(&mr->Color, c, 16);
                if (auto* sr = Comp<SpriteRendererComponent>(s, e)) std::memcpy(&sr->Color, c, 16);
            };
            a.RegisterScriptClass = &RegisterScriptClass;
            a.GetDeltaTime = [] { return s_DeltaTime; };
            a.AnimatorSetParameter = [](void* s, uint64_t e, const char* name, float value, int kind) {
                auto* an = Comp<AnimatorComponent>(s, e);
                if (!an) return;
                if (kind == 3) an->SetTrigger(name);
                else an->Runtime.Parameters[name] = value;
            };
            a.AnimatorGetState = [](void* s, uint64_t e) -> const char* {
                auto* an = Comp<AnimatorComponent>(s, e);
                s_StringReturn = an ? an->CurrentState : std::string();
                return s_StringReturn.c_str();
            };
            a.OverlapSphere = [](void* s, const float* c, float radius, uint32_t mask, uint64_t* out, int max) -> int {
                if (!s) return 0;
                auto hits = Physics3DWorld::OverlapSphere(S(s), { c[0], c[1], c[2] }, radius, mask);
                int n = std::min((int)hits.size(), max);
                for (int i = 0; i < n; ++i) out[i] = Pack(hits[i]);
                return (int)hits.size();
            };
            a.Raycast3DMask = [](void* s, const float* o, const float* d, float maxDist, uint32_t mask, uint64_t* hitEntity, float* point,
                                 float* normal, float* distance) -> int {
                if (!s) return 0;
                RaycastHit hit = Physics3DWorld::Raycast(S(s), { o[0], o[1], o[2] }, { d[0], d[1], d[2] }, maxDist, mask);
                if (!hit) return 0;
                *hitEntity = Pack(hit.Hit);
                std::memcpy(point, &hit.Point, 12);
                std::memcpy(normal, &hit.Normal, 12);
                *distance = hit.Distance;
                return 1;
            };
            a.LoadSceneByName = [](const char* name) { SceneManager::LoadScene(std::string(name ? name : "")); };
            a.LoadSceneByIndex = [](int index) { SceneManager::LoadScene(index); };
            a.GetSceneName = [](void* s) -> const char* {
                s_StringReturn = s ? S(s).GetName() : std::string();
                return s_StringReturn.c_str();
            };
            a.GetSceneCount = []() -> int { return (int)SceneManager::GetBuildScenes().size(); };
            a.GetTime = [](void* s) -> float { return s ? S(s).GetTime() : 0.0f; };
            a.AudioIsPlaying = [](void* s, uint64_t e) -> int { return s && AudioWorld::IsPlaying(S(s), Unpack(e)) ? 1 : 0; };
            a.AudioPlayOneShot = [](void* s, uint64_t e, uint64_t clip, float scale) {
                if (s) AudioWorld::PlayOneShot(S(s), Unpack(e), AssetHandle(clip), scale);
            };
            a.MixerSetVolume = [](const char* group, float db) { AudioMixer::SetVolumeDb(group ? group : "", db); };
            a.CharacterMove = [](void* s, uint64_t e, const float* v, int simple) {
                if (!s) return;
                if (simple) Physics3DWorld::SimpleMoveCharacter(S(s), Unpack(e), { v[0], v[1], v[2] });
                else Physics3DWorld::MoveCharacter(S(s), Unpack(e), { v[0], v[1], v[2] });
            };
            a.CharacterState = [](void* s, uint64_t e, float* velocity, float* verticalSpeed) -> int {
                auto* cc = Comp<CharacterControllerComponent>(s, e);
                if (!cc) return 0;
                std::memcpy(velocity, &cc->Velocity, 12);
                *verticalSpeed = cc->VerticalSpeed;
                return cc->IsGrounded ? 1 : 0;
            };
            a.CharacterSetVerticalSpeed = [](void* s, uint64_t e, float v) {
                if (auto* cc = Comp<CharacterControllerComponent>(s, e)) cc->VerticalSpeed = v;
            };
            a.MixerGetVolume = [](const char* group) -> float { return AudioMixer::GetVolumeDb(group ? group : ""); };
            a.LineSetPositions = [](void* s, uint64_t e, const float* xyz, int count) {
                auto* line = Comp<LineRendererComponent>(s, e);
                if (!line) return;
                line->Positions.resize(std::max(count, 0));
                if (count > 0) std::memcpy(line->Positions.data(), xyz, sizeof(glm::vec3) * count);
            };
            a.LineGetPositions = [](void* s, uint64_t e, float* out, int max) -> int {
                auto* line = Comp<LineRendererComponent>(s, e);
                if (!line) return 0;
                int n = std::min((int)line->Positions.size(), std::max(max, 0));
                if (n > 0) std::memcpy(out, line->Positions.data(), sizeof(glm::vec3) * n);
                return (int)line->Positions.size();
            };
            a.TrailClear = [](void* s, uint64_t e) { if (auto* t = Comp<TrailRendererComponent>(s, e)) t->Clear(); };
            a.TrailSetEmitting = [](void* s, uint64_t e, int on) { if (auto* t = Comp<TrailRendererComponent>(s, e)) t->Emitting = on != 0; };
            a.TrailIsEmitting = [](void* s, uint64_t e) -> int {
                auto* t = Comp<TrailRendererComponent>(s, e);
                return t && t->Emitting ? 1 : 0;
            };
            a.GetFrameCount = [](void* s) -> uint64_t { return s ? S(s).GetFrameCount() : 0; };
            a.GetWorldMatrix = [](void* s, uint64_t e, float* out) {
                glm::mat4 m(1.0f);
                if (auto* t = Comp<TransformComponent>(s, e)) m = t->World;
                std::memcpy(out, &m, sizeof(m));
            };
            a.GetUIValue = [](void* s, uint64_t e) -> float {
                if (auto* sl = Comp<UISliderComponent>(s, e)) return sl->Value;
                if (auto* tg = Comp<UIToggleComponent>(s, e)) return tg->IsOn ? 1.0f : 0.0f;
                if (auto* dd = Comp<UIDropdownComponent>(s, e)) return (float)dd->Value;
                return 0.0f;
            };
            a.SetUIValue = [](void* s, uint64_t e, float v) {
                if (auto* sl = Comp<UISliderComponent>(s, e)) sl->Value = std::clamp(v, std::min(sl->MinValue, sl->MaxValue), std::max(sl->MinValue, sl->MaxValue));
                if (auto* tg = Comp<UIToggleComponent>(s, e)) tg->IsOn = v != 0.0f;
                if (auto* dd = Comp<UIDropdownComponent>(s, e)) dd->Value = std::clamp((int)std::lround(v), 0, std::max(0, (int)dd->Options.size() - 1));
            };
            a.SetJointMotor = [](void* s, uint64_t e, int use, float speed) {
                if (auto* j = Comp<JointComponent>(s, e)) { j->UseMotor = use != 0; j->MotorSpeed = speed; }
                if (auto* j = Comp<Joint2DComponent>(s, e)) { j->UseMotor = use != 0; j->MotorSpeed = speed; }
            };
            // out: use motor, motor speed, force, torque. Returns 1 if broken, 0 if intact, -1 without a joint.
            a.GetJointState = [](void* s, uint64_t e, float* out) -> int {
                auto fill = [&](auto* j) {
                    out[0] = j->UseMotor ? 1.0f : 0.0f;
                    out[1] = j->MotorSpeed;
                    out[2] = j->CurrentForce;
                    out[3] = j->CurrentTorque;
                    return j->Broken ? 1 : 0;
                };
                if (auto* j = Comp<JointComponent>(s, e)) return fill(j);
                if (auto* j = Comp<Joint2DComponent>(s, e)) return fill(j);
                return -1;
            };
            a.GamepadButton = [](int button, int pad, int pressed) -> int {
                if (button < 0 || button > (int)GamepadButton::DpadLeft) return 0;
                return (pressed ? Input::IsGamepadButtonPressed((GamepadButton)button, pad) : Input::IsGamepadButtonDown((GamepadButton)button, pad)) ? 1 : 0;
            };
            a.GamepadAxis = [](int axis, int pad) -> float {
                return axis >= 0 && axis <= (int)GamepadAxis::RightTrigger ? Input::GetGamepadAxis((GamepadAxis)axis, pad) : 0.0f;
            };
            a.GamepadVibrate = [](float low, float high, float seconds, int pad) { Input::SetGamepadVibration(low, high, seconds, pad); };
            a.GetComponentField = [](void* s, uint64_t e, const char* component, const char* field, float* out) -> int {
                const FieldInfo* f = nullptr;
                void* value = ReflectedField(s, e, component, field, f);
                return value ? FieldToFloats(*f, value, out) : 0;
            };
            a.SetComponentField = [](void* s, uint64_t e, const char* component, const char* field, const float* in, int count) -> int {
                const FieldInfo* f = nullptr;
                void* value = ReflectedField(s, e, component, field, f);
                return value && FieldFromFloats(*f, value, in, count) ? 1 : 0;
            };
            a.VehicleState = [](void* s, uint64_t e, int what) -> float {
                auto* v = Comp<VehicleComponent>(s, e);
                if (!v) return 0.0f;
                if (what == 0) return v->SpeedKmh;
                if (what == 1) return v->EngineRPM;
                if (what == 2) return (float)v->Gear;
                return what >= 3 && what <= 6 && v->WheelGrounded[what - 3] ? 1.0f : 0.0f;
            };
            a.MicrophoneStart = [](int device, float seconds, int loop) -> int { return Microphone::Start(device, seconds, loop != 0) ? 1 : 0; };
            a.MicrophoneState = [](int what) -> float {
                switch (what)
                {
                case 0: Microphone::Stop(); return 0.0f;
                case 1: return Microphone::IsRecording() ? 1.0f : 0.0f;
                case 2: return Microphone::GetLevel();
                case 3: return Microphone::GetPeak();
                case 4: return (float)Microphone::GetPosition();
                case 5: return (float)Microphone::GetSampleRate();
                }
                return 0.0f;
            };
            a.MicrophoneSave = [](const char* path) -> int {
                // Relative paths are inside the project's Assets folder (so the recording can be used as a clip).
                fs::path p = fs::u8path(path ? path : "");
                if (p.is_relative()) p = AssetManager::GetAssetsDirectory() / p;
                bool ok = Microphone::SaveWav(p);
                if (ok) AssetManager::Import(p);
                return ok ? 1 : 0;
            };
            a.MicrophoneDevices = []() -> const char* {
                static std::string names;
                names.clear();
                for (const auto& d : Microphone::Devices()) names += d + "\n";
                return names.c_str();
            };
            a.RagdollImpulse = [](void* s, uint64_t e, const float* v, const float* p) {
                Physics3DWorld::AddRagdollImpulse(S(s), Unpack(e), { v[0], v[1], v[2] }, { p[0], p[1], p[2] });
            };
            a.RagdollSetPartial = [](void* s, uint64_t e, const char* bone) {
                if (auto* r = Comp<RagdollComponent>(s, e)) r->PartialRoot = bone ? bone : "";
            };
            a.NavAgent = [](void* s, uint64_t e, int op, float* v) -> float {
                auto* agent = Comp<NavMeshAgentComponent>(s, e);
                if (!agent) return 0.0f;
                Scene& scene = S(s);
                switch (op)
                {
                case 0: return Navigation::SetDestination(scene, Unpack(e), { v[0], v[1], v[2] }) ? 1.0f : 0.0f;
                case 1: std::memcpy(v, &agent->Velocity, 12); return 1.0f;
                case 2: return agent->RemainingDistance;
                case 3: return agent->HasDestination && !agent->Path.empty() ? 1.0f : 0.0f;
                case 4: return (float)agent->PathStatus;
                case 5: return agent->IsStopped ? 1.0f : 0.0f;
                case 6: agent->IsStopped = v[0] != 0.0f; return 1.0f;
                case 7: Navigation::ResetPath(scene, Unpack(e)); return 1.0f;
                case 8: return Navigation::Warp(scene, Unpack(e), { v[0], v[1], v[2] }) ? 1.0f : 0.0f;
                case 9: std::memcpy(v, &agent->Destination, 12); return 1.0f;
                case 10: return agent->Traversing ? 1.0f : 0.0f;
                default: return 0.0f;
                }
            };
            a.NavQuery = [](void* s, int op, const float* pa, const float* pb, float maxDistance, float* out, int maxOut) -> int {
                if (!s) return 0;
                Scene& scene = S(s);
                if (op == 0)
                {
                    glm::vec3 hit;
                    if (!Navigation::SamplePosition(scene, { pa[0], pa[1], pa[2] }, maxDistance, hit)) return 0;
                    std::memcpy(out, &hit, 12);
                    return 1;
                }
                if (op == 1)
                {
                    std::vector<glm::vec3> corners;
                    NavPathStatus status = Navigation::CalculatePath(scene, { pa[0], pa[1], pa[2] }, { pb[0], pb[1], pb[2] }, corners);
                    if (status == NavPathStatus::Invalid) return 0;
                    int n = std::min((int)corners.size(), std::max(maxOut, 0));
                    if (out && n > 0) std::memcpy(out, corners.data(), (size_t)n * 12);
                    return status == NavPathStatus::Partial ? -n : n;
                }
                if (op == 2) Navigation::BakeAll(scene);
                if (op == 3) return Navigation::UpdateArea(scene, glm::vec3(pa[0], pa[1], pa[2]) - glm::vec3(pb[0], pb[1], pb[2]) * 0.5f,
                                                           glm::vec3(pa[0], pa[1], pa[2]) + glm::vec3(pb[0], pb[1], pb[2]) * 0.5f);
                return 0;
            };
            a.TerrainCrater = [](void* s, const float* p, float radius, float depth, int layer) -> int {
                return Terrain::DigCrater(S(s), { p[0], p[1], p[2] }, radius, depth, layer);
            };
            a.BreakableBreak = [](void* s, uint64_t e, const float* p) -> int {
                return (int)Physics3DWorld::Break(S(s), Unpack(e), { p[0], p[1], p[2] }).size();
            };
            a.RagdollQuery = [](void* s, uint64_t e, int what) -> float {
                auto* r = Comp<RagdollComponent>(s, e);
                if (!r) return what == 1 ? 1.0f : 0.0f;
                return what == 0 ? (r->FaceUp ? 1.0f : 0.0f) : r->GetUpProgress();
            };
            a.TimelineControl = [](void* s, uint64_t e, int action, float value) -> float {
                auto* tl = Comp<TimelineComponent>(s, e);
                if (!tl) return 0.0f;
                switch (action)
                {
                case 0: tl->Play(); break;
                case 1: tl->Stop(); break;
                case 3: tl->Time = std::max(value, 0.0f); tl->Started = true; break;
                case 4: return tl->Playing ? 1.0f : 0.0f;
                }
                return tl->Time;
            };
            a.MixerTransition = [](const char* name, float seconds) -> int {
                return AudioMixer::TransitionToSnapshot(name ? name : "", seconds) ? 1 : 0;
            };
            a.LoadSceneAsync = [](const char* name, int index, int additive) -> int {
                LoadSceneMode mode = additive ? LoadSceneMode::Additive : LoadSceneMode::Single;
                auto op = name && *name ? SceneManager::LoadSceneAsync(std::string(name), mode) : SceneManager::LoadSceneAsync(index, mode);
                if (!op) return 0;
                std::erase_if(s_AsyncOps, [](const auto& kv) { return kv.second->IsDone.load(); }); // finished ones were read
                int id = s_NextAsyncId++;
                s_AsyncOps[id] = op;
                return id;
            };
            a.AsyncState = [](int id, float* progress) -> int {
                auto it = s_AsyncOps.find(id);
                if (it == s_AsyncOps.end()) { *progress = 1.0f; return -1; }
                *progress = it->second->Progress;
                return it->second->IsDone ? 1 : 0;
            };
            a.AsyncAllowActivation = [](int id, int allow) {
                if (auto it = s_AsyncOps.find(id); it != s_AsyncOps.end()) it->second->AllowSceneActivation = allow != 0;
            };
            a.LoadSceneWithMode = [](const char* name, int index, int additive) {
                LoadSceneMode mode = additive ? LoadSceneMode::Additive : LoadSceneMode::Single;
                if (name && *name) SceneManager::LoadScene(std::string(name), mode);
                else SceneManager::LoadScene(index, mode);
            };
            a.UnloadScene = [](const char* name) { SceneManager::UnloadScene(name ? name : ""); };
            a.DontDestroyOnLoad = [](void* s, uint64_t e) { if (s) S(s).DontDestroyOnLoad(Unpack(e)); };
            a.GetLoadedSceneCount = [](void* s) -> int { return s ? (int)S(s).GetLoadedScenes().size() : 0; };
            a.GetLoadedSceneName = [](void* s, int i) -> const char* {
                s_StringReturn.clear();
                if (s && i >= 0 && i < (int)S(s).GetLoadedScenes().size()) s_StringReturn = S(s).GetLoadedScenes()[i].Name;
                return s_StringReturn.c_str();
            };
            a.DropdownGetOptions = [](void* s, uint64_t e) -> const char* {
                s_StringReturn.clear();
                if (auto* dd = Comp<UIDropdownComponent>(s, e))
                    for (size_t i = 0; i < dd->Options.size(); ++i) s_StringReturn += (i ? "\n" : "") + dd->Options[i];
                return s_StringReturn.c_str();
            };
            a.DropdownSetOptions = [](void* s, uint64_t e, const char* joined) {
                auto* dd = Comp<UIDropdownComponent>(s, e);
                if (!dd) return;
                dd->Options.clear();
                std::string all = joined ? joined : "";
                for (size_t start = 0; start <= all.size();)
                {
                    size_t end = all.find('\n', start);
                    if (end == std::string::npos) end = all.size();
                    if (end > start || end < all.size()) dd->Options.push_back(all.substr(start, end - start));
                    start = end + 1;
                }
                dd->Value = std::clamp(dd->Value, 0, std::max(0, (int)dd->Options.size() - 1));
            };
        }
    }

    bool CSharpRuntime::Init(const fs::path& managedDir)
    {
        if (s_Available) return true;
        fs::path config = managedDir / "Dimension.Core.runtimeconfig.json";
        fs::path assembly = managedDir / "Dimension.Core.dll";
        if (!fs::exists(config) || !fs::exists(assembly))
        {
            Log::Info("C# scripting disabled: Dimension.Core not found in '{}'", managedDir.string());
            return false;
        }

        char_t hostfxrPath[MAX_PATH];
        size_t size = MAX_PATH;
        if (get_hostfxr_path(hostfxrPath, &size, nullptr) != 0)
        {
            Log::Warn("C# scripting disabled: .NET runtime (hostfxr) not found. Install the .NET 10 runtime.");
            return false;
        }
        HMODULE hostfxr = LoadLibraryW(hostfxrPath);
        auto init = (hostfxr_initialize_for_runtime_config_fn)GetProcAddress(hostfxr, "hostfxr_initialize_for_runtime_config");
        auto getDelegate = (hostfxr_get_runtime_delegate_fn)GetProcAddress(hostfxr, "hostfxr_get_runtime_delegate");
        auto close = (hostfxr_close_fn)GetProcAddress(hostfxr, "hostfxr_close");
        if (!init || !getDelegate || !close)
        {
            Log::Error("C# scripting disabled: incompatible hostfxr");
            return false;
        }

        hostfxr_handle context = nullptr;
        int rc = init(config.c_str(), nullptr, &context);
        if (rc < 0 || !context)
        {
            Log::Error("C# scripting disabled: runtime initialization failed (0x{:08x})", (uint32_t)rc);
            if (context) close(context);
            return false;
        }
        load_assembly_and_get_function_pointer_fn loadAssembly = nullptr;
        rc = getDelegate(context, hdt_load_assembly_and_get_function_pointer, (void**)&loadAssembly);
        close(context);
        if (rc < 0 || !loadAssembly)
        {
            Log::Error("C# scripting disabled: could not get the runtime loader (0x{:08x})", (uint32_t)rc);
            return false;
        }

        int (*initialize)(void*, void*) = nullptr;
        rc = loadAssembly(assembly.c_str(), L"Dimension.Interop.Host, Dimension.Core", L"Initialize", UNMANAGEDCALLERSONLY_METHOD, nullptr,
                          (void**)&initialize);
        if (rc < 0 || !initialize)
        {
            Log::Error("C# scripting disabled: Dimension.Core entry point not found (0x{:08x})", (uint32_t)rc);
            return false;
        }
        FillNativeApi(s_Native);
        if (!initialize(&s_Native, &s_Managed)) return false;
        s_Available = true;
        // C# events on engine objects (UIButton.Clicked, Entity.CollisionEnter, ...).
        Scene::SetEventHook([](Scene& scene, Entity e, Scene::EventKind kind, float value, Entity other, const char* text) {
            if (s_Managed.RaiseEvent) s_Managed.RaiseEvent(&scene, Pack(e), (int)kind, value, Pack(other), text);
        });
        return true;
    }

    bool CSharpRuntime::IsAvailable() { return s_Available; }

    bool CSharpRuntime::LoadScripts(const fs::path& assemblyPath)
    {
        if (!s_Available) return false;
        UnloadScripts();
        s_ScriptsPath = assemblyPath;
        std::error_code ec;
        if (!fs::exists(assemblyPath)) return false;
        s_ScriptsTime = fs::last_write_time(assemblyPath, ec);
        int count = s_Managed.LoadScripts(assemblyPath.string().c_str());
        Log::Info("Loaded {} C# script class(es) from '{}'", count, assemblyPath.filename().string());
        return count >= 0;
    }

    void CSharpRuntime::UnloadScripts()
    {
        if (!s_Available) return;
        ScriptRegistry::ClearModule("csharp");
        s_Classes.clear();
        s_Managed.UnloadScripts();
    }

    const fs::path& CSharpRuntime::GetScriptsPath() { return s_ScriptsPath; }

    bool CSharpRuntime::ScriptsChangedOnDisk()
    {
        if (!s_Available || s_ScriptsPath.empty()) return false;
        std::error_code ec;
        auto t = fs::last_write_time(s_ScriptsPath, ec);
        return !ec && t != s_ScriptsTime;
    }

    size_t CSharpRuntime::GetScriptCount() { return s_Classes.size(); }

    bool CSharpRuntime::IsDebuggerAttached() { return s_Available && s_Managed.Debugger && s_Managed.Debugger(0) != 0; }
    bool CSharpRuntime::LaunchDebugger() { return s_Available && s_Managed.Debugger && s_Managed.Debugger(1) != 0; }

    bool CSharpRuntime::WaitForDebugger(float seconds)
    {
        if (!s_Available) return false;
        DWORD pid = GetCurrentProcessId();
        Log::Info("Waiting up to {:g} s for a C# debugger: attach to process {} (Rider: Run > Attach to Process, .NET; Visual Studio: "
                  "Debug > Attach to Process, Managed (.NET Core))", seconds, (unsigned)pid);
        auto end = std::chrono::steady_clock::now() + std::chrono::duration<float>(std::max(seconds, 0.0f));
        while (!IsDebuggerAttached())
        {
            if (std::chrono::steady_clock::now() >= end) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        Log::Info("C# debugger attached");
        return true;
    }
#else
    bool CSharpRuntime::IsDebuggerAttached() { return false; }
    bool CSharpRuntime::LaunchDebugger() { return false; }
    bool CSharpRuntime::WaitForDebugger(float) { return false; }
    bool CSharpRuntime::Init(const fs::path&) { return false; }
    bool CSharpRuntime::IsAvailable() { return false; }
    bool CSharpRuntime::LoadScripts(const fs::path&) { return false; }
    void CSharpRuntime::UnloadScripts() {}
    const fs::path& CSharpRuntime::GetScriptsPath() { static fs::path p; return p; }
    bool CSharpRuntime::ScriptsChangedOnDisk() { return false; }
    size_t CSharpRuntime::GetScriptCount() { return 0; }
#endif
}
