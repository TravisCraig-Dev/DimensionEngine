#include "Panels.h"
#include "../EditorUI.h"
#include <Dimension/Animation/AnimatorController.h>
#include <Dimension/Renderer/Mesh.h>
#include <imgui.h>
#include <imgui_stdlib.h>
#include <cmath>
#include <cstring>

namespace Editor
{
    namespace
    {
        constexpr float kNodeW = 170.0f, kNodeH = 44.0f;
        const char* kOpNames[] = { ">", "<", "==", "!=", "is true", "is false" };
        const char* kTypeNames[] = { "Float", "Int", "Bool", "Trigger" };

        ImVec2 ToV(const glm::vec2& v) { return ImVec2(v.x, v.y); }

        // A 2D blend tree's space: its children as dots (sized by weight) and the blend point. While playing the point
        // follows the live parameters; otherwise it can be dragged to preview the weights.
        void Draw2DBlendSpace(const AnimState& st, std::vector<float>& weights, const AnimatorController* c, const AnimatorRuntime* runtime)
        {
            ImGuiStorage* storage = ImGui::GetStateStorage();
            ImGuiID px = ImGui::GetID("##previewx"), py = ImGui::GetID("##previewy");
            glm::vec2 point(storage->GetFloat(px, 0.0f), storage->GetFloat(py, 0.0f));
            bool livePoint = c && runtime;
            if (livePoint)
                point = { AnimatorStateMachine::GetParameter(*c, *runtime, st.BlendParameter), AnimatorStateMachine::GetParameter(*c, *runtime, st.BlendParameterY) };

            glm::vec2 lo(-1.0f), hi(1.0f);
            for (auto& ch : st.Children)
            {
                lo = glm::min(lo, glm::vec2(ch.Threshold, ch.ThresholdY));
                hi = glm::max(hi, glm::vec2(ch.Threshold, ch.ThresholdY));
            }
            glm::vec2 margin = (hi - lo) * 0.12f;
            lo -= margin;
            hi += margin;
            float width = ImGui::GetContentRegionAvail().x, height = std::min(170.0f, width * 0.6f);
            ImVec2 p0 = ImGui::GetCursorScreenPos(), p1(p0.x + width, p0.y + height);
            ImGui::InvisibleButton("##blendspace", ImVec2(width, height));
            auto toScreen = [&](glm::vec2 v) {
                return ImVec2(p0.x + (v.x - lo.x) / (hi.x - lo.x) * width, p1.y - (v.y - lo.y) / (hi.y - lo.y) * height);
            };
            if (!livePoint && ImGui::IsItemActive())
            {
                ImVec2 m = ImGui::GetIO().MousePos;
                point = { lo.x + (m.x - p0.x) / width * (hi.x - lo.x), lo.y + (p1.y - m.y) / height * (hi.y - lo.y) };
                point = glm::clamp(point, lo, hi);
                storage->SetFloat(px, point.x);
                storage->SetFloat(py, point.y);
            }
            if (!livePoint) AnimatorStateMachine::BlendWeightsAt(st, point, weights);

            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(p0, p1, IM_COL32(28, 28, 32, 255), 3.0f);
            ImVec2 o = toScreen({ 0.0f, 0.0f });
            dl->AddLine(ImVec2(p0.x, o.y), ImVec2(p1.x, o.y), IM_COL32(60, 60, 70, 255));
            dl->AddLine(ImVec2(o.x, p0.y), ImVec2(o.x, p1.y), IM_COL32(60, 60, 70, 255));
            if (st.Motion == AnimMotionType::BlendTree2DDirectional) // directions are spokes from the center
                for (auto& ch : st.Children)
                    dl->AddLine(o, toScreen({ ch.Threshold, ch.ThresholdY }), IM_COL32(70, 80, 100, 255));
            int hovered = -1;
            ImVec2 mouse = ImGui::GetIO().MousePos;
            for (int k = 0; k < (int)st.Children.size(); ++k)
            {
                ImVec2 s = toScreen({ st.Children[k].Threshold, st.Children[k].ThresholdY });
                float w = k < (int)weights.size() ? weights[k] : 0.0f;
                dl->AddCircleFilled(s, 4.0f + 8.0f * w, w > 0.0f ? IM_COL32(110, 200, 255, 255) : IM_COL32(120, 125, 135, 255));
                char label[16];
                std::snprintf(label, sizeof(label), "%d", k + 1);
                dl->AddText(ImVec2(s.x + 7.0f + 8.0f * w, s.y - 8.0f), IM_COL32(220, 220, 225, 255), label);
                if (ImGui::IsItemHovered() && std::hypot(mouse.x - s.x, mouse.y - s.y) < 8.0f) hovered = k;
            }
            ImVec2 b = toScreen(point);
            dl->AddLine(ImVec2(b.x - 6, b.y), ImVec2(b.x + 6, b.y), IM_COL32(255, 90, 80, 255), 2.0f);
            dl->AddLine(ImVec2(b.x, b.y - 6), ImVec2(b.x, b.y + 6), IM_COL32(255, 90, 80, 255), 2.0f);
            if (hovered >= 0)
                ImGui::SetTooltip("%d: %s (%.2f, %.2f)", hovered + 1, st.Children[hovered].Clip.empty() ? "(no clip)" : st.Children[hovered].Clip.c_str(),
                                  st.Children[hovered].Threshold, st.Children[hovered].ThresholdY);
            else if (ImGui::IsItemHovered() && !livePoint)
                ImGui::SetTooltip("Drag to preview the blend at (%.2f, %.2f)", point.x, point.y);
        }

