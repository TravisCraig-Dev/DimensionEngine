#pragma once
#include <Dimension/Reflection/TypeInfo.h>
#include <Dimension/Asset/AssetManager.h>
#include <Dimension/Scene/Scene.h>
#include <glm/glm.hpp>
#include <imgui.h>

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
    EditResult Vec3Control(const char* label, glm::vec3& value, float resetValue = 0.0f, float speed = 0.05f);
    EditResult AssetField(const char* label, AssetHandle& handle, AssetType type);
    EditResult EntityField(const char* label, Entity& entity, Scene& scene);
    // Inline editors: drag keys, double-click to add, right-click a key to remove.
    EditResult CurveEditor(const char* id, Curve& curve, float height = 64.0f);
    EditResult GradientEditor(const char* id, Gradient& gradient);

    // Asset type color/letter used for thumbnails and slots.
    ImVec4 AssetColor(AssetType type);
    const char* AssetLetter(AssetType type);

    void HelpMarker(const char* text);
}
