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
#include <glm/gtc/constants.hpp>
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
            // 5 is stopped, 6 set stopped (v[0]), 7 reset path, 8 warp (v), 9 destination (out v), 10 on an off-mesh link, 11 waiting at a closed door
            float (*NavAgent)(void*, uint64_t, int, float*);
            // 0 sample position (a within maxDistance -> out), 1 path a -> b (corners into out, up to maxOut; returns the
            // count, negative when partial), 2 bake every surface again, 3 update the area of box a (center) by b (size)
            int (*NavQuery)(void*, int, const float*, const float*, float, float*, int);
            // Editor extensions: through the hooks the editor sets (CSharpRuntime::SetEditorHooks).
            int (*EditorGui)(int, const char*, float*, int);
            const char* (*EditorGuiText)(const char*, const char*);
            uint64_t (*EditorSelection)(int, uint64_t, const char*);
            void (*EditorUndo)(const char*);
            void (*RegisterEditorItem)(const char*);
            uint64_t (*EditorGuiRef)(int, const char*, uint64_t); // 0 an entity (packed), else an asset kind (1 Prefab ... 5 Audio)
            void (*Gizmo)(int, const float*); // 0 line, 1 wire sphere, 2 wire cube (see Gizmos in EditorApi.cs)
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
            void (*EditorCall)(int, int, void*); // 0 run menu item, 1 window OnGUI; id; scene
            // A custom Inspector: id, live instance (or 0), class, saved values (JSON array), scene, entity -> values after
            const char* (*Inspect)(int, intptr_t, const char*, const char*, void*, uint64_t);
            // A field's property drawer: field index, then as Inspect
            const char* (*DrawField)(int, intptr_t, const char*, const char*, void*, uint64_t);
            // The Inspector's Debug mode: unsaved fields as text, JSON [{name, type, value}] (arguments as DrawField's)
            const char* (*DebugFields)(int, intptr_t, const char*, const char*, void*, uint64_t);
            // OnDrawGizmos (1: and OnDrawGizmosSelected); [ContextMenu] item n (the values after). As DrawField's.
            const char* (*DrawGizmos)(int, intptr_t, const char*, const char*, void*, uint64_t);
            const char* (*RunContextMenu)(int, intptr_t, const char*, const char*, void*, uint64_t);
        };
        // Where gizmos go while a script draws them (null: they are dropped).
        std::vector<CSharpRuntime::GizmoLine>* s_GizmoSink = nullptr;

        // A gizmo shape as lines into the sink: 1 three circles (center, radius), 2 a box's twelve edges (center, size);
        // `data` as Gizmos.Shape writes it (a matrix, then the shape, then the color).
        void AddGizmo(int kind, const float* data)
        {
            if (!s_GizmoSink) return;
            if (kind == 0)
            {
                s_GizmoSink->push_back({ { data[0], data[1], data[2] }, { data[3], data[4], data[5] }, { data[6], data[7], data[8], data[9] } });
                return;
            }
            glm::mat4 m;
            std::memcpy(&m, data, sizeof(m));
            glm::vec3 center(data[16], data[17], data[18]);
            auto at = [&](const glm::vec3& p) { return glm::vec3(m * glm::vec4(p, 1.0f)); };
            if (kind == 1)
            {
                float radius = data[19];
                glm::vec4 color(data[20], data[21], data[22], data[23]);
                constexpr int kSegments = 32;
                for (int axis = 0; axis < 3; ++axis)
                    for (int s = 0; s < kSegments; ++s)
                    {
                        auto point = [&](int k) {
                            float t = glm::two_pi<float>() * (float)k / kSegments;
                            glm::vec3 p(0.0f);
                            p[(axis + 1) % 3] = std::cos(t) * radius;
                            p[(axis + 2) % 3] = std::sin(t) * radius;
                            return at(center + p);
                        };
                        s_GizmoSink->push_back({ point(s), point(s + 1), color });
                    }
            }
            else if (kind == 2)
            {
                glm::vec3 half = glm::vec3(data[19], data[20], data[21]) * 0.5f;
                glm::vec4 color(data[22], data[23], data[24], data[25]);
                auto corner = [&](int i) { return at(center + half * glm::vec3(i & 1 ? 1.0f : -1.0f, i & 2 ? 1.0f : -1.0f, i & 4 ? 1.0f : -1.0f)); };
                for (int i = 0; i < 8; ++i)
                    for (int bit = 1; bit < 8; bit <<= 1)
                        if (!(i & bit)) s_GizmoSink->push_back({ corner(i), corner(i | bit), color });
            }
        }
        CSharpRuntime::EditorHooks s_EditorHooks;
        std::vector<CSharpRuntime::EditorItem> s_EditorItems;

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
        // Field values live natively, one kSlotSize slot per field. Enums are stored as Int slots; a List slot holds a
        // ManagedList; a Struct slot (a [Serializable] struct or class) holds a Block of its own fields.
        enum class Slot : uint8_t { Float, Int, Bool, String, Vec2, Vec3, Vec4, Color4, Entity, Asset, List, Struct };
        constexpr size_t kSlotSize = 64;

        struct Layout;

        // The values of a Layout's fields (heap storage, copied deeply). A default one holds nothing.
        class Block
        {
        public:
            Block() = default;
            explicit Block(const Layout* layout) { Init(layout); }
            Block(const Block& o) : Block(o.m_Layout) { CopyFrom(o); }
            Block(Block&& o) noexcept : m_Layout(o.m_Layout), m_Data(o.m_Data) { o.m_Layout = nullptr, o.m_Data = nullptr; }
            Block& operator=(const Block& o)
            {
                if (this != &o)
                {
                    Reset();
                    Init(o.m_Layout);
                    CopyFrom(o);
                }
                return *this;
            }
            Block& operator=(Block&& o) noexcept
            {
                std::swap(m_Layout, o.m_Layout);
                std::swap(m_Data, o.m_Data);
                return *this;
            }
            ~Block() { Reset(); }

            uint8_t* Data() const { return m_Data; }
            void* Slot(size_t i) const { return m_Data + i * kSlotSize; }

        private:
            void Init(const Layout* layout);
            void Reset();
            void CopyFrom(const Block& o);
            const Layout* m_Layout = nullptr;
            uint8_t* m_Data = nullptr;
        };

        // An element of a C# list or array field: its value in the member its type uses.
        struct ListElement
        {
            alignas(16) uint8_t Bytes[16] = {}; // numbers, vectors, colors, enums, asset GUIDs
            std::string Text;
            Entity Ref = Entity::Null();
            Block Nested; // a [Serializable] type's fields
        };
        using ManagedList = std::vector<ListElement>;

        // The fields of a script class, or of a [Serializable] type in one.
        struct Layout
        {
            std::vector<Slot> Slots;
            std::vector<Slot> ElementSlots;      // per field: a List's element type
            std::vector<const Layout*> Nested;   // per field: a Struct's fields, or a List's element type's
            std::vector<FieldInfo> Fields;
            std::vector<nlohmann::json> Defaults;
        };

        void Block::Init(const Layout* layout)
        {
            m_Layout = layout;
            if (!layout) return;
            size_t size = std::max<size_t>(1, layout->Slots.size()) * kSlotSize;
            m_Data = static_cast<uint8_t*>(::operator new(size, std::align_val_t(16)));
            std::memset(m_Data, 0, size);
            for (size_t i = 0; i < layout->Slots.size(); ++i)
            {
                switch (layout->Slots[i])
                {
                case Slot::String: new (Slot(i)) std::string(); break;
                case Slot::Entity: new (Slot(i)) Entity(); break;
                case Slot::List: new (Slot(i)) ManagedList(); break;
                case Slot::Struct: new (Slot(i)) Block(layout->Nested[i]); break;
                default: break;
                }
                FieldFromJson(layout->Fields[i], m_Data, layout->Defaults[i], nullptr);
            }
        }

        void Block::Reset()
        {
            if (m_Layout && m_Data)
                for (size_t i = 0; i < m_Layout->Slots.size(); ++i)
                {
                    switch (m_Layout->Slots[i])
                    {
                    case Slot::String: static_cast<std::string*>(Slot(i))->~basic_string(); break;
                    case Slot::List: static_cast<ManagedList*>(Slot(i))->~ManagedList(); break;
                    case Slot::Struct: static_cast<Block*>(Slot(i))->~Block(); break;
                    default: break;
                    }
                }
            if (m_Data) ::operator delete(m_Data, std::align_val_t(16));
            m_Layout = nullptr;
            m_Data = nullptr;
        }

        void Block::CopyFrom(const Block& o)
        {
            if (!m_Layout || !o.m_Data) return;
            for (size_t i = 0; i < m_Layout->Slots.size(); ++i)
            {
                switch (m_Layout->Slots[i])
                {
                case Slot::String: *static_cast<std::string*>(Slot(i)) = *static_cast<const std::string*>(o.Slot(i)); break;
                case Slot::Entity: *static_cast<Entity*>(Slot(i)) = *static_cast<const Entity*>(o.Slot(i)); break;
                case Slot::List: *static_cast<ManagedList*>(Slot(i)) = *static_cast<const ManagedList*>(o.Slot(i)); break;
                case Slot::Struct: *static_cast<Block*>(Slot(i)) = *static_cast<const Block*>(o.Slot(i)); break;
                default: std::memcpy(Slot(i), o.Slot(i), kSlotSize); break;
                }
            }
        }

        struct ManagedClass
        {
            std::string Name;
            Layout Values;
            std::deque<Layout> NestedLayouts; // the [Serializable] types' (stable addresses)
            std::vector<bool> Drawn;          // per field: a C# property drawer draws it
            bool HasGizmos = false;           // overrides OnDrawGizmos or OnDrawGizmosSelected
            std::vector<std::string> ContextMenu;
        };

        // A field's (or list element's) slot and Inspector type from its registration ("type", and for enums
        // "enumNames", "enumValues" and "flags").
        Slot ParseFieldType(const nlohmann::json& f, FieldInfo& info)
        {
            std::string type = f.at("type").get<std::string>();
            // Asset references (Prefab, Texture, ...) are stored as their GUID and picked like built-in ones.
            const std::pair<const char*, FieldType> assets[] = { { "Prefab", FieldType::Prefab }, { "Texture", FieldType::Texture },
                                                                 { "Material", FieldType::Material }, { "Mesh", FieldType::Mesh },
                                                                 { "Audio", FieldType::Audio } };
            for (auto& a : assets)
                if (type == a.first)
                {
                    info.Type = a.second;
                    return Slot::Asset;
                }
            if (type == "Enum")
            {
                info.Type = FieldType::Enum;
                for (auto& n : f.value("enumNames", nlohmann::json::array())) info.EnumNames.push_back(n.get<std::string>());
                for (auto& v : f.value("enumValues", nlohmann::json::array())) info.EnumValues.push_back(v.get<int32_t>());
                info.EnumFlags = f.value("flags", false);
                return Slot::Int;
            }
            Slot slot = type == "Float" ? Slot::Float : type == "Int" ? Slot::Int : type == "Bool" ? Slot::Bool
                      : type == "String" ? Slot::String : type == "Vec2" ? Slot::Vec2 : type == "Vec3" ? Slot::Vec3
                      : type == "Vec4" ? Slot::Vec4 : type == "Color4" ? Slot::Color4 : Slot::Entity;
            info.Type = slot == Slot::Float ? FieldType::Float : slot == Slot::Int ? FieldType::Int : slot == Slot::Bool ? FieldType::Bool
                      : slot == Slot::String ? FieldType::String : slot == Slot::Vec2 ? FieldType::Vec2 : slot == Slot::Vec3 ? FieldType::Vec3
                      : slot == Slot::Vec4 ? FieldType::Vec4 : slot == Slot::Color4 ? FieldType::Color4 : FieldType::EntityRef;
            info.Speed = slot == Slot::Int ? 1.0f : 0.05f;
            return slot;
        }

        // The layout of registered fields (a class's, or a [Serializable] type's "fields"); nested types' layouts go
        // to `cls`.
        void BuildLayout(const nlohmann::json& fields, Layout& layout, ManagedClass& cls)
        {
            for (auto& f : fields)
            {
                FieldInfo info;
                info.Name = f.at("name").get<std::string>();
                Slot slot, elementSlot = Slot::Float;
                const Layout* nested = nullptr;
                std::string type = f.at("type").get<std::string>();
                if (type == "List")
                {
                    slot = Slot::List;
                    const nlohmann::json& element = f.at("element");
                    if (element.at("type").get<std::string>() == "Struct")
                    {
                        // A list of [Serializable] values: a list of structs, each element its own Block.
                        elementSlot = Slot::Struct;
                        Layout& inner = cls.NestedLayouts.emplace_back();
                        BuildLayout(element.at("fields"), inner, cls);
                        nested = &inner;
                        info.Elements = inner.Fields;
                        info.ListAt = [](void* list, size_t i) -> void* { return (*static_cast<ManagedList*>(list))[i].Nested.Data(); };
                        info.ListResize = [nested](void* list, size_t n) {
                            auto& v = *static_cast<ManagedList*>(list);
                            size_t old = v.size();
                            v.resize(n);
                            for (size_t i = old; i < n; ++i) v[i].Nested = Block(nested);
                        };
                    }
                    else
                    {
                        // A list of plain values (one unnamed element field).
                        FieldInfo value;
                        elementSlot = ParseFieldType(element, value);
                        value.Access = [elementSlot](void* item) -> void* {
                            auto* e = static_cast<ListElement*>(item);
                            return elementSlot == Slot::String ? (void*)&e->Text : elementSlot == Slot::Entity ? (void*)&e->Ref : (void*)e->Bytes;
                        };
                        info.Elements.push_back(std::move(value));
                        info.ListAt = [](void* list, size_t i) -> void* { return &(*static_cast<ManagedList*>(list))[i]; };
                        info.ListResize = [](void* list, size_t n) { static_cast<ManagedList*>(list)->resize(n); };
                    }
                    info.Type = FieldType::StructList;
                    info.ListSize = [](const void* list) { return static_cast<const ManagedList*>(list)->size(); };
                    info.ListErase = [](void* list, size_t i) {
                        auto& v = *static_cast<ManagedList*>(list);
                        if (i < v.size()) v.erase(v.begin() + i);
                    };
                    info.ListMove = [](void* list, size_t from, size_t to) {
                        auto& v = *static_cast<ManagedList*>(list);
                        if (from >= v.size() || to >= v.size() || from == to) return;
                        ListElement e = std::move(v[from]);
                        v.erase(v.begin() + from);
                        v.insert(v.begin() + to, std::move(e));
                    };
                }
                else if (type == "Struct")
                {
                    // A [Serializable] struct or class: its fields, opened under the field in the Inspector.
                    slot = Slot::Struct;
                    Layout& inner = cls.NestedLayouts.emplace_back();
                    BuildLayout(f.at("fields"), inner, cls);
                    nested = &inner;
                    info.Type = FieldType::Struct;
                    info.Elements = inner.Fields;
                }
                else
                    slot = ParseFieldType(f, info);
                // Inspector attributes: [Range], [Tooltip], [Header], [HideInInspector].
                if (f.contains("min") && f.contains("max") && slot != Slot::Struct && elementSlot != Slot::Struct)
                {
                    // On a list, the range applies to its elements.
                    FieldInfo& ranged = slot == Slot::List ? info.Elements[0] : info;
                    ranged.Min = f["min"].get<float>();
                    ranged.Max = f["max"].get<float>();
                    if ((slot == Slot::List ? elementSlot : slot) == Slot::Float && ranged.Max > ranged.Min) ranged.Speed = (ranged.Max - ranged.Min) / 200.0f;
                }
                info.Tooltip = f.value("tooltip", std::string());
                info.Header = f.value("header", std::string());
                info.Hidden = f.value("hidden", false);
                size_t offset = layout.Slots.size() * kSlotSize;
                if (slot == Slot::Struct)
                    info.Access = [offset](void* data) -> void* { return reinterpret_cast<Block*>(static_cast<uint8_t*>(data) + offset)->Data(); };
                else
                    info.Access = [offset](void* data) -> void* { return static_cast<uint8_t*>(data) + offset; };
                layout.Slots.push_back(slot);
                layout.ElementSlots.push_back(elementSlot);
                layout.Nested.push_back(nested);
                layout.Fields.push_back(std::move(info));
                layout.Defaults.push_back(f.value("default", nlohmann::json()));
            }
        }

        // Values as the scripts exchange them: a class's fields as an array in order (`array`) and a nested type's as
        // an object by name; entities as packed ids (null for none), lists as arrays.
        nlohmann::json ValuesJson(const Layout& layout, const Block& block, bool array)
        {
            nlohmann::json out = array ? nlohmann::json::array() : nlohmann::json::object();
            for (size_t i = 0; i < layout.Slots.size(); ++i)
            {
                nlohmann::json v;
                switch (layout.Slots[i])
                {
                case Slot::Entity:
                {
                    Entity e = *static_cast<const Entity*>(block.Slot(i));
                    if (!e.IsNull()) v = Pack(e);
                    break;
                }
                case Slot::Struct: v = ValuesJson(*layout.Nested[i], *static_cast<const Block*>(block.Slot(i)), false); break;
                case Slot::List:
                {
                    v = nlohmann::json::array();
                    for (const ListElement& item : *static_cast<const ManagedList*>(block.Slot(i)))
                    {
                        if (layout.ElementSlots[i] == Slot::Entity) v.push_back(item.Ref.IsNull() ? nlohmann::json() : nlohmann::json(Pack(item.Ref)));
                        else if (layout.ElementSlots[i] == Slot::Struct) v.push_back(ValuesJson(*layout.Nested[i], item.Nested, false));
                        else v.push_back(FieldToJson(layout.Fields[i].Elements[0], const_cast<ListElement*>(&item), nullptr));
                    }
                    break;
                }
                default: v = FieldToJson(layout.Fields[i], block.Data(), nullptr); break;
                }
                if (array) out.push_back(std::move(v));
                else out[layout.Fields[i].Name] = std::move(v);
            }
            return out;
        }

        void ReadValues(const Layout& layout, Block& block, const nlohmann::json& values)
        {
            for (size_t i = 0; i < layout.Slots.size(); ++i)
            {
                const nlohmann::json* v = nullptr;
                if (values.is_array() && i < values.size()) v = &values[i];
                if (values.is_object() && values.contains(layout.Fields[i].Name)) v = &values[layout.Fields[i].Name];
                if (!v) continue;
                switch (layout.Slots[i])
                {
                case Slot::Entity: *static_cast<Entity*>(block.Slot(i)) = v->is_number_unsigned() ? Unpack(v->get<uint64_t>()) : Entity::Null(); break;
                case Slot::Struct: ReadValues(*layout.Nested[i], *static_cast<Block*>(block.Slot(i)), *v); break;
                case Slot::List:
                {
                    if (!v->is_array()) break;
                    const FieldInfo& field = layout.Fields[i];
                    field.ListResize(block.Slot(i), 0);
                    field.ListResize(block.Slot(i), v->size());
                    auto& list = *static_cast<ManagedList*>(block.Slot(i));
                    for (size_t k = 0; k < v->size(); ++k)
                    {
                        const nlohmann::json& item = (*v)[k];
                        if (layout.ElementSlots[i] == Slot::Entity) list[k].Ref = item.is_number_unsigned() ? Unpack(item.get<uint64_t>()) : Entity::Null();
                        else if (layout.ElementSlots[i] == Slot::Struct) ReadValues(*layout.Nested[i], list[k].Nested, item);
                        else FieldFromJson(field.Elements[0], &list[k], item, nullptr);
                    }
                    break;
                }
                default:
                    if (!v->is_null()) FieldFromJson(layout.Fields[i], block.Data(), *v, nullptr);
                    break;
                }
            }
        }

        std::deque<ManagedClass> s_Classes; // stable addresses (captured by ScriptInfo lambdas)

        // Script instance whose behaviour lives in C#. Inspector-visible field values are stored
        // natively (one slot per field) and pushed to the managed object when it is created.
        class ManagedScript final : public ScriptableEntity
        {
        public:
            explicit ManagedScript(const ManagedClass* cls) : m_Class(cls), m_Values(&cls->Values) {}

            ~ManagedScript() override
            {
                if (m_Handle && s_Available) s_Managed.DestroyInstance(m_Handle);
            }

            void* Storage() { return m_Values.Data(); }

            // Draws custom Inspector `id` for this script (its live object while playing, else one made from the slots)
            // and takes the values it leaves; true when they changed.
            bool Inspect(int id, Scene& scene, Entity e) { return Draw(s_Managed.Inspect, id, scene, e); }
            // Draws field `index` with its property drawer; true when the values changed.
            bool DrawField(size_t index, Scene& scene, Entity e) { return Draw(s_Managed.DrawField, (int)index, scene, e); }
            const ManagedClass& Class() const { return *m_Class; }

            void DrawGizmos(bool selected, Scene& scene, Entity e) const
            {
                if (!s_Managed.DrawGizmos || !m_Class->HasGizmos) return;
                std::string values = SlotsJson().dump();
                s_Managed.DrawGizmos(selected ? 1 : 0, m_Handle, m_Class->Name.c_str(), values.c_str(), &scene, Pack(e));
            }

            bool RunContextMenu(size_t item, Scene& scene, Entity e) { return Draw(s_Managed.RunContextMenu, (int)item, scene, e); }

            std::vector<CSharpRuntime::DebugField> DebugFields(Scene& scene, Entity e) const
            {
                std::vector<CSharpRuntime::DebugField> out;
                if (!s_Managed.DebugFields) return out;
                std::string values = SlotsJson().dump();
                const char* text = s_Managed.DebugFields(0, m_Handle, m_Class->Name.c_str(), values.c_str(), &scene, Pack(e));
                if (!text) return out;
                try
                {
                    for (auto& f : nlohmann::json::parse(text))
                        out.push_back({ f.value("name", std::string()), f.value("type", std::string()), f.value("value", std::string()) });
                }
                catch (const std::exception&)
                {
                }
                return out;
            }

            bool Draw(const char* (*call)(int, intptr_t, const char*, const char*, void*, uint64_t), int id, Scene& scene, Entity e)
            {
                if (!call) return false;
                std::string before = SlotsJson().dump();
                const char* after = call(id, m_Handle, m_Class->Name.c_str(), before.c_str(), &scene, Pack(e));
                if (!after) return false;
                ReadSlots(after);
                return SlotsJson().dump() != before;
            }

            void OnCreate() override { CreateManaged(0); }
            void OnScriptsReloaded() override { CreateManaged(7); }

            // Live values of the managed object's fields into the native slots (for saving across a reload).
            void OnBeforeSerialize() override
            {
                if (!m_Handle || !s_Managed.GetFields) return;
                if (const char* text = s_Managed.GetFields(m_Handle)) ReadSlots(text);
            }

            // The slots as GetFields writes the managed values (see ValuesJson).
            nlohmann::json SlotsJson() const { return ValuesJson(m_Class->Values, m_Values, true); }

            void ReadSlots(const char* text)
            {
                try
                {
                    ReadValues(m_Class->Values, m_Values, nlohmann::json::parse(text));
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
                const Layout& layout = m_Class->Values;
                nlohmann::json values;
                for (size_t i = 0; i < layout.Slots.size(); ++i)
                {
                    switch (layout.Slots[i])
                    {
                    case Slot::String: s_Managed.SetField(m_Handle, (int)i, static_cast<std::string*>(m_Values.Slot(i))->c_str()); break;
                    case Slot::List:
                    case Slot::Struct:
                    {
                        // As JSON text.
                        if (values.is_null()) values = SlotsJson();
                        s_Managed.SetField(m_Handle, (int)i, values[i].dump().c_str());
                        break;
                    }
                    case Slot::Entity:
                    {
                        uint64_t packed = Pack(*static_cast<Entity*>(m_Values.Slot(i)));
                        s_Managed.SetField(m_Handle, (int)i, &packed);
                        break;
                    }
                    default: s_Managed.SetField(m_Handle, (int)i, m_Values.Slot(i)); break;
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
            const ManagedClass* m_Class;
            Block m_Values;
            intptr_t m_Handle = 0;
        };

        void RegisterScriptClass(const char* jsonText)
        {
            try
            {
                auto j = nlohmann::json::parse(jsonText);
                ManagedClass& cls = s_Classes.emplace_back();
                cls.Name = j.at("name").get<std::string>();
                BuildLayout(j.at("fields"), cls.Values, cls);
                cls.HasGizmos = j.value("gizmos", false);
                for (auto& item : j.value("contextMenu", nlohmann::json::array())) cls.ContextMenu.push_back(item.get<std::string>());
                for (auto& f : j.at("fields")) cls.Drawn.push_back(f.value("drawer", false));

                ScriptInfo& info = ScriptRegistry::RegisterDynamic(cls.Name, "csharp");
                const ManagedClass* ptr = &cls;
                info.Create = [ptr] { return std::make_shared<ManagedScript>(ptr); };
                info.ToObject = [](ScriptableEntity* base) -> void* { return static_cast<ManagedScript*>(base)->Storage(); };
                info.Fields = cls.Values.Fields;
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
            a.EditorGui = [](int op, const char* label, float* v, int n) -> int {
                return s_EditorHooks.Gui ? s_EditorHooks.Gui(op, label ? label : "", v, n) : 0;
            };
            a.EditorGuiText = [](const char* label, const char* text) -> const char* {
                s_StringReturn = text ? text : "";
                if (s_EditorHooks.Text) s_StringReturn = s_EditorHooks.Text(label ? label : "", s_StringReturn);
                return s_StringReturn.c_str();
            };
            a.EditorSelection = [](int op, uint64_t value, const char* text) -> uint64_t {
                if (!s_EditorHooks.Selection) return 0;
                // Entities go both ways packed; the count (3) and an index (4) are plain numbers.
                bool entityIn = op == 1;
                Entity in = entityIn ? Unpack(value) : Entity{ (uint32_t)value, 0 };
                Entity out = s_EditorHooks.Selection(op, in, text ? text : "");
                return op == 3 ? (uint64_t)out.Index : Pack(out);
            };
            a.EditorUndo = [](const char* name) { if (s_EditorHooks.Undo) s_EditorHooks.Undo(name ? name : ""); };
            a.Gizmo = [](int kind, const float* data) { AddGizmo(kind, data); };
            a.EditorGuiRef = [](int kind, const char* label, uint64_t value) -> uint64_t {
                if (kind == 0) return s_EditorHooks.EntityField ? Pack(s_EditorHooks.EntityField(label ? label : "", Unpack(value))) : value;
                return s_EditorHooks.AssetField ? s_EditorHooks.AssetField(kind, label ? label : "", value) : value;
            };
            a.RegisterEditorItem = [](const char* json) {
                try
                {
                    auto j = nlohmann::json::parse(json);
                    CSharpRuntime::EditorItem item;
                    item.Id = j.value("id", -1);
                    item.Window = j.value("kind", std::string()) == "window";
                    item.MenuPath = j.value("path", std::string());
                    item.Title = j.value("title", std::string());
                    item.Inspects = j.value("inspects", std::string());
                    s_EditorItems.push_back(std::move(item));
                }
                catch (const std::exception& e)
                {
                    Log::Error("Bad editor extension registration: {}", e.what());
                }
            };
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
                case 11: return agent->Waiting ? 1.0f : 0.0f;
                case 12: std::memcpy(v, &agent->DesiredVelocity, 12); return 1.0f;
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

    void CSharpRuntime::SetEditorHooks(const EditorHooks& hooks) { s_EditorHooks = hooks; }
    const std::vector<CSharpRuntime::EditorItem>& CSharpRuntime::GetEditorItems() { return s_EditorItems; }
    void CSharpRuntime::CallEditorItem(int id, bool window, Scene& scene)
    {
        if (s_Available && s_Managed.EditorCall) s_Managed.EditorCall(window ? 1 : 0, id, &scene);
    }

    const CSharpRuntime::EditorItem* CSharpRuntime::FindInspector(const std::string& inspected)
    {
        if (inspected.empty()) return nullptr;
        for (const auto& item : s_EditorItems)
            if (item.Inspects == inspected) return &item;
        return nullptr;
    }

    bool CSharpRuntime::InspectScript(int id, ScriptableEntity& script, Scene& scene, Entity e)
    {
        auto* managed = s_Available ? dynamic_cast<ManagedScript*>(&script) : nullptr;
        return managed && managed->Inspect(id, scene, e);
    }

    void CSharpRuntime::InspectComponent(int id, Scene& scene, Entity e)
    {
        if (s_Available && s_Managed.Inspect) s_Managed.Inspect(id, 0, nullptr, nullptr, &scene, Pack(e));
    }

    bool CSharpRuntime::HasFieldDrawer(ScriptableEntity& script, size_t field)
    {
        auto* managed = s_Available ? dynamic_cast<ManagedScript*>(&script) : nullptr;
        return managed && field < managed->Class().Drawn.size() && managed->Class().Drawn[field];
    }

    std::vector<CSharpRuntime::GizmoLine> CSharpRuntime::CollectGizmos(Scene& scene, const std::vector<Entity>& selected)
    {
        std::vector<GizmoLine> lines;
        if (!s_Available || !s_Managed.DrawGizmos) return lines;
        EntityRefResolver resolver;
        resolver.FromGuid = [&scene](uint64_t g) { return scene.FindEntityByGuid(Guid(g)); };
        s_GizmoSink = &lines;
        auto& reg = scene.GetRegistry();
        for (auto [e, sc] : reg.ViewOf<ScriptComponent>())
        {
            if (auto* t = reg.TryGet<TransformComponent>(e); t && !t->ActiveInHierarchy) continue;
            bool isSelected = std::find(selected.begin(), selected.end(), e) != selected.end();
            for (ScriptInstance& script : sc.Scripts)
            {
                if (!script.Enabled) continue;
                const ScriptInfo* info = ScriptRegistry::Find(script.ClassName);
                if (!info || info->Module != "csharp") continue;
                // The live script while playing, else a stand-in holding the saved values.
                std::shared_ptr<ScriptableEntity> temp;
                ScriptableEntity* target = script.Instance.get();
                if (!target)
                {
                    temp = info->Create();
                    target = temp.get();
                    void* obj = info->ToObject(target);
                    for (auto& f : info->Fields)
                        if (script.Fields.is_object() && script.Fields.contains(f.Name)) FieldFromJson(f, obj, script.Fields[f.Name], &resolver);
                }
                if (auto* managed = dynamic_cast<ManagedScript*>(target)) managed->DrawGizmos(isSelected, scene, e);
            }
        }
        s_GizmoSink = nullptr;
        return lines;
    }

    const std::vector<std::string>& CSharpRuntime::GetContextMenu(const std::string& className)
    {
        static const std::vector<std::string> none;
        for (const ManagedClass& cls : s_Classes)
            if (cls.Name == className) return cls.ContextMenu;
        return none;
    }

    bool CSharpRuntime::RunContextMenu(ScriptableEntity& script, size_t item, Scene& scene, Entity e)
    {
        auto* managed = s_Available ? dynamic_cast<ManagedScript*>(&script) : nullptr;
        return managed && managed->RunContextMenu(item, scene, e);
    }

    std::vector<CSharpRuntime::DebugField> CSharpRuntime::GetDebugFields(ScriptableEntity& script, Scene& scene, Entity e)
    {
        auto* managed = s_Available ? dynamic_cast<ManagedScript*>(&script) : nullptr;
        return managed ? managed->DebugFields(scene, e) : std::vector<DebugField>{};
    }

    bool CSharpRuntime::DrawScriptField(ScriptableEntity& script, size_t field, Scene& scene, Entity e)
    {
        auto* managed = s_Available ? dynamic_cast<ManagedScript*>(&script) : nullptr;
        return managed && managed->DrawField(field, scene, e);
    }

    void CSharpRuntime::UnloadScripts()
    {
        if (!s_Available) return;
        s_EditorItems.clear();
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
    void CSharpRuntime::SetEditorHooks(const EditorHooks&) {}
    const std::vector<CSharpRuntime::EditorItem>& CSharpRuntime::GetEditorItems() { static std::vector<EditorItem> none; return none; }
    void CSharpRuntime::CallEditorItem(int, bool, Scene&) {}
    const CSharpRuntime::EditorItem* CSharpRuntime::FindInspector(const std::string&) { return nullptr; }
    bool CSharpRuntime::InspectScript(int, ScriptableEntity&, Scene&, Entity) { return false; }
    void CSharpRuntime::InspectComponent(int, Scene&, Entity) {}
    bool CSharpRuntime::HasFieldDrawer(ScriptableEntity&, size_t) { return false; }
    bool CSharpRuntime::DrawScriptField(ScriptableEntity&, size_t, Scene&, Entity) { return false; }
    std::vector<CSharpRuntime::DebugField> CSharpRuntime::GetDebugFields(ScriptableEntity&, Scene&, Entity) { return {}; }
    std::vector<CSharpRuntime::GizmoLine> CSharpRuntime::CollectGizmos(Scene&, const std::vector<Entity>&) { return {}; }
    const std::vector<std::string>& CSharpRuntime::GetContextMenu(const std::string&) { static const std::vector<std::string> none; return none; }
    bool CSharpRuntime::RunContextMenu(ScriptableEntity&, size_t, Scene&, Entity) { return false; }
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
