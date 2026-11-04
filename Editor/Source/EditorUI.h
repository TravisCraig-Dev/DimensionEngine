#pragma once
#include <Dimension/Reflection/TypeInfo.h>
#include <Dimension/Asset/AssetManager.h>
#include <Dimension/Scene/Scene.h>
#include <glm/glm.hpp>
#include <imgui.h>
#include <nlohmann/json.hpp>

namespace Editor::UI
{
    using namespace Dimension;

    constexpr const char* kEntityPayload = "DIM_ENTITY"; // payload: uint64 Guid
    constexpr const char* kAssetPayload = "DIM_ASSET";   // payload: uint64 AssetHandle
    constexpr const char* kPathPayload = "DIM_PATH";     // payload: UTF-8 absolute path (content browser moves)

    struct EditResult
    {
        bool Changed = false;
        bool Activated = false; // user started interacting (record undo here)
        EditResult& operator|=(const EditResult& o)
        {
            Changed |= o.Changed;
            Activated |= o.Activated;
            return *this;
        }
    };

    // Two-column property grid.
    bool BeginProperties(const char* id);
    void EndProperties();
    void PropertyLabel(const char* label, const char* tooltip = nullptr);

    EditResult Field(const FieldInfo& field, void* object, Scene* scene);
    // Multi-object editing: marks the next Field() as differing across the selection (bit i = vector component i or
    // list element i, bit 0 for scalars). Differing values show as "--"; the mask is cleared after that Field().
    void SetMixedValue(uint32_t mask);
    // Mask of differing components between two serialized field values. Vectors compare per component and lists
    // of equal length per element (elements 31 and up share bit 31); lists of different lengths give kMixedAll.
    constexpr uint32_t kMixedAll = 0xFFFFFFFFu;
    uint32_t MixedMask(FieldType type, const nlohmann::json& a, const nlohmann::json& b);
    bool IsVectorField(FieldType type);
    bool IsListField(FieldType type);
    EditResult Vec3Control(const char* label, glm::vec3& value, float resetValue = 0.0f, float speed = 0.05f);
    EditResult AssetField(const char* label, AssetHandle& handle, AssetType type);
    // Only the assets `accept` takes (type label in the button: "Enemy Stats" for data assets of a class).
    EditResult AssetField(const char* label, AssetHandle& handle, AssetType type, const std::function<bool(AssetHandle)>& accept,
                          const std::string& typeLabel);
    // Theme-aware colors: a background tinted toward the accent (amount 0-1), and accent-colored text that stays
    // readable on light and dark themes (prefab overrides, highlights).
    ImVec4 AccentBackground(float amount = 0.3f);
    ImVec4 AccentText();
    EditResult EntityField(const char* label, Entity& entity, Scene& scene);
    // Only the objects `accept` takes (type label in the button: "None (Health)" for a script's field).
    EditResult EntityField(const char* label, Entity& entity, Scene& scene, const std::function<bool(Entity)>& accept,
                           const std::string& typeLabel);
    // Inline editors: drag keys, double-click to add, right-click a key to remove.
    EditResult CurveEditor(const char* id, Curve& curve, float height = 64.0f);
    EditResult GradientEditor(const char* id, Gradient& gradient);

    // Asset type color/letter used for thumbnails and slots.
    ImVec4 AssetColor(AssetType type);
    const char* AssetLetter(AssetType type);

    void HelpMarker(const char* text);
}
