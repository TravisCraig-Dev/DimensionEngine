#include "Panels.h"
#include "../EditorUI.h"
#include <Dimension/Animation/ClipAsset.h>
#include <Dimension/Renderer/Mesh.h>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <ImGuizmo.h>
#include <imgui.h>
#include <imgui_stdlib.h>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace Editor
{
    namespace
    {
        // A file name from a clip name (characters Windows refuses replaced), made unique beside the others.
        fs::path NewClipPath(const fs::path& folder, const std::string& name)
        {
            std::string base;
            for (char c : name) base += std::strchr("<>:\"/\\|?*", c) || (unsigned char)c < 32 ? '_' : c;
            if (base.empty()) base = "Clip";
            fs::path path = folder / (base + ".anim");
            for (int n = 1; fs::exists(path); ++n) path = folder / (base + " " + std::to_string(n) + ".anim");
            return path;
        }

        int Depth(const Skeleton& s, int j)
        {
            int d = 0;
            for (int p = s.Joints[j].Parent; p >= 0 && d < 64; p = s.Joints[p].Parent) ++d;
            return d;
        }

        // Every key time of the clip (or of one joint), in order, close ones merged.
        std::vector<float> KeyTimes(const AnimationClipAsset& clip, const std::string* joint)
        {
            std::vector<float> times;
            for (const auto& c : clip.Channels)
                if (!joint || c.Joint == *joint) times.insert(times.end(), c.Times.begin(), c.Times.end());
            std::sort(times.begin(), times.end());
            std::vector<float> merged;
            for (float t : times)
                if (merged.empty() || t - merged.back() > AnimationClipAsset::kKeyTolerance * 0.5f) merged.push_back(t);
            return merged;
        }

        // A rotation key as the curve view shows it: Euler angles in degrees.
        glm::vec3 EulerDegrees(const glm::vec4& v)
        {
            return glm::degrees(glm::eulerAngles(glm::normalize(glm::quat(v.w, v.x, v.y, v.z))));
        }

        // A key's tangent as the curve view shows it (change per second of each drawn value: Euler degrees for
        // rotations), and back: the tangent that gives those rates. Central differences, so the two agree.
        glm::vec3 ShownRate(bool rotation, const glm::vec4& value, const glm::vec4& tangent)
        {
            if (!rotation) return glm::vec3(tangent);
            const float h = 0.01f;
            glm::vec3 d = EulerDegrees(value + tangent * h) - EulerDegrees(value - tangent * h);
            for (int a = 0; a < 3; ++a) d[a] -= 360.0f * std::round(d[a] / 360.0f);
            return d / (2.0f * h);
        }

        glm::vec4 TangentFor(bool rotation, const glm::vec4& value, const glm::vec3& rate)
        {
            if (!rotation) return glm::vec4(rate, 0.0f);
            const float h = 0.01f;
            glm::vec3 e = EulerDegrees(value);
            auto at = [&](float s) {
                glm::quat q(glm::radians(e + rate * s));
                glm::vec4 v(q.x, q.y, q.z, q.w);
                return glm::dot(v, value) < 0.0f ? -v : v; // on the key's side
            };
            float length = glm::length(value);
            return (at(h) - at(-h)) / (2.0f * h) * length;
        }
    }

    void AnimationPanel::StopPreview()
    {
        if (m_PreviewEntity.IsNull() || !m_Ctx.EditorScene) return;
        Scene& scene = *m_Ctx.EditorScene; // (previews run in edit mode only)
        auto& reg = scene.GetRegistry();
        if (scene.IsValid(m_PreviewEntity))
            if (auto* anim = reg.TryGet<AnimatorComponent>(m_PreviewEntity))
            {
                anim->Controller = m_SavedController;
                anim->Clip = m_SavedClip;
                anim->Time = m_SavedTime;
                anim->Playing = m_SavedPlaying;
            }
        if (scene.IsValid(m_PreviewEntity))
            if (auto* mr = reg.TryGet<MeshRendererComponent>(m_PreviewEntity)) mr->BlendShapeWeights = m_SavedWeights; // (the clip set them)
        m_PreviewEntity = Entity::Null();
    }

    void AnimationPanel::OnImGui(bool* open)
    {
        Scene& scene = m_Ctx.ActiveScene();
        ImGui::SetNextWindowSize(ImVec2(900, 440), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Animation", open) || !*open)
        {
            StopPreview();
            ImGui::End();
            return;
        }
        // Target: the selected object's Animator on a skinned model; its .anim clips are those made for that model.
        auto& reg = scene.GetRegistry();
        Entity entity = m_Ctx.HasValidSelection() ? m_Ctx.Selected : Entity::Null();
        auto* anim = entity.IsNull() ? nullptr : reg.TryGet<AnimatorComponent>(entity);
        auto* mr = entity.IsNull() ? nullptr : reg.TryGet<MeshRendererComponent>(entity);
        AssetHandle modelHandle = anim && mr ? (anim->ClipSource.IsValid() ? anim->ClipSource : mr->Mesh) : AssetHandle::None();
        Ref<Mesh> model = modelHandle.IsValid() ? AssetManager::GetMesh(modelHandle) : nullptr;
        if (entity != m_PreviewEntity) StopPreview();
        if (!model || !model->GetSkeleton())
        {
            ImGui::TextDisabled("Select an object with an Animator on a skinned model to edit its animation clips (.anim).");
            ImGui::End();
            return;
        }
        const Skeleton& skeleton = *model->GetSkeleton();
        bool playing = m_Ctx.IsPlaying();
        // The model's blend shapes (rows after the joints; their keys are channels named after them).
        std::vector<std::string> shapeNames;
        if (auto data = model->GetBlendShapeCount() ? AssetManager::GetMeshData(modelHandle) : nullptr)
            for (const BlendShape& b : data->BlendShapes) shapeNames.push_back(b.Name);
        if (m_Shape >= (int)shapeNames.size()) m_Shape = -1;
        if (playing) StopPreview();
        const std::vector<AssetHandle>& clips = AssetManager::ClipsFor(modelHandle);
        if (std::find(clips.begin(), clips.end(), m_Clip) == clips.end()) m_Clip = clips.empty() ? AssetHandle::None() : clips.front();
        auto clip = AssetManager::GetAnimationClip(m_Clip);

        // ---- Header: the clip, new clips, preview and time ----
        auto clipLabel = [](AssetHandle h) {
            auto c = AssetManager::GetAnimationClip(h);
            return (c ? c->Name : std::string("?")) + "  (" + AssetManager::GetDisplayName(h) + ")";
        };
        ImGui::SetNextItemWidth(230);
        if (ImGui::BeginCombo("##clip", clip ? clipLabel(m_Clip).c_str() : "(no clips for this model)"))
        {
            for (AssetHandle h : clips)
                if (ImGui::Selectable(clipLabel(h).c_str(), h == m_Clip))
                {
                    m_Clip = h;
                    m_Time = 0.0f;
                }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        fs::path folder = AssetManager::GetAbsolutePath(modelHandle).parent_path();
        auto created = [&](AssetHandle h) {
            if (!h.IsValid()) return;
            m_Clip = h;
            m_Time = 0.0f;
            AssetManager::TouchAnimationClips();
        };
        if (ImGui::Button("Extract")) ImGui::OpenPopup("##extract");
        ImGui::SetItemTooltip("Copy one of the model's clips into an editable .anim beside the model (same name: it plays wherever the original did)");
        if (ImGui::BeginPopup("##extract"))
        {
            if (model->GetClips().empty()) ImGui::TextDisabled("The model has no clips");
            for (const AnimationClip& c : model->GetClips())
                if (ImGui::Selectable(c.Name.c_str()))
                {
                    AnimationClipAsset made = AnimationClipAsset::FromClip(skeleton, c, &shapeNames);
                    made.Model = modelHandle;
                    created(AssetManager::CreateAnimationClip(NewClipPath(folder, c.Name), made));
                }
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("New"))
        {
            AnimationClipAsset made;
            made.Model = modelHandle;
            made.Name = "New Clip";
            made.Duration = 1.0f;
            created(AssetManager::CreateAnimationClip(NewClipPath(folder, made.Name), made));
        }
        ImGui::SetItemTooltip("An empty clip for this model (keys set below)");
        if (!clip)
        {
            ImGui::TextDisabled("Extract one of the model's clips, or make a new one.");
            ImGui::End();
            return;
        }
        AnimationClipAsset& asset = *clip;
        bool changed = false, commit = false; // edited (in memory) / write the file now
        ImGui::SameLine();
        if (playing) ImGui::BeginDisabled();
        bool previewing = !m_PreviewEntity.IsNull();
        if (ImGui::Checkbox("Preview", &previewing))
        {
            if (previewing)
            {
                m_PreviewEntity = entity;
                m_SavedController = anim->Controller;
                m_SavedClip = anim->Clip;
                m_SavedTime = anim->Time;
                m_SavedPlaying = anim->Playing;
                m_SavedWeights = mr->BlendShapeWeights;
            }
            else
                StopPreview();
        }
        ImGui::SetItemTooltip("Pose the object at the clip's current time (its Animator is put back when the preview ends)");
        if (playing) ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(220);
        ImGui::SliderFloat("##time", &m_Time, 0.0f, asset.Duration, "%.3f s");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80);
        if (ImGui::DragFloat("Length", &asset.Duration, 0.01f, 0.05f, 600.0f, "%.2f s")) changed = true;
        if (ImGui::IsItemDeactivatedAfterEdit()) commit = true;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(140);
        if (ImGui::InputText("Name", &asset.Name)) changed = true;
        if (ImGui::IsItemDeactivatedAfterEdit()) commit = true;
        ImGui::SetItemTooltip("The name Animators and controller states play it by");
        m_Time = std::clamp(m_Time, 0.0f, asset.Duration);
        if (!m_PreviewEntity.IsNull() && anim)
        {
            anim->Controller = AssetHandle::None();
            anim->Clip = asset.Name;
            anim->Time = m_Time;
            anim->Playing = false;
        }

        // The pose at the current time (the selected joint's values come from it).
        AnimationClip sampled = asset.ToClip(skeleton, &shapeNames);
        Pose pose;
        Animation::Sample(skeleton, sampled, m_Time, pose);
        if (m_Joint >= (int)skeleton.Joints.size()) m_Joint = -1;
        if (!m_AutoCurve.Joint.empty() && m_Joint < 0) // (automation: --anim-key)
            for (size_t j = 0; j < skeleton.Joints.size(); ++j)
                if (skeleton.Joints[j].Name == m_AutoCurve.Joint) m_Joint = (int)j, m_Selected = { (int)j };

        // ---- Joints and their keys (a dope sheet), then the selected joint's values ----
        const float labelW = 220.0f, rowH = ImGui::GetFrameHeight(), inset = 8.0f;
        const float curveH = 120.0f;
        float detailH = m_Joint >= 0 ? ImGui::GetFrameHeightWithSpacing() * 3.0f + curveH + 12.0f
                                     : ImGui::GetFrameHeightWithSpacing() * (m_Shape >= 0 || m_Event >= 0 ? 2.0f : 1.0f) + 4.0f;
        ImGui::BeginChild("##sheet", ImVec2(0, -detailH), ImGuiChildFlags_Borders);
        float stripW = std::max(ImGui::GetContentRegionAvail().x - labelW - 6.0f, 80.0f);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        auto timeToX = [&](float x0, float t) { return x0 + inset + t / std::max(asset.Duration, 0.01f) * (stripW - 2.0f * inset); };
        auto xToTime = [&](float x0, float x) { return std::clamp((x - x0 - inset) / (stripW - 2.0f * inset) * asset.Duration, 0.0f, asset.Duration); };
        auto diamond = [&](float x, float cy, ImU32 col) { dl->AddQuadFilled(ImVec2(x, cy - 5), ImVec2(x + 5, cy), ImVec2(x, cy + 5), ImVec2(x - 5, cy), col); };
        float mouseX = ImGui::GetIO().MousePos.x;

        // Ruler (click or drag to scrub) with all keys on it.
        ImGui::Dummy(ImVec2(labelW - 8.0f, rowH));
        ImGui::SameLine(labelW);
        ImVec2 r0 = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##ruler", ImVec2(stripW, rowH));
        if (ImGui::IsItemActive()) m_Time = xToTime(r0.x, mouseX);
        dl->AddRectFilled(r0, ImVec2(r0.x + stripW, r0.y + rowH), IM_COL32(44, 45, 52, 255), 3.0f);
        float step = asset.Duration > 20.0f ? 5.0f : asset.Duration > 4.0f ? 1.0f : asset.Duration > 1.0f ? 0.25f : 0.1f;
        for (float t = 0.0f; t <= asset.Duration + 1e-4f; t += step)
        {
            float x = timeToX(r0.x, t);
            dl->AddLine(ImVec2(x, r0.y + rowH * 0.6f), ImVec2(x, r0.y + rowH), IM_COL32(120, 120, 130, 255));
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%g", std::round(t * 100.0f) / 100.0f);
            dl->AddText(ImVec2(x + 2, r0.y), IM_COL32(150, 150, 160, 255), buf);
        }
        for (float t : KeyTimes(asset, nullptr)) diamond(timeToX(r0.x, t), r0.y + rowH * 0.72f, IM_COL32(160, 160, 170, 255));

        // Events: markers on a row of their own (click one to select it, drag to move it, right-click to delete it).
        if (m_Event >= (int)asset.Events.size()) m_Event = -1;
        {
            if (playing) ImGui::BeginDisabled();
            if (ImGui::SmallButton("+ Event"))
            {
                m_Event = asset.AddEvent(m_Time, "OnEvent");
                m_Joint = m_Shape = -1;
                m_Selected.clear();
                changed = commit = true;
            }
            if (playing) ImGui::EndDisabled();
            ImGui::SetItemTooltip("Add an event at the current time: scripts' OnAnimationEvent(name) is called when playback passes it");
            ImGui::SameLine(labelW);
            ImVec2 e0 = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("##events", ImVec2(stripW, rowH));
            bool hovered = ImGui::IsItemHovered();
            dl->AddRectFilled(e0, ImVec2(e0.x + stripW, e0.y + rowH), IM_COL32(38, 36, 30, 255), 3.0f);
            int hit = -1;
            for (int i = 0; i < (int)asset.Events.size(); ++i)
            {
                float x = timeToX(e0.x, asset.Events[i].Time);
                ImU32 col = i == m_Event ? IM_COL32(255, 210, 90, 255) : IM_COL32(230, 180, 90, 255);
                dl->AddRectFilled(ImVec2(x - 2.0f, e0.y + 3.0f), ImVec2(x + 2.0f, e0.y + rowH - 3.0f), col, 1.0f);
                dl->AddTriangleFilled(ImVec2(x - 6.0f, e0.y + 3.0f), ImVec2(x + 6.0f, e0.y + 3.0f), ImVec2(x, e0.y + 9.0f), col);
                if (hovered && std::abs(mouseX - timeToX(e0.x, asset.Events[i].Time)) < 6.0f) hit = i;
            }
            if (ImGui::IsItemClicked())
            {
                m_Event = hit;
                m_EventDragging = hit >= 0 && !playing;
                if (hit >= 0)
                {
                    m_Joint = m_Shape = -1;
                    m_Selected.clear();
                    m_Time = asset.Events[hit].Time;
                }
                else
                    m_Time = xToTime(e0.x, mouseX);
            }
            if (hovered && hit >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && !playing)
            {
                asset.Events.erase(asset.Events.begin() + hit);
                ++asset.Version;
                m_Event = -1;
                changed = commit = true;
            }
            if (m_EventDragging && m_Event >= 0 && ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f))
            {
                float to = xToTime(e0.x, mouseX);
                if (std::abs(to - asset.Events[m_Event].Time) > 1e-4f)
                {
                    m_Event = asset.MoveEvent(m_Event, to);
                    m_Time = to;
                    changed = true;
                }
            }
            if (ImGui::IsItemDeactivated() && m_EventDragging)
            {
                m_EventDragging = false;
                commit = true;
            }
        }

        std::string pendingRemove;
        float removeAt = 0.0f;
        // Selection: a click selects one joint, Ctrl adds or removes one, Shift adds the rows from the last one.
        std::erase_if(m_Selected, [&](int j) { return j < 0 || j >= (int)skeleton.Joints.size(); });
        if (m_Joint >= 0 && std::find(m_Selected.begin(), m_Selected.end(), m_Joint) == m_Selected.end()) m_Selected = { m_Joint };
        auto isSelected = [&](int j) { return std::find(m_Selected.begin(), m_Selected.end(), j) != m_Selected.end(); };
        auto select = [&](int j) {
            ImGuiIO& io = ImGui::GetIO();
            if (io.KeyCtrl)
            {
                if (isSelected(j) && m_Selected.size() > 1)
                {
                    std::erase(m_Selected, j);
                    if (m_Joint == j) m_Joint = m_Selected.back();
                    return;
                }
                if (!isSelected(j)) m_Selected.push_back(j);
            }
            else if (io.KeyShift && m_Joint >= 0)
            {
                auto from = std::find(skeleton.Order.begin(), skeleton.Order.end(), m_Joint), to = std::find(skeleton.Order.begin(), skeleton.Order.end(), j);
                if (from > to) std::swap(from, to);
                for (auto it = from; it != skeleton.Order.end() && it <= to; ++it)
                    if (!isSelected(*it)) m_Selected.push_back(*it);
            }
            else
                m_Selected = { j };
            m_Joint = j;
            m_Shape = -1;
            m_Event = -1;
        };
        auto selectedNames = [&]() {
            std::vector<std::string> names;
            for (int j : m_Selected) names.push_back(skeleton.Joints[j].Name);
            return names;
        };
        for (int j : skeleton.Order)
        {
            const std::string& name = skeleton.Joints[j].Name;
            ImGui::PushID(j);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + Depth(skeleton, j) * 10.0f);
            ImVec2 labelAt = ImGui::GetCursorScreenPos();
            if (ImGui::Selectable(name.c_str(), isSelected(j), 0, ImVec2(std::max(labelW - 12.0f - (labelAt.x - ImGui::GetWindowPos().x), 20.0f), rowH)))
                select(j);
            ImGui::SameLine(labelW);
            ImVec2 p0 = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("##strip", ImVec2(stripW, rowH));
            bool hovered = ImGui::IsItemHovered();
            dl->AddRectFilled(p0, ImVec2(p0.x + stripW, p0.y + rowH), isSelected(j) ? IM_COL32(44, 52, 66, 255) : IM_COL32(34, 35, 40, 255), 3.0f);
            float hit = -1.0f;
            for (float t : KeyTimes(asset, &name))
            {
                float x = timeToX(p0.x, t);
                bool sel = isSelected(j) && m_KeyTime >= 0.0f && std::abs(m_KeyTime - t) <= AnimationClipAsset::kKeyTolerance * 0.5f;
                diamond(x, p0.y + rowH * 0.5f, sel ? IM_COL32(255, 210, 90, 255) : IM_COL32(200, 200, 210, 255));
                if (hovered && std::abs(mouseX - x) < 6.0f) hit = t;
            }
            if (ImGui::IsItemClicked())
            {
                // A key on a selected row keeps the selection (dragging moves every selected joint's keys there).
                if (!(hit >= 0.0f && isSelected(j) && !ImGui::GetIO().KeyCtrl && !ImGui::GetIO().KeyShift)) select(j);
                m_Joint = j;
                m_KeyTime = hit;
                m_Dragging = hit >= 0.0f && !playing;
                m_Time = hit >= 0.0f ? hit : xToTime(p0.x, mouseX);
            }
            if (hovered && hit >= 0.0f && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
            {
                pendingRemove = name;
                removeAt = hit;
            }
            if (m_Dragging && m_Joint == j && ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f))
            {
                float to = xToTime(p0.x, mouseX);
                if (std::abs(to - m_KeyTime) > AnimationClipAsset::kKeyTolerance)
                {
                    bool moved = false;
                    for (const std::string& n : selectedNames()) moved |= asset.MoveKeys(n, m_KeyTime, to);
                    if (moved)
                    {
                        m_KeyTime = to;
                        m_Time = to;
                        changed = true;
                    }
                }
            }
            if (ImGui::IsItemDeactivated() && m_Dragging)
            {
                m_Dragging = false;
                commit = true;
            }
            ImGui::PopID();
        }
        // Blend shapes: a row each, keys as the joints' (a click selects it; drag keys to move them, right-click to delete).
        if (!shapeNames.empty()) ImGui::TextDisabled("Blend Shapes");
        for (int s = 0; s < (int)shapeNames.size(); ++s)
        {
            std::string channel = AnimationClipAsset::kBlendShape + shapeNames[s];
            ImGui::PushID(100000 + s);
            bool selected = m_Shape == s;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 10.0f);
            if (ImGui::Selectable(shapeNames[s].c_str(), selected, 0, ImVec2(labelW - 22.0f, rowH)))
            {
                m_Event = -1;
                m_Shape = s;
                m_Joint = -1;
                m_Selected.clear();
            }
            ImGui::SameLine(labelW);
            ImVec2 p0 = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("##strip", ImVec2(stripW, rowH));
            bool hovered = ImGui::IsItemHovered();
            dl->AddRectFilled(p0, ImVec2(p0.x + stripW, p0.y + rowH), selected ? IM_COL32(52, 48, 66, 255) : IM_COL32(34, 35, 40, 255), 3.0f);
            float hit = -1.0f;
            for (float t : KeyTimes(asset, &channel))
            {
                float x = timeToX(p0.x, t);
                bool sel = selected && m_KeyTime >= 0.0f && std::abs(m_KeyTime - t) <= AnimationClipAsset::kKeyTolerance * 0.5f;
                diamond(x, p0.y + rowH * 0.5f, sel ? IM_COL32(255, 210, 90, 255) : IM_COL32(200, 170, 230, 255));
                if (hovered && std::abs(mouseX - x) < 6.0f) hit = t;
            }
            if (ImGui::IsItemClicked())
            {
                m_Shape = s;
                m_Joint = -1;
                m_Selected.clear();
                m_KeyTime = hit;
                m_Dragging = hit >= 0.0f && !playing;
                m_Time = hit >= 0.0f ? hit : xToTime(p0.x, mouseX);
            }
            if (hovered && hit >= 0.0f && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
            {
                pendingRemove = channel;
                removeAt = hit;
            }
            if (m_Dragging && m_Shape == s && ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f))
            {
                float to = xToTime(p0.x, mouseX);
                if (std::abs(to - m_KeyTime) > AnimationClipAsset::kKeyTolerance && asset.MoveKeys(channel, m_KeyTime, to))
                {
                    m_KeyTime = to;
                    m_Time = to;
                    changed = true;
                }
            }
            if (ImGui::IsItemDeactivated() && m_Dragging && m_Shape == s)
            {
                m_Dragging = false;
                commit = true;
            }
            ImGui::PopID();
        }
        // The current time across every row.
        {
            float x = timeToX(r0.x, m_Time);
            dl->AddLine(ImVec2(x, r0.y), ImVec2(x, ImGui::GetCursorScreenPos().y), IM_COL32(240, 120, 90, 255), 1.5f);
        }
        ImGui::EndChild();
        if (!pendingRemove.empty() && asset.RemoveKeys(pendingRemove, removeAt))
        {
            m_KeyTime = -1.0f;
            changed = commit = true;
        }

        if (m_Joint >= 0)
        {
            // The selected joint at the current time: editing a value keys it there (as recording does).
            const std::string& name = skeleton.Joints[m_Joint].Name;
            glm::quat q = pose.Rotations[m_Joint];
            if (!ImGui::IsAnyItemActive() || m_EulerJoint != m_Joint || std::abs(m_EulerTime - m_Time) > 1e-6f)
            {
                m_Euler = glm::degrees(glm::eulerAngles(q));
                m_EulerJoint = m_Joint;
                m_EulerTime = m_Time;
            }
            ImGui::TextUnformatted(name.c_str());
            ImGui::SameLine(labelW);
            if (playing) ImGui::BeginDisabled();
            ImGui::SetNextItemWidth(260);
            if (ImGui::DragFloat3("Rotation", &m_Euler.x, 0.5f, -360.0f, 360.0f, "%.1f"))
            {
                glm::quat r = glm::normalize(glm::quat(glm::radians(m_Euler)));
                asset.SetKey(name, AnimationPath::Rotation, m_Time, glm::vec4(r.x, r.y, r.z, r.w));
                m_KeyTime = m_Time;
                changed = true;
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) commit = true;
            ImGui::SameLine();
            glm::vec3 position = pose.Translations[m_Joint];
            ImGui::SetNextItemWidth(220);
            if (ImGui::DragFloat3("Position", &position.x, 0.005f, 0.0f, 0.0f, "%.3f"))
            {
                asset.SetKey(name, AnimationPath::Translation, m_Time, glm::vec4(position, 0.0f));
                m_KeyTime = m_Time;
                changed = true;
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) commit = true;
            ImGui::SetCursorPosX(labelW);
            // Keys the joints as they are posed now at the current time (rotation, and position where it is animated).
            auto keyPose = [&](const std::vector<int>& joints) {
                for (int k : joints)
                {
                    const std::string& n = skeleton.Joints[k].Name;
                    glm::quat r = pose.Rotations[k];
                    asset.SetKey(n, AnimationPath::Rotation, m_Time, glm::vec4(r.x, r.y, r.z, r.w));
                    if (asset.FindChannel(n, AnimationPath::Translation)) asset.SetKey(n, AnimationPath::Translation, m_Time, glm::vec4(pose.Translations[k], 0.0f));
                }
                m_KeyTime = m_Time;
                changed = commit = true;
            };
            if (ImGui::Button(m_Selected.size() > 1 ? "Key Selected" : "Key")) keyPose(m_Selected);
            ImGui::SetItemTooltip("Key the selected joints as they are posed now at the current time");
            ImGui::SameLine();
            if (ImGui::Button("Key All"))
            {
                std::vector<int> all(skeleton.Joints.size());
                for (size_t k = 0; k < all.size(); ++k) all[k] = (int)k;
                keyPose(all);
            }
            ImGui::SetItemTooltip("Key every joint as it is posed now at the current time (a whole pose)");
            ImGui::SameLine();
            bool keyed = false;
            for (const std::string& n : selectedNames()) keyed |= asset.HasKey(n, m_Time);
            if (!keyed) ImGui::BeginDisabled();
            if (ImGui::Button("Delete Key"))
            {
                for (const std::string& n : selectedNames()) asset.RemoveKeys(n, m_Time);
                m_KeyTime = -1.0f;
                changed = commit = true;
            }
            if (!keyed) ImGui::EndDisabled();
            // How the joint's keys are joined: straight, held, or smooth curves (automatic tangents).
            ImGui::SameLine();
            int mode = -1; // its channels' (the first's)
            for (const auto& c : asset.Channels)
                if (c.Joint == name && mode < 0)
                    mode = AnimationClipAsset::HasExplicitTangents(c) ? 3
                         : c.Smooth || c.Interpolation == AnimationInterpolation::CubicSpline ? 2
                         : c.Interpolation == AnimationInterpolation::Step ? 1 : 0;
            const char* modes[] = { "Linear", "Step", "Smooth", "Free" };
            ImGui::SetNextItemWidth(90);
            if (mode >= 0 && ImGui::Combo("##interp", &mode, modes, 4))
            {
                for (auto& c : asset.Channels)
                    if (c.Joint == name)
                    {
                        if (mode == 3)
                        {
                            AnimationClipAsset::MakeTangentsExplicit(c); // the automatic ones, to drag from
                            continue;
                        }
                        c.Smooth = mode == 2;
                        c.Interpolation = mode == 1 ? AnimationInterpolation::Step : AnimationInterpolation::Linear;
                        c.InTangents.clear();
                        c.OutTangents.clear();
                    }
                changed = commit = true;
            }
            if (mode >= 0)
                ImGui::SetItemTooltip("How the joint's keys are joined: straight lines, held until the next key, smooth curves (automatic "
                                      "tangents), or curves with tangents set by hand (drag a key's handles in the curve view)");
            if (playing) ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextDisabled("Ctrl / Shift click rows to select several; drag keys to move them, right-click one to delete it.");

            // The joint's curves over the clip: rotation (degrees, unwrapped) and position, each set scaled to fit,
            // keys as dots, the current time as a line. Clicking a key selects it (and goes to its time); a smooth
            // channel's selected key shows tangent handles: dragging one sets the curve's slope there (both sides
            // together, Alt for one side only), right-clicking a key puts its tangents back to automatic or flat.
            if (m_CurveJoint != m_Joint || m_CurveClip != m_Clip)
            {
                m_CurveJoint = m_Joint;
                m_CurveClip = m_Clip;
                m_CurveKey = -1;
                m_CurveDrag = 0;
            }
            if (m_AutoCurve.Key >= 0 && m_AutoCurve.Joint == name)
            {
                m_CurveSet = m_AutoCurve.Set;
                m_CurveAxis = m_AutoCurve.Axis;
                m_CurveKey = m_AutoCurve.Key;
                m_AutoCurve = {};
            }
            ImGuiIO& cio = ImGui::GetIO();
            ImVec2 c0 = ImGui::GetCursorScreenPos();
            float curveW = std::max(ImGui::GetContentRegionAvail().x, 100.0f);
            ImGui::InvisibleButton("##curves", ImVec2(curveW, curveH), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
            bool curveClicked = ImGui::IsItemClicked(ImGuiMouseButton_Left), curveRightClicked = ImGui::IsItemClicked(ImGuiMouseButton_Right);
            bool curveActive = ImGui::IsItemActive();
            ImDrawList* cdl = ImGui::GetWindowDrawList();
            cdl->AddRectFilled(c0, ImVec2(c0.x + curveW, c0.y + curveH), IM_COL32(30, 31, 36, 255), 3.0f);
            cdl->PushClipRect(c0, ImVec2(c0.x + curveW, c0.y + curveH), true);
            const float pxPerSecond = (curveW - 2.0f * inset) / std::max(asset.Duration, 0.01f);
            auto curveX = [&](float t) { return c0.x + inset + t * pxPerSecond; };
            const int samples = 160;
            const ImU32 axisColors[3] = { IM_COL32(235, 90, 90, 255), IM_COL32(110, 205, 95, 255), IM_COL32(90, 150, 245, 255) };
            // What the mouse is over: a key (part 0) or the selected key's in / out handle (1 / 2), nearest first.
            struct CurveHit { int Set = -1, Axis = 0, Key = -1, Part = 0; float Dist = 1e30f; } hit;
            auto consider = [&](ImVec2 p, int set, int axis, int key, int part, float radius) {
                float d = std::hypot(cio.MousePos.x - p.x, cio.MousePos.y - p.y);
                if (d > radius) return;
                if (part > 0) d -= 100.0f; // handles over the keys beneath them
                if (d < hit.Dist) hit = { set, axis, key, part, d };
            };
            // The selected key's handles, for the drag below: where the key is, and the scale.
            ImVec2 keyPos{ 0, 0 };
            float pxPerValue = 1.0f;
            const AnimationChannel* selectedChannel = nullptr;
            std::string keyInfo;
            for (int set = 0; set < 2; ++set)
            {
                AnimationPath path = set == 0 ? AnimationPath::Rotation : AnimationPath::Translation;
                const AnimationChannel* channel = nullptr;
                int jointIndex = m_Joint;
                for (const auto& ch : sampled.Channels)
                    if (ch.Joint == jointIndex && ch.Path == path) channel = &ch;
                if (!channel) continue;
                bool rotation = set == 0;
                std::vector<glm::vec3> values(samples + 1);
                for (int i = 0; i <= samples; ++i)
                {
                    glm::vec4 v = Animation::SampleChannel(*channel, asset.Duration * i / samples);
                    glm::vec3 x = rotation ? EulerDegrees(v) : glm::vec3(v);
                    if (rotation && i > 0)
                        for (int a = 0; a < 3; ++a) x[a] += 360.0f * std::round((values[i - 1][a] - x[a]) / 360.0f); // unwrapped
                    values[i] = x;
                }
                // The scale fits the curves, held while a handle is dragged (so the curve moves, not the axis).
                float lo = 1e30f, hi = -1e30f;
                if (m_CurveDrag != 0)
                    lo = m_CurveLo[set], hi = m_CurveHi[set];
                else
                {
                    for (const glm::vec3& v : values)
                        for (int a = 0; a < 3; ++a) lo = std::min(lo, v[a]), hi = std::max(hi, v[a]);
                    if (hi - lo < 1e-4f) lo -= 1.0f, hi += 1.0f;
                    float pad = (hi - lo) * 0.08f;
                    lo -= pad, hi += pad;
                    m_CurveLo[set] = lo, m_CurveHi[set] = hi;
                }
                float valueScale = (curveH - 12.0f) / (hi - lo);
                auto curveY = [&](float v) { return c0.y + curveH - 6.0f - (v - lo) * valueScale; };
                bool tangents = channel->Interpolation == AnimationInterpolation::CubicSpline && channel->InTangents.size() == channel->Values.size() &&
                                channel->OutTangents.size() == channel->Values.size();
                for (int a = 0; a < 3; ++a)
                {
                    ImU32 col = rotation ? axisColors[a] : (axisColors[a] & 0x00FFFFFF) | 0x90000000;
                    for (int i = 0; i < samples; ++i)
                        cdl->AddLine(ImVec2(curveX(asset.Duration * i / samples), curveY(values[i][a])),
                                     ImVec2(curveX(asset.Duration * (i + 1) / samples), curveY(values[i + 1][a])), col, rotation ? 1.5f : 1.0f);
                    for (size_t k = 0; k < channel->Times.size(); ++k)
                    {
                        float t = channel->Times[k];
                        int i = std::clamp((int)std::round(t / std::max(asset.Duration, 0.01f) * samples), 0, samples);
                        // The key's own value (unwrapped beside the drawn curve).
                        float v = rotation ? EulerDegrees(channel->Values[k])[a] : channel->Values[k][a];
                        if (rotation) v += 360.0f * std::round((values[i][a] - v) / 360.0f);
                        ImVec2 p(curveX(t), curveY(v));
                        bool selected = m_CurveSet == set && m_CurveAxis == a && m_CurveKey == (int)k;
                        cdl->AddCircleFilled(p, selected ? 4.0f : 2.5f, col);
                        if (selected) cdl->AddCircle(p, 6.0f, IM_COL32(255, 255, 255, 230), 0, 1.5f);
                        consider(p, set, a, (int)k, 0, 7.0f);
                        if (!selected) continue;
                        keyPos = p;
                        pxPerValue = valueScale;
                        selectedChannel = channel;
                        char info[160];
                        std::snprintf(info, sizeof(info), "%s %c, key %d at %.2f s: %.2f%s", rotation ? "Rotation" : "Position", "xyz"[a], (int)k + 1, t, v,
                                      rotation ? "\xC2\xB0" : "");
                        keyInfo = info;
                        if (!tangents) continue;
                        // Its tangents as the curve shows them (slope per second), drawn as handles a fixed length out.
                        float slopes[2] = { ShownRate(rotation, channel->Values[k], channel->InTangents[k])[a],
                                            ShownRate(rotation, channel->Values[k], channel->OutTangents[k])[a] };
                        for (int side = 0; side < 2; ++side)
                        {
                            glm::vec2 dir = glm::normalize(glm::vec2(pxPerSecond, -slopes[side] * valueScale)) * 34.0f * (side == 0 ? -1.0f : 1.0f);
                            ImVec2 h(p.x + dir.x, p.y + dir.y);
                            cdl->AddLine(p, h, IM_COL32(220, 220, 225, 200), 1.0f);
                            cdl->AddRectFilled(ImVec2(h.x - 3.5f, h.y - 3.5f), ImVec2(h.x + 3.5f, h.y + 3.5f),
                                               m_CurveDrag == side + 1 ? IM_COL32(255, 200, 80, 255) : IM_COL32(235, 235, 240, 255));
                            consider(h, set, a, (int)k, side + 1, 7.0f);
                        }
                        std::snprintf(info, sizeof(info), "   in %.1f, out %.1f%s per second", slopes[0], slopes[1], rotation ? "\xC2\xB0" : "");
                        keyInfo += info;
                    }
                }
            }
            if (!ImGui::IsItemHovered() && !curveActive) hit = {};
            if (curveClicked)
            {
                m_CurveScrub = false;
                if (hit.Part > 0 && !playing)
                    m_CurveDrag = hit.Part;
                else if (hit.Key >= 0)
                {
                    m_CurveSet = hit.Set, m_CurveAxis = hit.Axis, m_CurveKey = hit.Key;
                    for (const auto& ch : sampled.Channels)
                        if (ch.Joint == m_Joint && ch.Path == (hit.Set == 0 ? AnimationPath::Rotation : AnimationPath::Translation) && hit.Key < (int)ch.Times.size())
                            m_Time = ch.Times[hit.Key];
                }
                else
                {
                    m_CurveKey = -1;
                    m_CurveScrub = true;
                }
            }
            if (curveActive && m_CurveScrub)
                m_Time = std::clamp((cio.MousePos.x - c0.x - inset) / (curveW - 2.0f * inset) * asset.Duration, 0.0f, asset.Duration);
            // Dragging a handle: the slope from the key to the mouse (pointing out of the key on its side).
            if (m_CurveDrag != 0 && curveActive && selectedChannel && m_CurveKey < (int)selectedChannel->Values.size() &&
                selectedChannel->InTangents.size() == selectedChannel->Values.size())
            {
                bool rotation = m_CurveSet == 0;
                AnimationPath path = rotation ? AnimationPath::Rotation : AnimationPath::Translation;
                float dx = cio.MousePos.x - keyPos.x, dy = cio.MousePos.y - keyPos.y;
                dx = m_CurveDrag == 2 ? std::max(dx, 1.0f) : std::min(dx, -1.0f);
                float slope = -(dy / pxPerValue) / (dx / pxPerSecond);
                const glm::vec4& value = selectedChannel->Values[m_CurveKey];
                glm::vec3 inRate = ShownRate(rotation, value, selectedChannel->InTangents[m_CurveKey]);
                glm::vec3 outRate = ShownRate(rotation, value, selectedChannel->OutTangents[m_CurveKey]);
                bool oneSide = cio.KeyAlt;
                if (m_CurveDrag == 1 || !oneSide) inRate[m_CurveAxis] = slope;
                if (m_CurveDrag == 2 || !oneSide) outRate[m_CurveAxis] = slope;
                glm::vec4 in = TangentFor(rotation, value, inRate), out = TangentFor(rotation, value, outRate);
                if (asset.SetTangents(name, path, (size_t)m_CurveKey, &in, &out)) changed = true;
            }
            if (m_CurveDrag != 0 && !curveActive)
            {
                m_CurveDrag = 0;
                commit = true;
            }
            // Right-click a key: its tangents back to automatic, or flat.
            if (curveRightClicked && hit.Key >= 0 && !playing)
            {
                m_CurveSet = hit.Set, m_CurveAxis = hit.Axis, m_CurveKey = hit.Key;
                ImGui::OpenPopup("##curveKey");
            }
            if (ImGui::BeginPopup("##curveKey"))
            {
                AnimationPath path = m_CurveSet == 0 ? AnimationPath::Rotation : AnimationPath::Translation;
                if (ImGui::MenuItem("Auto Tangents") && asset.AutoTangent(name, path, (size_t)std::max(m_CurveKey, 0))) changed = commit = true;
                ImGui::SetItemTooltip("The key's slope from its neighbours, as Smooth keys have");
                if (ImGui::MenuItem("Flat Tangents") && asset.FlatTangent(name, path, (size_t)std::max(m_CurveKey, 0))) changed = commit = true;
                ImGui::SetItemTooltip("Level on both sides: the curve eases in and out of the key");
                ImGui::EndPopup();
            }
            cdl->AddLine(ImVec2(curveX(m_Time), c0.y), ImVec2(curveX(m_Time), c0.y + curveH), IM_COL32(240, 120, 90, 255), 1.5f);
            cdl->AddText(ImVec2(c0.x + 6, c0.y + 3), IM_COL32(150, 150, 160, 255), "Rotation (solid), position (faint): x red, y green, z blue");
            if (!keyInfo.empty()) cdl->AddText(ImVec2(c0.x + 6, c0.y + curveH - ImGui::GetFontSize() - 3), IM_COL32(210, 210, 220, 255), keyInfo.c_str());
            cdl->PopClipRect();
        }
        else if (m_Event >= 0 && m_Event < (int)asset.Events.size())
        {
            // The selected event: its function name and time.
            auto& ev = asset.Events[m_Event];
            ImGui::TextUnformatted("Event");
            ImGui::SameLine(labelW);
            if (playing) ImGui::BeginDisabled();
            ImGui::SetNextItemWidth(220);
            if (ImGui::InputText("Function", &ev.Function)) changed = true;
            if (ImGui::IsItemDeactivatedAfterEdit()) commit = true;
            ImGui::SetItemTooltip("Scripts on the object get OnAnimationEvent with this name when playback passes the event");
            ImGui::SameLine();
            float t = ev.Time;
            ImGui::SetNextItemWidth(90);
            if (ImGui::DragFloat("Time", &t, 0.005f, 0.0f, asset.Duration, "%.3f s"))
            {
                m_Event = asset.MoveEvent(m_Event, t);
                m_Time = t;
                changed = true;
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) commit = true;
            ImGui::SameLine();
            if (ImGui::Button("Delete Event"))
            {
                asset.Events.erase(asset.Events.begin() + m_Event);
                ++asset.Version;
                m_Event = -1;
                changed = commit = true;
            }
            if (playing) ImGui::EndDisabled();
        }
        else if (m_Shape >= 0)
        {
            // The selected blend shape's weight at the current time (0..100): editing it keys it there.
            std::string channel = AnimationClipAsset::kBlendShape + shapeNames[m_Shape];
            float weight = m_Shape < (int)pose.Weights.size() && !std::isnan(pose.Weights[m_Shape]) ? pose.Weights[m_Shape] * 100.0f
                         : m_Shape < (int)mr->BlendShapeWeights.size() ? mr->BlendShapeWeights[m_Shape] : 0.0f;
            ImGui::TextUnformatted(shapeNames[m_Shape].c_str());
            ImGui::SameLine(labelW);
            if (playing) ImGui::BeginDisabled();
            ImGui::SetNextItemWidth(260);
            if (ImGui::SliderFloat("Weight", &weight, 0.0f, 100.0f, "%.1f"))
            {
                asset.SetKey(channel, AnimationPath::Translation, m_Time, glm::vec4(weight * 0.01f, 0.0f, 0.0f, 0.0f));
                changed = true;
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) commit = true;
            ImGui::SetItemTooltip("The shape's weight at the current time; changing it keys it there");
            ImGui::SameLine();
            bool keyed = asset.HasKey(channel, m_Time);
            if (ImGui::Button("Key"))
            {
                asset.SetKey(channel, AnimationPath::Translation, m_Time, glm::vec4(weight * 0.01f, 0.0f, 0.0f, 0.0f));
                changed = commit = true;
            }
            ImGui::SameLine();
            if (!keyed) ImGui::BeginDisabled();
            if (ImGui::Button("Delete Key"))
            {
                asset.RemoveKeys(channel, m_Time);
                m_KeyTime = -1.0f;
                changed = commit = true;
            }
            if (!keyed) ImGui::EndDisabled();
            if (playing) ImGui::EndDisabled();
        }
        else
            ImGui::TextDisabled("Select a joint to key its rotation and position, a blend shape its weight, or an event to name it (+ Event adds one).");

        if (changed)
        {
            ++asset.Version;
            AssetManager::TouchAnimationClips(); // Animators take the edit up at once
        }
        if (commit) AssetManager::SaveAnimationClip(m_Clip);
        ImGui::End();
    }

    void AnimationPanel::SelectCurveKey(const std::string& spec)
    {
        // "Joint,rotation|position,axis,key"
        std::vector<std::string> parts;
        size_t from = 0;
        for (size_t comma; (comma = spec.find(',', from)) != std::string::npos; from = comma + 1) parts.push_back(spec.substr(from, comma - from));
        parts.push_back(spec.substr(from));
        m_AutoCurve = {};
        m_AutoCurve.Joint = parts[0];
        if (parts.size() >= 4)
        {
            m_AutoCurve.Set = parts[1] == "position" ? 1 : 0;
            m_AutoCurve.Axis = std::clamp(std::atoi(parts[2].c_str()), 0, 2);
            m_AutoCurve.Key = std::max(std::atoi(parts[3].c_str()), 0);
        }
    }

    bool AnimationPanel::IsPosing() const { return !m_PreviewEntity.IsNull() && m_Joint >= 0 && m_Clip.IsValid(); }

    bool AnimationPanel::UpdateSceneView(ImVec2 origin, ImVec2 size, const glm::mat4& view, const glm::mat4& proj, bool orthographic, bool hovered)
    {
        if (m_PreviewEntity.IsNull() || !m_Ctx.EditorScene || m_Ctx.IsPlaying()) return false;
        Scene& scene = *m_Ctx.EditorScene;
        auto& reg = scene.GetRegistry();
        if (!scene.IsValid(m_PreviewEntity)) return false;
        auto* anim = reg.TryGet<AnimatorComponent>(m_PreviewEntity);
        auto* mr = reg.TryGet<MeshRendererComponent>(m_PreviewEntity);
        auto clip = AssetManager::GetAnimationClip(m_Clip);
        // Posing works on the clip's own model (an Animation Source's skeleton is not the one drawn).
        if (!anim || !mr || !clip || anim->ClipSource.IsValid() || clip->Model != mr->Mesh) return false;
        Ref<Mesh> model = AssetManager::GetMesh(mr->Mesh);
        if (!model || !model->GetSkeleton()) return false;
        const Skeleton& skeleton = *model->GetSkeleton();
        const glm::mat4& world = reg.Get<TransformComponent>(m_PreviewEntity).World;

        // The pose at the current time, in the world.
        Pose pose;
        Animation::Sample(skeleton, clip->ToClip(skeleton), m_Time, pose); // (joints only: no shapes needed here)
        std::vector<glm::mat4> globals;
        Animation::JointGlobals(skeleton, pose, globals);
        auto worldOf = [&](int j) { return world * globals[j]; };
        auto screenOf = [&](const glm::vec3& p, bool& visible) {
            glm::vec4 c = proj * view * glm::vec4(p, 1.0f);
            visible = c.w > 1e-4f;
            glm::vec2 ndc = glm::vec2(c) / std::max(c.w, 1e-4f);
            return ImVec2(origin.x + (ndc.x * 0.5f + 0.5f) * size.x, origin.y + (0.5f - ndc.y * 0.5f) * size.y);
        };

        // The skeleton: bones from each joint to its parent, the joints as dots (click one to select it).
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 mouse = ImGui::GetMousePos();
        int closest = -1;
        float best = 8.0f;
        ImU32 accent = ImGui::GetColorU32(ImGuiCol_HeaderActive);
        for (int j = 0; j < (int)skeleton.Joints.size(); ++j)
        {
            bool visible, parentVisible = false;
            ImVec2 s = screenOf(glm::vec3(worldOf(j)[3]), visible);
            if (!visible) continue;
            if (int p = skeleton.Joints[j].Parent; p >= 0)
            {
                ImVec2 ps = screenOf(glm::vec3(worldOf(p)[3]), parentVisible);
                if (parentVisible) dl->AddLine(ps, s, IM_COL32(120, 200, 255, 200), 2.0f);
            }
            float d = std::hypot(mouse.x - s.x, mouse.y - s.y);
            if (hovered && d < best)
            {
                best = d;
                closest = j;
            }
        }
        for (int j = 0; j < (int)skeleton.Joints.size(); ++j)
        {
            bool visible;
            ImVec2 s = screenOf(glm::vec3(worldOf(j)[3]), visible);
            if (!visible) continue;
            bool picked = j == m_Joint;
            dl->AddCircleFilled(s, picked ? 6.0f : 4.0f, picked ? accent : closest == j ? IM_COL32(255, 230, 140, 255) : IM_COL32(120, 200, 255, 255));
            dl->AddCircle(s, picked ? 6.0f : 4.0f, IM_COL32(20, 20, 20, 255), 0, 1.5f);
        }
        if (closest >= 0 && !ImGuizmo::IsUsing()) ImGui::SetTooltip("%s", skeleton.Joints[closest].Name.c_str());

        // The selected joint's rotation gizmo: turning it keys the joint's rotation at the current time.
        bool overGizmo = false;
        if (m_Joint >= 0 && m_Joint < (int)skeleton.Joints.size())
        {
            ImGuizmo::SetOrthographic(orthographic);
            ImGuizmo::SetDrawlist();
            ImGuizmo::SetRect(origin.x, origin.y, size.x, size.y);
            ImGuizmo::PushID(0xA41);
            glm::mat4 m = worldOf(m_Joint);
            // Without scale (a gizmo turns a frame), at the joint.
            for (int c = 0; c < 3; ++c) m[c] = glm::vec4(glm::normalize(glm::vec3(m[c])), 0.0f);
            overGizmo = ImGuizmo::IsOver();
            if (ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(proj), ImGuizmo::ROTATE, ImGuizmo::LOCAL, glm::value_ptr(m)))
            {
                int parent = skeleton.Joints[m_Joint].Parent;
                glm::mat4 parentWorld = parent >= 0 ? worldOf(parent) : world * skeleton.RootTransform;
                auto rotationOf = [](const glm::mat4& x) {
                    glm::mat3 r(x);
                    for (int c = 0; c < 3; ++c) r[c] /= std::max(glm::length(r[c]), 1e-8f);
                    return glm::normalize(glm::quat_cast(r));
                };
                glm::quat local = glm::normalize(glm::inverse(rotationOf(parentWorld)) * rotationOf(m));
                clip->SetKey(skeleton.Joints[m_Joint].Name, AnimationPath::Rotation, m_Time, glm::vec4(local.x, local.y, local.z, local.w));
                m_KeyTime = m_Time;
                ++clip->Version;
                AssetManager::TouchAnimationClips();
            }
            bool using_ = ImGuizmo::IsUsing();
            if (m_GizmoUsing && !using_) AssetManager::SaveAnimationClip(m_Clip); // a turn done: written
            m_GizmoUsing = using_;
            overGizmo |= using_;
            ImGuizmo::PopID();
        }
        if (hovered && closest >= 0 && !overGizmo && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            m_Joint = closest;
            return true; // consumed: the object stays selected
        }
        return overGizmo || closest >= 0;
    }
}
