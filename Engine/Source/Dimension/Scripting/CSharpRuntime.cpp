#include "Dimension/Scripting/CSharpRuntime.h"
#include "Dimension/Scripting/ScriptRegistry.h"
#include "Dimension/Scripting/ScriptableEntity.h"
#include "Dimension/Scene/ComponentRegistry.h"
#include "Dimension/Physics/Physics2D.h"
#include "Dimension/Physics/Physics3D.h"
#include "Dimension/Scene/SceneManager.h"
#include "Dimension/Audio/AudioEngine.h"
#include "Dimension/Core/Input.h"
#include "Dimension/Core/Log.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstring>
#include <deque>
#include <new>

#ifdef DIM_DOTNET
#include <windows.h>
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
        };

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

            void OnCreate() override
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
                s_Managed.Invoke(m_Handle, 0, 0.0f);
            }
            void OnUpdate(float dt) override { s_DeltaTime = dt; if (m_Handle) s_Managed.Invoke(m_Handle, 1, dt); }
            void OnFixedUpdate(float dt) override { if (m_Handle) s_Managed.Invoke(m_Handle, 2, dt); }
            void OnDestroy() override { if (m_Handle) s_Managed.Invoke(m_Handle, 3, 0.0f); }
            void OnClick() override { if (m_Handle) s_Managed.Invoke(m_Handle, 4, 0.0f); }
            void OnUIValueChanged(float v) override { if (m_Handle) s_Managed.Invoke(m_Handle, 5, v); }
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
            a.GetFrameCount = [](void* s) -> uint64_t { return s ? S(s).GetFrameCount() : 0; };
            a.GetWorldMatrix = [](void* s, uint64_t e, float* out) {
                glm::mat4 m(1.0f);
                if (auto* t = Comp<TransformComponent>(s, e)) m = t->World;
                std::memcpy(out, &m, sizeof(m));
            };
            a.GetUIValue = [](void* s, uint64_t e) -> float {
                if (auto* sl = Comp<UISliderComponent>(s, e)) return sl->Value;
                if (auto* tg = Comp<UIToggleComponent>(s, e)) return tg->IsOn ? 1.0f : 0.0f;
                return 0.0f;
            };
            a.SetUIValue = [](void* s, uint64_t e, float v) {
                if (auto* sl = Comp<UISliderComponent>(s, e)) sl->Value = std::clamp(v, std::min(sl->MinValue, sl->MaxValue), std::max(sl->MinValue, sl->MaxValue));
                if (auto* tg = Comp<UIToggleComponent>(s, e)) tg->IsOn = v != 0.0f;
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
#else
    bool CSharpRuntime::Init(const fs::path&) { return false; }
    bool CSharpRuntime::IsAvailable() { return false; }
    bool CSharpRuntime::LoadScripts(const fs::path&) { return false; }
    void CSharpRuntime::UnloadScripts() {}
    const fs::path& CSharpRuntime::GetScriptsPath() { static fs::path p; return p; }
    bool CSharpRuntime::ScriptsChangedOnDisk() { return false; }
    size_t CSharpRuntime::GetScriptCount() { return 0; }
#endif
}