        void Arrow(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col, float offset)
        {
            ImVec2 d(b.x - a.x, b.y - a.y);
            float len = std::sqrt(d.x * d.x + d.y * d.y);
            if (len < 1.0f) return;
            ImVec2 n(d.x / len, d.y / len), perp(-n.y, n.x);
            a = ImVec2(a.x + perp.x * offset, a.y + perp.y * offset);
            b = ImVec2(b.x + perp.x * offset, b.y + perp.y * offset);
            dl->AddLine(a, b, col, 2.0f);
            ImVec2 m((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
            ImVec2 tip(m.x + n.x * 7, m.y + n.y * 7);
            dl->AddTriangleFilled(tip, ImVec2(m.x - n.x * 5 + perp.x * 6, m.y - n.y * 5 + perp.y * 6),
                                  ImVec2(m.x - n.x * 5 - perp.x * 6, m.y - n.y * 5 - perp.y * 6), col);
        }
    }

    void AnimatorPanel::OnImGui(bool* open)
    {
        ImGui::SetNextWindowSize(ImVec2(900, 420), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Animator", open))
        {
            ImGui::End();
            return;
        }

        // Target: the selected entity's controller (with live state), else a selected controller asset.
        Scene& scene = m_Ctx.ActiveScene();
        Entity entity = Entity::Null();
        if (m_Ctx.HasValidSelection())
            if (auto* anim = scene.GetRegistry().TryGet<AnimatorComponent>(m_Ctx.Selected); anim && anim->Controller.IsValid())
            {
                m_Handle = AssetManager::GetBaseController(anim->Controller); // an override's machine is its controller's
                entity = m_Ctx.Selected;
            }
        if (auto* meta = AssetManager::GetMetadata(m_Ctx.SelectedAsset); meta && meta->Type == AssetType::AnimatorController)
            m_Handle = m_Ctx.SelectedAsset;
        auto controller = AssetManager::GetAnimatorController(m_Handle);
        if (!controller)
        {
            ImGui::TextDisabled("Select an Animator Controller asset, or an entity whose Animator uses one.");
            ImGui::End();
            return;
        }
        AnimatorController& root = *controller;
        root.LinkLayers();
        bool changed = false;
        AnimatorComponent* live = entity.IsNull() ? nullptr : scene.GetRegistry().TryGet<AnimatorComponent>(entity);
        Ref<Mesh> mesh;
        if (!entity.IsNull())
            if (auto* mr = scene.GetRegistry().TryGet<MeshRendererComponent>(entity)) mesh = AssetManager::GetMesh(mr->Mesh);

        ImGui::TextDisabled("%s%s", AssetManager::GetDisplayName(m_Handle).c_str(),
                            live ? ("   -   " + scene.GetRegistry().Get<TagComponent>(entity).Name).c_str() : "");

        if (ImGui::BeginTable("AnimatorLayout", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV))
        {
            ImGui::TableSetupColumn("Sidebar", ImGuiTableColumnFlags_WidthFixed, 300.0f);
            ImGui::TableSetupColumn("Graph", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableNextRow();

            // ---------------- Sidebar: parameters + selected state ----------------
            ImGui::TableSetColumnIndex(0);
            ImGui::BeginChild("Sidebar");
            ImGui::SeparatorText("Parameters");
            int removeParam = -1;
            for (int i = 0; i < (int)root.Parameters.size(); ++i)
            {
                auto& p = root.Parameters[i];
                ImGui::PushID(i);
                ImGui::SetNextItemWidth(110);
                changed |= ImGui::InputText("##name", &p.Name);
                ImGui::SameLine();
                ImGui::TextDisabled("%s", kTypeNames[(int)p.Type]);
                ImGui::SameLine(190);
                ImGui::SetNextItemWidth(70);
                // While playing, show and edit the live value of the selected entity.
                if (live && m_Ctx.IsPlaying())
                {
                    if (p.Type == AnimParameterType::Trigger)
                    {
                        if (ImGui::SmallButton("Fire")) live->SetTrigger(p.Name);
                    }
                    else
                    {
                        float v = AnimatorStateMachine::GetParameter(root, live->Runtime, p.Name);
                        if (p.Type == AnimParameterType::Bool)
                        {
                            bool b = v != 0.0f;
                            if (ImGui::Checkbox("##live", &b)) live->SetBool(p.Name, b);
                        }
                        else if (ImGui::DragFloat("##live", &v, p.Type == AnimParameterType::Int ? 1.0f : 0.01f)) live->SetFloat(p.Name, p.Type == AnimParameterType::Int ? std::round(v) : v);
                    }
                }
                else if (p.Type == AnimParameterType::Bool)
                {
                    bool b = p.Default != 0.0f;
                    if (ImGui::Checkbox("##def", &b)) { p.Default = b ? 1.0f : 0.0f; changed = true; }
                }
                else if (p.Type != AnimParameterType::Trigger)
                    changed |= ImGui::DragFloat("##def", &p.Default, p.Type == AnimParameterType::Int ? 1.0f : 0.01f);
                ImGui::SameLine(270);
                if (ImGui::SmallButton("x")) removeParam = i;
                ImGui::PopID();
            }
            if (removeParam >= 0) { root.Parameters.erase(root.Parameters.begin() + removeParam); changed = true; }
            for (int t = 0; t < 4; ++t)
            {
                if (t) ImGui::SameLine();
                if (ImGui::SmallButton((std::string("+ ") + kTypeNames[t]).c_str()))
                {
                    root.Parameters.push_back({ std::string("New") + kTypeNames[t], (AnimParameterType)t, 0.0f });
                    changed = true;
                }
            }

            // ---------------- Layers ----------------
            ImGui::SeparatorText("Layers");
            m_Layer = std::clamp(m_Layer, 0, (int)root.Layers.size());
            if (ImGui::Selectable("Base Layer", m_Layer == 0)) { m_Layer = 0; m_SelectedState = -1; m_ViewGroup = m_SelectedGroup = -1; }
            int removeLayer = -1;
            for (int li = 0; li < (int)root.Layers.size(); ++li)
            {
                ImGui::PushID(1000 + li);
                const auto& layer = root.Layers[li];
                std::string label = layer.Name + (layer.Blending == AnimLayerBlending::Additive ? "  (additive)" : "") + (layer.SyncedLayer >= 0 ? "  (synced)" : "");
                if (ImGui::Selectable(label.c_str(), m_Layer == li + 1)) { m_Layer = li + 1; m_SelectedState = -1; m_ViewGroup = m_SelectedGroup = -1; }
                ImGui::PopID();
            }
            if (ImGui::SmallButton("+ Layer"))
            {
                AnimLayer layer;
                layer.Name = "Layer " + std::to_string(root.Layers.size() + 1);
                root.Layers.push_back(std::move(layer));
                root.LinkLayers();
                m_Layer = (int)root.Layers.size();
                m_SelectedState = -1;
                changed = true;
            }
            if (m_Layer > 0)
            {
                AnimLayer& layer = root.Layers[m_Layer - 1];
                ImGui::SameLine();
                if (ImGui::SmallButton("Remove Layer")) removeLayer = m_Layer - 1;
                ImGui::SetNextItemWidth(180);
                changed |= ImGui::InputText("Name##layer", &layer.Name);
                ImGui::SetNextItemWidth(180);
                changed |= ImGui::SliderFloat("Weight##layer", &layer.Weight, 0.0f, 1.0f);
                int blending = (int)layer.Blending;
                ImGui::SetNextItemWidth(180);
                if (ImGui::Combo("Blending##layer", &blending, "Override\0Additive\0")) { layer.Blending = (AnimLayerBlending)blending; changed = true; }
                // Sync: follow an earlier, unsynced layer's state machine with this layer's own motions.
                auto layerName = [&](int index) { return index == 0 ? std::string("Base Layer") : root.Layers[index - 1].Name; };
                ImGui::SetNextItemWidth(180);
                if (ImGui::BeginCombo("Sync##layer", layer.SyncedLayer < 0 ? "(none)" : layerName(layer.SyncedLayer).c_str()))
                {
                    if (ImGui::Selectable("(none)", layer.SyncedLayer < 0))
                    {
                        layer.SyncedLayer = -1;
                        changed = true;
                    }
                    for (int s = 0; s < m_Layer; ++s)
                    {
                        if (s > 0 && root.Layers[s - 1].SyncedLayer >= 0) continue;
                        if (ImGui::Selectable(layerName(s).c_str(), layer.SyncedLayer == s))
                        {
                            if (layer.SyncedLayer < 0) // start from "play the source's motion" in every state
                            {
                                layer.Machine.States.clear();
                                layer.Machine.Transitions.clear();
                            }
                            layer.SyncedLayer = s;
                            changed = true;
                        }
                    }
                    ImGui::EndCombo();
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("A synced layer runs the chosen layer's states and transitions, playing its own motion for each state\n"
                                      "(e.g. a wounded version of every locomotion state). States without a motion play the source's.");
                if (live && m_Ctx.IsPlaying() && (int)live->Layers.size() >= m_Layer)
                {
                    float w = live->Layers[m_Layer - 1].Weight >= 0.0f ? live->Layers[m_Layer - 1].Weight : layer.Weight;
                    ImGui::SetNextItemWidth(180);
                    if (ImGui::SliderFloat("Live Weight##layer", &w, 0.0f, 1.0f)) live->SetLayerWeight(m_Layer, w);
                }
                // Mask: an Avatar Mask asset (shared with other layers and controllers), or this layer's own joints;
                // either way joints with their descendants, none = the whole body.
                auto maskAsset = layer.MaskAsset.IsValid() ? AssetManager::GetAvatarMask(layer.MaskAsset) : nullptr;
                ImGui::SetNextItemWidth(200);
                std::string maskName = maskAsset ? AssetManager::GetDisplayName(layer.MaskAsset) : std::string("(this layer's own joints)");
                if (ImGui::BeginCombo("Mask##layerMask", maskName.c_str()))
                {
                    if (ImGui::Selectable("(this layer's own joints)", !maskAsset)) { layer.MaskAsset = AssetHandle::None(); changed = true; }
                    for (AssetHandle h : AssetManager::GetAssetsOfType(AssetType::AvatarMask))
                        if (ImGui::Selectable(AssetManager::GetDisplayName(h).c_str(), h == layer.MaskAsset)) { layer.MaskAsset = h; changed = true; }
                    ImGui::EndCombo();
                }
                ImGui::SetItemTooltip("An Avatar Mask asset (.mask) shared by the layers that use it, or this layer's own list");
                if (!maskAsset)
                {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Save As Asset"))
                    {
                        // Beside the controller, named after the layer.
                        fs::path folder = AssetManager::GetAbsolutePath(m_Handle).parent_path();
                        std::string base;
                        for (char ch : layer.Name) base += std::strchr("<>:\"/\\|?*", ch) || (unsigned char)ch < 32 ? '_' : ch;
                        fs::path file = folder / (base + " Mask.mask");
                        for (int n = 1; fs::exists(file); ++n) file = folder / (base + " Mask " + std::to_string(n) + ".mask");
                        AvatarMaskAsset made;
                        made.Joints = layer.Mask;
                        if (AssetHandle h = AssetManager::CreateAvatarMask(file, made); h.IsValid())
                        {
                            layer.MaskAsset = h;
                            changed = true;
                        }
                    }
                    ImGui::SetItemTooltip("Make this list an Avatar Mask asset (other layers can then use it too)");
                }
                std::vector<std::string>& maskJoints = maskAsset ? maskAsset->Joints : layer.Mask;
                bool maskChanged = false;
                ImGui::TextDisabled("Mask: %s%s", maskJoints.empty() ? "whole skeleton" : "these joints and their children", maskAsset ? " (the asset's, shared)" : "");
                int removeJoint = -1;
                for (int mj = 0; mj < (int)maskJoints.size(); ++mj)
                {
                    ImGui::PushID(2000 + mj);
                    ImGui::BulletText("%s", maskJoints[mj].c_str());
                    ImGui::SameLine();
                    if (ImGui::SmallButton("x")) removeJoint = mj;
                    ImGui::PopID();
                }
                if (removeJoint >= 0) { maskJoints.erase(maskJoints.begin() + removeJoint); maskChanged = true; }
                const Skeleton* skeleton = mesh ? mesh->GetSkeleton() : nullptr;
                if (skeleton)
                {
                    ImGui::SetNextItemWidth(180);
                    if (ImGui::BeginCombo("##addjoint", "+ Add joint"))
                    {
                        for (int j : skeleton->Order)
                        {
                            int depth = 0;
                            for (int q = skeleton->Joints[j].Parent; q >= 0; q = skeleton->Joints[q].Parent) ++depth;
                            std::string item = std::string(depth * 2, ' ') + skeleton->Joints[j].Name;
                            if (ImGui::Selectable(item.c_str()))
                            {
                                maskJoints.push_back(skeleton->Joints[j].Name);
                                maskChanged = true;
                            }
                        }
                        ImGui::EndCombo();
                    }
                }
                else
                    ImGui::TextDisabled("Select an entity using this controller to pick mask joints.");
                if (maskChanged)
                {
                    if (maskAsset) AssetManager::SaveAvatarMask(layer.MaskAsset); // (every layer using it takes it up)
                    else changed = true;
                }
            }
            if (removeLayer >= 0)
            {
                root.Layers.erase(root.Layers.begin() + removeLayer);
                root.LinkLayers();
                m_Layer = 0;
                m_SelectedState = -1;
                changed = true;
            }

            // The rest of the window edits the selected layer's state machine. A synced layer mirrors its source's
            // states and transitions; only each state's motion is its own.
            root.SyncLayerStates();
            AnimatorController& c = m_Layer == 0 ? root : root.Layers[m_Layer - 1].Machine;
            const bool syncedLayer = m_Layer > 0 && root.Layers[m_Layer - 1].SyncedLayer >= 0;
            AnimatorRuntime emptyRuntime;
            const AnimatorRuntime* liveRuntime = &emptyRuntime;
            float liveFadeDuration = 0.0f, liveFadeElapsed = 0.0f;
            std::string liveState;
            if (live)
            {
                if (m_Layer == 0)
                {
                    liveRuntime = &live->Runtime;
                    liveFadeDuration = live->FadeDuration;
                    liveFadeElapsed = live->FadeElapsed;
                    liveState = live->CurrentState;
                }
                else if ((int)live->Layers.size() >= m_Layer)
                {
                    const auto& lp = live->Layers[m_Layer - 1];
                    liveRuntime = &lp.Runtime;
                    liveFadeDuration = lp.FadeDuration;
                    liveFadeElapsed = lp.FadeElapsed;
                    liveState = lp.CurrentState;
                }
            }

            bool anySelected = m_SelectedState == -2;
            bool entrySelected = m_SelectedState == -3; // the Entry node of the shown level
            if (m_SelectedState >= (int)c.States.size()) m_SelectedState = -1;
            if (m_SelectedGroup >= (int)c.Groups.size()) m_SelectedGroup = -1;
            if (m_ViewGroup >= (int)c.Groups.size()) m_ViewGroup = -1;
            bool groupSelected = m_SelectedGroup >= 0 && m_SelectedState < 0;
            if (groupSelected)
            {
                auto& grp = c.Groups[m_SelectedGroup];
                ImGui::SeparatorText("Sub-State Machine");
                if (syncedLayer) ImGui::Text("%s", grp.Name.c_str());
                else changed |= ImGui::InputText("Name##group", &grp.Name);
                int entry = c.EntryState(m_SelectedGroup);
                if (!syncedLayer && ImGui::BeginCombo("Entry State", entry >= 0 ? c.States[entry].Name.c_str() : "(no states)"))
                {
                    for (int k = 0; k < (int)c.States.size(); ++k)
                        if (c.IsInGroup(k, m_SelectedGroup) && ImGui::Selectable(c.States[k].Name.c_str(), k == entry)) { grp.DefaultState = k; changed = true; }
                    ImGui::EndCombo();
                }
                ImGui::TextDisabled("Transitions into it start at the entry state; its transitions below apply from any state inside it.");
            }
            if (m_SelectedState >= 0 || anySelected || groupSelected || entrySelected)
            {
                if (entrySelected)
                    ImGui::TextDisabled("Entering %s: the first entry transition whose conditions hold picks the state;\n"
                                        "otherwise the %s.", m_ViewGroup >= 0 ? "this sub-state machine" : "the layer",
                                        m_ViewGroup >= 0 ? "Entry State" : "default state");
                if (!anySelected && !groupSelected && !entrySelected)
                {
                    auto& st = c.States[m_SelectedState];
                    ImGui::SeparatorText("State");
                    if (syncedLayer)
                    {
                        ImGui::Text("%s", st.Name.c_str());
                        ImGui::TextDisabled(HasOwnMotion(st) ? "This layer's motion for the synced state:" : "Plays the source layer's motion (set one to replace it):");
                    }
                    else
                        changed |= ImGui::InputText("Name", &st.Name);
                    // Clip picker: the preview mesh's clips when available, else free text.
                    auto clipCombo = [&](const char* label, std::string& value) {
                        if (mesh && !mesh->GetClips().empty())
                        {
                            if (ImGui::BeginCombo(label, value.empty() ? "(none)" : value.c_str()))
                            {
                                for (auto& clip : mesh->GetClips())
                                    if (ImGui::Selectable(clip.Name.c_str(), clip.Name == value)) { value = clip.Name; changed = true; }
                                ImGui::EndCombo();
                            }
                        }
                        else
                            changed |= ImGui::InputText(label, &value);
                    };
                    int motion = (int)st.Motion;
                    if (ImGui::Combo("Motion", &motion, "Clip\0Blend Tree 1D\0Blend Tree 2D Freeform\0Blend Tree 2D Directional\0"))
                    {
                        st.Motion = (AnimMotionType)motion;
                        changed = true;
                    }
                    if (ImGui::IsItemHovered() && IsBlendTree2D(st.Motion))
                        ImGui::SetTooltip(st.Motion == AnimMotionType::BlendTree2DDirectional
                                              ? "One motion per direction (e.g. walk forward, back, left, right), optionally one at (0, 0) for idle"
                                              : "Any layout of motions; nearby motions blend (e.g. several speeds per direction)");
                    bool tree2D = IsBlendTree2D(st.Motion);
                    if (st.Motion == AnimMotionType::Clip)
                        clipCombo("Clip", st.Clip);
                    else
                    {
                        auto parameterCombo = [&](const char* label, std::string& value) {
                            if (ImGui::BeginCombo(label, value.empty() ? "(none)" : value.c_str()))
                            {
                                for (auto& p : root.Parameters)
                                    if (p.Type == AnimParameterType::Float && ImGui::Selectable(p.Name.c_str(), p.Name == value))
                                    {
                                        value = p.Name;
                                        changed = true;
                                    }
                                ImGui::EndCombo();
                            }
                        };
                        parameterCombo(tree2D ? "Parameter X" : "Parameter", st.BlendParameter);
                        if (tree2D) parameterCombo("Parameter Y", st.BlendParameterY);
                        std::vector<float> weights;
                        if (live && m_Ctx.IsPlaying()) AnimatorStateMachine::TreeWeights(c, *liveRuntime, st, weights);
                        if (tree2D) Draw2DBlendSpace(st, weights, live && m_Ctx.IsPlaying() ? &c : nullptr, live ? liveRuntime : nullptr);
                        int removeChild = -1;
                        for (int k = 0; k < (int)st.Children.size(); ++k)
                        {
                            ImGui::PushID(k + 5000);
                            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * (tree2D ? 0.38f : 0.45f));
                            clipCombo("##clip", st.Children[k].Clip);
                            ImGui::SameLine();
                            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - (weights.empty() ? 28 : 72));
                            if (tree2D) changed |= ImGui::DragFloat2("##position", &st.Children[k].Threshold, 0.01f);
                            else changed |= ImGui::DragFloat("##threshold", &st.Children[k].Threshold, 0.01f);
                            if (k < (int)weights.size())
                            {
                                ImGui::SameLine();
                                ImGui::TextDisabled("%3.0f%%", weights[k] * 100.0f);
                            }
                            ImGui::SameLine();
                            if (ImGui::SmallButton("x")) removeChild = k;
                            ImGui::PopID();
                        }
                        if (removeChild >= 0) { st.Children.erase(st.Children.begin() + removeChild); changed = true; }
                        if (ImGui::Button("+ Motion"))
                        {
                            BlendTreeChild child;
                            if (tree2D)
                            {
                                // Fill the four directions around the center first.
                                static const glm::vec2 spots[] = { { 0, 0 }, { 0, 1 }, { 1, 0 }, { 0, -1 }, { -1, 0 } };
                                glm::vec2 spot = st.Children.size() < 5 ? spots[st.Children.size()] : glm::vec2(1.0f, 1.0f);
                                child.Threshold = spot.x;
                                child.ThresholdY = spot.y;
                            }
                            else
                                child.Threshold = st.Children.empty() ? 0.0f : st.Children.back().Threshold + 1.0f;
                            st.Children.push_back(child);
                            changed = true;
                        }
                        ImGui::SameLine();
                        ImGui::TextDisabled(tree2D ? "clip / position (X, Y)" : "clip / threshold");
                    }
                    if (syncedLayer)
                    {
                        if (HasOwnMotion(st) && ImGui::Button("Use Source Motion"))
                        {
                            st.Motion = AnimMotionType::Clip;
                            st.Clip.clear();
                            st.Children.clear();
                            changed = true;
                        }
                    }
                    else
                    {
                        changed |= ImGui::DragFloat("Speed", &st.Speed, 0.01f, -10.0f, 10.0f);
                        changed |= ImGui::Checkbox("Loop", &st.Loop);
                        changed |= ImGui::InputTextWithHint("Tag", "for AnimatorStateInfo.IsTag", &st.Tag);

                        ImGui::SeparatorText("Behaviours");
                        int removeBehaviour = -1;
                        for (int k = 0; k < (int)st.Behaviours.size(); ++k)
                        {
                            ImGui::PushID(k + 7000);
                            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 28);
                            changed |= ImGui::InputTextWithHint("##behaviour", "C# class, e.g. Game.Footsteps", &st.Behaviours[k]);
                            ImGui::SameLine();
                            if (ImGui::SmallButton("x")) removeBehaviour = k;
                            ImGui::PopID();
                        }
                        if (removeBehaviour >= 0) { st.Behaviours.erase(st.Behaviours.begin() + removeBehaviour); changed = true; }
                        if (ImGui::Button("+ Behaviour")) { st.Behaviours.emplace_back(); changed = true; }
                        ImGui::SameLine();
                        ImGui::TextDisabled("StateMachineBehaviour: OnStateEnter / Update / Exit");

                        ImGui::SeparatorText("Events");
                        int removeEvent = -1;
                        for (int k = 0; k < (int)st.Events.size(); ++k)
                        {
                            ImGui::PushID(k + 6000);
                            ImGui::SetNextItemWidth(70);
                            changed |= ImGui::SliderFloat("##time", &st.Events[k].Time, 0.0f, 1.0f, "%.2f");
                            ImGui::SameLine();
                            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 28);
                            changed |= ImGui::InputText("##fn", &st.Events[k].Function);
                            ImGui::SameLine();
                            if (ImGui::SmallButton("x")) removeEvent = k;
                            ImGui::PopID();
                        }
                        if (removeEvent >= 0) { st.Events.erase(st.Events.begin() + removeEvent); changed = true; }
                        if (ImGui::Button("+ Event")) { st.Events.push_back({}); changed = true; }
                        ImGui::SameLine();
                        ImGui::TextDisabled("normalized time / OnAnimationEvent name");
                        ImGui::BeginDisabled(c.DefaultState == m_SelectedState);
                        if (ImGui::Button("Set as Default State")) { c.DefaultState = m_SelectedState; changed = true; }
                        ImGui::EndDisabled();
                    }
                }
                if (!syncedLayer) // a synced layer's transitions belong to its source
                {
                    ImGui::SeparatorText(anySelected ? "Transitions from Any State" : groupSelected ? "Transitions from the Sub-State Machine"
                                         : entrySelected ? "Entry Transitions" : "Transitions");
                    int from = anySelected ? -1 : groupSelected ? AnimTransition::FromGroup(m_SelectedGroup)
                             : entrySelected ? AnimTransition::FromEntry(m_ViewGroup) : m_SelectedState;
                    int removeTransition = -1;
                    for (int ti = 0; ti < (int)c.Transitions.size(); ++ti)
                    {
                        auto& t = c.Transitions[ti];
                        if (t.From != from) continue;
                        ImGui::PushID(ti);
                        std::string to = t.ToExit ? std::string("Exit")
                                       : t.ToGroup >= 0 && t.ToGroup < (int)c.Groups.size() ? c.Groups[t.ToGroup].Name + " (entry)"
                                       : t.To >= 0 && t.To < (int)c.States.size() ? c.States[t.To].Name : "?";
                        bool openT = ImGui::TreeNodeEx("##t", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap, "-> %s", to.c_str());
                        ImGui::SameLine(270);
                        if (ImGui::SmallButton("x")) removeTransition = ti;
                        if (openT)
                        {
                            if (!entrySelected) // entering picks a state at once: no timing
                            {
                                changed |= ImGui::Checkbox("Exit Time", &t.HasExitTime);
                                if (t.HasExitTime)
                                {
                                    ImGui::SameLine();
                                    ImGui::SetNextItemWidth(80);
                                    changed |= ImGui::DragFloat("##exit", &t.ExitTime, 0.01f, 0.0f, 10.0f);
                                }
                                ImGui::SetNextItemWidth(120);
                                changed |= ImGui::DragFloat("Blend (s)", &t.Duration, 0.01f, 0.0f, 10.0f);
                            }
                            int removeCond = -1;
                            for (int ci = 0; ci < (int)t.Conditions.size(); ++ci)
                            {
                                auto& cond = t.Conditions[ci];
                                ImGui::PushID(ci);
                                ImGui::SetNextItemWidth(100);
                                if (ImGui::BeginCombo("##param", cond.Parameter.empty() ? "(param)" : cond.Parameter.c_str()))
                                {
                                    for (auto& p : root.Parameters)
                                        if (ImGui::Selectable(p.Name.c_str(), p.Name == cond.Parameter))
                                        {
                                            cond.Parameter = p.Name;
                                            if (p.Type == AnimParameterType::Bool || p.Type == AnimParameterType::Trigger) cond.Op = AnimConditionOp::IsTrue;
                                            else if (cond.Op >= AnimConditionOp::IsTrue) cond.Op = AnimConditionOp::Greater;
                                            changed = true;
                                        }
                                    ImGui::EndCombo();
                                }
                                const AnimParameter* param = c.FindParameter(cond.Parameter);
                                AnimParameterType type = param ? param->Type : AnimParameterType::Float;
                                if (type != AnimParameterType::Trigger)
                                {
                                    ImGui::SameLine();
                                    ImGui::SetNextItemWidth(70);
                                    int first = type == AnimParameterType::Bool ? 4 : 0, last = type == AnimParameterType::Bool ? 5 : 3;
                                    if (ImGui::BeginCombo("##op", kOpNames[(int)cond.Op]))
                                    {
                                        for (int o = first; o <= last; ++o)
                                            if (ImGui::Selectable(kOpNames[o], (int)cond.Op == o)) { cond.Op = (AnimConditionOp)o; changed = true; }
                                        ImGui::EndCombo();
                                    }
                                    if (type != AnimParameterType::Bool)
                                    {
                                        ImGui::SameLine();
                                        ImGui::SetNextItemWidth(55);
                                        changed |= ImGui::DragFloat("##th", &cond.Threshold, 0.01f);
                                    }
                                }
                                ImGui::SameLine(270);
                                if (ImGui::SmallButton("x")) removeCond = ci;
                                ImGui::PopID();
                            }
                            if (removeCond >= 0) { t.Conditions.erase(t.Conditions.begin() + removeCond); changed = true; }
                            if (ImGui::SmallButton("+ Condition")) { t.Conditions.push_back({}); changed = true; }
                            ImGui::TreePop();
                        }
                        ImGui::PopID();
                    }
                    if (removeTransition >= 0) { c.Transitions.erase(c.Transitions.begin() + removeTransition); changed = true; }
                    ImGui::TextDisabled("Right-click a state in the graph to add transitions.");
                }
            }
            ImGui::EndChild();

            // ---------------- Graph ----------------
            ImGui::TableSetColumnIndex(1);
            ImGui::BeginChild("Graph", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 canvasMin = ImGui::GetCursorScreenPos(), canvasSize = ImGui::GetContentRegionAvail();
            ImVec2 canvasMax(canvasMin.x + canvasSize.x, canvasMin.y + canvasSize.y);
            dl->AddRectFilled(canvasMin, canvasMax, IM_COL32(32, 32, 34, 255));
            // Frame all nodes the first time a controller is shown.
            if (m_FramedHandle != m_Handle)
            {
                m_FramedHandle = m_Handle;
                m_ViewGroup = m_SelectedGroup = -1;
                glm::vec2 lo = c.AnyStatePosition, hi = c.AnyStatePosition;
                for (auto& st : c.States)
                {
                    lo = glm::min(lo, st.EditorPosition);
                    hi = glm::max(hi, st.EditorPosition);
                }
                glm::vec2 extent = hi - lo + glm::vec2(kNodeW, kNodeH) + glm::vec2(60.0f);
                m_Zoom = std::clamp(std::min(canvasSize.x / extent.x, canvasSize.y / extent.y), 0.3f, 1.0f);
                m_Pan = -((lo + hi) * 0.5f + glm::vec2(kNodeW, kNodeH) * 0.5f) * m_Zoom;
            }
            const float z = m_Zoom;
            ImVec2 origin(canvasMin.x + canvasSize.x * 0.5f + m_Pan.x, canvasMin.y + canvasSize.y * 0.5f + m_Pan.y);
            float gridStep = 32.0f * z;
            for (float x = std::fmod(origin.x - canvasMin.x, gridStep); x < canvasSize.x; x += gridStep)
                dl->AddLine(ImVec2(canvasMin.x + x, canvasMin.y), ImVec2(canvasMin.x + x, canvasMax.y), IM_COL32(45, 45, 48, 255));
            for (float y = std::fmod(origin.y - canvasMin.y, gridStep); y < canvasSize.y; y += gridStep)
                dl->AddLine(ImVec2(canvasMin.x, canvasMin.y + y), ImVec2(canvasMax.x, canvasMin.y + y), IM_COL32(45, 45, 48, 255));

            ImGui::InvisibleButton("canvas", canvasSize, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
            if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Middle))
            {
                m_Pan.x += ImGui::GetIO().MouseDelta.x;
                m_Pan.y += ImGui::GetIO().MouseDelta.y;
            }
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) m_SelectedState = m_SelectedGroup = -1;
            ImVec2 mouse = ImGui::GetMousePos();
            if (ImGui::IsWindowHovered() && ImGui::GetIO().MouseWheel != 0.0f)
            {
                // Zoom around the mouse cursor.
                float before = m_Zoom;
                m_Zoom = std::clamp(m_Zoom * (1.0f + ImGui::GetIO().MouseWheel * 0.1f), 0.25f, 2.0f);
                glm::vec2 anchor(mouse.x - origin.x, mouse.y - origin.y);
                m_Pan -= anchor * (m_Zoom / before - 1.0f);
            }
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && !syncedLayer)
            {
                m_ContextPos = glm::vec2(mouse.x - origin.x, mouse.y - origin.y) / z - glm::vec2(kNodeW, kNodeH) * 0.5f;
                ImGui::OpenPopup("GraphContext");
            }

