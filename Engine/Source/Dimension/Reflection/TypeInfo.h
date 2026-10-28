#pragma once
#include "Dimension/Core/Guid.h"
#include "Dimension/ECS/Entity.h"
#include "Dimension/Core/Curve.h"
#include <glm/glm.hpp>
#include <nlohmann/json_fwd.hpp>
#include <functional>
#include <string>
#include <type_traits>
#include <vector>

namespace Dimension
{
    // Field kinds understood by the inspector and serializer.
    enum class FieldType
    {
        Bool, Int, Float, Vec2, Vec3, Vec4,
        Color3, Color4,   // vec3/vec4 edited with a color picker (stored in sRGB)
        Angles,           // vec3 radians, shown in degrees
        String,
        Enum,             // int32-backed enum with display names
        Texture, Mesh, Scene, Prefab, Audio, Material, Font, AnimatorController, Timeline, // AssetHandle referencing an asset of that type
        EntityRef,        // Entity, serialized as the target's Guid
        Curve,            // Dimension::Curve (normalized time -> value)
        Gradient,         // Dimension::Gradient (normalized time -> color)
        Vec3List,         // std::vector<glm::vec3>
        StringList,       // std::vector<std::string>
        MaterialList,     // std::vector<AssetHandle> of materials (per-slot overrides)
        StructList,       // std::vector of a struct whose fields are Elements (FieldListBuilder::List)
        Struct,           // a nested struct: Elements are its fields (Access relative to the field's address)
    };

    struct FieldInfo
    {
        std::string Name;
        FieldType Type = FieldType::Float;
        std::function<void*(void*)> Access; // object pointer -> field pointer
        float Speed = 0.05f;
        float Min = 0.0f, Max = 0.0f; // Min == Max means unbounded
        std::vector<std::string> EnumNames;
        std::vector<int32_t> EnumValues; // the stored value of each name (empty: its index)
        bool EnumFlags = false;          // the value combines names' bit values ([Flags]): a multi-select dropdown
        std::string Tooltip;
        std::string Header;  // heading drawn above the field in the Inspector
        bool Hidden = false; // serialized but not shown in the Inspector
        // StructList: the element struct's fields (relative to an element), the list's operations, and the element
        // field whose text titles each element in the Inspector ("" = numbered). Max (when above 0) caps the length.
        // A single unnamed element field makes a list of plain values: saved as an array of them, each drawn in one
        // row (see IsValueList).
        std::vector<FieldInfo> Elements;
        std::function<size_t(const void*)> ListSize;
        std::function<void*(void*, size_t)> ListAt;
        std::function<void(void*, size_t)> ListResize;
        std::function<void(void*, size_t)> ListErase;
        std::function<void(void*, size_t, size_t)> ListMove; // an element from one place to another
        std::string ElementTitle;
    };

    inline bool IsValueList(const FieldInfo& f) { return f.Type == FieldType::StructList && f.Elements.size() == 1 && f.Elements[0].Name.empty(); }

    template <class M> constexpr FieldType DeduceFieldType()
    {
        if constexpr (std::is_same_v<M, bool>) return FieldType::Bool;
        else if constexpr (std::is_enum_v<M>) return FieldType::Enum;
        else if constexpr (std::is_integral_v<M>) return FieldType::Int;
        else if constexpr (std::is_same_v<M, float>) return FieldType::Float;
        else if constexpr (std::is_same_v<M, glm::vec2>) return FieldType::Vec2;
        else if constexpr (std::is_same_v<M, glm::vec3>) return FieldType::Vec3;
        else if constexpr (std::is_same_v<M, glm::vec4>) return FieldType::Vec4;
        else if constexpr (std::is_same_v<M, std::string>) return FieldType::String;
        else if constexpr (std::is_same_v<M, Entity>) return FieldType::EntityRef;
        else if constexpr (std::is_same_v<M, Curve>) return FieldType::Curve;
        else if constexpr (std::is_same_v<M, Gradient>) return FieldType::Gradient;
        else if constexpr (std::is_same_v<M, std::vector<glm::vec3>>) return FieldType::Vec3List;
        else if constexpr (std::is_same_v<M, std::vector<std::string>>) return FieldType::StringList;
        else if constexpr (std::is_same_v<M, std::vector<Guid>>) return FieldType::MaterialList;
        else if constexpr (std::is_same_v<M, Guid>) return FieldType::Texture; // override with .As()
        else static_assert(sizeof(M) == 0, "Unsupported reflected field type");
    }

