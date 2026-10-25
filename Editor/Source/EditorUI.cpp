#include "EditorUI.h"
#include <Dimension/Renderer/Texture.h>
#include <Dimension/ImGui/ImGuiLayer.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>
#include <glm/gtc/type_ptr.hpp>

namespace Editor::UI
{
    bool BeginProperties(const char* id)
    {
        if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_NoSavedSettings)) return false;
        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthStretch, 0.38f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 0.62f);
        return true;
    }

    void EndProperties() { ImGui::EndTable(); }

    void PropertyLabel(const char* label, const char* tooltip)
    {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        if (tooltip && *tooltip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tooltip);
        ImGui::TableSetColumnIndex(1);
        ImGui::SetNextItemWidth(-FLT_MIN);
    }

    static EditResult Track(bool changed)
    {
        return { changed, ImGui::IsItemActivated() };
    }

    static uint32_t s_Mixed = 0; // see SetMixedValue
    constexpr const char* kMixedText = "--";

    void SetMixedValue(uint32_t mask) { s_Mixed = mask; }

    bool IsVectorField(FieldType type)
    {
        switch (type)
        {
        case FieldType::Vec2:
        case FieldType::Vec3:
        case FieldType::Vec4:
        case FieldType::Color3:
        case FieldType::Color4:
        case FieldType::Angles: return true;
        default: return false;
        }
    }

    uint32_t MixedMask(FieldType type, const nlohmann::json& a, const nlohmann::json& b)
    {
        if (IsVectorField(type) && a.is_array() && b.is_array() && a.size() == b.size())
        {
            uint32_t mask = 0;
            for (size_t i = 0; i < a.size() && i < 32; ++i)
                if (a[i] != b[i]) mask |= 1u << i;
            return mask;
        }
        return a == b ? 0u : 1u;
    }

    EditResult Vec3Control(const char* label, glm::vec3& v, float reset, float speed)
    {
        EditResult r;
        PropertyLabel(label);
        ImGui::PushID(label);
        float full = ImGui::GetContentRegionAvail().x;
        float lineH = ImGui::GetFrameHeight();
        float itemW = (full - lineH * 3.0f - ImGui::GetStyle().ItemSpacing.x * 2.0f) / 3.0f;
        const ImVec4 colors[3] = { { 0.75f, 0.2f, 0.2f, 1 }, { 0.25f, 0.6f, 0.25f, 1 }, { 0.2f, 0.35f, 0.8f, 1 } };
        const char* names[3] = { "X", "Y", "Z" };
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
        for (int i = 0; i < 3; ++i)
        {
            if (i > 0) ImGui::SameLine(0, ImGui::GetStyle().FramePadding.x);
            ImGui::PushStyleColor(ImGuiCol_Button, colors[i]);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(colors[i].x + 0.1f, colors[i].y + 0.1f, colors[i].z + 0.1f, 1));
            if (ImGui::Button(names[i], ImVec2(lineH, lineH)))
            {
                r.Activated = true; // treat reset as an edit for undo purposes
                v[i] = reset;
                r.Changed = true;
            }
            ImGui::PopStyleColor(2);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(itemW);
            ImGui::PushID(i);
            r |= Track(ImGui::DragFloat("##v", &v[i], speed, 0.0f, 0.0f, (s_Mixed >> i) & 1u ? kMixedText : "%.3f"));
            ImGui::PopID();
        }
        ImGui::PopStyleVar();
        ImGui::PopID();
        return r;
    }

    ImVec4 AssetColor(AssetType type)
    {
        switch (type)
        {
        case AssetType::Texture: return { 0.85f, 0.55f, 0.25f, 1 };
        case AssetType::Mesh: return { 0.35f, 0.6f, 0.9f, 1 };
        case AssetType::Scene: return { 0.5f, 0.8f, 0.4f, 1 };
        case AssetType::Prefab: return { 0.35f, 0.75f, 0.95f, 1 };
        case AssetType::Audio: return { 0.85f, 0.4f, 0.75f, 1 };
        case AssetType::Material: return { 0.95f, 0.85f, 0.4f, 1 };
        case AssetType::Font: return { 0.75f, 0.75f, 0.95f, 1 };
        case AssetType::AnimatorController: return { 0.55f, 0.9f, 0.7f, 1 };
        default: return { 0.5f, 0.5f, 0.5f, 1 };
        }
    }

    const char* AssetLetter(AssetType type)
    {
        switch (type)
        {
        case AssetType::Texture: return "IMG";
        case AssetType::Mesh: return "MESH";
        case AssetType::Scene: return "SCENE";
        case AssetType::Prefab: return "PREFAB";
        case AssetType::Audio: return "AUDIO";
        case AssetType::Material: return "MAT";
        case AssetType::Font: return "FONT";
        case AssetType::AnimatorController: return "ANIM";
        default: return "FILE";
        }
    }

    EditResult AssetField(const char* label, AssetHandle& handle, AssetType type)
    {
        EditResult r;
        PropertyLabel(label);
        ImGui::PushID(label);
        std::string name = s_Mixed ? std::string(kMixedText) : AssetManager::GetDisplayName(handle);
        float h = ImGui::GetFrameHeight();

        if (type == AssetType::Texture)
        {
            if (auto tex = AssetManager::GetTexture(handle))
            {
                ImGui::Image(ImTextureRef((ImTextureID)tex->GetImGuiId()), ImVec2(h, h));
                ImGui::SameLine(0, 4);
            }
        }
        float width = ImGui::GetContentRegionAvail().x;
        std::string text = name + "  (" + AssetTypeToString(type) + ")";
        ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));
        if (ImGui::Button(text.c_str(), ImVec2(width, 0))) ImGui::OpenPopup("picker");
        ImGui::PopStyleVar();
        if (ImGui::IsItemHovered() && handle.IsValid()) ImGui::SetTooltip("%s", AssetManager::GetAbsolutePath(handle).string().c_str());

        if (ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kAssetPayload))
            {
                AssetHandle dropped(*(const uint64_t*)p->Data);
                auto* meta = AssetManager::GetMetadata(dropped);
                if (meta && meta->Type == type)
                {
                    r.Activated = r.Changed = true;
                    handle = dropped;
                }
            }
            ImGui::EndDragDropTarget();
        }

        if (ImGui::BeginPopup("picker"))
        {
            static std::string filter;
            if (ImGui::IsWindowAppearing()) { filter.clear(); ImGui::SetKeyboardFocusHere(); }
            ImGui::SetNextItemWidth(220);
            ImGui::InputTextWithHint("##filter", "Search...", &filter);
            auto option = [&](AssetHandle h, const std::string& n) {
                if (!filter.empty())
                {
                    std::string a = n, b = filter;
                    for (auto& c : a) c = (char)std::tolower(c);
                    for (auto& c : b) c = (char)std::tolower(c);
                    if (a.find(b) == std::string::npos) return;
                }
                if (ImGui::Selectable(n.c_str(), h == handle))
                {
                    r.Activated = r.Changed = true;
                    handle = h;
                }
            };
            option(AssetHandle::None(), type == AssetType::Font ? "Default (built-in)" : "None");
            if (type == AssetType::Mesh)
                for (AssetHandle b : { AssetManager::kCube, AssetManager::kSphere, AssetManager::kPlane, AssetManager::kQuad, AssetManager::kCylinder })
                    option(b, AssetManager::GetDisplayName(b));
            for (AssetHandle a : AssetManager::GetAssetsOfType(type))
                option(a, AssetManager::GetMetadata(a)->Path.generic_string());
            ImGui::EndPopup();
        }
        ImGui::PopID();
        return r;
    }

    EditResult EntityField(const char* label, Entity& entity, Scene& scene)
    {
        EditResult r;
        PropertyLabel(label);
        ImGui::PushID(label);
        std::string name = s_Mixed ? std::string(kMixedText) : scene.IsValid(entity) ? scene.GetRegistry().Get<TagComponent>(entity).Name : "None (Entity)";
        ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));
        if (ImGui::Button(name.c_str(), ImVec2(-FLT_MIN, 0))) ImGui::OpenPopup("picker");
        ImGui::PopStyleVar();
        if (ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kEntityPayload))
            {
                Entity e = scene.FindEntityByGuid(Guid(*(const uint64_t*)p->Data));
                if (!e.IsNull())
                {
                    r.Activated = r.Changed = true;
                    entity = e;
                }
            }
            ImGui::EndDragDropTarget();
        }
        if (ImGui::BeginPopup("picker"))
        {
            if (ImGui::Selectable("None", entity.IsNull()))
            {
                r.Activated = r.Changed = true;
                entity = Entity::Null();
            }
            scene.GetRegistry().EachEntity([&](Entity e) {
                ImGui::PushID((int)e.Index);
                if (ImGui::Selectable(scene.GetRegistry().Get<TagComponent>(e).Name.c_str(), e == entity))
                {
                    r.Activated = r.Changed = true;
                    entity = e;
                }
                ImGui::PopID();
            });
            ImGui::EndPopup();
        }
        ImGui::PopID();
        return r;
    }

    EditResult CurveEditor(const char* id, Curve& curve, float height)
    {
        EditResult r;
        ImGui::PushID(id);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 size(ImGui::GetContentRegionAvail().x, height);
        ImVec2 p0 = ImGui::GetCursorScreenPos(), p1(p0.x + size.x, p0.y + size.y);
        ImGui::InvisibleButton("##canvas", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
        bool canvasHovered = ImGui::IsItemHovered();

        // Vertical range covers 0-1 and every key (with headroom), so values above 1 stay editable.
        float lo = 0.0f, hi = 1.0f;
        for (auto& k : curve.Keys) { lo = std::min(lo, k.Value); hi = std::max(hi, k.Value); }
        hi += (hi - lo) * 0.1f;
        auto toScreen = [&](float t, float v) { return ImVec2(p0.x + t * size.x, p1.y - (v - lo) / (hi - lo) * size.y); };
        auto fromScreen = [&](ImVec2 s) {
            return glm::vec2(std::clamp((s.x - p0.x) / size.x, 0.0f, 1.0f), lo + (p1.y - s.y) / size.y * (hi - lo));
        };

        dl->AddRectFilled(p0, p1, IM_COL32(28, 28, 32, 255), 3.0f);
        for (int i = 1; i < 4; ++i)
            dl->AddLine(ImVec2(p0.x + size.x * i / 4, p0.y), ImVec2(p0.x + size.x * i / 4, p1.y), IM_COL32(50, 50, 56, 255));
        if (lo < 0.0f) dl->AddLine(toScreen(0, 0), toScreen(1, 0), IM_COL32(70, 70, 80, 255));
        dl->AddLine(toScreen(0, 1), toScreen(1, 1), IM_COL32(60, 60, 70, 255));
        const int samples = 48;
        for (int i = 0; i < samples; ++i)
        {
            float a = (float)i / samples, b = (float)(i + 1) / samples;
            dl->AddLine(toScreen(a, curve.Evaluate(a)), toScreen(b, curve.Evaluate(b)), IM_COL32(110, 200, 255, 255), 2.0f);
        }

        ImGuiStorage* storage = ImGui::GetStateStorage();
        ImGuiID dragId = ImGui::GetID("##dragkey");
        int dragging = storage->GetInt(dragId, -1);
        int hoveredKey = -1;
        ImVec2 mouse = ImGui::GetIO().MousePos;
        for (int i = 0; i < (int)curve.Keys.size(); ++i)
        {
            ImVec2 kp = toScreen(curve.Keys[i].Time, curve.Keys[i].Value);
            bool hot = std::abs(mouse.x - kp.x) < 6 && std::abs(mouse.y - kp.y) < 6 && canvasHovered;
            if (hot) hoveredKey = i;
            dl->AddCircleFilled(kp, hot || i == dragging ? 5.0f : 4.0f, hot || i == dragging ? IM_COL32(255, 255, 255, 255) : IM_COL32(200, 230, 255, 255));
        }
        if (hoveredKey >= 0)
            ImGui::SetTooltip("t %.2f   value %.3f", curve.Keys[hoveredKey].Time, curve.Keys[hoveredKey].Value);

        if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && hoveredKey >= 0)
        {
            storage->SetInt(dragId, hoveredKey);
            r.Activated = true;
        }
        dragging = storage->GetInt(dragId, -1);
        if (dragging >= 0 && dragging < (int)curve.Keys.size())
        {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
            {
                glm::vec2 v = fromScreen(mouse);
                // Keys keep their order: clamp between neighbours.
                float minT = dragging > 0 ? curve.Keys[dragging - 1].Time : 0.0f;
                float maxT = dragging + 1 < (int)curve.Keys.size() ? curve.Keys[dragging + 1].Time : 1.0f;
                CurveKey k{ std::clamp(v.x, minT, maxT), v.y };
                if (k.Time != curve.Keys[dragging].Time || k.Value != curve.Keys[dragging].Value)
                {
                    curve.Keys[dragging] = k;
                    r.Changed = true;
                }
            }
            else
                storage->SetInt(dragId, -1);
        }
        if (canvasHovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && hoveredKey < 0)
        {
            glm::vec2 v = fromScreen(mouse);
            curve.Keys.push_back({ v.x, v.y });
            curve.Sort();
            r.Activated = r.Changed = true;
        }
        if (hoveredKey >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && curve.Keys.size() > 1)
        {
            curve.Keys.erase(curve.Keys.begin() + hoveredKey);
            r.Activated = r.Changed = true;
        }
        ImGui::PopID();
        return r;
    }

    EditResult GradientEditor(const char* id, Gradient& gradient)
    {
        EditResult r;
        ImGui::PushID(id);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        float width = ImGui::GetContentRegionAvail().x, barH = ImGui::GetFrameHeight(), markerH = 10.0f;
        ImVec2 p0 = ImGui::GetCursorScreenPos(), p1(p0.x + width, p0.y + barH);
        ImGui::InvisibleButton("##bar", ImVec2(width, barH + markerH), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
        bool hovered = ImGui::IsItemHovered();

        // Checkerboard behind, so alpha is visible.
        const float cell = barH * 0.5f;
        for (float x = p0.x; x < p1.x; x += cell)
            for (int row = 0; row < 2; ++row)
            {
                bool dark = ((int)((x - p0.x) / cell) + row) % 2 == 0;
                dl->AddRectFilled(ImVec2(x, p0.y + row * cell), ImVec2(std::min(x + cell, p1.x), p0.y + (row + 1) * cell),
                                  dark ? IM_COL32(90, 90, 90, 255) : IM_COL32(150, 150, 150, 255));
            }
        const int steps = 32;
        for (int i = 0; i < steps; ++i)
        {
            float a = (float)i / steps, b = (float)(i + 1) / steps;
            ImU32 ca = ImGui::ColorConvertFloat4ToU32(ImVec4(gradient.Evaluate(a).r, gradient.Evaluate(a).g, gradient.Evaluate(a).b, gradient.Evaluate(a).a));
            ImU32 cb = ImGui::ColorConvertFloat4ToU32(ImVec4(gradient.Evaluate(b).r, gradient.Evaluate(b).g, gradient.Evaluate(b).b, gradient.Evaluate(b).a));
            dl->AddRectFilledMultiColor(ImVec2(p0.x + a * width, p0.y), ImVec2(p0.x + b * width + 0.5f, p1.y), ca, cb, cb, ca);
        }
        dl->AddRect(p0, p1, IM_COL32(20, 20, 20, 255));

        ImGuiStorage* storage = ImGui::GetStateStorage();
        ImGuiID dragId = ImGui::GetID("##dragkey"), editId = ImGui::GetID("##editkey");
        ImVec2 mouse = ImGui::GetIO().MousePos;
        int hoveredKey = -1;
        for (int i = 0; i < (int)gradient.Keys.size(); ++i)
        {
            float x = p0.x + gradient.Keys[i].Time * width;
            bool hot = hovered && std::abs(mouse.x - x) < 6 && mouse.y >= p1.y - 2;
            if (hot) hoveredKey = i;
            ImVec2 a(x, p1.y), b(x - 5, p1.y + markerH), c(x + 5, p1.y + markerH);
            glm::vec4 col = gradient.Keys[i].Color;
            dl->AddTriangleFilled(a, b, c, ImGui::ColorConvertFloat4ToU32(ImVec4(col.r, col.g, col.b, 1.0f)));
            dl->AddTriangle(a, b, c, hot ? IM_COL32(255, 255, 255, 255) : IM_COL32(20, 20, 20, 255), hot ? 2.0f : 1.0f);
        }
        if (hoveredKey >= 0) ImGui::SetTooltip("t %.2f (double-click: color, right-click: remove)", gradient.Keys[hoveredKey].Time);

        if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && hoveredKey >= 0)
        {
            storage->SetInt(dragId, hoveredKey);
            r.Activated = true;
        }
        int dragging = storage->GetInt(dragId, -1);
        if (dragging >= 0 && dragging < (int)gradient.Keys.size())
        {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
            {
                float minT = dragging > 0 ? gradient.Keys[dragging - 1].Time : 0.0f;
                float maxT = dragging + 1 < (int)gradient.Keys.size() ? gradient.Keys[dragging + 1].Time : 1.0f;
                float t = std::clamp((mouse.x - p0.x) / width, minT, maxT);
                if (t != gradient.Keys[dragging].Time)
                {
                    gradient.Keys[dragging].Time = t;
                    r.Changed = true;
                }
            }
            else
                storage->SetInt(dragId, -1);
        }
        if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        {
            if (hoveredKey >= 0)
            {
                storage->SetInt(editId, hoveredKey);
                ImGui::OpenPopup("##keycolor");
            }
            else
            {
                float t = std::clamp((mouse.x - p0.x) / width, 0.0f, 1.0f);
                gradient.Keys.push_back({ t, gradient.Evaluate(t) });
                gradient.Sort();
                r.Activated = r.Changed = true;
            }
        }
        if (hoveredKey >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && gradient.Keys.size() > 1)
        {
            gradient.Keys.erase(gradient.Keys.begin() + hoveredKey);
            r.Activated = r.Changed = true;
        }
        if (ImGui::BeginPopup("##keycolor"))
        {
            int k = storage->GetInt(editId, -1);
            if (k >= 0 && k < (int)gradient.Keys.size())
            {
                if (ImGui::IsWindowAppearing()) r.Activated = true;
                if (ImGui::ColorPicker4("##picker", &gradient.Keys[k].Color.x, ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf))
                    r.Changed = true;
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
        return r;
    }

    ImVec4 AccentBackground(float amount)
    {
        ImVec4 bg = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg), accent = ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive);
        return ImVec4(bg.x + (accent.x - bg.x) * amount, bg.y + (accent.y - bg.y) * amount, bg.z + (accent.z - bg.z) * amount, 1.0f);
    }

    ImVec4 AccentText()
    {
        ImVec4 a = ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive);
        // Dark themes: lift the accent toward white; light theme: darken it.
        float k = ImGuiLayer::IsLightTheme() ? 0.6f : 0.45f;
        return ImGuiLayer::IsLightTheme() ? ImVec4(a.x * k, a.y * k, a.z * k, 1.0f)
                                          : ImVec4(a.x + (1 - a.x) * k, a.y + (1 - a.y) * k, a.z + (1 - a.z) * k, 1.0f);
    }

    EditResult Field(const FieldInfo& f, void* object, Scene* scene)
    {
        void* p = f.Access(object);
        const char* label = f.Name.c_str();
        ImGui::PushID(label);
        EditResult r;
        bool bounded = f.Min != f.Max;
        bool mixed = s_Mixed != 0;
        const char* floatFormat = mixed ? kMixedText : "%.3f";
        // Whole-value fields (curves, gradients, lists) that differ across the selection: say so above the editor,
        // which shows the active object's value; any edit gives every selected object that value.
        auto mixedNote = [&] {
            if (!mixed) return;
            ImGui::TextDisabled("%s  differs between objects", kMixedText);
            ImGui::SetItemTooltip("Showing the active object's value. Editing it applies the result to every selected object.");
        };
        switch (f.Type)
        {
        case FieldType::Bool:
            PropertyLabel(label, f.Tooltip.c_str());
            ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, mixed); // draws a dash in the box
            r = Track(ImGui::Checkbox("##v", (bool*)p));
            ImGui::PopItemFlag();
            break;
        case FieldType::Int:
            PropertyLabel(label, f.Tooltip.c_str());
            r = Track(ImGui::DragInt("##v", (int*)p, f.Speed, (int)f.Min, (int)f.Max, mixed ? kMixedText : "%d"));
            break;
        case FieldType::Float:
            PropertyLabel(label, f.Tooltip.c_str());
            r = Track(ImGui::DragFloat("##v", (float*)p, f.Speed, f.Min, f.Max, floatFormat, bounded ? ImGuiSliderFlags_AlwaysClamp : 0));
            break;
        case FieldType::Vec2:
            PropertyLabel(label, f.Tooltip.c_str());
            r = Track(ImGui::DragFloat2("##v", (float*)p, f.Speed, f.Min, f.Max, floatFormat));
            break;
        case FieldType::Vec3:
            r = Vec3Control(label, *(glm::vec3*)p, f.Name == "Scale" || f.Name == "Size" ? 1.0f : 0.0f, f.Speed);
            break;
        case FieldType::Vec4:
            PropertyLabel(label, f.Tooltip.c_str());
            r = Track(ImGui::DragFloat4("##v", (float*)p, f.Speed, f.Min, f.Max, floatFormat));
            break;
        case FieldType::Angles:
        {
            glm::vec3 degrees = glm::degrees(*(glm::vec3*)p);
            r = Vec3Control(label, degrees, 0.0f, f.Speed);
            if (r.Changed) *(glm::vec3*)p = glm::radians(degrees);
            break;
        }
        case FieldType::Color3:
        case FieldType::Color4:
        {
            PropertyLabel(label, f.Tooltip.c_str());
            bool alpha = f.Type == FieldType::Color4;
            ImGuiColorEditFlags flags = ImGuiColorEditFlags_Float | ImGuiColorEditFlags_AlphaPreviewHalf;
            if (mixed) flags |= ImGuiColorEditFlags_NoInputs; // swatch of the active object, then "--"
            r = Track(alpha ? ImGui::ColorEdit4("##v", (float*)p, flags) : ImGui::ColorEdit3("##v", (float*)p, flags));
            if (mixed)
            {
                ImGui::SameLine();
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("%s", kMixedText);
            }
            break;
        }
        case FieldType::String:
            PropertyLabel(label, f.Tooltip.c_str());
            if (mixed)
            {
                // Empty box with a "--" hint; typing replaces the value on every selected object.
                static std::string typed;
                typed.clear();
                r = Track(ImGui::InputTextWithHint("##v", kMixedText, &typed));
                if (r.Changed) *(std::string*)p = typed;
            }
            else
                r = Track(ImGui::InputText("##v", (std::string*)p));
            break;
        case FieldType::Enum:
        {
            PropertyLabel(label, f.Tooltip.c_str());
            int32_t* value = (int32_t*)p;
            const char* preview = mixed ? kMixedText : (*value >= 0 && *value < (int)f.EnumNames.size()) ? f.EnumNames[*value].c_str() : "?";
            if (ImGui::BeginCombo("##v", preview))
            {
                for (int i = 0; i < (int)f.EnumNames.size(); ++i)
                    if (ImGui::Selectable(f.EnumNames[i].c_str(), i == *value) && i != *value)
                    {
                        r.Activated = r.Changed = true;
                        *value = i;
                    }
                ImGui::EndCombo();
            }
            break;
        }
        case FieldType::Texture: r = AssetField(label, *(AssetHandle*)p, AssetType::Texture); break;
        case FieldType::Mesh: r = AssetField(label, *(AssetHandle*)p, AssetType::Mesh); break;
        case FieldType::Scene: r = AssetField(label, *(AssetHandle*)p, AssetType::Scene); break;
        case FieldType::Prefab: r = AssetField(label, *(AssetHandle*)p, AssetType::Prefab); break;
        case FieldType::Audio: r = AssetField(label, *(AssetHandle*)p, AssetType::Audio); break;
        case FieldType::Material: r = AssetField(label, *(AssetHandle*)p, AssetType::Material); break;
        case FieldType::Font: r = AssetField(label, *(AssetHandle*)p, AssetType::Font); break;
        case FieldType::AnimatorController: r = AssetField(label, *(AssetHandle*)p, AssetType::AnimatorController); break;
        case FieldType::EntityRef:
            if (scene) r = EntityField(label, *(Entity*)p, *scene);
            break;
        case FieldType::Curve:
        {
            PropertyLabel(label, f.Tooltip.c_str());
            mixedNote();
            Curve& curve = *(Curve*)p;
            r = CurveEditor("##curve", curve);
            r |= Track(ImGui::Checkbox("Smooth##curve", &curve.Smooth));
            ImGui::SetItemTooltip("Monotone cubic through the keys instead of straight segments");
            break;
        }
        case FieldType::Gradient:
            PropertyLabel(label, f.Tooltip.c_str());
            mixedNote();
            r = GradientEditor("##gradient", *(Gradient*)p);
            break;
        case FieldType::StringList:
        {
            auto& list = *(std::vector<std::string>*)p;
            PropertyLabel(label, f.Tooltip.c_str());
            mixedNote();
            if (ImGui::Button("+ Add"))
            {
                list.push_back("Option " + std::to_string(list.size() + 1));
                r.Activated = r.Changed = true;
            }
            ImGui::SameLine();
            ImGui::TextDisabled("%d items", (int)list.size());
            int remove = -1;
            for (int i = 0; i < (int)list.size(); ++i)
            {
                ImGui::PushID(i);
                char name[16];
                std::snprintf(name, sizeof(name), "  %d", i);
                PropertyLabel(name, "");
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight() - ImGui::GetStyle().ItemSpacing.x);
                r |= Track(ImGui::InputText("##s", &list[i]));
                ImGui::SameLine();
                if (ImGui::Button("x", ImVec2(ImGui::GetFrameHeight(), 0.0f))) remove = i;
                ImGui::PopID();
            }
            if (remove >= 0)
            {
                list.erase(list.begin() + remove);
                r.Activated = r.Changed = true;
            }
            break;
        }
        case FieldType::MaterialList:
        {
            auto& list = *(std::vector<AssetHandle>*)p;
            PropertyLabel(label, f.Tooltip.c_str());
            mixedNote();
            int size = (int)list.size();
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
            if (ImGui::InputInt("##count", &size, 1, 1))
            {
                list.resize(std::clamp(size, 0, 256), AssetHandle::None());
                r.Activated = r.Changed = true;
            }
            ImGui::SameLine();
            ImGui::TextDisabled("slots");
            for (int i = 0; i < (int)list.size(); ++i)
            {
                ImGui::PushID(i);
                char name[24];
                std::snprintf(name, sizeof(name), "  Slot %d", i);
                r |= AssetField(name, list[i], AssetType::Material);
                ImGui::PopID();
            }
            break;
        }
        case FieldType::Vec3List:
        {
            auto& list = *(std::vector<glm::vec3>*)p;
            PropertyLabel(label, f.Tooltip.c_str());
            mixedNote();
            int size = (int)list.size();
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
            if (ImGui::InputInt("##count", &size, 1, 1))
            {
                size = std::clamp(size, 0, 4096);
                // New points continue the line in the direction of its last segment.
                while ((int)list.size() < size)
                {
                    glm::vec3 last = list.empty() ? glm::vec3(0.0f) : list.back();
                    glm::vec3 step = list.size() >= 2 ? last - list[list.size() - 2] : glm::vec3(0.0f, 1.0f, 0.0f);
                    list.push_back(list.empty() ? last : last + step);
                }
                list.resize(size);
                r.Activated = r.Changed = true;
            }
            ImGui::SameLine();
            ImGui::TextDisabled("points");
            int remove = -1;
            for (int i = 0; i < (int)list.size(); ++i)
            {
                ImGui::PushID(i);
                char name[16];
                std::snprintf(name, sizeof(name), "  %d", i);
                PropertyLabel(name, "");
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight() - ImGui::GetStyle().ItemSpacing.x);
                r |= Track(ImGui::DragFloat3("##p", &list[i].x, f.Speed, 0.0f, 0.0f, "%.2f"));
                ImGui::SameLine();
                if (ImGui::Button("x", ImVec2(ImGui::GetFrameHeight(), 0.0f))) remove = i;
                ImGui::PopID();
            }
            if (remove >= 0)
            {
                list.erase(list.begin() + remove);
                r.Activated = r.Changed = true;
            }
            break;
        }
        }
        s_Mixed = 0;
        ImGui::PopID();
        return r;
    }

    void HelpMarker(const char* text)
    {
        ImGui::TextDisabled("(?)");
        if (ImGui::BeginItemTooltip())
        {
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
            ImGui::TextUnformatted(text);
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    }
}
