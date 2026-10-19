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
        Texture, Mesh, Scene, Prefab, Audio, Material, Font, AnimatorController, // AssetHandle referencing an asset of that type
        EntityRef,        // Entity, serialized as the target's Guid
        Curve,            // Dimension::Curve (normalized time -> value)
        Gradient,         // Dimension::Gradient (normalized time -> color)
        Vec3List,         // std::vector<glm::vec3>
        StringList,       // std::vector<std::string>
    };

    struct FieldInfo
    {
        std::string Name;
        FieldType Type = FieldType::Float;
        std::function<void*(void*)> Access; // object pointer -> field pointer
        float Speed = 0.05f;
        float Min = 0.0f, Max = 0.0f; // Min == Max means unbounded
        std::vector<std::string> EnumNames;
        std::string Tooltip;
        bool Hidden = false; // serialized but not shown in the Inspector
    };

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
}