            // Nodes of the shown level (the top level or an open sub-state machine): its states and groups, Any State,
            // and an "Up" node standing for everything outside. Ids: state >= 0, -1 Any State, kUpNode, groupNode(g).
            constexpr int kUpNode = -99, kEntryNode = -98, kExitNode = -97, kHidden = -96;
            auto groupNode = [](int g) { return -100 - g; };
            auto nodeGroup = [](int id) { return id <= -100 ? -100 - id : -1; };
            // The node a state shows as: itself, the child group of the view that contains it, or Up.
            auto visibleOf = [&](int state) {
                if (state < 0 || state >= (int)c.States.size()) return kUpNode;
                if (c.States[state].Group == m_ViewGroup) return state;
                for (int g = c.States[state].Group, depth = 0; g >= 0 && depth < 64; g = c.Groups[g].Parent, ++depth)
                    if (c.Groups[g].Parent == m_ViewGroup) return groupNode(g);
                return kUpNode;
            };
            auto visibleOfGroup = [&](int group) {
                for (int g = group, depth = 0; g >= 0 && depth < 64; g = c.Groups[g].Parent, ++depth)
                    if (c.Groups[g].Parent == m_ViewGroup) return groupNode(g);
                return kUpNode;
            };
            // Up sits above Any State (not saved); Entry and Exit belong to the shown level.
            glm::vec2 upPosition = c.AnyStatePosition + glm::vec2(0.0f, -90.0f);
            glm::vec2& entryPosition = m_ViewGroup >= 0 ? c.Groups[m_ViewGroup].EntryPosition : c.EntryPosition;
            glm::vec2& exitPosition = m_ViewGroup >= 0 ? c.Groups[m_ViewGroup].ExitPosition : c.ExitPosition;
            auto nodePos = [&](int id) -> glm::vec2& {
                if (id >= 0) return c.States[id].EditorPosition;
                if (id <= -100) return c.Groups[nodeGroup(id)].EditorPosition;
                if (id == kUpNode) return upPosition;
                if (id == kEntryNode) return entryPosition;
                if (id == kExitNode) return exitPosition;
                return c.AnyStatePosition;
            };
            auto nodeCenter = [&](int id) {
                glm::vec2 p = nodePos(id);
                return ImVec2(origin.x + (p.x + kNodeW * 0.5f) * z, origin.y + (p.y + kNodeH * 0.5f) * z);
            };

