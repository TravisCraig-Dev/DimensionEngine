#include "Panels.h"
#include "../EditorUI.h"
#include <Dimension/Asset/AssetManager.h>
#include <Dimension/Particles/VisualEffect.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <cstring>
#include <map>

namespace Editor
{
    // The Visual Effect Graph window: an effect's four contexts in a column (Spawn, Initialize, Update, Output), the
    // Initialize and Update blocks listed in theirs, and operator nodes around them whose outputs link into the blocks'
    // (and each other's) inputs. The sidebar edits the effect's settings, its exposed properties and the selected node.
    // Every change saves the asset (the effects using it recompile); Ctrl+Z steps back through this window's changes.
    namespace
    {
        constexpr float kContextW = 280.0f, kHeaderH = 24.0f, kRowH = 20.0f, kNodeW = 180.0f, kPinR = 5.0f, kGap = 14.0f;
        constexpr float kSpawnH = 64.0f, kOutputH = 64.0f;

        ImU32 KindColor(VfxValue kind)
        {
            switch (kind)
            {
            case VfxValue::Color: return IM_COL32(240, 150, 210, 255);
            case VfxValue::Vector: return IM_COL32(120, 200, 255, 255);
            default: return IM_COL32(150, 230, 140, 255);
            }
        }

        std::string ValueText(const glm::vec4& v, VfxValue kind)
        {
            char buf[96];
            if (kind == VfxValue::Float) std::snprintf(buf, sizeof(buf), "%.3g", v.x);
            else if (kind == VfxValue::Vector) std::snprintf(buf, sizeof(buf), "%.3g, %.3g, %.3g", v.x, v.y, v.z);
            else std::snprintf(buf, sizeof(buf), "%.2g, %.2g, %.2g, %.2g", v.x, v.y, v.z, v.w);
            return buf;
        }

        // The kind an input's value is shown and edited as: the type's, or for attribute blocks the attribute's.
        VfxValue InputKind(const VfxNode& n, int input)
        {
            auto ports = VisualEffects::InputsOf(n);
            VfxValue kind = input < (int)ports.size() ? ports[input].Kind : VfxValue::Float;
            if (n.Type == "Set Attribute" || n.Type == "Add Attribute" || n.Type == "Add Per Second")
            {
                VfxAttribute a = (VfxAttribute)std::clamp(n.Option, 0, (int)VfxAttribute::Count - 1);
                kind = a == VfxAttribute::Color ? VfxValue::Color : a == VfxAttribute::Position || a == VfxAttribute::Velocity ? VfxValue::Vector : VfxValue::Float;
            }
            return kind;
        }

        // A node input's value editor (the sidebar).
        bool ValueEditor(const char* id, glm::vec4& v, VfxValue kind)
        {
            ImGui::SetNextItemWidth(-1);
            if (kind == VfxValue::Float) return ImGui::DragFloat(id, &v.x, 0.01f);
            if (kind == VfxValue::Vector) return ImGui::DragFloat3(id, &v.x, 0.01f);
            return ImGui::ColorEdit4(id, &v.x, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_AlphaPreviewHalf);
        }
    }

    VfxNode* VisualEffectPanel::Selected()
    {
        auto graph = AssetManager::GetVisualEffect(m_Handle);
        return graph ? graph->Find(m_Selected) : nullptr;
    }

    void VisualEffectPanel::Open(AssetHandle handle)
    {
        if (handle != m_Handle)
        {
            m_Handle = handle;
            m_Selected = m_PendingSelect;
            m_PendingSelect = 0;
            m_History.clear();
            m_Framed = m_UserPanned = false;
        }
        m_Focus = true;
    }

    void VisualEffectPanel::BeginChange()
    {
        // The graph as it was, for Ctrl+Z (one step per change; a drag is one change).
        if (auto graph = AssetManager::GetVisualEffect(m_Handle))
        {
            m_History.push_back(graph->ToJson());
            if (m_History.size() > 100) m_History.erase(m_History.begin());
        }
    }

    void VisualEffectPanel::Commit()
    {
        AssetManager::SaveVisualEffect(m_Handle);
    }

    void VisualEffectPanel::Undo()
    {
        auto graph = AssetManager::GetVisualEffect(m_Handle);
        if (!graph || m_History.empty()) return;
        *graph = VisualEffectAsset::FromJson(m_History.back());
        m_History.pop_back();
        if (!graph->Find(m_Selected)) m_Selected = 0;
        Commit();
    }

    void VisualEffectPanel::OnImGui(bool* open)
    {
        if (m_Focus)
        {
            ImGui::SetNextWindowFocus();
            m_Focus = false;
        }
        if (!ImGui::Begin("Visual Effect Graph", open))
        {
            ImGui::End();
            return;
        }
        // The selected .vfx asset opens here too.
        if (m_Ctx.SelectedAsset.IsValid() && m_Ctx.SelectedAsset != m_Handle)
            if (auto* meta = AssetManager::GetMetadata(m_Ctx.SelectedAsset); meta && meta->Type == AssetType::VisualEffect) Open(m_Ctx.SelectedAsset);
        // Nothing open yet: the selected object's effect.
        if (!m_Handle.IsValid() && !m_Ctx.Selected.IsNull() && m_Ctx.ActiveScene().IsValid(m_Ctx.Selected))
            if (auto* vfx = m_Ctx.ActiveScene().GetRegistry().TryGet<VisualEffectComponent>(m_Ctx.Selected); vfx && vfx->Asset.IsValid()) Open(vfx->Asset);
        auto graph = AssetManager::GetVisualEffect(m_Handle);
        if (!graph)
        {
            ImGui::TextDisabled("Select a Visual Effect (.vfx) in the Project window, or create one (Create > Visual Effect).");
            ImGui::End();
            return;
        }
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput && ImGui::GetIO().KeyCtrl &&
            ImGui::IsKeyPressed(ImGuiKey_Z, false))
            Undo();