    // Fluent field declaration helper:
    //   FieldListBuilder<Camera>(fields).Field("FOV", &Camera::FOV).Range(1, 179)
    template <class T>
    class FieldListBuilder
    {
    public:
        explicit FieldListBuilder(std::vector<FieldInfo>& fields) : m_Fields(fields) {}

        template <class M>
        FieldListBuilder& Field(std::string name, M T::*member)
        {
            static_assert(!std::is_enum_v<M> || sizeof(M) == sizeof(int32_t), "Reflected enums must be 32-bit");
            FieldInfo f;
            f.Name = std::move(name);
            f.Type = DeduceFieldType<M>();
            f.Access = [member](void* obj) -> void* { return &(static_cast<T*>(obj)->*member); };
            if constexpr (std::is_integral_v<M>) f.Speed = 1.0f;
            m_Fields.push_back(std::move(f));
            return *this;
        }

        // A list of structs: `elements` declares the element type's fields (as for a component).
        template <class E>
        FieldListBuilder& List(std::string name, std::vector<E> T::*member, const std::type_identity_t<std::function<void(FieldListBuilder<E>&)>>& elements)
        {
            FieldInfo f;
            f.Name = std::move(name);
            f.Type = FieldType::StructList;
            f.Access = [member](void* obj) -> void* { return &(static_cast<T*>(obj)->*member); };
            FieldListBuilder<E> inner(f.Elements);
            elements(inner);
            f.ListSize = [](const void* list) { return static_cast<const std::vector<E>*>(list)->size(); };
            f.ListAt = [](void* list, size_t i) -> void* { return &(*static_cast<std::vector<E>*>(list))[i]; };
            f.ListResize = [](void* list, size_t n) { static_cast<std::vector<E>*>(list)->resize(n); };
            f.ListErase = [](void* list, size_t i) {
                auto& v = *static_cast<std::vector<E>*>(list);
                if (i < v.size()) v.erase(v.begin() + i);
            };
            f.ListMove = [](void* list, size_t from, size_t to) {
                auto& v = *static_cast<std::vector<E>*>(list);
                if (from >= v.size() || to >= v.size() || from == to) return;
                E e = std::move(v[from]);
                v.erase(v.begin() + from);
                v.insert(v.begin() + to, std::move(e));
            };
            m_Fields.push_back(std::move(f));
            return *this;
        }
        FieldListBuilder& ElementTitle(std::string field) { m_Fields.back().ElementTitle = std::move(field); return *this; }

        FieldListBuilder& As(FieldType type) { m_Fields.back().Type = type; return *this; }
        FieldListBuilder& Range(float min, float max) { m_Fields.back().Min = min; m_Fields.back().Max = max; return *this; }
        FieldListBuilder& Speed(float speed) { m_Fields.back().Speed = speed; return *this; }
        FieldListBuilder& Enum(std::vector<std::string> names)
        {
            auto& f = m_Fields.back();
            f.EnumNames = std::move(names);
            if (f.Type == FieldType::Int) f.Type = FieldType::Enum; // plain int32 fields with names become a dropdown too
            return *this;
        }
        FieldListBuilder& Tooltip(std::string text) { m_Fields.back().Tooltip = std::move(text); return *this; }
        FieldListBuilder& HideInInspector() { m_Fields.back().Hidden = true; return *this; }
        FieldListBuilder& Header(std::string text) { m_Fields.back().Header = std::move(text); return *this; }

    private:
        std::vector<FieldInfo>& m_Fields;
    };

    // Maps Entity <-> Guid while (de)serializing EntityRef fields.
    struct EntityRefResolver
    {
        std::function<uint64_t(Entity)> ToGuid;
        std::function<Entity(uint64_t)> FromGuid;
    };

    nlohmann::json FieldToJson(const FieldInfo& field, const void* object, const EntityRefResolver* resolver);
    void FieldFromJson(const FieldInfo& field, void* object, const nlohmann::json& value, const EntityRefResolver* resolver);

    // Numeric fields as floats (scripting, timelines): bools 0/1, ints and enums by value, vectors and colors by
    // component. FloatCount is 0 for other types (strings, assets, curves, ...). `value` points at the field itself.
    int FloatCount(FieldType type);
    int FieldToFloats(const FieldInfo& field, const void* value, float* out); // returns the count written
    bool FieldFromFloats(const FieldInfo& field, void* value, const float* in, int count); // clamps to the field's range
}