            dl->PushClipRect(canvasMin, canvasMax, true);
            int activeTransition = -1;
            if (live && m_Ctx.IsPlaying() && liveFadeDuration > 0.0f && liveFadeElapsed < liveFadeDuration) activeTransition = (*liveRuntime).State;
            auto fromNode = [&](const AnimTransition& t) {
                if (t.From == -1) return -1;
                if (AnimTransition::IsEntry(t.From)) return AnimTransition::EntryGroupOf(t.From) == m_ViewGroup ? kEntryNode : kHidden;
                if (int g = AnimTransition::GroupOf(t.From); g >= 0) return g == m_ViewGroup ? kUpNode : visibleOfGroup(g);
                return visibleOf(t.From);
            };
            auto toNode = [&](const AnimTransition& t) {
                if (t.ToExit) return t.From >= 0 && t.From < (int)c.States.size() && c.States[t.From].Group == m_ViewGroup ? kExitNode : kHidden;
                return t.ToGroup >= 0 ? visibleOfGroup(t.ToGroup) : visibleOf(t.To);
            };
            for (auto& t : c.Transitions)
            {
                if (!t.ToExit && t.ToGroup < 0 && (t.To < 0 || t.To >= (int)c.States.size())) continue;
                if (t.From >= (int)c.States.size() || t.ToGroup >= (int)c.Groups.size()) continue;
                int a = fromNode(t), b = toNode(t);
                if (a == kHidden || b == kHidden) continue;
                if (a == b || (a == -1 && m_ViewGroup >= 0 && b == kUpNode)) continue;
                bool reverse = false;
                for (auto& o : c.Transitions) reverse |= fromNode(o) == b && toNode(o) == a;
                bool hot = activeTransition == t.To && live && c.FindState(liveState) == t.To;
                Arrow(dl, nodeCenter(a), nodeCenter(b), hot ? IM_COL32(110, 190, 255, 255) : IM_COL32(200, 200, 205, 220), reverse ? 6.0f : 0.0f);
            }