        float sidebar = 300.0f;
        ImGui::BeginChild("VfxSidebar", ImVec2(sidebar, 0), ImGuiChildFlags_ResizeX | ImGuiChildFlags_Borders);
        DrawSidebar(*graph);
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("VfxGraph", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        DrawGraph(*graph);
        ImGui::EndChild();
        ImGui::End();
    }

    void VisualEffectPanel::DrawSidebar(VisualEffectAsset& g)
    {
        auto* meta = AssetManager::GetMetadata(m_Handle);
        ImGui::TextUnformatted(meta ? meta->Path.filename().string().c_str() : "?");
        if (auto program = VisualEffects::ProgramOf(m_Handle))
        {
            if (program->Valid())
                ImGui::TextDisabled("%u + %u instructions", program->InitCount, program->UpdateCount);
            else
                for (const std::string& e : program->Errors) ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "%s", e.c_str());
        }
        bool changed = false;
        auto edit = [&](bool edited) {
            if (ImGui::IsItemActivated()) BeginChange();
            if (edited) changed = true;
        };

        if (ImGui::CollapsingHeader("Spawn", ImGuiTreeNodeFlags_DefaultOpen) && UI::BeginProperties("##spawn"))
        {
            UI::PropertyLabel("Capacity", "Particles alive at most");
            ImGui::SetNextItemWidth(-1);
            edit(ImGui::DragInt("##capacity", &g.Capacity, 10.0f, 1, 1000000));
            UI::PropertyLabel("Rate", "New particles per second");
            ImGui::SetNextItemWidth(-1);
            edit(ImGui::DragFloat("##rate", &g.Rate, 1.0f, 0.0f, 1e6f));
            UI::PropertyLabel("Duration", "A loop's seconds (bursts repeat every loop)");
            ImGui::SetNextItemWidth(-1);
            edit(ImGui::DragFloat("##duration", &g.Duration, 0.05f, 0.01f, 3600.0f));
            UI::PropertyLabel("Loop", "Off: spawning stops after one Duration");
            edit(ImGui::Checkbox("##loop", &g.Loop));
            for (size_t i = 0; i < g.Bursts.size(); ++i)
            {
                ImGui::PushID((int)i);
                UI::PropertyLabel(i == 0 ? "Bursts" : "", "At a time in each loop, a number of particles at once");
                float w = (ImGui::GetContentRegionAvail().x - 30.0f) * 0.5f;
                ImGui::SetNextItemWidth(w);
                edit(ImGui::DragFloat("##t", &g.Bursts[i].Time, 0.01f, 0.0f, 3600.0f, "at %.2f s"));
                ImGui::SameLine();
                ImGui::SetNextItemWidth(w);
                edit(ImGui::DragInt("##n", &g.Bursts[i].Count, 1.0f, 0, 1000000, "%d"));
                ImGui::SameLine();
                if (ImGui::SmallButton("x"))
                {
                    BeginChange();
                    g.Bursts.erase(g.Bursts.begin() + (long long)i);
                    changed = true;
                    ImGui::PopID();
                    break;
                }
                ImGui::PopID();
            }
            UI::PropertyLabel(g.Bursts.empty() ? "Bursts" : "");
            if (ImGui::SmallButton("+ Burst"))
            {
                BeginChange();
                g.Bursts.push_back({ 0.0f, 50 });
                changed = true;
            }
            UI::EndProperties();
        }

        if (ImGui::CollapsingHeader("Output", ImGuiTreeNodeFlags_DefaultOpen) && UI::BeginProperties("##output"))
        {
            int space = (int)g.Space, blend = (int)g.Blend;
            UI::PropertyLabel("Space", "World: particles stay where they started as the object moves; Local: they move with it");
            ImGui::SetNextItemWidth(-1);
            const char* spaces[] = { "Local", "World" };
            if (ImGui::Combo("##space", &space, spaces, 2))
            {
                BeginChange();
                g.Space = (ParticleSpace)space;
                changed = true;
            }
            UI::PropertyLabel("Blend");
            ImGui::SetNextItemWidth(-1);
            const char* blends[] = { "Alpha", "Additive" };
            if (ImGui::Combo("##blend", &blend, blends, 2))
            {
                BeginChange();
                g.Blend = (ParticleBlend)blend;
                changed = true;
            }
            UI::EditResult r = UI::AssetField("Texture", g.Texture, AssetType::Texture);
            if (r.Activated) BeginChange();
            if (r.Changed) changed = true;
            UI::PropertyLabel("Sort", "Alpha: drawn back to front");
            edit(ImGui::Checkbox("##sort", &g.Sort));
            UI::PropertyLabel("Lit", "Shaded by the scene's lights");
            edit(ImGui::Checkbox("##lit", &g.Lit));
            UI::PropertyLabel("Draw As Mesh", "Each particle a lit mesh (Size its scale, Rotation its turn) instead of a camera-facing quad");
            edit(ImGui::Checkbox("##meshout", &g.MeshOutput));
            if (g.MeshOutput)
            {
                UI::EditResult m = UI::AssetField("Mesh", g.Mesh, AssetType::Mesh);
                if (m.Activated) BeginChange();
                if (m.Changed) changed = true;
                UI::EditResult mat = UI::AssetField("Material", g.Material, AssetType::Material);
                if (mat.Activated) BeginChange();
                if (mat.Changed) changed = true;
            }
            UI::PropertyLabel("Trails", "Each particle also draws a ribbon back through where it was (strips; up to 262,144 particles)");
            edit(ImGui::Checkbox("##trails", &g.Trails));
            if (g.Trails)
            {
                UI::PropertyLabel("Trail Lifetime", "Seconds of path the ribbon covers");
                ImGui::SetNextItemWidth(-1);
                edit(ImGui::DragFloat("##trailLife", &g.TrailLifetime, 0.01f, 0.01f, 10.0f));
                UI::PropertyLabel("Trail Width", "Times the particle's size");
                ImGui::SetNextItemWidth(-1);
                edit(ImGui::DragFloat("##trailWidth", &g.TrailWidth, 0.01f, 0.0f, 10.0f));
            }
            UI::EndProperties();
        }

