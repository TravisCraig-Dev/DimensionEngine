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
                    AnimationClipAsset made = AnimationClipAsset::FromClip(skeleton, c);
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
        AnimationClip sampled = asset.ToClip(skeleton);
        Pose pose;
        Animation::Sample(skeleton, sampled, m_Time, pose);
        if (m_Joint >= (int)skeleton.Joints.size()) m_Joint = -1;

        // ---- Joints and their keys (a dope sheet), then the selected joint's values ----
        const float labelW = 220.0f, rowH = ImGui::GetFrameHeight(), inset = 8.0f;
        const float curveH = 120.0f;
        float detailH = m_Joint >= 0 ? ImGui::GetFrameHeightWithSpacing() * 3.0f + curveH + 12.0f : ImGui::GetFrameHeightWithSpacing() + 4.0f;
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

        std::string pendingRemove;
        float removeAt = 0.0f;
        for (int j : skeleton.Order)
        {
            const std::string& name = skeleton.Joints[j].Name;
            ImGui::PushID(j);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + Depth(skeleton, j) * 10.0f);
            ImVec2 labelAt = ImGui::GetCursorScreenPos();
            if (ImGui::Selectable(name.c_str(), m_Joint == j, 0, ImVec2(std::max(labelW - 12.0f - (labelAt.x - ImGui::GetWindowPos().x), 20.0f), rowH)))
                m_Joint = j;
            ImGui::SameLine(labelW);
            ImVec2 p0 = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("##strip", ImVec2(stripW, rowH));
            bool hovered = ImGui::IsItemHovered();
            dl->AddRectFilled(p0, ImVec2(p0.x + stripW, p0.y + rowH), m_Joint == j ? IM_COL32(44, 52, 66, 255) : IM_COL32(34, 35, 40, 255), 3.0f);
            float hit = -1.0f;
            for (float t : KeyTimes(asset, &name))
            {
                float x = timeToX(p0.x, t);
                bool sel = m_Joint == j && m_KeyTime >= 0.0f && std::abs(m_KeyTime - t) <= AnimationClipAsset::kKeyTolerance * 0.5f;
                diamond(x, p0.y + rowH * 0.5f, sel ? IM_COL32(255, 210, 90, 255) : IM_COL32(200, 200, 210, 255));
                if (hovered && std::abs(mouseX - x) < 6.0f) hit = t;
            }
            if (ImGui::IsItemClicked())
            {
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
                if (std::abs(to - m_KeyTime) > AnimationClipAsset::kKeyTolerance && asset.MoveKeys(name, m_KeyTime, to))
                {
                    m_KeyTime = to;
                    m_Time = to;
                    changed = true;
                }
            }
            if (ImGui::IsItemDeactivated() && m_Dragging)
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
            if (ImGui::Button("Key"))
            {
                // The joint's current pose (rotation, and position where it is animated) keyed at this time.
                glm::quat r = pose.Rotations[m_Joint];
                asset.SetKey(name, AnimationPath::Rotation, m_Time, glm::vec4(r.x, r.y, r.z, r.w));
                if (asset.FindChannel(name, AnimationPath::Translation)) asset.SetKey(name, AnimationPath::Translation, m_Time, glm::vec4(pose.Translations[m_Joint], 0.0f));
                m_KeyTime = m_Time;
                changed = commit = true;
            }
            ImGui::SetItemTooltip("Key the joint as it is posed now at the current time");
            ImGui::SameLine();
            bool keyed = asset.HasKey(name, m_Time);
            if (!keyed) ImGui::BeginDisabled();
            if (ImGui::Button("Delete Key") && asset.RemoveKeys(name, m_Time))
            {
                m_KeyTime = -1.0f;
                changed = commit = true;
            }
            if (!keyed) ImGui::EndDisabled();
            // How the joint's keys are joined: straight, held, or smooth curves (automatic tangents).
            ImGui::SameLine();
            int mode = -1; // its channels' (the first's)
            for (const auto& c : asset.Channels)
                if (c.Joint == name && mode < 0)
                    mode = c.Smooth || c.Interpolation == AnimationInterpolation::CubicSpline ? 2 : c.Interpolation == AnimationInterpolation::Step ? 1 : 0;
            const char* modes[] = { "Linear", "Step", "Smooth" };
            ImGui::SetNextItemWidth(90);
            if (mode >= 0 && ImGui::Combo("##interp", &mode, modes, 3))
            {
                for (auto& c : asset.Channels)
                    if (c.Joint == name)
                    {
                        c.Smooth = mode == 2;
                        c.Interpolation = mode == 1 ? AnimationInterpolation::Step : AnimationInterpolation::Linear;
                        c.InTangents.clear();
                        c.OutTangents.clear();
                    }
                changed = commit = true;
            }
            if (mode >= 0) ImGui::SetItemTooltip("How the joint's keys are joined: straight lines, held until the next key, or smooth curves");
            if (playing) ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextDisabled("Drag keys along a row to move them; right-click one to delete it.");

            // The joint's curves over the clip: rotation (degrees, unwrapped) and position, each set scaled to fit,
            // keys as dots, the current time as a line.
            ImVec2 c0 = ImGui::GetCursorScreenPos();
            float curveW = std::max(ImGui::GetContentRegionAvail().x, 100.0f);
            ImGui::InvisibleButton("##curves", ImVec2(curveW, curveH));
            if (ImGui::IsItemActive()) m_Time = std::clamp((ImGui::GetIO().MousePos.x - c0.x - inset) / (curveW - 2.0f * inset) * asset.Duration, 0.0f, asset.Duration);
            ImDrawList* cdl = ImGui::GetWindowDrawList();
            cdl->AddRectFilled(c0, ImVec2(c0.x + curveW, c0.y + curveH), IM_COL32(30, 31, 36, 255), 3.0f);
            auto curveX = [&](float t) { return c0.x + inset + t / std::max(asset.Duration, 0.01f) * (curveW - 2.0f * inset); };
            const int samples = 160;
            const ImU32 axisColors[3] = { IM_COL32(235, 90, 90, 255), IM_COL32(110, 205, 95, 255), IM_COL32(90, 150, 245, 255) };
            for (int set = 0; set < 2; ++set)
            {
                AnimationPath path = set == 0 ? AnimationPath::Rotation : AnimationPath::Translation;
                const AnimationChannel* channel = nullptr;
                int jointIndex = m_Joint;
                for (const auto& ch : sampled.Channels)
                    if (ch.Joint == jointIndex && ch.Path == path) channel = &ch;
                if (!channel) continue;
                std::vector<glm::vec3> values(samples + 1);
                for (int i = 0; i <= samples; ++i)
                {
                    glm::vec4 v = Animation::SampleChannel(*channel, asset.Duration * i / samples);
                    glm::vec3 x = set == 0 ? glm::degrees(glm::eulerAngles(glm::quat(v.w, v.x, v.y, v.z))) : glm::vec3(v);
                    if (set == 0 && i > 0)
                        for (int a = 0; a < 3; ++a) x[a] += 360.0f * std::round((values[i - 1][a] - x[a]) / 360.0f); // unwrapped
                    values[i] = x;
                }
                float lo = 1e30f, hi = -1e30f;
                for (const glm::vec3& v : values)
                    for (int a = 0; a < 3; ++a) lo = std::min(lo, v[a]), hi = std::max(hi, v[a]);
                if (hi - lo < 1e-4f) lo -= 1.0f, hi += 1.0f;
                auto curveY = [&](float v) { return c0.y + curveH - 6.0f - (v - lo) / (hi - lo) * (curveH - 12.0f); };
                for (int a = 0; a < 3; ++a)
                {
                    ImU32 col = set == 0 ? axisColors[a] : (axisColors[a] & 0x00FFFFFF) | 0x90000000;
                    for (int i = 0; i < samples; ++i)
                        cdl->AddLine(ImVec2(curveX(asset.Duration * i / samples), curveY(values[i][a])),
                                     ImVec2(curveX(asset.Duration * (i + 1) / samples), curveY(values[i + 1][a])), col, set == 0 ? 1.5f : 1.0f);
                    for (float t : channel->Times)
                    {
                        int i = std::clamp((int)std::round(t / std::max(asset.Duration, 0.01f) * samples), 0, samples);
                        cdl->AddCircleFilled(ImVec2(curveX(t), curveY(values[i][a])), 2.5f, col);
                    }
                }
            }
            cdl->AddLine(ImVec2(curveX(m_Time), c0.y), ImVec2(curveX(m_Time), c0.y + curveH), IM_COL32(240, 120, 90, 255), 1.5f);
            cdl->AddText(ImVec2(c0.x + 6, c0.y + 3), IM_COL32(150, 150, 160, 255), "Rotation (solid), position (faint): x red, y green, z blue");
        }
        else
            ImGui::TextDisabled("Select a joint to see and key its rotation and position at the current time.");

        if (changed)
        {
            ++asset.Version;
            AssetManager::TouchAnimationClips(); // Animators take the edit up at once
        }
        if (commit) AssetManager::SaveAnimationClip(m_Clip);
        ImGui::End();
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
        Animation::Sample(skeleton, clip->ToClip(skeleton), m_Time, pose);
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
