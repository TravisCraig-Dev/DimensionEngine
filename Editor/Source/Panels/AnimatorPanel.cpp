#include "Panels.h"
#include "../EditorUI.h"
#include <Dimension/Animation/AnimatorController.h>
#include <Dimension/Renderer/Mesh.h>
#include <imgui.h>
#include <imgui_stdlib.h>
#include <cmath>

namespace Editor
{
    namespace
    {
        constexpr float kNodeW = 170.0f, kNodeH = 44.0f;
        const char* kOpNames[] = { ">", "<", "==", "!=", "is true", "is false" };
        const char* kTypeNames[] = { "Float", "Int", "Bool", "Trigger" };

        ImVec2 ToV(const glm::vec2& v) { return ImVec2(v.x, v.y); }

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
                m_Handle = anim->Controller;
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
        AnimatorController& c = *controller;
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
            for (int i = 0; i < (int)c.Parameters.size(); ++i)
            {
                auto& p = c.Parameters[i];
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
                        float v = AnimatorStateMachine::GetParameter(c, live->Runtime, p.Name);
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
            if (removeParam >= 0) { c.Parameters.erase(c.Parameters.begin() + removeParam); changed = true; }
            for (int t = 0; t < 4; ++t)
            {
                if (t) ImGui::SameLine();
                if (ImGui::SmallButton((std::string("+ ") + kTypeNames[t]).c_str()))
                {
                    c.Parameters.push_back({ std::string("New") + kTypeNames[t], (AnimParameterType)t, 0.0f });
                    changed = true;
                }
            }

            bool anySelected = m_SelectedState == -2;
            if (m_SelectedState >= (int)c.States.size()) m_SelectedState = -1;
            if (m_SelectedState >= 0 || anySelected)
            {
                if (!anySelected)
                {
                    auto& st = c.States[m_SelectedState];
                    ImGui::SeparatorText("State");
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
                    if (ImGui::Combo("Motion", &motion, "Clip\0Blend Tree 1D\0"))
                    {
                        st.Motion = (AnimMotionType)motion;
                        changed = true;
                    }
                    if (st.Motion == AnimMotionType::Clip)
                        clipCombo("Clip", st.Clip);
                    else
                    {
                        if (ImGui::BeginCombo("Parameter", st.BlendParameter.empty() ? "(none)" : st.BlendParameter.c_str()))
                        {
                            for (auto& p : c.Parameters)
                                if (p.Type == AnimParameterType::Float && ImGui::Selectable(p.Name.c_str(), p.Name == st.BlendParameter))
                                {
                                    st.BlendParameter = p.Name;
                                    changed = true;
                                }
                            ImGui::EndCombo();
                        }
                        std::vector<float> weights;
                        if (live && m_Ctx.IsPlaying()) AnimatorStateMachine::BlendWeights(st, AnimatorStateMachine::GetParameter(c, live->Runtime, st.BlendParameter), weights);
                        int removeChild = -1;
                        for (int k = 0; k < (int)st.Children.size(); ++k)
                        {
                            ImGui::PushID(k + 5000);
                            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.45f);
                            clipCombo("##clip", st.Children[k].Clip);
                            ImGui::SameLine();
                            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - (weights.empty() ? 28 : 72));
                            changed |= ImGui::DragFloat("##threshold", &st.Children[k].Threshold, 0.01f);
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
                            child.Threshold = st.Children.empty() ? 0.0f : st.Children.back().Threshold + 1.0f;
                            st.Children.push_back(child);
                            changed = true;
                        }
                        ImGui::SameLine();
                        ImGui::TextDisabled("clip / threshold");
                    }
                    changed |= ImGui::DragFloat("Speed", &st.Speed, 0.01f, -10.0f, 10.0f);
                    changed |= ImGui::Checkbox("Loop", &st.Loop);

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
                ImGui::SeparatorText(anySelected ? "Transitions from Any State" : "Transitions");
                int from = anySelected ? -1 : m_SelectedState;
                int removeTransition = -1;
                for (int ti = 0; ti < (int)c.Transitions.size(); ++ti)
                {
                    auto& t = c.Transitions[ti];
                    if (t.From != from) continue;
                    ImGui::PushID(ti);
                    std::string to = t.To >= 0 && t.To < (int)c.States.size() ? c.States[t.To].Name : "?";
                    bool openT = ImGui::TreeNodeEx("##t", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap, "-> %s", to.c_str());
                    ImGui::SameLine(270);
                    if (ImGui::SmallButton("x")) removeTransition = ti;
                    if (openT)
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
                        int removeCond = -1;
                        for (int ci = 0; ci < (int)t.Conditions.size(); ++ci)
                        {
                            auto& cond = t.Conditions[ci];
                            ImGui::PushID(ci);
                            ImGui::SetNextItemWidth(100);
                            if (ImGui::BeginCombo("##param", cond.Parameter.empty() ? "(param)" : cond.Parameter.c_str()))
                            {
                                for (auto& p : c.Parameters)
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
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) m_SelectedState = -1;
            ImVec2 mouse = ImGui::GetMousePos();
            if (ImGui::IsWindowHovered() && ImGui::GetIO().MouseWheel != 0.0f)
            {
                // Zoom around the mouse cursor.
                float before = m_Zoom;
                m_Zoom = std::clamp(m_Zoom * (1.0f + ImGui::GetIO().MouseWheel * 0.1f), 0.25f, 2.0f);
                glm::vec2 anchor(mouse.x - origin.x, mouse.y - origin.y);
                m_Pan -= anchor * (m_Zoom / before - 1.0f);
            }
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
            {
                m_ContextPos = glm::vec2(mouse.x - origin.x, mouse.y - origin.y) / z - glm::vec2(kNodeW, kNodeH) * 0.5f;
                ImGui::OpenPopup("GraphContext");
            }

            auto nodeCenter = [&](int index) {
                glm::vec2 p = index < 0 ? c.AnyStatePosition : c.States[index].EditorPosition;
                return ImVec2(origin.x + (p.x + kNodeW * 0.5f) * z, origin.y + (p.y + kNodeH * 0.5f) * z);
            };

            dl->PushClipRect(canvasMin, canvasMax, true);
            int activeTransition = -1;
            if (live && m_Ctx.IsPlaying() && live->FadeDuration > 0.0f && live->FadeElapsed < live->FadeDuration) activeTransition = live->Runtime.State;
            for (auto& t : c.Transitions)
            {
                if (t.To < 0 || t.To >= (int)c.States.size() || (t.From >= (int)c.States.size())) continue;
                bool reverse = false;
                for (auto& o : c.Transitions) reverse |= o.From == t.To && o.To == t.From;
                bool hot = activeTransition == t.To && live && c.FindState(live->CurrentState) == t.To;
                Arrow(dl, nodeCenter(t.From), nodeCenter(t.To), hot ? IM_COL32(110, 190, 255, 255) : IM_COL32(200, 200, 205, 220), reverse ? 6.0f : 0.0f);
            }

            // Nodes (Any State + states)
            for (int i = -1; i < (int)c.States.size(); ++i)
            {
                glm::vec2& pos = i < 0 ? c.AnyStatePosition : c.States[i].EditorPosition;
                ImVec2 min(origin.x + pos.x * z, origin.y + pos.y * z), max(min.x + kNodeW * z, min.y + kNodeH * z);
                float nodeW = kNodeW * z, nodeH = kNodeH * z;
                bool isCurrent = live && i >= 0 && live->Runtime.State == i && c.FindState(live->CurrentState) == i;
                bool selected = (i < 0 && m_SelectedState == -2) || (i >= 0 && m_SelectedState == i);
                ImU32 fill = i < 0 ? IM_COL32(40, 120, 115, 255) : i == c.DefaultState ? IM_COL32(170, 100, 35, 255) : IM_COL32(70, 72, 78, 255);
                if (isCurrent) fill = IM_COL32(45, 105, 175, 255);
                dl->AddRectFilled(min, max, fill, 6.0f);
                dl->AddRect(min, max, selected ? IM_COL32(255, 255, 255, 255) : IM_COL32(20, 20, 20, 255), 6.0f, 0, selected ? 2.0f : 1.0f);
                const char* name = i < 0 ? "Any State" : c.States[i].Name.c_str();
                ImVec2 ts = ImGui::CalcTextSize(name);
                bool isTree = i >= 0 && c.States[i].Motion == AnimMotionType::BlendTree1D;
                dl->AddText(ImVec2(min.x + (nodeW - ts.x) * 0.5f, min.y + (nodeH - ts.y) * 0.5f - (isCurrent || isTree ? 5 : 0)), IM_COL32(240, 240, 240, 255), name);
                if (isTree && !isCurrent) // the progress bar takes this space on the current state
                {
                    const char* sub = "Blend Tree";
                    ImVec2 ss = ImGui::CalcTextSize(sub);
                    dl->AddText(nullptr, ImGui::GetFontSize() * 0.8f, ImVec2(min.x + (nodeW - ss.x * 0.8f) * 0.5f, min.y + nodeH * 0.5f + 2),
                                IM_COL32(190, 200, 215, 255), sub);
                }
                if (isCurrent && mesh)
                {
                    auto duration = [&](const std::string& n) {
                        const AnimationClip* clip = Animation::FindClip(mesh->GetClips(), n);
                        return clip ? clip->Duration : 0.0f;
                    };
                    float t = AnimatorStateMachine::NormalizedTime(c, live->Runtime, duration);
                    float frac = c.States[i].Loop ? t - std::floor(t) : std::min(t, 1.0f);
                    dl->AddRectFilled(ImVec2(min.x + 8, max.y - 9), ImVec2(min.x + 8 + (nodeW - 16) * frac, max.y - 5), IM_COL32(160, 210, 255, 255), 2.0f);
                }

                ImGui::SetCursorScreenPos(min);
                ImGui::PushID(i + 1000);
                ImGui::InvisibleButton("node", ImVec2(nodeW, nodeH));
                if (ImGui::IsItemClicked()) m_SelectedState = i < 0 ? -2 : i;
                if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
                {
                    pos.x += ImGui::GetIO().MouseDelta.x / z;
                    pos.y += ImGui::GetIO().MouseDelta.y / z;
                }
                if (ImGui::IsItemDeactivated()) changed = true; // save layout after a drag
                if (ImGui::BeginPopupContextItem("NodeContext"))
                {
                    if (ImGui::BeginMenu("Make Transition To"))
                    {
                        for (int j = 0; j < (int)c.States.size(); ++j)
                            if (j != i && ImGui::MenuItem(c.States[j].Name.c_str()))
                            {
                                AnimTransition t;
                                t.From = i;
                                t.To = j;
                                t.HasExitTime = i >= 0; // Any State transitions need a condition instead
                                c.Transitions.push_back(t);
                                m_SelectedState = i < 0 ? -2 : i;
                                changed = true;
                            }
                        ImGui::EndMenu();
                    }
                    if (i >= 0)
                    {
                        if (ImGui::MenuItem("Set as Default State")) { c.DefaultState = i; changed = true; }
                        if (ImGui::MenuItem("Delete State"))
                        {
                            c.RemoveState(i);
                            m_SelectedState = -1;
                            changed = true;
                            ImGui::EndPopup();
                            ImGui::PopID();
                            break;
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
                    c.States.push_back(st);
                    m_SelectedState = (int)c.States.size() - 1;
                    changed = true;
                }
                ImGui::EndPopup();
            }
            ImGui::SetCursorScreenPos(ImVec2(canvasMin.x + 8, canvasMax.y - 22));
            ImGui::TextDisabled("Right-click: create state / transitions.  Middle-drag: pan.  Wheel: zoom.  Orange: default.  Blue: current.");
            ImGui::EndChild();
            ImGui::EndTable();
        }

        if (changed) AssetManager::SaveAnimatorController(m_Handle);
        ImGui::End();
    }
}