        if (ImGui::CollapsingHeader("Properties", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::TextDisabled("Set per object (Inspector) and from scripts.");
            for (size_t i = 0; i < g.Properties.size(); ++i)
            {
                VfxProperty& p = g.Properties[i];
                ImGui::PushID((int)i);
                char name[64];
                std::snprintf(name, sizeof(name), "%s", p.Name.c_str());
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.45f);
                if (ImGui::InputText("##name", name, sizeof(name), ImGuiInputTextFlags_EnterReturnsTrue) && name[0])
                {
                    BeginChange();
                    p.Name = name;
                    changed = true;
                }
                ImGui::SameLine();
                int type = (int)p.Type;
                ImGui::SetNextItemWidth(80.0f);
                const char* types[] = { "Float", "Vector", "Color" };
                if (ImGui::Combo("##type", &type, types, 3))
                {
                    BeginChange();
                    p.Type = (VfxValue)type;
                    if (p.Type == VfxValue::Color && p.Value == glm::vec4(0.0f)) p.Value = glm::vec4(1.0f);
                    changed = true;
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Node"))
                {
                    // A Property node for it, left of the contexts.
                    BeginChange();
                    VfxNode& n = g.Add("Property", glm::vec2(-kNodeW - 80.0f, 40.0f + 30.0f * (float)i));
                    n.Option = (int)i;
                    m_Selected = n.Id;
                    changed = true;
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("x"))
                {
                    BeginChange();
                    // Property nodes after it name the one before them now; its own nodes go.
                    std::vector<uint32_t> gone;
                    for (VfxNode& n : g.Nodes)
                        if (n.Type == "Property")
                        {
                            if (n.Option == (int)i) gone.push_back(n.Id);
                            else if (n.Option > (int)i) --n.Option;
                        }
                    for (uint32_t id : gone) g.Remove(id);
                    g.Properties.erase(g.Properties.begin() + (long long)i);
                    changed = true;
                    ImGui::PopID();
                    break;
                }
                edit(ValueEditor("##value", p.Value, p.Type));
                ImGui::PopID();
            }
            if (ImGui::SmallButton("+ Property"))
            {
                BeginChange();
                std::string name = "Property";
                for (int k = 1; std::any_of(g.Properties.begin(), g.Properties.end(), [&](const VfxProperty& p) { return p.Name == name; }); ++k)
                    name = "Property " + std::to_string(k);
                g.Properties.push_back({ name, VfxValue::Float, glm::vec4(1.0f) });
                changed = true;
            }
        }

        if (VfxNode* n = g.Find(m_Selected))
        {
            const auto* type = VisualEffects::FindType(n->Type);
            if (type && ImGui::CollapsingHeader(n->Type.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
            {
                ImGui::PushID((int)n->Id);
                ImGui::TextWrapped("%s", type->Help);
                if (!type->HasOutput)
                {
                    edit(ImGui::Checkbox("Enabled", &n->Enabled));
                    // Order within the context.
                    auto& list = std::find(g.Initialize.begin(), g.Initialize.end(), n->Id) != g.Initialize.end() ? g.Initialize : g.Update;
                    auto at = std::find(list.begin(), list.end(), n->Id);
                    ImGui::SameLine();
                    ImGui::BeginDisabled(at == list.begin());
                    if (ImGui::SmallButton("Up"))
                    {
                        BeginChange();
                        std::iter_swap(at, at - 1);
                        changed = true;
                    }
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    ImGui::BeginDisabled(at == list.end() || at + 1 == list.end());
                    if (ImGui::SmallButton("Down"))
                    {
                        BeginChange();
                        std::iter_swap(at, at + 1);
                        changed = true;
                    }
                    ImGui::EndDisabled();
                }
                if (UI::BeginProperties("##node"))
                {
                    if (n->Type == "Property")
                    {
                        UI::PropertyLabel("Property");
                        ImGui::SetNextItemWidth(-1);
                        const char* current = n->Option >= 0 && n->Option < (int)g.Properties.size() ? g.Properties[n->Option].Name.c_str() : "(none)";
                        if (ImGui::BeginCombo("##prop", current))
                        {
                            for (size_t i = 0; i < g.Properties.size(); ++i)
                                if (ImGui::Selectable(g.Properties[i].Name.c_str(), n->Option == (int)i))
                                {
                                    BeginChange();
                                    n->Option = (int)i;
                                    changed = true;
                                }
                            ImGui::EndCombo();
                        }
                    }
                    else if (!type->Options.empty())
                    {
                        UI::PropertyLabel(type->HasOutput && n->Type == "Split" ? "Component" : "Attribute");
                        ImGui::SetNextItemWidth(-1);
                        int option = std::clamp(n->Option, 0, (int)type->Options.size() - 1);
                        if (ImGui::Combo("##option", &option, type->Options.data(), (int)type->Options.size()))
                        {
                            BeginChange();
                            n->Option = option;
                            changed = true;
                        }
                    }
                    if (n->Type == "Trigger On Die")
                    {
                        UI::EditResult r = UI::AssetField("Child", n->Child, AssetType::VisualEffect);
                        if (r.Activated) BeginChange();
                        if (r.Changed)
                        {
                            if (n->Child == m_Handle) n->Child = AssetHandle::None(); // (not itself)
                            changed = true;
                        }
                    }
                    if (n->Type == "Subgraph")
                    {
                        UI::EditResult r = UI::AssetField("Graph", n->Child, AssetType::VisualEffect);
                        if (r.Activated) BeginChange();
                        if (r.Changed)
                        {
                            if (n->Child == m_Handle) n->Child = AssetHandle::None(); // (not itself)
                            changed = true;
                        }
                    }
                    auto ports = VisualEffects::InputsOf(*n);
                    // (a subgraph's inputs follow its properties: their values start at the properties' own)
                    while (n->Inputs.size() < ports.size()) n->Inputs.push_back(ports[n->Inputs.size()].Default);
                    for (size_t i = 0; i < ports.size() && i < n->Inputs.size(); ++i)
                    {
                        ImGui::PushID((int)i);
                        UI::PropertyLabel(ports[i].Name);
                        if (const VfxLink* l = g.LinkInto(n->Id, (int)i))
                        {
                            const VfxNode* from = g.Find(l->From);
                            ImGui::TextDisabled("from %s", from ? from->Type.c_str() : "?");
                            ImGui::SameLine();
                            if (ImGui::SmallButton("Unlink"))
                            {
                                BeginChange();
                                uint32_t to = n->Id;
                                std::erase_if(g.Links, [&](const VfxLink& k) { return k.To == to && k.Input == (int)i; });
                                changed = true;
                            }
                        }
                        else
                        {
                            // Color-like Set Attribute values edit as colors.
                            VfxValue kind = ports[i].Kind;
                            if ((n->Type == "Set Attribute" || n->Type == "Add Attribute" || n->Type == "Add Per Second"))
                            {
                                VfxAttribute a = (VfxAttribute)std::clamp(n->Option, 0, (int)VfxAttribute::Count - 1);
                                kind = a == VfxAttribute::Color ? (n->Type == "Set Attribute" ? VfxValue::Color : VfxValue::Vector)
                                       : a == VfxAttribute::Position || a == VfxAttribute::Velocity ? VfxValue::Vector : VfxValue::Float;
                                if (a == VfxAttribute::Color && n->Type != "Set Attribute")
                                {
                                    // (an amount to add: all four channels)
                                    ImGui::SetNextItemWidth(-1);
                                    edit(ImGui::DragFloat4("##v", &n->Inputs[i].x, 0.01f));
                                    ImGui::PopID();
                                    continue;
                                }
                            }
                            edit(ValueEditor("##v", n->Inputs[i], kind));
                        }
                        ImGui::PopID();
                    }
                    UI::EndProperties();
                }
                if (n->Type == "Sample Gradient" || n->Type == "Color Over Life")
                {
                    UI::EditResult r = UI::GradientEditor("##gradient", n->Colors);
                    if (r.Activated) BeginChange();
                    if (r.Changed) changed = true;
                }
                if (n->Type == "Sample Curve" || n->Type == "Size Over Life")
                {
                    UI::EditResult r = UI::CurveEditor("##curve", n->Values, 90.0f);
                    if (r.Activated) BeginChange();
                    if (r.Changed) changed = true;
                }
                if (ImGui::Button("Delete Node"))
                {
                    BeginChange();
                    g.Remove(n->Id);
                    m_Selected = 0;
                    changed = true;
                }
                ImGui::PopID();
            }
        }
        if (changed) Commit();
    }

    void VisualEffectPanel::DrawGraph(VisualEffectAsset& g)
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 canvasMin = ImGui::GetCursorScreenPos(), canvasSize = ImGui::GetContentRegionAvail();
        if (canvasSize.x < 50.0f || canvasSize.y < 50.0f) return;
        ImVec2 canvasMax(canvasMin.x + canvasSize.x, canvasMin.y + canvasSize.y);
        dl->AddRectFilled(canvasMin, canvasMax, ImGui::GetColorU32(ImGuiCol_FrameBg));
        // Grid.
        float step = 32.0f;
        for (float x = std::fmod(m_Pan.x, step); x < canvasSize.x; x += step)
            dl->AddLine(ImVec2(canvasMin.x + x, canvasMin.y), ImVec2(canvasMin.x + x, canvasMax.y), IM_COL32(128, 128, 128, 18));
        for (float y = std::fmod(m_Pan.y, step); y < canvasSize.y; y += step)
            dl->AddLine(ImVec2(canvasMin.x, canvasMin.y + y), ImVec2(canvasMax.x, canvasMin.y + y), IM_COL32(128, 128, 128, 18));
        bool frameKey = ImGui::IsWindowHovered() && !ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_F, false);
        // (framed again while the window settles into its size, until the view is panned by hand)
        if (!m_Framed || frameKey || (!m_UserPanned && std::abs(canvasSize.x - m_FramedWidth) > 1.0f))
        {
            // The whole graph (the contexts and the operators around them) centered across, its top at the top.
            float lo = 0.0f, hi = kContextW + 20.0f, top = 0.0f;
            for (const VfxNode& n : g.Nodes)
                if (const auto* t = VisualEffects::FindType(n.Type); t && t->HasOutput)
                {
                    lo = std::min(lo, n.Position.x);
                    hi = std::max(hi, n.Position.x + kNodeW + 16.0f);
                    top = std::min(top, n.Position.y);
                }
            m_Zoom = std::clamp((canvasSize.x - 32.0f) / std::max(hi - lo, 1.0f), 0.5f, 1.0f);
            m_Pan = glm::vec2(std::floor(canvasSize.x * 0.5f - (lo + hi) * 0.5f * m_Zoom), 16.0f - top * m_Zoom);
            m_Framed = true;
            m_FramedWidth = canvasSize.x;
        }
        // Zoom with the mouse wheel, around the cursor.
        if (ImGui::IsWindowHovered() && ImGui::GetIO().MouseWheel != 0.0f)
        {
            ImVec2 m = ImGui::GetMousePos();
            glm::vec2 anchor(m.x - canvasMin.x - m_Pan.x, m.y - canvasMin.y - m_Pan.y);
            float before = m_Zoom;
            m_Zoom = std::clamp(m_Zoom * (1.0f + ImGui::GetIO().MouseWheel * 0.1f), 0.3f, 1.5f);
            m_Pan -= anchor * (m_Zoom / before - 1.0f);
            m_UserPanned = true;
        }
        const float z = m_Zoom;
        ImVec2 origin(canvasMin.x + m_Pan.x, canvasMin.y + m_Pan.y);
        auto toScreen = [&](glm::vec2 p) { return ImVec2(origin.x + p.x * z, origin.y + p.y * z); };
        // Text at the zoom (so it fits its boxes).
        ImFont* font = ImGui::GetFont();
        float fontSize = ImGui::GetFontSize() * z;
        auto label = [&](ImVec2 p, ImU32 col, const char* s) { if (z > 0.35f) dl->AddText(font, fontSize, p, col, s); };
        ImGui::PushClipRect(canvasMin, canvasMax, true);

        // The background button: panning (middle or right drag) and clicks on nothing.
        ImGui::SetCursorScreenPos(canvasMin);
        ImGui::SetNextItemAllowOverlap(); // (the nodes over it take their own clicks)
        ImGui::InvisibleButton("##canvas", canvasSize, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle | ImGuiButtonFlags_MouseButtonRight);
        if (ImGui::IsItemActive() && (ImGui::IsMouseDragging(ImGuiMouseButton_Middle) || ImGui::IsMouseDragging(ImGuiMouseButton_Right, 4.0f)))
        {
            m_Pan.x += ImGui::GetIO().MouseDelta.x;
            m_Pan.y += ImGui::GetIO().MouseDelta.y;
            m_Panned = m_UserPanned = true;
        }
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) m_Selected = 0;
        if (ImGui::IsItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Right) && !m_Panned)
        {
            ImVec2 m = ImGui::GetMousePos();
            m_AddAt = glm::vec2(m.x - origin.x, m.y - origin.y) / z;
            ImGui::OpenPopup("##addnode");
        }
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Right)) m_Panned = false;

        // Layout: the contexts' rectangles and each pin's spot.
        struct Pin
        {
            ImVec2 Pos;
            uint32_t Node;
            int Input; // -1 = output
            VfxValue Kind;
        };
        std::vector<Pin> pins;
        std::map<uint32_t, ImVec2> outputs;
        auto rowsOf = [&](const VfxNode& n) { return (int)VisualEffects::InputsOf(n).size(); };
        float y = 0.0f;
        glm::vec2 spawnAt(0.0f, y);
        y += kSpawnH + kGap;
        auto contextHeight = [&](const std::vector<uint32_t>& blocks) {
            float h = kHeaderH + 6.0f;
            for (uint32_t id : blocks)
                if (const VfxNode* n = g.Find(id)) h += kRowH + kRowH * (float)rowsOf(*n) + 4.0f;
            return std::max(h, kHeaderH + kRowH + 6.0f);
        };
        glm::vec2 initAt(0.0f, y);
        float initH = contextHeight(g.Initialize);
        y += initH + kGap;
        glm::vec2 updateAt(0.0f, y);
        float updateH = contextHeight(g.Update);
        y += updateH + kGap;
        glm::vec2 outputAt(0.0f, y);

        ImU32 text = ImGui::GetColorU32(ImGuiCol_Text), dim = ImGui::GetColorU32(ImGuiCol_TextDisabled);
        ImU32 body = ImGui::GetColorU32(ImGuiCol_WindowBg), border = ImGui::GetColorU32(ImGuiCol_Border);
        ImU32 accent = ImGui::GetColorU32(ImGuiCol_HeaderActive);
        auto contextBox = [&](glm::vec2 at, float h, const char* title, ImU32 tint) {
            ImVec2 a = toScreen(at), b = toScreen(at + glm::vec2(kContextW, h));
            dl->AddRectFilled(a, b, body, 6.0f);
            dl->AddRectFilled(a, ImVec2(b.x, a.y + kHeaderH * z), tint, 6.0f, ImDrawFlags_RoundCornersTop);
            dl->AddRect(a, b, border, 6.0f);
            label(ImVec2(a.x + 8.0f * z, a.y + 4.0f * z), IM_COL32(255, 255, 255, 255), title);
        };

        // Spawn and Output: their settings in short.
        contextBox(spawnAt, kSpawnH, "Spawn", IM_COL32(60, 120, 80, 255));
        {
            char line[128];
            std::snprintf(line, sizeof(line), "%.0f / s, %zu burst%s, %s %.2g s", g.Rate, g.Bursts.size(), g.Bursts.size() == 1 ? "" : "s",
                          g.Loop ? "loop" : "once", g.Duration);
            label(toScreen(spawnAt + glm::vec2(8.0f, kHeaderH + 6.0f)), dim, line);
            std::snprintf(line, sizeof(line), "up to %d alive", g.Capacity);
            label(toScreen(spawnAt + glm::vec2(8.0f, kHeaderH + 22.0f)), dim, line);
        }
        contextBox(outputAt, kOutputH, "Output Particle Quad", IM_COL32(120, 80, 60, 255));
        {
            char line[128];
            std::snprintf(line, sizeof(line), "%s, %s space%s%s", g.Blend == ParticleBlend::Additive ? "Additive" : "Alpha",
                          g.Space == ParticleSpace::World ? "world" : "local", g.Sort && g.Blend == ParticleBlend::Alpha ? ", sorted" : "",
                          (std::string(g.Lit ? ", lit" : "") + (g.Trails ? ", trails" : "")).c_str());
            label(toScreen(outputAt + glm::vec2(8.0f, kHeaderH + 6.0f)), dim, line);
            label(toScreen(outputAt + glm::vec2(8.0f, kHeaderH + 22.0f)), dim, "color, size, rotation from the attributes");
        }
        // Flow between the contexts.
        for (auto [from, h, to] : { std::tuple{ spawnAt, kSpawnH, initAt }, std::tuple{ initAt, initH, updateAt }, std::tuple{ updateAt, updateH, outputAt } })
        {
            ImVec2 a = toScreen(from + glm::vec2(kContextW * 0.5f, h)), b = toScreen(to + glm::vec2(kContextW * 0.5f, 0.0f));
            dl->AddLine(a, b, IM_COL32(200, 200, 200, 120), 3.0f * z);
        }

        // Initialize and Update: their blocks, each a title row and a row per input (pins on the context's left edge).
        uint32_t hoveredNode = 0;
        auto contextBlocks = [&](glm::vec2 at, float h, const char* title, std::vector<uint32_t>& blocks, bool update) {
            contextBox(at, h, title, update ? IM_COL32(70, 90, 150, 255) : IM_COL32(150, 110, 50, 255));
            // "+": a block.
            ImVec2 plus = toScreen(at + glm::vec2(kContextW - 24.0f, 3.0f));
            ImGui::SetCursorScreenPos(plus);
            ImGui::PushID(title);
            if (ImGui::SmallButton("+")) ImGui::OpenPopup("##addblock");
            if (ImGui::BeginPopup("##addblock"))
            {
                for (const auto& t : VisualEffects::NodeTypes())
                {
                    using VisualEffects::Category;
                    bool fits = t.Where == Category::Both || (update ? t.Where == Category::Update : t.Where == Category::Initialize);
                    if (!fits || t.HasOutput) continue;
                    if (ImGui::MenuItem(t.Menu))
                    {
                        BeginChange();
                        m_Selected = g.Add(t.Name, glm::vec2(0.0f), update).Id;
                        Commit();
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", t.Help);
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();
            float by = at.y + kHeaderH + 6.0f;
            for (uint32_t id : blocks)
            {
                VfxNode* n = g.Find(id);
                if (!n) continue;
                const auto* t = VisualEffects::FindType(n->Type);
                auto ports = VisualEffects::InputsOf(*n);
                int rows = (int)ports.size();
                glm::vec2 a(at.x + 6.0f, by), b(at.x + kContextW - 6.0f, by + kRowH * (1 + rows));
                ImVec2 sa = toScreen(a), sb = toScreen(b);
                bool selected = m_Selected == id;
                dl->AddRectFilled(sa, sb, ImGui::GetColorU32(selected ? ImGuiCol_HeaderHovered : ImGuiCol_Header), 3.0f);
                if (selected) dl->AddRect(sa, sb, accent, 3.0f, 0, 2.0f);
                std::string name = n->Type;
                if (t && !t->Options.empty())
                    name += std::string(": ") + VisualEffects::AttributeName((VfxAttribute)std::clamp(n->Option, 0, (int)VfxAttribute::Count - 1));
                if (n->Type == "Trigger On Die")
                {
                    auto* child = AssetManager::GetMetadata(n->Child);
                    name += std::string(" -> ") + (child ? child->Path.stem().string() : "(no child)");
                }
                label(ImVec2(sa.x + 6.0f * z, sa.y + 3.0f * z), n->Enabled ? text : dim, name.c_str());
                ImGui::SetCursorScreenPos(sa);
                ImGui::PushID((int)id);
                ImGui::InvisibleButton("##block", ImVec2(sb.x - sa.x, sb.y - sa.y));
                if (ImGui::IsItemClicked()) m_Selected = id;
                if (ImGui::IsItemHovered()) hoveredNode = id;
                ImGui::PopID();
                for (int i = 0; t && i < rows; ++i)
                {
                    float ry = by + kRowH * (1 + i);
                    ImVec2 pin = toScreen(glm::vec2(at.x, ry + kRowH * 0.5f));
                    pins.push_back({ pin, id, i, ports[i].Kind });
                    std::string row = std::string(ports[i].Name);
                    if (!g.LinkInto(id, i)) row += "  " + ValueText(i < (int)n->Inputs.size() ? n->Inputs[i] : glm::vec4(0.0f), InputKind(*n, i));
                    label(toScreen(glm::vec2(a.x + 14.0f, ry + 2.0f)), dim, row.c_str());
                }
                by = b.y + 4.0f;
            }
            if (blocks.empty()) label(toScreen(glm::vec2(at.x + 10.0f, by)), dim, "(no blocks: + adds one)");
        };
        contextBlocks(initAt, initH, "Initialize Particle", g.Initialize, false);
        contextBlocks(updateAt, updateH, "Update Particle", g.Update, true);

        // Operators: free nodes, a row per input, the output on the title row's right.
        for (VfxNode& n : g.Nodes)
        {
            const auto* t = VisualEffects::FindType(n.Type);
            if (!t || !t->HasOutput) continue;
            auto ports = VisualEffects::InputsOf(n);
            int rows = (int)ports.size();
            std::string title = n.Type;
            if (n.Type == "Property") title = n.Option >= 0 && n.Option < (int)g.Properties.size() ? g.Properties[n.Option].Name : "(property?)";
            else if (n.Type == "Get Attribute") title = std::string("Get ") + VisualEffects::AttributeName((VfxAttribute)std::clamp(n.Option, 0, (int)VfxAttribute::Count - 1));
            else if (n.Type == "Split") title = std::string("Split ") + "XYZW"[std::clamp(n.Option, 0, 3)];
            else if (n.Type == "Subgraph")
            {
                auto* meta = AssetManager::GetMetadata(n.Child);
                title = meta ? meta->Path.stem().string() + " (subgraph)" : "Subgraph (none)";
            }
            float extra = (n.Type == "Sample Gradient" || n.Type == "Sample Curve") ? kRowH : 0.0f;
            glm::vec2 a = n.Position, b = n.Position + glm::vec2(kNodeW, kHeaderH + kRowH * (float)rows + extra + 4.0f);
            ImVec2 sa = toScreen(a), sb = toScreen(b);
            bool selected = m_Selected == n.Id;
            VfxValue outKind = t->Output;
            if (n.Type == "Property" && n.Option >= 0 && n.Option < (int)g.Properties.size()) outKind = g.Properties[n.Option].Type;
            dl->AddRectFilled(sa, sb, body, 5.0f);
            dl->AddRectFilled(sa, ImVec2(sb.x, sa.y + kHeaderH * z), ImGui::GetColorU32(ImGuiCol_Header), 5.0f, ImDrawFlags_RoundCornersTop);
            dl->AddRect(sa, sb, selected ? accent : border, 5.0f, 0, selected ? 2.0f : 1.0f);
            label(ImVec2(sa.x + 8.0f * z, sa.y + 4.0f * z), text, title.c_str());
            ImGui::SetCursorScreenPos(sa);
            ImGui::PushID((int)n.Id);
            ImGui::InvisibleButton("##node", ImVec2(kNodeW * z, kHeaderH * z));
            if (ImGui::IsItemActivated())
            {
                m_Selected = n.Id;
                m_Moving = false;
            }
            if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f))
            {
                if (!m_Moving) BeginChange();
                m_Moving = true;
                n.Position += glm::vec2(ImGui::GetIO().MouseDelta.x, ImGui::GetIO().MouseDelta.y) / z;
            }
            if (ImGui::IsItemDeactivated() && m_Moving)
            {
                m_Moving = false;
                Commit();
            }
            if (ImGui::IsItemHovered()) hoveredNode = n.Id;
            ImGui::PopID();
            ImVec2 out(sb.x + kPinR + 4.0f, sa.y + kHeaderH * 0.5f * z); // (just outside: dragging it does not move the node)
            outputs[n.Id] = out;
            pins.push_back({ out, n.Id, -1, outKind });
            for (int i = 0; i < rows; ++i)
            {
                float ry = a.y + kHeaderH + kRowH * (float)i;
                ImVec2 pin = toScreen(glm::vec2(a.x, ry + kRowH * 0.5f));
                pins.push_back({ pin, n.Id, i, ports[i].Kind });
                std::string row = ports[i].Name;
                if (!g.LinkInto(n.Id, i)) row += "  " + ValueText(i < (int)n.Inputs.size() ? n.Inputs[i] : ports[i].Default, ports[i].Kind);
                label(toScreen(glm::vec2(a.x + 12.0f, ry + 2.0f)), dim, row.c_str());
                if (ports[i].Kind == VfxValue::Color && !g.LinkInto(n.Id, i) && i < (int)n.Inputs.size())
                {
                    ImVec2 c = toScreen(glm::vec2(b.x - 30.0f, ry + 4.0f)), d = toScreen(glm::vec2(b.x - 8.0f, ry + 16.0f));
                    const glm::vec4& v = n.Inputs[i];
                    dl->AddRectFilled(c, d, ImGui::ColorConvertFloat4ToU32(ImVec4(v.x, v.y, v.z, 1.0f)), 2.0f);
                }
            }
            if (extra > 0.0f)
            {
                // The gradient or curve, small.
                ImVec2 c0 = toScreen(glm::vec2(a.x + 10.0f, b.y - kRowH - 2.0f)), c1 = toScreen(glm::vec2(b.x - 10.0f, b.y - 4.0f));
                const int steps = 24;
                for (int k = 0; k < steps; ++k)
                {
                    float t0 = (float)k / steps, t1 = (float)(k + 1) / steps;
                    float x0 = c0.x + (c1.x - c0.x) * t0, x1 = c0.x + (c1.x - c0.x) * t1;
                    if (n.Type == "Sample Gradient")
                    {
                        glm::vec4 col = n.Colors.Evaluate((t0 + t1) * 0.5f);
                        dl->AddRectFilled(ImVec2(x0, c0.y), ImVec2(x1, c1.y), ImGui::ColorConvertFloat4ToU32(ImVec4(col.r, col.g, col.b, 1.0f)));
                    }
                    else
                    {
                        auto yOf = [&](float v) { return c1.y - (c1.y - c0.y) * std::clamp(v, 0.0f, 1.0f); };
                        dl->AddLine(ImVec2(x0, yOf(n.Values.Evaluate(t0))), ImVec2(x1, yOf(n.Values.Evaluate(t1))), IM_COL32(150, 230, 140, 255), 1.5f);
                    }
                }
            }
        }
        // Context blocks' title rows sit inside the contexts; Color Over Life / Size Over Life show their tables there.

        // Links: from an operator's output to an input pin, a curve colored by the value's kind.
        auto findPin = [&](uint32_t node, int input) -> const Pin* {
            for (const Pin& p : pins)
                if (p.Node == node && p.Input == input) return &p;
            return nullptr;
        };
        for (const VfxLink& l : g.Links)
        {
            auto from = outputs.find(l.From);
            const Pin* to = findPin(l.To, l.Input);
            if (from == outputs.end() || !to) continue;
            ImVec2 a = from->second, b = to->Pos;
            float dx = std::max(std::abs(b.x - a.x) * 0.5f, 40.0f);
            const Pin* src = findPin(l.From, -1);
            dl->AddBezierCubic(a, ImVec2(a.x + dx, a.y), ImVec2(b.x - dx, b.y), b, src ? KindColor(src->Kind) : text, std::max(2.0f * z, 1.0f));
        }
        // Pins over everything; dragging from an output (or from a linked input, picking its link up) makes a link.
        ImVec2 mouse = ImGui::GetMousePos();
        const Pin* hoveredPin = nullptr;
        for (const Pin& p : pins)
        {
            bool linked = p.Input < 0 ? std::any_of(g.Links.begin(), g.Links.end(), [&](const VfxLink& l) { return l.From == p.Node; })
                                      : g.LinkInto(p.Node, p.Input) != nullptr;
            float d2 = (mouse.x - p.Pos.x) * (mouse.x - p.Pos.x) + (mouse.y - p.Pos.y) * (mouse.y - p.Pos.y);
            bool hot = d2 < (kPinR + 4.0f) * (kPinR + 4.0f);
            if (hot) hoveredPin = &p;
            ImU32 c = KindColor(p.Kind);
            if (linked || hot) dl->AddCircleFilled(p.Pos, kPinR, c);
            else dl->AddCircle(p.Pos, kPinR, c, 12, 1.5f);
        }
        if (hoveredPin && !m_DragFrom && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem))
        {
            if (hoveredPin->Input < 0) m_DragFrom = hoveredPin->Node;
            else if (const VfxLink* l = g.LinkInto(hoveredPin->Node, hoveredPin->Input))
            {
                // Pick the link up from its end.
                BeginChange();
                m_DragFrom = l->From;
                uint32_t to = hoveredPin->Node;
                int input = hoveredPin->Input;
                std::erase_if(g.Links, [&](const VfxLink& k) { return k.To == to && k.Input == input; });
                m_PickedUp = true;
            }
        }
        if (m_DragFrom)
        {
            auto from = outputs.find(m_DragFrom);
            if (from != outputs.end())
            {
                ImVec2 a = from->second;
                float dx = std::max(std::abs(mouse.x - a.x) * 0.5f, 40.0f);
                dl->AddBezierCubic(a, ImVec2(a.x + dx, a.y), ImVec2(mouse.x - dx, mouse.y), mouse, accent, 2.0f);
            }
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
            {
                if (hoveredPin && hoveredPin->Input >= 0)
                {
                    if (!m_PickedUp) BeginChange();
                    if (!g.Link(m_DragFrom, hoveredPin->Node, hoveredPin->Input) && !m_PickedUp) m_History.pop_back(); // (nothing changed)
                    Commit();
                }
                else if (m_PickedUp)
                    Commit(); // (the link was dropped on nothing: it stays removed)
                m_DragFrom = 0;
                m_PickedUp = false;
            }
        }
        if (hoveredPin)
        {
            if (const VfxNode* pn = g.Find(hoveredPin->Node); pn && hoveredPin->Input >= 0)
            {
                auto ports = VisualEffects::InputsOf(*pn);
                if (hoveredPin->Input < (int)ports.size()) ImGui::SetTooltip("%s", ports[hoveredPin->Input].Name);
            }
        }
        else if (hoveredNode)
            if (const VfxNode* n = g.Find(hoveredNode))
                if (const auto* t = VisualEffects::FindType(n->Type)) ImGui::SetItemTooltip("%s", t->Help);

        // Delete removes the selected node.
        if (m_Selected && ImGui::IsWindowFocused() && !ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Delete, false))
        {
            BeginChange();
            g.Remove(m_Selected);
            m_Selected = 0;
            Commit();
        }

        // Right click on nothing: an operator, by menu.
        if (ImGui::BeginPopup("##addnode"))
        {
            std::map<std::string, std::vector<const VisualEffects::NodeType*>> menus;
            for (const auto& t : VisualEffects::NodeTypes())
                if (t.HasOutput)
                {
                    std::string menu = t.Menu;
                    menus[menu.substr(0, menu.find('/'))].push_back(&t);
                }
            for (auto& [menu, types] : menus)
                if (ImGui::BeginMenu(menu.c_str()))
                {
                    for (const auto* t : types)
                    {
                        if (ImGui::MenuItem(t->Name))
                        {
                            BeginChange();
                            m_Selected = g.Add(t->Name, m_AddAt).Id;
                            Commit();
                        }
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", t->Help);
                    }
                    ImGui::EndMenu();
                }
            ImGui::EndPopup();
        }
        ImGui::PopClipRect();
        dl->AddRectFilled(ImVec2(canvasMin.x, canvasMax.y - 24.0f), canvasMax, ImGui::GetColorU32(ImGuiCol_WindowBg, 0.85f));
        dl->AddText(ImVec2(canvasMin.x + 8.0f, canvasMax.y - 20.0f), dim,
                    "Right-click: add an operator. Drag from an output to an input to link. Right/middle-drag pans, F frames. Ctrl+Z undoes.");
    }
}
