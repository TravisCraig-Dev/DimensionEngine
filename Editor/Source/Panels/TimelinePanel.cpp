#include "Panels.h"
#include "../EditorUI.h"
#include <Dimension/Animation/Timeline.h>
#include <Dimension/Scene/ComponentRegistry.h>
#include <imgui.h>
#include <imgui_stdlib.h>
#include <algorithm>
#include <cmath>

namespace Editor
{
    namespace
    {
        const char* kInterpNames[] = { "Smooth", "Linear", "Step" };

        void CollectObjects(Scene& scene, Entity e, std::vector<Entity>& out)
        {
            out.push_back(e);
            for (Entity c : scene.GetChildren(e)) CollectObjects(scene, c, out);
        }

        std::string TrackLabel(const TimelineTrack& t)
        {
            return (t.Path.empty() ? std::string() : t.Path + " : ") + t.Component + "." + t.Field;
        }
    }

    void TimelinePanel::OnImGui(bool* open)
    {
        ImGui::SetNextWindowSize(ImVec2(900, 360), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Timeline", open))
        {
            ImGui::End();
            return;
        }
        // Target: the selected object's Timeline (its asset, its objects for preview).
        Scene& scene = m_Ctx.ActiveScene();
        auto& reg = scene.GetRegistry();
        Entity entity = Entity::Null();
        if (m_Ctx.HasValidSelection())
            if (auto* tl = reg.TryGet<TimelineComponent>(m_Ctx.Selected); tl && tl->Timeline.IsValid())
            {
                entity = m_Ctx.Selected;
                if (m_Handle != tl->Timeline) { m_Track = m_Key = -1; m_Time = 0.0f; }
                m_Handle = tl->Timeline;
            }
        auto timeline = entity.IsNull() ? nullptr : AssetManager::GetTimeline(m_Handle);
        if (!timeline)
        {
            ImGui::TextDisabled("Select an object with a Timeline component (and a Timeline asset: Project > Create > New Timeline).");
            ImGui::End();
            return;
        }
        TimelineAsset& tl = *timeline;
        auto& comp = reg.Get<TimelineComponent>(entity);
        bool playing = m_Ctx.IsPlaying();
        bool changed = false; // the asset (saved at the end)
        bool scrubbed = false; // the preview time (applied to the scene)

        // ---- Header: playback, time and length ----
        ImGui::TextDisabled("%s  -  %s", AssetManager::GetDisplayName(m_Handle).c_str(), reg.Get<TagComponent>(entity).Name.c_str());
        if (playing)
        {
            m_Time = comp.Time;
            ImGui::SameLine();
            ImGui::TextDisabled(comp.Playing ? "   (playing %.2f s)" : "   (stopped at %.2f s)", comp.Time);
        }
        else
        {
            if (ImGui::Button(m_Previewing ? "Pause" : "Preview"))
            {
                if (!m_Previewing) m_Ctx.RecordUndo();
                m_Previewing = !m_Previewing;
            }
            ImGui::SetItemTooltip("Plays the timeline on the scene's objects (Undo puts them back)");
            if (m_Previewing)
            {
                m_Time += ImGui::GetIO().DeltaTime * comp.Speed;
                if (m_Time > tl.Duration) m_Time = comp.Loop ? std::fmod(m_Time, tl.Duration) : (m_Previewing = false, tl.Duration);
                scrubbed = true;
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(260);
            if (ImGui::SliderFloat("Time", &m_Time, 0.0f, tl.Duration, "%.2f s")) scrubbed = true;
            if (ImGui::IsItemActivated()) m_Ctx.RecordUndo();
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90);
        if (ImGui::DragFloat("Duration", &tl.Duration, 0.05f, 0.05f, 3600.0f, "%.2f s")) changed = true;

        // ---- Tracks: a label column and a strip with the keys ----
        const float labelW = 360.0f, controlsX = labelW - 170.0f, inset = 8.0f; // keys stay clear of the strip edges
        float stripW = std::max(ImGui::GetContentRegionAvail().x - labelW - 10.0f, 100.0f);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        auto timeToX = [&](float x0, float t) { return x0 + inset + t / std::max(tl.Duration, 0.01f) * (stripW - 2.0f * inset); };
        auto xToTime = [&](float x0, float x) { return std::clamp((x - x0 - inset) / (stripW - 2.0f * inset) * tl.Duration, 0.0f, tl.Duration); };
        int removeTrack = -1;
        for (int ti = 0; ti < (int)tl.Tracks.size(); ++ti)
        {
            TimelineTrack& track = tl.Tracks[ti];
            ImGui::PushID(ti);
            ImGui::BeginGroup();
            if (ImGui::Checkbox("##mute", &track.Muted)) changed = true;
            ImGui::SetItemTooltip("Mute (checked tracks are not applied)");
            ImGui::SameLine();
            {
                ImVec2 at = ImGui::GetCursorScreenPos();
                float clipX = ImGui::GetWindowPos().x - ImGui::GetScrollX() + controlsX - 6.0f;
                ImGui::PushClipRect(at, ImVec2(clipX, at.y + ImGui::GetFrameHeight()), true);
                ImGui::TextUnformatted(TrackLabel(track).c_str());
                ImGui::PopClipRect();
                ImGui::SetItemTooltip("%s", TrackLabel(track).c_str());
            }
            ImGui::SameLine(controlsX);
            ImGui::SetNextItemWidth(70);
            int interp = (int)track.Interpolation;
            if (ImGui::Combo("##interp", &interp, kInterpNames, 3)) { track.Interpolation = (TimelineInterpolation)interp; changed = true; }
            ImGui::SameLine();
            if (ImGui::SmallButton("Key"))
            {
                float value[4];
                if (int n = Timeline::ReadCurrent(scene, entity, track, value))
                {
                    Timeline::SetKey(track, m_Time, value, n);
                    changed = true;
                }
            }
            ImGui::SetItemTooltip("Key the field's current value at the current time");
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) removeTrack = ti;
            ImGui::EndGroup();
            // Strip.
            ImGui::SameLine(labelW);
            ImVec2 p0 = ImGui::GetCursorScreenPos();
            float h = ImGui::GetFrameHeight();
            ImGui::InvisibleButton("##strip", ImVec2(stripW, h));
            bool stripHovered = ImGui::IsItemHovered();
            dl->AddRectFilled(p0, ImVec2(p0.x + stripW, p0.y + h), IM_COL32(34, 35, 40, 255), 3.0f);
            int hitKey = -1;
            for (int k = 0; k < (int)track.Keys.size(); ++k)
            {
                float x = timeToX(p0.x, track.Keys[k].Time), cy = p0.y + h * 0.5f;
                bool sel = m_Track == ti && m_Key == k;
                ImU32 col = sel ? IM_COL32(255, 210, 90, 255) : track.Muted ? IM_COL32(110, 110, 115, 255) : IM_COL32(200, 200, 210, 255);
                dl->AddQuadFilled(ImVec2(x, cy - 6), ImVec2(x + 6, cy), ImVec2(x, cy + 6), ImVec2(x - 6, cy), col);
                if (stripHovered && std::abs(ImGui::GetIO().MousePos.x - x) < 7.0f) hitKey = k;
            }
            if (ImGui::IsItemClicked())
            {
                if (hitKey >= 0) { m_Track = ti; m_Key = hitKey; m_DraggingKey = true; }
                else if (!playing)
                {
                    m_Ctx.RecordUndo();
                    m_Time = xToTime(p0.x, ImGui::GetIO().MousePos.x);
                    scrubbed = true;
                }
            }
            if (m_DraggingKey && m_Track == ti && m_Key >= 0 && m_Key < (int)track.Keys.size() && ImGui::IsItemActive() &&
                ImGui::IsMouseDragging(ImGuiMouseButton_Left))
            {
                track.Keys[m_Key].Time = xToTime(p0.x, ImGui::GetIO().MousePos.x);
                changed = true;
            }
            if (ImGui::IsItemDeactivated() && m_DraggingKey && m_Track == ti)
            {
                // Keep keys sorted, following the dragged one.
                float t = m_Key < (int)track.Keys.size() ? track.Keys[m_Key].Time : 0.0f;
                std::stable_sort(track.Keys.begin(), track.Keys.end(), [](const TimelineKey& a, const TimelineKey& b) { return a.Time < b.Time; });
                for (int k = 0; k < (int)track.Keys.size(); ++k)
                    if (track.Keys[k].Time == t) m_Key = k;
                m_DraggingKey = false;
            }
            float cx = timeToX(p0.x, m_Time);
            dl->AddLine(ImVec2(cx, p0.y), ImVec2(cx, p0.y + h), IM_COL32(90, 170, 255, 255), 2.0f);
            ImGui::PopID();
        }
        if (removeTrack >= 0)
        {
            tl.Tracks.erase(tl.Tracks.begin() + removeTrack);
            m_Track = m_Key = -1;
            changed = true;
        }
        if (tl.Tracks.empty()) ImGui::TextDisabled("No tracks yet: add one below for any numeric field of this object or the objects under it.");

        // ---- Selected key ----
        if (m_Track >= 0 && m_Track < (int)tl.Tracks.size() && m_Key >= 0 && m_Key < (int)tl.Tracks[m_Track].Keys.size())
        {
            TimelineTrack& track = tl.Tracks[m_Track];
            TimelineKey& key = track.Keys[m_Key];
            ImGui::SeparatorText(("Key on " + TrackLabel(track)).c_str());
            ImGui::SetNextItemWidth(90);
            if (ImGui::DragFloat("Time##key", &key.Time, 0.01f, 0.0f, tl.Duration, "%.2f s")) changed = true;
            ImGui::SameLine();
            float probe[4];
            int channels = std::max(Timeline::ReadCurrent(scene, entity, track, probe), 1);
            ImGui::SetNextItemWidth(70.0f * channels);
            if (ImGui::DragScalarN("Value##key", ImGuiDataType_Float, key.Value.data(), channels, 0.01f)) changed = true;
            ImGui::SameLine();
            if (ImGui::SmallButton("Delete Key"))
            {
                track.Keys.erase(track.Keys.begin() + m_Key);
                m_Key = -1;
                changed = true;
            }
        }

        // ---- Add a track ----
        ImGui::SeparatorText("Add Track");
        {
            std::vector<Entity> objects;
            CollectObjects(scene, entity, objects);
            if (m_AddObject >= (int)objects.size()) m_AddObject = 0;
            Entity target = objects[m_AddObject];
            ImGui::SetNextItemWidth(180);
            if (ImGui::BeginCombo("##object", m_AddObject == 0 ? "(this object)" : Timeline::PathOf(scene, entity, target).c_str()))
            {
                for (int i = 0; i < (int)objects.size(); ++i)
                    if (ImGui::Selectable(i == 0 ? "(this object)" : Timeline::PathOf(scene, entity, objects[i]).c_str(), i == m_AddObject))
                    {
                        m_AddObject = i;
                        m_AddComponent.clear();
                        m_AddField.clear();
                    }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(150);
            if (ImGui::BeginCombo("##component", m_AddComponent.empty() ? "(component)" : m_AddComponent.c_str()))
            {
                for (const ComponentInfo& info : ComponentRegistry::All())
                {
                    if (info.Hidden || !info.Has(scene, target)) continue;
                    bool numeric = false;
                    for (const FieldInfo& f : info.Fields) numeric |= FloatCount(f.Type) > 0;
                    if (numeric && ImGui::Selectable(info.Name.c_str(), info.Name == m_AddComponent))
                    {
                        m_AddComponent = info.Name;
                        m_AddField.clear();
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(150);
            const ComponentInfo* info = ComponentRegistry::Find(m_AddComponent);
            if (ImGui::BeginCombo("##field", m_AddField.empty() ? "(field)" : m_AddField.c_str()))
            {
                if (info)
                    for (const FieldInfo& f : info->Fields)
                        if (FloatCount(f.Type) > 0 && ImGui::Selectable(f.Name.c_str(), f.Name == m_AddField)) m_AddField = f.Name;
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(!info || m_AddField.empty());
            if (ImGui::Button("Add Track"))
            {
                TimelineTrack track;
                track.Path = m_AddObject == 0 ? std::string() : Timeline::PathOf(scene, entity, target);
                track.Component = m_AddComponent;
                track.Field = m_AddField;
                float value[4];
                if (int n = Timeline::ReadCurrent(scene, entity, track, value)) Timeline::SetKey(track, m_Time, value, n);
                tl.Tracks.push_back(std::move(track));
                m_Track = (int)tl.Tracks.size() - 1;
                m_Key = 0;
                changed = true;
            }
            ImGui::EndDisabled();
        }

        // ---- Events ----
        if (ImGui::TreeNodeEx("Events", ImGuiTreeNodeFlags_None, "Events (%zu)", tl.Events.size()))
        {
            int removeEvent = -1;
            for (int i = 0; i < (int)tl.Events.size(); ++i)
            {
                ImGui::PushID(i);
                ImGui::SetNextItemWidth(80);
                if (ImGui::DragFloat("##t", &tl.Events[i].Time, 0.01f, 0.0f, tl.Duration, "%.2f s")) changed = true;
                ImGui::SameLine();
                ImGui::SetNextItemWidth(200);
                if (ImGui::InputText("##fn", &tl.Events[i].Function)) changed = true;
                ImGui::SameLine();
                if (ImGui::SmallButton("x")) removeEvent = i;
                ImGui::PopID();
            }
            if (removeEvent >= 0) { tl.Events.erase(tl.Events.begin() + removeEvent); changed = true; }
            if (ImGui::SmallButton("+ Event")) { tl.Events.push_back({ m_Time, "OnTimelineEvent" }); changed = true; }
            ImGui::SameLine();
            ImGui::TextDisabled("calls OnAnimationEvent(name) on this object's scripts");
            ImGui::TreePop();
        }

        if (changed)
        {
            AssetManager::SaveTimeline(m_Handle);
            if (!playing) scrubbed = true;
        }
        if (scrubbed && !playing) Timeline::Apply(scene, entity, tl, m_Time);
        ImGui::End();
    }
}
