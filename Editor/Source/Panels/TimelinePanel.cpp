#include "Panels.h"
#include "../EditorUI.h"
#include <Dimension/Animation/Timeline.h>
#include <Dimension/Audio/AudioEngine.h>
#include <Dimension/Renderer/Mesh.h>
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
        const char* kKindNames[] = { "Property", "Activation", "Audio", "Animation", "Control" };
        const ImU32 kChannelColors[4] = { IM_COL32(235, 90, 90, 255), IM_COL32(110, 205, 95, 255), IM_COL32(90, 150, 245, 255), IM_COL32(220, 220, 220, 255) };

        void CollectObjects(Scene& scene, Entity e, std::vector<Entity>& out)
        {
            out.push_back(e);
            for (Entity c : scene.GetChildren(e)) CollectObjects(scene, c, out);
        }

        std::string TrackLabel(const TimelineTrack& t)
        {
            std::string what = t.IsClipTrack() ? std::string(kKindNames[(int)t.Kind]) : t.Component + "." + t.Field;
            return (t.Path.empty() ? std::string() : t.Path + " : ") + what;
        }

        // The animation clips an Animator can play (its Animation Source's, or its own model's).
        const std::vector<AnimationClip>* AnimatorClipList(Scene& scene, Entity e)
        {
            auto& reg = scene.GetRegistry();
            auto* anim = e.IsNull() ? nullptr : reg.TryGet<AnimatorComponent>(e);
            auto* mr = e.IsNull() ? nullptr : reg.TryGet<MeshRendererComponent>(e);
            if (!anim || !mr) return nullptr;
            Ref<Mesh> mesh = AssetManager::GetMesh(anim->ClipSource.IsValid() ? anim->ClipSource : mr->Mesh);
            return mesh ? &mesh->GetClips() : nullptr;
        }

        // Natural length of a clip's content (animation or audio), 0 when unknown.
        float ContentLength(Scene& scene, Entity target, const TimelineTrack& track, const TimelineClip& clip)
        {
            if (track.Kind == TimelineTrackKind::Animation)
                if (auto* clips = AnimatorClipList(scene, target))
                    if (const AnimationClip* c = Animation::FindClip(*clips, clip.Animation)) return c->Duration / std::max(std::abs(clip.Speed), 0.01f);
            if (track.Kind == TimelineTrackKind::Audio && clip.Audio.IsValid())
                if (auto info = AudioEngine::GetClipInfo(AssetManager::GetAbsolutePath(clip.Audio))) return info->Seconds;
            if (track.Kind == TimelineTrackKind::Control)
                if (auto nested = AssetManager::GetTimeline(clip.Timeline)) return nested->Duration / std::max(std::abs(clip.Speed), 0.01f);
            return 0.0f;
        }

        std::string ClipLabel(const TimelineTrack& track, const TimelineClip& clip)
        {
            if (track.Kind == TimelineTrackKind::Animation) return clip.Animation.empty() ? "(no clip)" : clip.Animation;
            if (track.Kind == TimelineTrackKind::Audio) return clip.Audio.IsValid() ? AssetManager::GetDisplayName(clip.Audio) : "(no audio)";
            if (track.Kind == TimelineTrackKind::Control) return clip.Timeline.IsValid() ? AssetManager::GetDisplayName(clip.Timeline) : "(no timeline)";
            return "Active";
        }
    }

    void TimelinePanel::OnImGui(bool* open)
    {
        ImGui::SetNextWindowSize(ImVec2(900, 420), ImGuiCond_FirstUseEver);
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
                if (m_Handle != tl->Timeline) { m_Track = m_Key = m_Clip = -1; m_Time = 0.0f; }
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
        auto validTrack = [&](int i) { return i >= 0 && i < (int)tl.Tracks.size(); };

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
            ImGui::SetItemTooltip("Plays the timeline on the scene's objects (Undo puts them back; audio plays only in play mode)");
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

        // ---- Tracks: a label column and a strip with the keys or clips ----
        const float labelW = 360.0f, controlsX = labelW - 170.0f, inset = 8.0f; // keys stay clear of the strip edges
        float stripW = std::max(ImGui::GetContentRegionAvail().x - labelW - 10.0f, 100.0f);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        auto timeToX = [&](float x0, float t) { return x0 + inset + t / std::max(tl.Duration, 0.01f) * (stripW - 2.0f * inset); };
        auto xToTime = [&](float x0, float x) { return std::clamp((x - x0 - inset) / (stripW - 2.0f * inset) * tl.Duration, 0.0f, tl.Duration); };
        float secondsPerPixel = tl.Duration / std::max(stripW - 2.0f * inset, 1.0f);
        int removeTrack = -1;
        for (int ti = 0; ti < (int)tl.Tracks.size(); ++ti)
        {
            TimelineTrack& track = tl.Tracks[ti];
            Entity target = Timeline::ResolvePath(scene, entity, track.Path);
            ImGui::PushID(ti);
            ImGui::BeginGroup();
            if (ImGui::Checkbox("##mute", &track.Muted)) changed = true;
            ImGui::SetItemTooltip("Mute (checked tracks are not applied)");
            ImGui::SameLine();
            {
                ImVec2 at = ImGui::GetCursorScreenPos();
                float clipX = ImGui::GetWindowPos().x - ImGui::GetScrollX() + controlsX - 6.0f;
                ImGui::PushClipRect(at, ImVec2(clipX, at.y + ImGui::GetFrameHeight()), true);
                if (target.IsNull()) ImGui::TextDisabled("%s", TrackLabel(track).c_str());
                else ImGui::TextUnformatted(TrackLabel(track).c_str());
                ImGui::PopClipRect();
                ImGui::SetItemTooltip(target.IsNull() ? "%s (object not found)" : "%s", TrackLabel(track).c_str());
            }
            ImGui::SameLine(controlsX);
            if (track.IsClipTrack())
            {
                if (ImGui::SmallButton("+ Clip"))
                {
                    TimelineClip clip;
                    clip.Start = m_Time;
                    if (track.Kind == TimelineTrackKind::Animation)
                        if (auto* clips = AnimatorClipList(scene, target); clips && !clips->empty()) clip.Animation = clips->front().Name;
                    float length = ContentLength(scene, target, track, clip);
                    clip.Duration = length > 0.0f ? length : 1.0f;
                    track.Clips.push_back(clip);
                    Timeline::SortClips(track);
                    for (int c = 0; c < (int)track.Clips.size(); ++c)
                        if (track.Clips[c].Start == clip.Start) m_Clip = c;
                    m_Track = ti;
                    m_Key = -1;
                    changed = true;
                }
                ImGui::SetItemTooltip("Add a clip at the current time");
            }
            else
            {
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
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) removeTrack = ti;
            ImGui::EndGroup();
            // Strip.
            ImGui::SameLine(labelW);
            ImVec2 p0 = ImGui::GetCursorScreenPos();
            float h = ImGui::GetFrameHeight();
            ImGui::InvisibleButton("##strip", ImVec2(stripW, h));
            bool stripHovered = ImGui::IsItemHovered();
            float mouseX = ImGui::GetIO().MousePos.x;
            dl->AddRectFilled(p0, ImVec2(p0.x + stripW, p0.y + h), IM_COL32(34, 35, 40, 255), 3.0f);
            int hitKey = -1, hitClip = -1;
            bool hitEdge = false;
            if (track.IsClipTrack())
            {
                const ImU32 fill[5] = { 0, IM_COL32(70, 120, 80, 255), IM_COL32(150, 110, 60, 255), IM_COL32(80, 100, 160, 255), IM_COL32(130, 85, 150, 255) };
                for (int c = 0; c < (int)track.Clips.size(); ++c)
                {
                    const TimelineClip& clip = track.Clips[c];
                    float x0 = timeToX(p0.x, clip.Start), x1 = std::max(timeToX(p0.x, clip.End()), x0 + 4.0f);
                    bool sel = m_Track == ti && m_Clip == c;
                    ImU32 col = track.Muted ? IM_COL32(70, 70, 75, 255) : fill[(int)track.Kind];
                    dl->AddRectFilled(ImVec2(x0, p0.y + 2), ImVec2(x1, p0.y + h - 2), col, 3.0f);
                    if (sel) dl->AddRect(ImVec2(x0, p0.y + 2), ImVec2(x1, p0.y + h - 2), IM_COL32(255, 210, 90, 255), 3.0f, 0, 2.0f);
                    dl->PushClipRect(ImVec2(x0 + 4, p0.y), ImVec2(x1 - 4, p0.y + h), true);
                    dl->AddText(ImVec2(x0 + 5, p0.y + (h - ImGui::GetTextLineHeight()) * 0.5f), IM_COL32(240, 240, 240, 255), ClipLabel(track, clip).c_str());
                    dl->PopClipRect();
                    if (stripHovered && mouseX >= x0 - 2 && mouseX <= x1 + 4)
                    {
                        hitClip = c;
                        hitEdge = mouseX >= x1 - 6;
                    }
                }
                if (hitClip >= 0 && hitEdge) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            }
            else
                for (int k = 0; k < (int)track.Keys.size(); ++k)
                {
                    float x = timeToX(p0.x, track.Keys[k].Time), cy = p0.y + h * 0.5f;
                    bool sel = m_Track == ti && m_Key == k;
                    ImU32 col = sel ? IM_COL32(255, 210, 90, 255) : track.Muted ? IM_COL32(110, 110, 115, 255) : IM_COL32(200, 200, 210, 255);
                    dl->AddQuadFilled(ImVec2(x, cy - 6), ImVec2(x + 6, cy), ImVec2(x, cy + 6), ImVec2(x - 6, cy), col);
                    if (stripHovered && std::abs(mouseX - x) < 7.0f) hitKey = k;
                }
            if (ImGui::IsItemClicked())
            {
                if (hitKey >= 0)
                {
                    m_Track = ti;
                    m_Key = hitKey;
                    m_Clip = -1;
                    m_Drag = DragKey;
                }
                else if (hitClip >= 0)
                {
                    m_Track = ti;
                    m_Clip = hitClip;
                    m_Key = -1;
                    m_Drag = hitEdge ? DragClipEnd : DragClip;
                    m_DragOffset = xToTime(p0.x, mouseX) - track.Clips[hitClip].Start;
                }
                else
                {
                    m_Track = ti;
                    if (!playing)
                    {
                        m_Ctx.RecordUndo();
                        m_Time = xToTime(p0.x, mouseX);
                        scrubbed = true;
                    }
                }
            }
            if (m_Drag != DragNone && m_Track == ti && ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f))
            {
                float t = xToTime(p0.x, mouseX);
                if (m_Drag == DragKey && m_Key >= 0 && m_Key < (int)track.Keys.size()) track.Keys[m_Key].Time = t;
                else if (m_Clip >= 0 && m_Clip < (int)track.Clips.size())
                {
                    TimelineClip& clip = track.Clips[m_Clip];
                    if (m_Drag == DragClip) clip.Start = std::clamp(t - m_DragOffset, 0.0f, std::max(tl.Duration - secondsPerPixel, 0.0f));
                    else clip.Duration = std::max(t - clip.Start, secondsPerPixel * 4.0f);
                }
                changed = true;
            }
            if (ImGui::IsItemDeactivated() && m_Drag != DragNone && m_Track == ti)
            {
                // Keep keys and clips sorted, following the dragged one.
                if (m_Drag == DragKey && m_Key >= 0 && m_Key < (int)track.Keys.size())
                {
                    float t = track.Keys[m_Key].Time;
                    std::stable_sort(track.Keys.begin(), track.Keys.end(), [](const TimelineKey& a, const TimelineKey& b) { return a.Time < b.Time; });
                    for (int k = 0; k < (int)track.Keys.size(); ++k)
                        if (track.Keys[k].Time == t) m_Key = k;
                }
                else if (m_Clip >= 0 && m_Clip < (int)track.Clips.size())
                {
                    float s = track.Clips[m_Clip].Start;
                    Timeline::SortClips(track);
                    for (int c = 0; c < (int)track.Clips.size(); ++c)
                        if (track.Clips[c].Start == s) m_Clip = c;
                }
                m_Drag = DragNone;
            }
            float cx = timeToX(p0.x, m_Time);
            dl->AddLine(ImVec2(cx, p0.y), ImVec2(cx, p0.y + h), IM_COL32(90, 170, 255, 255), 2.0f);
            ImGui::PopID();
        }
        if (removeTrack >= 0)
        {
            if (tl.Tracks[removeTrack].Kind == TimelineTrackKind::Animation)
                if (auto* anim = reg.TryGet<AnimatorComponent>(Timeline::ResolvePath(scene, entity, tl.Tracks[removeTrack].Path))) anim->TimelineDriven = false;
            tl.Tracks.erase(tl.Tracks.begin() + removeTrack);
            m_Track = m_Key = m_Clip = -1;
            changed = true;
        }
        if (tl.Tracks.empty()) ImGui::TextDisabled("No tracks yet: add one below for any numeric field of this object or the objects under it, or for clips.");

        // ---- Selected key ----
        if (validTrack(m_Track) && !tl.Tracks[m_Track].IsClipTrack() && m_Key >= 0 && m_Key < (int)tl.Tracks[m_Track].Keys.size())
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

        // ---- Selected clip ----
        if (validTrack(m_Track) && tl.Tracks[m_Track].IsClipTrack() && m_Clip >= 0 && m_Clip < (int)tl.Tracks[m_Track].Clips.size())
        {
            TimelineTrack& track = tl.Tracks[m_Track];
            TimelineClip& clip = track.Clips[m_Clip];
            Entity target = Timeline::ResolvePath(scene, entity, track.Path);
            ImGui::SeparatorText(("Clip on " + TrackLabel(track)).c_str());
            ImGui::SetNextItemWidth(90);
            if (ImGui::DragFloat("Start", &clip.Start, 0.01f, 0.0f, tl.Duration, "%.2f s")) changed = true;
            ImGui::SameLine();
            ImGui::SetNextItemWidth(90);
            if (ImGui::DragFloat("Length", &clip.Duration, 0.01f, 0.01f, 3600.0f, "%.2f s")) changed = true;
            if (track.Kind != TimelineTrackKind::Activation)
            {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(90);
                if (ImGui::DragFloat("Clip In", &clip.ClipIn, 0.01f, 0.0f, 3600.0f, "%.2f s")) changed = true;
                ImGui::SetItemTooltip("Seconds into the content where the clip starts");
            }
            if (track.Kind == TimelineTrackKind::Animation)
            {
                const auto* clips = AnimatorClipList(scene, target);
                ImGui::SetNextItemWidth(180);
                if (ImGui::BeginCombo("Animation", clip.Animation.empty() ? "(none)" : clip.Animation.c_str()))
                {
                    if (clips)
                        for (const AnimationClip& c : *clips)
                            if (ImGui::Selectable(c.Name.c_str(), c.Name == clip.Animation)) { clip.Animation = c.Name; changed = true; }
                    ImGui::EndCombo();
                }
                if (!clips) ImGui::SetItemTooltip("The track's object needs an Animator and a skinned Mesh Renderer");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(70);
                if (ImGui::DragFloat("Speed", &clip.Speed, 0.01f, -10.0f, 10.0f, "%.2f")) changed = true;
                ImGui::SameLine();
                if (ImGui::Checkbox("Loop", &clip.Loop)) changed = true;
            }
            if (track.Kind == TimelineTrackKind::Audio)
            {
                if (UI::BeginProperties("##clipaudio"))
                {
                    if (UI::AssetField("Audio Clip", clip.Audio, AssetType::Audio).Changed) changed = true;
                    UI::EndProperties();
                }
                if (!reg.Has<AudioSourceComponent>(target)) ImGui::TextDisabled("The track's object needs an Audio Source to play this.");
            }
            if (track.Kind == TimelineTrackKind::Control)
            {
                if (UI::BeginProperties("##clipcontrol"))
                {
                    if (UI::AssetField("Timeline", clip.Timeline, AssetType::Timeline).Changed) changed = true;
                    UI::EndProperties();
                }
                ImGui::SetNextItemWidth(70);
                if (ImGui::DragFloat("Speed", &clip.Speed, 0.01f, 0.0f, 10.0f, "%.2f")) changed = true;
                ImGui::SameLine();
                if (ImGui::Checkbox("Loop", &clip.Loop)) changed = true;
                ImGui::SetItemTooltip("Loop the nested timeline over the clip (otherwise it holds its end)");
                if (clip.Timeline == m_Handle) ImGui::TextDisabled("A timeline playing itself is ignored past %d levels.", 8);
            }
            float length = ContentLength(scene, target, track, clip);
            if (length > 0.0f)
            {
                if (ImGui::SmallButton("Match Length")) { clip.Duration = std::max(length - clip.ClipIn, 0.01f); changed = true; }
                ImGui::SetItemTooltip("Set Length to the content's (%.2f s)", length);
                ImGui::SameLine();
            }
            if (ImGui::SmallButton("Delete Clip"))
            {
                track.Clips.erase(track.Clips.begin() + m_Clip);
                m_Clip = -1;
                changed = true;
            }
        }

        // ---- Curves: the selected property track's channels over time (or the first property track's) ----
        int curveTrack = validTrack(m_Track) && !tl.Tracks[m_Track].IsClipTrack() ? m_Track : -1;
        for (int i = 0; curveTrack < 0 && i < (int)tl.Tracks.size(); ++i)
            if (!tl.Tracks[i].IsClipTrack()) curveTrack = i;
        if (curveTrack >= 0 && !tl.Tracks[curveTrack].Keys.empty() &&
            ImGui::TreeNodeEx("Curves", ImGuiTreeNodeFlags_DefaultOpen, "Curves: %s", TrackLabel(tl.Tracks[curveTrack]).c_str()))
        {
            TimelineTrack& track = tl.Tracks[curveTrack];
            if (m_Track != curveTrack) m_Key = -1; // keys picked here belong to this track
            float probe[4];
            int channels = std::clamp(Timeline::ReadCurrent(scene, entity, track, probe), 1, 4);
            // Value range: the keys' and the sampled curve's, frozen while a key is dragged so the view stays put.
            if (m_CurveDragKey < 0)
            {
                float lo = 1e30f, hi = -1e30f;
                for (float t = 0.0f; t <= tl.Duration + 1e-4f; t += tl.Duration / 96.0f)
                {
                    float v[4];
                    Timeline::Sample(track, t, v);
                    for (int c = 0; c < channels; ++c) { lo = std::min(lo, v[c]); hi = std::max(hi, v[c]); }
                }
                for (const TimelineKey& k : track.Keys)
                    for (int c = 0; c < channels; ++c) { lo = std::min(lo, k.Value[c]); hi = std::max(hi, k.Value[c]); }
                float pad = std::max((hi - lo) * 0.12f, 0.05f);
                m_CurveMin = lo - pad;
                m_CurveMax = hi + pad;
            }
            ImGui::Dummy(ImVec2(labelW - ImGui::GetStyle().ItemSpacing.x - ImGui::GetStyle().IndentSpacing, 1));
            ImGui::SameLine(labelW);
            ImVec2 c0 = ImGui::GetCursorScreenPos();
            float ch = 150.0f;
            ImGui::InvisibleButton("##curves", ImVec2(stripW, ch));
            bool hovered = ImGui::IsItemHovered();
            auto valueToY = [&](float v) { return c0.y + ch - 6.0f - (v - m_CurveMin) / std::max(m_CurveMax - m_CurveMin, 1e-6f) * (ch - 12.0f); };
            auto yToValue = [&](float y) { return m_CurveMin + (c0.y + ch - 6.0f - y) / (ch - 12.0f) * (m_CurveMax - m_CurveMin); };
            dl->AddRectFilled(c0, ImVec2(c0.x + stripW, c0.y + ch), IM_COL32(28, 29, 33, 255), 3.0f);
            if (m_CurveMin < 0.0f && m_CurveMax > 0.0f)
                dl->AddLine(ImVec2(c0.x, valueToY(0.0f)), ImVec2(c0.x + stripW, valueToY(0.0f)), IM_COL32(70, 70, 78, 255));
            char range[64];
            std::snprintf(range, sizeof(range), "%.2f", m_CurveMax);
            dl->AddText(ImVec2(c0.x + 4, c0.y + 2), IM_COL32(120, 120, 130, 255), range);
            std::snprintf(range, sizeof(range), "%.2f", m_CurveMin);
            dl->AddText(ImVec2(c0.x + 4, c0.y + ch - ImGui::GetTextLineHeight() - 2), IM_COL32(120, 120, 130, 255), range);
            dl->PushClipRect(c0, ImVec2(c0.x + stripW, c0.y + ch), true);
            const int samples = std::max((int)(stripW / 3.0f), 16);
            for (int c = 0; c < channels; ++c)
            {
                ImVec2 prev;
                for (int i = 0; i <= samples; ++i)
                {
                    float t = tl.Duration * (float)i / (float)samples, v[4];
                    Timeline::Sample(track, t, v);
                    ImVec2 p(timeToX(c0.x, t), valueToY(v[c]));
                    if (i > 0) dl->AddLine(prev, p, channels == 1 ? kChannelColors[3] : kChannelColors[c], 1.5f);
                    prev = p;
                }
            }
            // Key points: drag up and down to change that channel's value, sideways to move the key.
            int hitKey = -1, hitChannel = -1;
            ImVec2 mouse = ImGui::GetIO().MousePos;
            for (int k = 0; k < (int)track.Keys.size(); ++k)
                for (int c = 0; c < channels; ++c)
                {
                    ImVec2 p(timeToX(c0.x, track.Keys[k].Time), valueToY(track.Keys[k].Value[c]));
                    bool sel = m_Track == curveTrack && m_Key == k && (m_CurveDragKey < 0 || m_CurveDragChannel == c);
                    dl->AddCircleFilled(p, sel ? 5.0f : 4.0f, sel ? IM_COL32(255, 210, 90, 255) : channels == 1 ? kChannelColors[3] : kChannelColors[c]);
                    if (hovered && std::abs(mouse.x - p.x) < 6.0f && std::abs(mouse.y - p.y) < 6.0f) { hitKey = k; hitChannel = c; }
                }
            float cx = timeToX(c0.x, m_Time);
            dl->AddLine(ImVec2(cx, c0.y), ImVec2(cx, c0.y + ch), IM_COL32(90, 170, 255, 160), 1.0f);
            dl->PopClipRect();
            if (ImGui::IsItemClicked() && hitKey >= 0)
            {
                m_Track = curveTrack;
                m_Clip = -1;
                m_Key = hitKey;
                m_CurveDragKey = hitKey;
                m_CurveDragChannel = hitChannel;
            }
            if (m_CurveDragKey >= 0 && m_CurveDragKey < (int)track.Keys.size() && ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 1.0f))
            {
                TimelineKey& key = track.Keys[m_CurveDragKey];
                key.Value[m_CurveDragChannel] = yToValue(mouse.y);
                if (ImGui::GetIO().KeyShift) key.Time = xToTime(c0.x, mouse.x); // Shift: also move it in time
                changed = true;
            }
            if (ImGui::IsItemDeactivated() && m_CurveDragKey >= 0)
            {
                float t = m_CurveDragKey < (int)track.Keys.size() ? track.Keys[m_CurveDragKey].Time : 0.0f;
                std::stable_sort(track.Keys.begin(), track.Keys.end(), [](const TimelineKey& a, const TimelineKey& b) { return a.Time < b.Time; });
                for (int k = 0; k < (int)track.Keys.size(); ++k)
                    if (track.Keys[k].Time == t) m_Key = k;
                m_CurveDragKey = -1;
            }
            if (hovered && hitKey < 0 && m_CurveDragKey < 0) ImGui::SetItemTooltip("Drag a key up or down to change its value (Shift: also along time)");
            ImGui::TreePop();
        }

        // ---- Add a track ----
        ImGui::SeparatorText("Add Track");
        {
            std::vector<Entity> objects;
            CollectObjects(scene, entity, objects);
            if (m_AddObject >= (int)objects.size()) m_AddObject = 0;
            ImGui::SetNextItemWidth(100);
            if (ImGui::Combo("##kind", &m_AddKind, kKindNames, IM_ARRAYSIZE(kKindNames))) { m_AddComponent.clear(); m_AddField.clear(); }
            ImGui::SetItemTooltip("Property: animate a field. Activation: the object is active during clips.\n"
                                  "Audio: clips play through the object's Audio Source. Animation: clips play on its Animator.\n"
                                  "Control: clips play other timelines on the object (nested sequences).");
            ImGui::SameLine();
            // Activation tracks drive objects under this one (deactivating the timeline's own object would stop it).
            if (m_AddKind == (int)TimelineTrackKind::Activation && m_AddObject == 0 && objects.size() > 1) m_AddObject = 1;
            Entity target = objects[m_AddObject];
            ImGui::SetNextItemWidth(180);
            if (ImGui::BeginCombo("##object", m_AddObject == 0 ? "(this object)" : Timeline::PathOf(scene, entity, target).c_str()))
            {
                for (int i = 0; i < (int)objects.size(); ++i)
                {
                    if (i == 0 && m_AddKind == (int)TimelineTrackKind::Activation) continue;
                    if (ImGui::Selectable(i == 0 ? "(this object)" : Timeline::PathOf(scene, entity, objects[i]).c_str(), i == m_AddObject))
                    {
                        m_AddObject = i;
                        m_AddComponent.clear();
                        m_AddField.clear();
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            bool canAdd = false;
            const char* missing = nullptr;
            const ComponentInfo* info = nullptr;
            if (m_AddKind == (int)TimelineTrackKind::Property)
            {
                ImGui::SetNextItemWidth(150);
                if (ImGui::BeginCombo("##component", m_AddComponent.empty() ? "(component)" : m_AddComponent.c_str()))
                {
                    for (const ComponentInfo& ci : ComponentRegistry::All())
                    {
                        if (ci.Hidden || !ci.Has(scene, target)) continue;
                        bool numeric = false;
                        for (const FieldInfo& f : ci.Fields) numeric |= FloatCount(f.Type) > 0;
                        if (numeric && ImGui::Selectable(ci.Name.c_str(), ci.Name == m_AddComponent))
                        {
                            m_AddComponent = ci.Name;
                            m_AddField.clear();
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::SameLine();
                ImGui::SetNextItemWidth(150);
                info = ComponentRegistry::Find(m_AddComponent);
                if (ImGui::BeginCombo("##field", m_AddField.empty() ? "(field)" : m_AddField.c_str()))
                {
                    if (info)
                        for (const FieldInfo& f : info->Fields)
                            if (FloatCount(f.Type) > 0 && ImGui::Selectable(f.Name.c_str(), f.Name == m_AddField)) m_AddField = f.Name;
                    ImGui::EndCombo();
                }
                ImGui::SameLine();
                canAdd = info && !m_AddField.empty();
            }
            else if (m_AddKind == (int)TimelineTrackKind::Activation)
                canAdd = m_AddObject != 0;
            else if (m_AddKind == (int)TimelineTrackKind::Audio)
            {
                canAdd = reg.Has<AudioSourceComponent>(target);
                if (!canAdd) missing = "needs an Audio Source";
            }
            else if (m_AddKind == (int)TimelineTrackKind::Control)
                canAdd = true;
            else
            {
                canAdd = reg.Has<AnimatorComponent>(target);
                if (!canAdd) missing = "needs an Animator";
            }
            ImGui::BeginDisabled(!canAdd);
            if (ImGui::Button("Add Track"))
            {
                TimelineTrack track;
                track.Kind = (TimelineTrackKind)m_AddKind;
                track.Path = m_AddObject == 0 ? std::string() : Timeline::PathOf(scene, entity, target);
                if (track.Kind == TimelineTrackKind::Property)
                {
                    track.Component = m_AddComponent;
                    track.Field = m_AddField;
                    float value[4];
                    if (int n = Timeline::ReadCurrent(scene, entity, track, value)) Timeline::SetKey(track, m_Time, value, n);
                }
                tl.Tracks.push_back(std::move(track));
                m_Track = (int)tl.Tracks.size() - 1;
                m_Key = tl.Tracks.back().IsClipTrack() ? -1 : 0;
                m_Clip = -1;
                changed = true;
            }
            ImGui::EndDisabled();
            if (missing)
            {
                ImGui::SameLine();
                ImGui::TextDisabled("(the object %s)", missing);
            }
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
