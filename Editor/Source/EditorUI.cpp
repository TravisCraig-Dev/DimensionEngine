#include "EditorUI.h"
#include <Dimension/Asset/DataAsset.h>
#include <Dimension/Scripting/ScriptRegistry.h>
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

    // Set when a row and its label are drawn already (a list's element): the next PropertyLabel only moves to the value
    // column.
    static bool s_InlineRow = false;

    void PropertyLabel(const char* label, const char* tooltip)
    {
        if (s_InlineRow)
        {
            s_InlineRow = false;
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(-FLT_MIN);
            return;
        }
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

    bool IsListField(FieldType type)
    {
        return type == FieldType::StringList || type == FieldType::MaterialList || type == FieldType::Vec3List;
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
        if (IsListField(type) && a.is_array() && b.is_array())
        {
            if (a.size() != b.size()) return kMixedAll;
            uint32_t mask = 0;
            for (size_t i = 0; i < a.size(); ++i)
                if (a[i] != b[i]) mask |= 1u << std::min<size_t>(i, 31);
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
        case AssetType::Timeline: return { 0.95f, 0.75f, 0.45f, 1 };
        case AssetType::SpriteAtlas: return { 0.95f, 0.6f, 0.55f, 1 };
        case AssetType::Preset: return { 0.6f, 0.85f, 0.85f, 1 };
        case AssetType::StringTable: return { 0.85f, 0.9f, 0.55f, 1 };
        case AssetType::DataAsset: return { 0.7f, 0.6f, 0.95f, 1 };
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
        case AssetType::Timeline: return "TIME";
        case AssetType::SpriteAtlas: return "ATLAS";
        case AssetType::Preset: return "PRESET";
        case AssetType::StringTable: return "TEXT";
        case AssetType::DataAsset: return "DATA";
        default: return "FILE";
        }
    }

    EditResult AssetField(const char* label, AssetHandle& handle, AssetType type)
    {
        return AssetField(label, handle, type, {}, AssetTypeToString(type));
    }

    EditResult AssetField(const char* label, AssetHandle& handle, AssetType type, const std::function<bool(AssetHandle)>& accept,
                          const std::string& typeLabel)
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
        std::string text = name + "  (" + typeLabel + ")";
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
                if (meta && meta->Type == type && (!accept || accept(dropped)))
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
                if (!accept || accept(a)) option(a, AssetManager::GetMetadata(a)->Path.generic_string());
            ImGui::EndPopup();
        }
        ImGui::PopID();
        return r;
    }

    EditResult EntityField(const char* label, Entity& entity, Scene& scene) { return EntityField(label, entity, scene, {}, "Entity"); }

    EditResult EntityField(const char* label, Entity& entity, Scene& scene, const std::function<bool(Entity)>& accept,
                           const std::string& typeLabel)
    {
        EditResult r;
        PropertyLabel(label);
        ImGui::PushID(label);
        std::string name = s_Mixed ? std::string(kMixedText)
                         : scene.IsValid(entity) ? scene.GetRegistry().Get<TagComponent>(entity).Name + (accept ? "  (" + typeLabel + ")" : "")
                                                 : "None (" + typeLabel + ")";
        ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));
        if (ImGui::Button(name.c_str(), ImVec2(-FLT_MIN, 0))) ImGui::OpenPopup("picker");
        ImGui::PopStyleVar();
        if (ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kEntityPayload))
            {
                Entity e = scene.FindEntityByGuid(Guid(*(const uint64_t*)p->Data));
                if (!e.IsNull() && (!accept || accept(e)))
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
                if (accept && !accept(e)) return;
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
        ImGuiID dragId = ImGui::GetID("##dragkey"), selectedId = ImGui::GetID("##selkey"), handleId = ImGui::GetID("##draghandle");
        ImGuiID menuId = ImGui::GetID("##menukey");
        int dragging = storage->GetInt(dragId, -1);
        int hoveredKey = -1;
        ImVec2 mouse = ImGui::GetIO().MousePos;

        // Tangent handles of the selected key (smooth curves, hand-set tangents): drag them to change the slopes.
        // Handles are drawn a fixed screen length from the key along each slope; side -1 = in, +1 = out.
        int selected = storage->GetInt(selectedId, -1);
        int hoveredHandle = 0;
        auto handlePos = [&](int key, int side) {
            float slope = side < 0 ? curve.InSlope(key) : curve.OutSlope(key);
            ImVec2 kp = toScreen(curve.Keys[key].Time, curve.Keys[key].Value);
            glm::vec2 d(size.x, -slope * size.y / (hi - lo)); // screen direction of a unit of time along the slope
            d = glm::normalize(d) * 36.0f * (float)side;
            return ImVec2(kp.x + d.x, kp.y + d.y);
        };
        bool showHandles = curve.Smooth && selected >= 0 && selected < (int)curve.Keys.size() && curve.Keys[selected].Mode != CurveTangentMode::Auto;
        if (showHandles)
        {
            ImVec2 kp = toScreen(curve.Keys[selected].Time, curve.Keys[selected].Value);
            for (int side : { -1, 1 })
            {
                if ((side < 0 && selected == 0) || (side > 0 && selected + 1 == (int)curve.Keys.size())) continue;
                ImVec2 h = handlePos(selected, side);
                bool hot = canvasHovered && std::abs(mouse.x - h.x) < 5 && std::abs(mouse.y - h.y) < 5;
                if (hot) hoveredHandle = side;
                dl->AddLine(kp, h, IM_COL32(170, 170, 180, 255), 1.0f);
                dl->AddCircleFilled(h, hot ? 4.5f : 3.5f, hot ? IM_COL32(255, 255, 255, 255) : IM_COL32(255, 200, 90, 255));
            }
        }
        int handleDrag = storage->GetInt(handleId, 0);
        if (hoveredHandle != 0 && ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            storage->SetInt(handleId, handleDrag = hoveredHandle);
            r.Activated = true;
        }
        if (handleDrag != 0)
        {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && showHandles)
            {
                CurveKey& k = curve.Keys[selected];
                glm::vec2 v = fromScreen(mouse), kv(k.Time, k.Value);
                float dt = (v.x - kv.x) * (float)handleDrag;
                float slope = dt > 1e-4f ? (v.y - kv.y) / (v.x - kv.x) : (v.y > kv.y ? 1.0f : -1.0f) * (float)handleDrag * 1e4f;
                slope = std::clamp(slope, -1e4f, 1e4f);
                float in = k.InTangent, out = k.OutTangent;
                if (k.Mode == CurveTangentMode::Free) in = out = slope;
                else (handleDrag < 0 ? in : out) = slope;
                if (in != k.InTangent || out != k.OutTangent)
                {
                    k.InTangent = in;
                    k.OutTangent = out;
                    r.Changed = true;
                }
            }
            else
                storage->SetInt(handleId, handleDrag = 0);
        }
        for (int i = 0; i < (int)curve.Keys.size(); ++i)
        {
            ImVec2 kp = toScreen(curve.Keys[i].Time, curve.Keys[i].Value);
            bool hot = std::abs(mouse.x - kp.x) < 6 && std::abs(mouse.y - kp.y) < 6 && canvasHovered;
            if (hot) hoveredKey = i;
            dl->AddCircleFilled(kp, hot || i == dragging ? 5.0f : 4.0f, hot || i == dragging ? IM_COL32(255, 255, 255, 255) : IM_COL32(200, 230, 255, 255));
        }
        if (hoveredKey >= 0)
            ImGui::SetTooltip("t %.2f   value %.3f (right-click: tangents, delete)", curve.Keys[hoveredKey].Time, curve.Keys[hoveredKey].Value);
        else if (hoveredHandle != 0)
            ImGui::SetTooltip("%s slope %.3f", hoveredHandle < 0 ? "In" : "Out",
                              hoveredHandle < 0 ? curve.InSlope(selected) : curve.OutSlope(selected));

        if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && hoveredKey >= 0 && handleDrag == 0)
        {
            storage->SetInt(dragId, hoveredKey);
            storage->SetInt(selectedId, hoveredKey);
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
                CurveKey k = curve.Keys[dragging];
                k.Time = std::clamp(v.x, minT, maxT);
                k.Value = v.y;
                if (k.Time != curve.Keys[dragging].Time || k.Value != curve.Keys[dragging].Value)
                {
                    curve.Keys[dragging] = k;
                    r.Changed = true;
                }
            }
            else
                storage->SetInt(dragId, -1);
        }
        if (canvasHovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && hoveredKey < 0 && hoveredHandle == 0)
        {
            glm::vec2 v = fromScreen(mouse);
            curve.Keys.push_back({ v.x, v.y });
            curve.Sort();
            r.Activated = r.Changed = true;
        }
        // Right-click a key: tangent modes (switching to hand-set tangents starts from the automatic ones, so the
        // curve does not jump; it also makes the curve smooth) and Delete Key.
        if (hoveredKey >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
        {
            storage->SetInt(menuId, hoveredKey);
            ImGui::OpenPopup("##keymenu");
        }
        if (ImGui::BeginPopup("##keymenu"))
        {
            int k = storage->GetInt(menuId, -1);
            if (k >= 0 && k < (int)curve.Keys.size())
            {
                CurveKey& key = curve.Keys[k];
                auto setMode = [&](CurveTangentMode mode, bool flat) {
                    float in = flat ? 0.0f : curve.InSlope(k), out = flat ? 0.0f : curve.OutSlope(k);
                    if (mode == CurveTangentMode::Free && !flat) in = out = 0.5f * (in + out);
                    key.Mode = mode;
                    key.InTangent = mode == CurveTangentMode::Auto ? 0.0f : in;
                    key.OutTangent = mode == CurveTangentMode::Auto ? 0.0f : out;
                    if (mode != CurveTangentMode::Auto) curve.Smooth = true;
                    storage->SetInt(selectedId, k);
                    r.Activated = r.Changed = true;
                };
                if (ImGui::MenuItem("Auto", nullptr, key.Mode == CurveTangentMode::Auto)) setMode(CurveTangentMode::Auto, false);
                ImGui::SetItemTooltip("Monotone tangents: smooth, never overshooting a key");
                if (ImGui::MenuItem("Free", nullptr, key.Mode == CurveTangentMode::Free)) setMode(CurveTangentMode::Free, false);
                ImGui::SetItemTooltip("One slope through the key, set by dragging its handles");
                if (ImGui::MenuItem("Broken", nullptr, key.Mode == CurveTangentMode::Broken)) setMode(CurveTangentMode::Broken, false);
                ImGui::SetItemTooltip("Separate incoming and outgoing slopes");
                if (ImGui::MenuItem("Flat")) setMode(CurveTangentMode::Free, true);
                ImGui::Separator();
                if (ImGui::MenuItem("Delete Key", nullptr, false, curve.Keys.size() > 1))
                {
                    curve.Keys.erase(curve.Keys.begin() + k);
                    storage->SetInt(selectedId, -1);
                    r.Activated = r.Changed = true;
                }
            }
            ImGui::EndPopup();
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
        if (!f.Header.empty())
        {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Dummy(ImVec2(0.0f, 2.0f));
            ImGui::PushStyleColor(ImGuiCol_Text, AccentText());
            ImGui::TextUnformatted(f.Header.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::PushID(label);
        EditResult r;
        bool bounded = f.Min != f.Max;
        bool mixed = s_Mixed != 0;
        const char* floatFormat = mixed ? kMixedText : "%.3f";
        // Whole-value fields (curves, gradients, lists of different lengths) that differ across the selection: say so
        // above the editor, which shows the active object's value; any edit gives every selected object that value.
        // Lists of equal length instead mark each differing element with "--".
        uint32_t listMixed = IsListField(f.Type) && s_Mixed != kMixedAll ? s_Mixed : 0u;
        auto elementMixed = [&](int i) { return (listMixed >> std::min(i, 31)) & 1u; };
        auto mixedNote = [&] {
            if (!mixed || listMixed) return;
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
            else if (f.Multiline)
            {
                // Grows with its text (3 to 12 lines).
                const std::string& text = *(std::string*)p;
                int lines = 1 + (int)std::count(text.begin(), text.end(), '\n');
                float height = ImGui::GetTextLineHeight() * (float)std::clamp(lines + 1, 3, 12) + ImGui::GetStyle().FramePadding.y * 2.0f;
                r = Track(ImGui::InputTextMultiline("##v", (std::string*)p, ImVec2(-FLT_MIN, height)));
            }
            else
                r = Track(ImGui::InputText("##v", (std::string*)p));
            break;
        case FieldType::Enum:
        {
            PropertyLabel(label, f.Tooltip.c_str());
            // The stored value is the name's index, or its entry in EnumValues when given.
            int32_t* value = (int32_t*)p;
            auto valueOf = [&](int i) { return i < (int)f.EnumValues.size() ? f.EnumValues[i] : i; };
            if (f.EnumFlags)
            {
                // Flags: the names set, and a toggle per name (names worth 0 clear them all).
                std::string shown;
                for (int i = 0; i < (int)f.EnumNames.size(); ++i)
                    if (valueOf(i) != 0 && (*value & valueOf(i)) == valueOf(i)) shown += (shown.empty() ? "" : ", ") + f.EnumNames[i];
                if (shown.empty()) shown = *value == 0 ? "Nothing" : std::to_string(*value);
                if (ImGui::BeginCombo("##v", mixed ? kMixedText : shown.c_str()))
                {
                    for (int i = 0; i < (int)f.EnumNames.size(); ++i)
                    {
                        int32_t bits = valueOf(i);
                        bool on = bits == 0 ? *value == 0 : (*value & bits) == bits;
                        if (ImGui::Selectable(f.EnumNames[i].c_str(), on, ImGuiSelectableFlags_DontClosePopups))
                        {
                            r.Activated = r.Changed = true;
                            *value = bits == 0 ? 0 : on ? (*value & ~bits) : (*value | bits);
                        }
                    }
                    ImGui::EndCombo();
                }
                break;
            }
            int current = -1;
            for (int i = 0; i < (int)f.EnumNames.size(); ++i)
                if (valueOf(i) == *value) current = i;
            std::string unnamed = std::to_string(*value);
            const char* preview = mixed ? kMixedText : current >= 0 ? f.EnumNames[current].c_str() : unnamed.c_str();
            if (ImGui::BeginCombo("##v", preview))
            {
                for (int i = 0; i < (int)f.EnumNames.size(); ++i)
                    if (ImGui::Selectable(f.EnumNames[i].c_str(), i == current) && i != current)
                    {
                        r.Activated = r.Changed = true;
                        *value = valueOf(i);
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
        case FieldType::Timeline: r = AssetField(label, *(AssetHandle*)p, AssetType::Timeline); break;
        case FieldType::DataAsset:
        {
            // Data assets of the field's class (or a class deriving from it).
            std::string cls = f.AssetClass;
            size_t dot = cls.find_last_of('.');
            r = AssetField(label, *(AssetHandle*)p, AssetType::DataAsset,
                           [cls](AssetHandle h) { return DataAssets::IsA(DataAssets::ClassOf(h), cls); },
                           dot == std::string::npos ? cls : cls.substr(dot + 1));
            break;
        }
        case FieldType::EntityRef:
            if (scene && !f.ScriptClass.empty())
            {
                // An object's script: only objects with a script of the field's class (or a derived one).
                std::string cls = f.ScriptClass;
                Scene* sc = scene;
                size_t dot = cls.find_last_of('.');
                r = EntityField(label, *(Entity*)p, *scene, [cls, sc](Entity e) {
                    if (auto* scripts = sc->GetRegistry().TryGet<ScriptComponent>(e))
                        for (auto& s : scripts->Scripts)
                            if (ScriptRegistry::IsA(s.ClassName, cls)) return true;
                    return false;
                }, dot == std::string::npos ? cls : cls.substr(dot + 1));
            }
            else if (scene) r = EntityField(label, *(Entity*)p, *scene);
            break;
        case FieldType::Curve:
        {
            PropertyLabel(label, f.Tooltip.c_str());
            mixedNote();
            Curve& curve = *(Curve*)p;
            r = CurveEditor("##curve", curve);
            r |= Track(ImGui::Checkbox("Smooth##curve", &curve.Smooth));
            ImGui::SetItemTooltip("A cubic through the keys instead of straight segments (right-click a key to set its tangents by hand)");
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
                if (elementMixed(i))
                {
                    // Empty box with a "--" hint: typing sets this element on every selected object.
                    static std::string typed;
                    typed.clear();
                    EditResult er = Track(ImGui::InputTextWithHint("##s", kMixedText, &typed));
                    if (er.Changed) list[i] = typed;
                    r |= er;
                }
                else
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
        case FieldType::Struct:
        {
            // A row that opens to the struct's fields.
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            bool open = ImGui::TreeNodeEx("##struct", ImGuiTreeNodeFlags_SpanAllColumns | ImGuiTreeNodeFlags_DefaultOpen, "%s", label);
            if (!f.Tooltip.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", f.Tooltip.c_str());
            if (open)
            {
                for (const FieldInfo& sub : f.Elements)
                    if (!sub.Hidden) r |= Field(sub, p, scene);
                ImGui::TreePop();
            }
            break;
        }
        case FieldType::StructList:
        {
            // A row per element (titled by its title field, or numbered) that opens to its fields, with buttons to
            // move it up or down and remove it.
            PropertyLabel(label, f.Tooltip.c_str());
            mixedNote();
            size_t count = f.ListSize(p);
            bool full = f.Max > 0.0f && count >= (size_t)f.Max;
            ImGui::BeginDisabled(full);
            if (ImGui::Button("+ Add"))
            {
                f.ListResize(p, count + 1);
                r.Activated = r.Changed = true;
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (full) ImGui::TextDisabled("%zu (the most)", count);
            else ImGui::TextDisabled("%zu", count);
            const FieldInfo* titleField = nullptr;
            for (const FieldInfo& sub : f.Elements)
                if (sub.Name == f.ElementTitle && sub.Type == FieldType::String) titleField = &sub;
            int remove = -1, moveFrom = -1, moveTo = -1;
            uint32_t savedMixed = s_Mixed;
            s_Mixed = 0;
            for (size_t i = 0; i < f.ListSize(p); ++i)
            {
                void* element = f.ListAt(p, i);
                ImGui::PushID((int)i);
                if (IsValueList(f))
                {
                    // One row per value: its index and buttons on the left, the value on the right.
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::AlignTextToFramePadding();
                    float button = ImGui::GetFrameHeight();
                    ImGui::Dummy(ImVec2(ImGui::GetStyle().IndentSpacing * 0.5f, 0.0f));
                    ImGui::SameLine(0.0f, 0.0f);
                    ImGui::TextDisabled("%2zu", i);
                    ImGui::SameLine();
                    ImGui::BeginDisabled(i == 0);
                    if (ImGui::ArrowButton("##up", ImGuiDir_Up)) moveFrom = (int)i, moveTo = (int)i - 1;
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    ImGui::BeginDisabled(i + 1 >= f.ListSize(p));
                    if (ImGui::ArrowButton("##down", ImGuiDir_Down)) moveFrom = (int)i, moveTo = (int)i + 1;
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    if (ImGui::Button("x", ImVec2(button, 0.0f))) remove = (int)i;
                    s_InlineRow = true;
                    r |= Field(f.Elements[0], element, scene);
                    s_InlineRow = false;
                    ImGui::PopID();
                    continue;
                }
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                std::string title = titleField ? *(const std::string*)titleField->Access(element) : std::string();
                bool open = ImGui::TreeNodeEx("##element", ImGuiTreeNodeFlags_SpanAvailWidth, "%zu  %s", i, title.c_str());
                ImGui::TableSetColumnIndex(1);
                float button = ImGui::GetFrameHeight();
                ImGui::BeginDisabled(i == 0);
                if (ImGui::ArrowButton("##up", ImGuiDir_Up)) moveFrom = (int)i, moveTo = (int)i - 1;
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::BeginDisabled(i + 1 >= f.ListSize(p));
                if (ImGui::ArrowButton("##down", ImGuiDir_Down)) moveFrom = (int)i, moveTo = (int)i + 1;
                ImGui::EndDisabled();
                ImGui::SameLine();
                if (ImGui::Button("x", ImVec2(button, 0.0f))) remove = (int)i;
                if (open)
                {
                    for (const FieldInfo& sub : f.Elements)
                        if (!sub.Hidden) r |= Field(sub, element, scene);
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
            s_Mixed = savedMixed;
            if (remove >= 0)
            {
                f.ListErase(p, (size_t)remove);
                r.Activated = r.Changed = true;
            }
            else if (moveFrom >= 0)
            {
                f.ListMove(p, (size_t)moveFrom, (size_t)moveTo);
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
                uint32_t saved = s_Mixed;
                s_Mixed = elementMixed(i); // AssetField shows "--" while set
                r |= AssetField(name, list[i], AssetType::Material);
                s_Mixed = saved;
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
                r |= Track(ImGui::DragFloat3("##p", &list[i].x, f.Speed, 0.0f, 0.0f, elementMixed(i) ? kMixedText : "%.2f"));
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