            // Entry's default: the layer's default state, or the open sub-state machine's Entry State (orange).
            if (int def = m_ViewGroup >= 0 ? c.EntryState(m_ViewGroup) : c.DefaultState; def >= 0 && def < (int)c.States.size())
                Arrow(dl, nodeCenter(kEntryNode), nodeCenter(visibleOf(def)), IM_COL32(230, 150, 60, 230), 0.0f);

            std::vector<int> nodes = { -1, kEntryNode, kExitNode };
            if (m_ViewGroup >= 0) nodes.push_back(kUpNode);
            for (int g = 0; g < (int)c.Groups.size(); ++g)
                if (c.Groups[g].Parent == m_ViewGroup) nodes.push_back(groupNode(g));
            for (int i = 0; i < (int)c.States.size(); ++i)
                if (c.States[i].Group == m_ViewGroup) nodes.push_back(i);
            // Targets for "Make Transition To": every state and sub-state machine, by path.
            auto groupPath = [&](int g) {
                std::string path;
                for (int depth = 0; g >= 0 && depth < 64; g = c.Groups[g].Parent, ++depth) path = c.Groups[g].Name + "/" + path;
                return path;
            };
            int liveCurrent = live ? (*liveRuntime).State : -1;
            bool deleted = false;
            for (int id : nodes)
            {
                if (deleted) break;
                int i = id; // state index when >= 0
                int g = nodeGroup(id);
                glm::vec2& pos = nodePos(id);
                ImVec2 min(origin.x + pos.x * z, origin.y + pos.y * z), max(min.x + kNodeW * z, min.y + kNodeH * z);
                float nodeW = kNodeW * z, nodeH = kNodeH * z;
                bool isCurrent = live && i >= 0 && liveCurrent == i && c.FindState(liveState) == i;
                bool containsCurrent = live && g >= 0 && c.IsInGroup(liveCurrent, g);
                bool selected = (id == -1 && m_SelectedState == -2) || (id == kEntryNode && m_SelectedState == -3) || (i >= 0 && m_SelectedState == i) ||
                                (g >= 0 && m_SelectedGroup == g);
                ImU32 fill = id == -1 ? IM_COL32(40, 120, 115, 255) : id == kUpNode ? IM_COL32(55, 58, 66, 255)
                           : id == kEntryNode ? IM_COL32(60, 125, 60, 255) : id == kExitNode ? IM_COL32(140, 45, 45, 255)
                           : g >= 0 ? IM_COL32(88, 70, 120, 255) : i == c.DefaultState ? IM_COL32(170, 100, 35, 255) : IM_COL32(70, 72, 78, 255);
                if (isCurrent || containsCurrent) fill = IM_COL32(45, 105, 175, 255);
                if (g >= 0) // a stacked look for sub-state machines
                    dl->AddRectFilled(ImVec2(min.x + 4 * z, min.y - 4 * z), ImVec2(max.x + 4 * z, max.y - 4 * z), IM_COL32(60, 50, 85, 255), 6.0f);
                dl->AddRectFilled(min, max, fill, 6.0f);
                dl->AddRect(min, max, selected ? IM_COL32(255, 255, 255, 255) : IM_COL32(20, 20, 20, 255), 6.0f, 0, selected ? 2.0f : 1.0f);
                std::string upName = m_ViewGroup >= 0 && c.Groups[m_ViewGroup].Parent >= 0 ? "(Up) " + c.Groups[c.Groups[m_ViewGroup].Parent].Name : "(Up) Top Level";
                const char* name = id == -1 ? "Any State" : id == kUpNode ? upName.c_str() : id == kEntryNode ? "Entry" : id == kExitNode ? "Exit"
                                 : g >= 0 ? c.Groups[g].Name.c_str() : c.States[i].Name.c_str();
                ImVec2 ts = ImGui::CalcTextSize(name);
                bool isTree = i >= 0 && c.States[i].Motion != AnimMotionType::Clip;
                bool sub = isTree || g >= 0;
                dl->AddText(ImVec2(min.x + (nodeW - ts.x) * 0.5f, min.y + (nodeH - ts.y) * 0.5f - (isCurrent || sub ? 5 : 0)), IM_COL32(240, 240, 240, 255), name);
                if (sub && !isCurrent) // the progress bar takes this space on the current state
                {
                    const char* label = g >= 0 ? "Sub-State Machine" : "Blend Tree";
                    ImVec2 ss = ImGui::CalcTextSize(label);
                    dl->AddText(nullptr, ImGui::GetFontSize() * 0.8f, ImVec2(min.x + (nodeW - ss.x * 0.8f) * 0.5f, min.y + nodeH * 0.5f + 2),
                                IM_COL32(190, 200, 215, 255), label);
                }
                if (isCurrent && mesh)
                {
                    auto duration = [&](const std::string& n) {
                        const AnimationClip* clip = Animation::FindClip(mesh->GetClips(), n);
                        return clip ? clip->Duration : 0.0f;
                    };
                    float t = AnimatorStateMachine::NormalizedTime(c, (*liveRuntime), duration);
                    float frac = c.States[i].Loop ? t - std::floor(t) : std::min(t, 1.0f);
                    dl->AddRectFilled(ImVec2(min.x + 8, max.y - 9), ImVec2(min.x + 8 + (nodeW - 16) * frac, max.y - 5), IM_COL32(160, 210, 255, 255), 2.0f);
                }

                ImGui::SetCursorScreenPos(min);
                ImGui::PushID(id + 100000);
                ImGui::InvisibleButton("node", ImVec2(nodeW, nodeH));
                if (ImGui::IsItemClicked())
                {
                    m_SelectedState = id == -1 ? -2 : id == kEntryNode ? -3 : i >= 0 ? i : -1;
                    m_SelectedGroup = g;
                }
                // Double-click opens a sub-state machine; Up goes back out.
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && (g >= 0 || id == kUpNode))
                {
                    m_ViewGroup = g >= 0 ? g : c.Groups[m_ViewGroup].Parent;
                    m_SelectedState = m_SelectedGroup = -1;
                }
                bool fixedNode = id == kUpNode || id == kEntryNode || id == kExitNode; // no state context menu
                if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left) && !syncedLayer && id != kUpNode)
                {
                    pos.x += ImGui::GetIO().MouseDelta.x / z;
                    pos.y += ImGui::GetIO().MouseDelta.y / z;
                }
                if (ImGui::IsItemDeactivated()) changed = true; // save layout after a drag
                if (!syncedLayer && id == kEntryNode && ImGui::BeginPopupContextItem("NodeContext"))
                {
                    // Entry transitions go to the states and sub-state machines of this level.
                    if (ImGui::BeginMenu("Make Entry Transition To"))
                    {
                        auto add = [&](int to, int toGroup) {
                            AnimTransition t;
                            t.From = AnimTransition::FromEntry(m_ViewGroup);
                            t.To = to;
                            t.ToGroup = toGroup;
                            t.Duration = 0.0f;
                            t.Conditions.push_back({});
                            c.Transitions.push_back(t);
                            m_SelectedState = -3;
                            m_SelectedGroup = -1;
                            changed = true;
                        };
                        for (int j = 0; j < (int)c.States.size(); ++j)
                            if (c.States[j].Group == m_ViewGroup && ImGui::MenuItem(c.States[j].Name.c_str())) add(j, -1);
                        for (int k = 0; k < (int)c.Groups.size(); ++k)
                            if (c.Groups[k].Parent == m_ViewGroup && ImGui::MenuItem((c.Groups[k].Name + "  (sub-state machine)").c_str())) add(0, k);
                        ImGui::EndMenu();
                    }
                    ImGui::EndPopup();
                }
                if (!syncedLayer && !fixedNode && ImGui::BeginPopupContextItem("NodeContext"))
                {
                    if (ImGui::BeginMenu("Make Transition To"))
                    {
                        auto add = [&](int to, int toGroup, bool toExit = false) {
                            AnimTransition t;
                            t.From = g >= 0 ? AnimTransition::FromGroup(g) : i;
                            t.To = to;
                            t.ToGroup = toGroup;
                            t.ToExit = toExit;
                            t.HasExitTime = i >= 0; // Any State and sub-state machine transitions need a condition instead
                            c.Transitions.push_back(t);
                            m_SelectedState = id == -1 ? -2 : i >= 0 ? i : -1;
                            m_SelectedGroup = g;
                            changed = true;
                        };
                        for (int j = 0; j < (int)c.States.size(); ++j)
                            if (j != i && ImGui::MenuItem((groupPath(c.States[j].Group) + c.States[j].Name).c_str())) add(j, -1);
                        if (!c.Groups.empty()) ImGui::Separator();
                        for (int k = 0; k < (int)c.Groups.size(); ++k)
                            if (k != g && ImGui::MenuItem((groupPath(k) + "  (its entry state)").c_str())) add(0, k);
                        if (i >= 0)
                        {
                            ImGui::Separator();
                            if (ImGui::MenuItem(c.States[i].Group >= 0 ? "Exit (leave this sub-state machine)" : "Exit (back to Entry)")) add(0, -1, true);
                        }
                        ImGui::EndMenu();
                    }
                    if (i >= 0)
                    {
                        if (ImGui::BeginMenu("Move To"))
                        {
                            if (ImGui::MenuItem("Top Level", nullptr, c.States[i].Group == -1)) { c.States[i].Group = -1; changed = true; }
                            for (int k = 0; k < (int)c.Groups.size(); ++k)
                                if (ImGui::MenuItem(groupPath(k).c_str(), nullptr, c.States[i].Group == k)) { c.States[i].Group = k; changed = true; }
                            ImGui::EndMenu();
                        }
                        if (ImGui::MenuItem("Set as Default State")) { c.DefaultState = i; changed = true; }
                        if (c.States[i].Group >= 0 && ImGui::MenuItem("Set as Sub-State Machine Entry")) { c.Groups[c.States[i].Group].DefaultState = i; changed = true; }
                        if (ImGui::MenuItem("Delete State"))
                        {
                            c.RemoveState(i);
                            m_SelectedState = -1;
                            changed = true;
                            deleted = true;
                        }
                    }
                    if (g >= 0)
                    {
                        if (ImGui::MenuItem("Open")) { m_ViewGroup = g; m_SelectedGroup = -1; }
                        if (ImGui::MenuItem("Delete Sub-State Machine (keeps its states)"))
                        {
                            c.RemoveGroup(g);
                            m_SelectedGroup = -1;
                            changed = true;
                            deleted = true;
                        }
                    }
                    ImGui::EndPopup();
                }
                ImGui::PopID();
            }
            dl->PopClipRect();

            if (ImGui::BeginPopup("GraphContext"))
            {
                if (ImGui::MenuItem("Create State"))
                {
                    AnimState st;
                    st.Name = "State " + std::to_string(c.States.size());
                    st.EditorPosition = m_ContextPos;
                    st.Group = m_ViewGroup;
                    c.States.push_back(st);
                    m_SelectedState = (int)c.States.size() - 1;
                    m_SelectedGroup = -1;
                    changed = true;
                }
                if (ImGui::MenuItem("Create Sub-State Machine"))
                {
                    AnimStateGroup grp;
                    grp.Name = "Sub-State Machine " + std::to_string(c.Groups.size() + 1);
                    grp.Parent = m_ViewGroup;
                    grp.EditorPosition = m_ContextPos;
                    c.Groups.push_back(grp);
                    m_SelectedGroup = (int)c.Groups.size() - 1;
                    m_SelectedState = -1;
                    changed = true;
                }
                ImGui::EndPopup();
            }
            // Where we are: Base Layer > Locomotion > ...
            {
                std::vector<int> chain;
                for (int g = m_ViewGroup, depth = 0; g >= 0 && depth < 64; g = c.Groups[g].Parent, ++depth) chain.insert(chain.begin(), g);
                ImGui::SetCursorScreenPos(ImVec2(canvasMin.x + 8, canvasMin.y + 6));
                std::string layerName = m_Layer == 0 ? "Base Layer" : root.Layers[m_Layer - 1].Name;
                if (ImGui::SmallButton(layerName.c_str())) { m_ViewGroup = -1; m_SelectedState = m_SelectedGroup = -1; }
                for (int g : chain)
                {
                    ImGui::SameLine();
                    ImGui::TextDisabled(">");
                    ImGui::SameLine();
                    ImGui::PushID(g);
                    if (ImGui::SmallButton(c.Groups[g].Name.c_str())) { m_ViewGroup = g; m_SelectedState = m_SelectedGroup = -1; }
                    ImGui::PopID();
                }
            }
            ImGui::SetCursorScreenPos(ImVec2(canvasMin.x + 8, canvasMax.y - 22));
            if (syncedLayer)
                ImGui::TextDisabled("Synced with %s: its states and transitions drive this layer. Select a state to set this layer's motion for it.",
                                    root.Layers[m_Layer - 1].SyncedLayer == 0 ? "Base Layer" : root.Layers[root.Layers[m_Layer - 1].SyncedLayer - 1].Name.c_str());
            else
                ImGui::TextDisabled("Right-click: states, sub-state machines, transitions.  Double-click a sub-state machine to open it.  Middle-drag: pan.  Wheel: zoom.");
            ImGui::EndChild();
            ImGui::EndTable();
        }

        if (changed) AssetManager::SaveAnimatorController(m_Handle);
        ImGui::End();
    }
}
