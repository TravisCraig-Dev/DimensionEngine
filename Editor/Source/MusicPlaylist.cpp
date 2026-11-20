// Music Mode's Playlist: the song's arrangement, patterns placed as clips on tracks along a timeline of bars.
#include "MusicMode.h"
#include "MusicModeUI.h"
#include <Dimension/Music/Editing.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>
#include <algorithm>
#include <cmath>

namespace Editor
{
    using namespace Dimension;
    using namespace Dimension::Music;
    using namespace Editor::MusicUI;

    void MusicMode::DrawPlaylist(const ImVec2& size)
    {
        ImGui::BeginChild("##Playlist", size, ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGuiIO& io = ImGui::GetIO();

        // Toolbar: the pattern clicks place (the brush), snap.
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Playlist");
        ImGui::SameLine(0.0f, 16.0f);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Places");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(140.0f);
        if (ImGui::BeginCombo("##brush", m_Song.Patterns[m_Pattern].Name.c_str()))
        {
            for (int i = 0; i < (int)m_Song.Patterns.size(); ++i)
                if (ImGui::Selectable(m_Song.Patterns[i].Name.c_str(), i == m_Pattern)) SetPattern(i);
            ImGui::EndCombo();
        }
        ImGui::SameLine(0.0f, 16.0f);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Snap");
        ImGui::SameLine();
        int snapCount;
        const SnapChoice* snaps = SnapChoices(snapCount);
        const char* snapName = "Beat";
        for (int i = 0; i < snapCount; ++i)
            if (snaps[i].Ticks == m_ListSnap) snapName = snaps[i].Name;
        ImGui::SetNextItemWidth(100.0f);
        if (ImGui::BeginCombo("##listSnap", snapName))
        {
            for (int i = 0; i < snapCount; ++i)
                if (ImGui::Selectable(snaps[i].Name, snaps[i].Ticks == m_ListSnap)) m_ListSnap = snaps[i].Ticks;
            ImGui::EndCombo();
        }
        ImGui::SameLine(0.0f, 16.0f);
        if (ImGui::Button("+ Automation")) ImGui::OpenPopup("AddAutomation");
        ImGui::SetItemTooltip("An automation clip moving a setting over time (or right-click a knob, fader or setting)");
        if (ImGui::BeginPopup("AddAutomation"))
        {
            // Grouped: each channel, then each mixer track.
            auto params = m_Song.AutomatableParams();
            auto group = [&](const std::string& name, auto belongs) {
                if (!ImGui::BeginMenu(name.c_str())) return;
                for (const ParamRef& r : params)
                    if (belongs(r) && ImGui::MenuItem(m_Song.ParamName(r).c_str())) CreateAutomation(r);
                ImGui::EndMenu();
            };
            ImGui::TextDisabled("Channels");
            for (int c = 0; c < (int)m_Song.Channels.size(); ++c)
                group(m_Song.Channels[c].Name + "##ch" + std::to_string(c), [&](const ParamRef& r) {
                    return r.Index == c && (r.Kind == ParamKind::ChannelVolume || r.Kind == ParamKind::ChannelPan || r.Kind == ParamKind::SynthCutoff ||
                                            r.Kind == ParamKind::SynthResonance);
                });
            ImGui::Separator();
            ImGui::TextDisabled("Mixer");
            for (int t = 0; t < (int)m_Song.Mixer.size(); ++t)
                group(m_Song.MixerTrackName(t) + "##mx" + std::to_string(t), [&](const ParamRef& r) {
                    return r.Index == t && !(r.Kind == ParamKind::ChannelVolume || r.Kind == ParamKind::ChannelPan || r.Kind == ParamKind::SynthCutoff ||
                                             r.Kind == ParamKind::SynthResonance);
                });
            ImGui::EndPopup();
        }
        ImGui::SameLine(0.0f, 16.0f);
        ImGui::TextDisabled("Click to place the pattern, drag to move, drag the end to resize, right-click deletes, double-click edits it; click the ruler to seek");
        if (!m_SongMode)
        {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.35f, 1.0f), "(Pattern mode: switch to SONG to play the Playlist)");
        }

        const float headerW = 140.0f, rulerH = 22.0f;
        if (m_SelectedAuto >= (int)m_Song.Automation.size()) m_SelectedAuto = -1;
        ImVec2 origin = ImGui::GetCursorScreenPos();
        ImVec2 avail = ImGui::GetContentRegionAvail();
        const float editorH = m_SelectedAuto >= 0 ? std::min(170.0f, avail.y * 0.4f) : 0.0f;
        ImRect ruler(ImVec2(origin.x + headerW, origin.y), ImVec2(origin.x + avail.x, origin.y + rulerH));
        ImRect grid(ImVec2(ruler.Min.x, ruler.Max.y), ImVec2(ruler.Max.x, origin.y + avail.y - (editorH > 0.0f ? editorH + 6.0f : 0.0f)));
        ImRect headers(ImVec2(origin.x, grid.Min.y), ImVec2(grid.Min.x, grid.Max.y));
        if (grid.GetWidth() < 20.0f || grid.GetHeight() < 20.0f)
        {
            ImGui::EndChild();
            return;
        }
        const int barT = m_Song.BarTicks();
        const float tickW = m_ListBarW / barT, trackH = m_ListTrackH;
        int usedTracks = 0;
        for (const auto& c : m_Song.Playlist) usedTracks = std::max(usedTracks, c.Track + 1);
        for (const auto& c : m_Song.Automation) usedTracks = std::max(usedTracks, c.Track + 1);
        const int trackCount = std::max({ usedTracks + 4, (int)m_Song.Tracks.size(), (int)std::ceil(grid.GetHeight() / trackH) });
        m_ListScrollY = std::clamp(m_ListScrollY, 0.0f, std::max(0.0f, trackCount * trackH - grid.GetHeight()));
        m_ListScrollX = std::max(0.0, m_ListScrollX);
        auto xOf = [&](double tick) { return grid.Min.x + (float)((tick - m_ListScrollX) * tickW); };
        auto yOf = [&](int track) { return grid.Min.y + track * trackH - m_ListScrollY; };
        auto tickAt = [&](float x) { return m_ListScrollX + (x - grid.Min.x) / tickW; };
        auto trackAt = [&](float y) { return std::max(0, (int)std::floor((y - grid.Min.y + m_ListScrollY) / trackH)); };
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 mouse = io.MousePos;
        const int snap = std::max(1, m_ListSnap);
        if (m_SelectedClip >= (int)m_Song.Playlist.size()) m_SelectedClip = -1;

        // ---- Ruler: bar numbers; click or drag to move the song position ----
        ImGui::SetCursorScreenPos(ruler.Min);
        ImGui::InvisibleButton("##ruler", ruler.GetSize());
        if (ImGui::IsItemActive() && m_Player)
        {
            if (!m_SongMode) SetSongMode(true);
            m_Player->SetTick(std::max(0.0, (double)Edit::SnapRound(tickAt(mouse.x), m_Song.StepTicks())));
        }
        dl->AddRectFilled(ruler.Min, ruler.Max, IM_COL32(38, 39, 43, 255));
        dl->PushClipRect(ruler.Min, ruler.Max, true);
        int firstBar = (int)std::floor(m_ListScrollX / barT);
        int labelEvery = m_ListBarW < 28.0f ? 4 : m_ListBarW < 48.0f ? 2 : 1;
        for (int b = firstBar; xOf((double)b * barT) < ruler.Max.x; ++b)
        {
            float x = xOf((double)b * barT);
            dl->AddLine(ImVec2(x, ruler.Max.y - 8), ImVec2(x, ruler.Max.y), IM_COL32(140, 140, 140, 255));
            if (b % labelEvery == 0) dl->AddText(ImVec2(x + 3, ruler.Min.y + 3), IM_COL32(200, 200, 200, 255), std::to_string(b + 1).c_str());
        }
        int songEnd = m_Song.SongTicks();
        if (songEnd > 0)
        {
            float x = xOf(songEnd);
            dl->AddTriangleFilled(ImVec2(x - 5, ruler.Min.y), ImVec2(x + 5, ruler.Min.y), ImVec2(x, ruler.Min.y + 7), IM_COL32(230, 120, 80, 255));
        }
        dl->PopClipRect();

        // ---- Track headers: name (double-click renames) and mute light ----
        ImGui::SetCursorScreenPos(headers.Min);
        ImGui::InvisibleButton("##trackHeaders", headers.GetSize(), ImGuiButtonFlags_MouseButtonLeft);
        if (ImGui::IsItemHovered() && io.MouseWheel != 0.0f) m_ListScrollY -= io.MouseWheel * trackH;
        int headerTrack = trackAt(mouse.y);
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
        {
            if (mouse.x < headers.Min.x + 22.0f)
            {
                Record();
                m_Song.Track(headerTrack).Mute = !m_Song.TrackMuted(headerTrack);
                Changed();
            }
        }
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && mouse.x >= headers.Min.x + 22.0f)
        {
            m_RenamingTrack = headerTrack;
            m_RenameBuffer = m_Song.TrackName(headerTrack);
            ImGui::OpenPopup("RenameTrack");
        }
        if (ImGui::BeginPopup("RenameTrack"))
        {
            ImGui::SetNextItemWidth(160.0f);
            if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
            if (ImGui::InputText("##trackName", &m_RenameBuffer, ImGuiInputTextFlags_EnterReturnsTrue))
            {
                Record();
                m_Song.Track(m_RenamingTrack).Name = m_RenameBuffer;
                Changed();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        dl->PushClipRect(headers.Min, headers.Max, true);
        for (int t = trackAt(headers.Min.y); t < trackCount && yOf(t) < headers.Max.y; ++t)
        {
            float y = yOf(t);
            dl->AddRectFilled(ImVec2(headers.Min.x, y), ImVec2(headers.Max.x - 2, y + trackH - 1), IM_COL32(48, 50, 55, 255));
            bool muted = m_Song.TrackMuted(t);
            dl->AddCircleFilled(ImVec2(headers.Min.x + 11, y + trackH * 0.5f), 5.0f, muted ? IM_COL32(70, 70, 70, 255) : IM_COL32(110, 220, 110, 255));
            dl->AddText(ImVec2(headers.Min.x + 24, y + (trackH - ImGui::GetTextLineHeight()) * 0.5f),
                        muted ? IM_COL32(130, 130, 130, 255) : IM_COL32(220, 220, 220, 255), m_Song.TrackName(t).c_str());
        }
        dl->PopClipRect();

        // ---- The grid's mouse ----
        ImGui::SetCursorScreenPos(grid.Min);
        ImGui::InvisibleButton("##playlistGrid", grid.GetSize(), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
        bool hovered = ImGui::IsItemHovered();
        double mt = tickAt(mouse.x);
        int mtrack = trackAt(mouse.y);
        if (hovered && io.MouseWheel != 0.0f)
        {
            if (io.KeyCtrl)
            {
                double anchor = mt;
                m_ListBarW = std::clamp(m_ListBarW * std::pow(1.15f, io.MouseWheel), 8.0f, 600.0f);
                m_ListScrollX = std::max(0.0, anchor - (mouse.x - grid.Min.x) / (m_ListBarW / barT));
            }
            else if (io.KeyShift) m_ListScrollX = std::max(0.0, m_ListScrollX - io.MouseWheel * 80.0 / tickW);
            else m_ListScrollY -= io.MouseWheel * trackH;
        }
        if (hovered && io.MouseWheelH != 0.0f) m_ListScrollX = std::max(0.0, m_ListScrollX - io.MouseWheelH * 80.0 / tickW);

        if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && Edit::AutomationAt(m_Song, mtrack, mt) < 0)
        {
            int hit = Edit::ClipAt(m_Song, mtrack, mt);
            if (hit >= 0)
            {
                // Edit the clip's pattern in the Piano Roll.
                SetPattern(m_Song.Playlist[hit].Pattern);
                m_View = View::PianoRoll;
                m_RollScrollY = -1.0f;
            }
        }
        if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            m_ListDrag = {};
            Record();
            if (int autoHit = Edit::AutomationAt(m_Song, mtrack, mt); autoHit >= 0)
            {
                const AutomationClip& a = m_Song.Automation[autoHit];
                bool edge = mouse.x > xOf(a.Start + a.Length) - std::min(7.0f, a.Length * tickW * 0.3f);
                m_ListDrag.Kind = edge ? ListDrag::Resize : ListDrag::Move;
                m_ListDrag.Automation = true;
                m_ListDrag.Clip = autoHit;
                m_ListDrag.Offset = mt - a.Start;
                m_SelectedAuto = autoHit;
                m_SelectedClip = -1;
            }
            else
            {
            int hit = Edit::ClipAt(m_Song, mtrack, mt);
            if (hit < 0)
            {
                m_Song.Playlist.push_back({ m_Pattern, mtrack, std::max(0, Edit::SnapFloor(mt, snap)), 0 });
                hit = (int)m_Song.Playlist.size() - 1;
                m_ListDrag.Moved = true;
                Changed();
            }
            const PlaylistClip& c = m_Song.Playlist[hit];
            bool edge = mouse.x > xOf(c.Start + m_Song.ClipTicks(c)) - std::min(7.0f, m_Song.ClipTicks(c) * tickW * 0.3f);
            m_ListDrag.Kind = edge ? ListDrag::Resize : ListDrag::Move;
            m_ListDrag.Clip = hit;
            m_ListDrag.Offset = mt - c.Start;
            m_SelectedClip = hit;
            m_SelectedAuto = -1;
            }
        }
        if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
        {
            m_ListDrag = {};
            m_ListDrag.Kind = ListDrag::Erase;
            Record();
        }
        if (ImGui::IsItemActive() && m_ListDrag.Automation && m_ListDrag.Clip >= 0 && m_ListDrag.Clip < (int)m_Song.Automation.size())
        {
            AutomationClip& a = m_Song.Automation[m_ListDrag.Clip];
            AutomationClip before = a;
            if (m_ListDrag.Kind == ListDrag::Move)
            {
                a.Start = std::max(0, Edit::SnapRound(mt - m_ListDrag.Offset, snap));
                a.Track = mtrack;
            }
            else if (m_ListDrag.Kind == ListDrag::Resize)
            {
                // Longer or shorter: the points stay where they are in time (those past the end are cut off).
                a.Length = std::max(snap > 1 ? snap : m_Song.StepTicks(), Edit::SnapRound(mt, snap) - a.Start);
            }
            if (a.Start != before.Start || a.Track != before.Track || a.Length != before.Length)
            {
                m_ListDrag.Moved = true;
                Changed();
            }
        }
        else if (ImGui::IsItemActive())
        {
            if ((m_ListDrag.Kind == ListDrag::Move || m_ListDrag.Kind == ListDrag::Resize) && m_ListDrag.Clip >= 0 && m_ListDrag.Clip < (int)m_Song.Playlist.size())
            {
                PlaylistClip& c = m_Song.Playlist[m_ListDrag.Clip];
                PlaylistClip before = c;
                if (m_ListDrag.Kind == ListDrag::Move)
                {
                    c.Start = std::max(0, Edit::SnapRound(mt - m_ListDrag.Offset, snap));
                    c.Track = mtrack;
                }
                else
                {
                    int end = Edit::SnapRound(mt, snap);
                    c.Length = std::max(snap > 1 ? snap : m_Song.StepTicks(), end - c.Start);
                    if (c.Length == m_Song.PatternTicks(c.Pattern)) c.Length = 0; // (back to the pattern's own length)
                }
                if (c.Start != before.Start || c.Track != before.Track || c.Length != before.Length)
                {
                    m_ListDrag.Moved = true;
                    Changed();
                }
            }
            else if (m_ListDrag.Kind == ListDrag::Erase)
            {
                int autoHit = Edit::AutomationAt(m_Song, mtrack, mt);
                if (autoHit >= 0)
                {
                    m_Song.Automation.erase(m_Song.Automation.begin() + autoHit);
                    m_SelectedAuto = -1;
                    m_ListDrag.Moved = true;
                    Changed();
                }
                int hit = autoHit >= 0 ? -1 : Edit::ClipAt(m_Song, mtrack, mt);
                if (hit >= 0)
                {
                    m_Song.Playlist.erase(m_Song.Playlist.begin() + hit);
                    m_SelectedClip = -1;
                    m_ListDrag.Moved = true;
                    Changed();
                }
            }
        }
        if (ImGui::IsItemDeactivated())
        {
            if (!m_ListDrag.Moved && !m_Undo.empty()) m_Undo.pop_back(); // (nothing changed: no undo step)
            m_ListDrag.Kind = ListDrag::None;
        }

        // ---- Drawing ----
        dl->PushClipRect(grid.Min, grid.Max, true);
        for (int t = trackAt(grid.Min.y); t < trackCount && yOf(t) < grid.Max.y; ++t)
        {
            float y = yOf(t);
            dl->AddRectFilled(ImVec2(grid.Min.x, y), ImVec2(grid.Max.x, y + trackH), t % 2 ? IM_COL32(44, 46, 50, 255) : IM_COL32(50, 52, 57, 255));
        }
        for (int b = firstBar; xOf((double)b * barT) < grid.Max.x; ++b)
        {
            float x = xOf((double)b * barT);
            dl->AddLine(ImVec2(x, grid.Min.y), ImVec2(x, grid.Max.y), IM_COL32(28, 28, 31, 255), 1.5f);
            if (m_ListBarW >= 40.0f)
                for (int beat = 1; beat < m_Song.BeatsPerBar; ++beat)
                {
                    float bx = xOf((double)b * barT + beat * kTicksPerBeat);
                    dl->AddLine(ImVec2(bx, grid.Min.y), ImVec2(bx, grid.Max.y), IM_COL32(38, 39, 43, 255));
                }
        }
        for (int i = 0; i < (int)m_Song.Playlist.size(); ++i)
        {
            const PlaylistClip& c = m_Song.Playlist[i];
            const Pattern& p = m_Song.Patterns[c.Pattern];
            float x0 = xOf(c.Start), x1 = xOf(c.Start + m_Song.ClipTicks(c)), y = yOf(c.Track);
            if (x1 < grid.Min.x || x0 > grid.Max.x || y + trackH < grid.Min.y || y > grid.Max.y) continue;
            bool muted = m_Song.TrackMuted(c.Track);
            ImU32 color = PatternColor(p);
            if (muted) color = Shade(color, 0.45f);
            dl->AddRectFilled(ImVec2(x0, y + 1), ImVec2(x1, y + trackH - 1), Shade(color, 0.45f), 3.0f);
            dl->AddRectFilled(ImVec2(x0, y + 1), ImVec2(x1, y + 15), Shade(color, 0.8f), 3.0f, ImDrawFlags_RoundCornersTop);
            dl->AddRect(ImVec2(x0, y + 1), ImVec2(x1, y + trackH - 1), i == m_SelectedClip ? IM_COL32(255, 255, 255, 255) : Shade(color, 0.3f), 3.0f, 0,
                        i == m_SelectedClip ? 2.0f : 1.0f);
            dl->PushClipRect(ImVec2(std::max(x0, grid.Min.x), y), ImVec2(std::min(x1, grid.Max.x), y + trackH), true);
            dl->AddText(ImVec2(x0 + 4, y + 1), IM_COL32(20, 20, 20, 255), p.Name.c_str());
            // A preview of the pattern's notes.
            int lo = 127, hi = 0;
            for (const Note& n : p.Notes) lo = std::min(lo, n.Key), hi = std::max(hi, n.Key);
            int clipT = m_Song.ClipTicks(c);
            float top = y + 17, h = trackH - 20;
            for (const Note& n : p.Notes)
            {
                if (n.Start >= clipT) continue;
                float ny = hi > lo ? top + (1.0f - (float)(n.Key - lo) / (hi - lo)) * (h - 2) : top + h * 0.5f;
                float nx0 = xOf(c.Start + n.Start), nx1 = xOf(c.Start + std::min(clipT, n.Start + n.Length));
                dl->AddRectFilled(ImVec2(nx0, ny), ImVec2(std::max(nx0 + 2, nx1 - 1), ny + 2), Shade(color, 1.3f));
            }
            dl->PopClipRect();
        }
        for (int i = 0; i < (int)m_Song.Automation.size(); ++i)
        {
            const AutomationClip& a = m_Song.Automation[i];
            float x0 = xOf(a.Start), x1 = xOf(a.Start + a.Length), y = yOf(a.Track);
            if (x1 < grid.Min.x || x0 > grid.Max.x || y + trackH < grid.Min.y || y > grid.Max.y) continue;
            bool muted = m_Song.TrackMuted(a.Track);
            ImU32 color = muted ? IM_COL32(70, 100, 100, 255) : IM_COL32(70, 190, 180, 255);
            dl->AddRectFilled(ImVec2(x0, y + 1), ImVec2(x1, y + trackH - 1), Shade(color, 0.35f), 3.0f);
            dl->AddRect(ImVec2(x0, y + 1), ImVec2(x1, y + trackH - 1), i == m_SelectedAuto ? IM_COL32(255, 255, 255, 255) : Shade(color, 0.6f), 3.0f, 0,
                        i == m_SelectedAuto ? 2.0f : 1.0f);
            dl->PushClipRect(ImVec2(std::max(x0, grid.Min.x), y), ImVec2(std::min(x1, grid.Max.x), y + trackH), true);
            // The curve, sampled across the clip.
            float top = y + 15, h = trackH - 18;
            ImVec2 prev;
            for (int k = 0; k <= 48; ++k)
            {
                double t = a.Length * k / 48.0;
                ImVec2 pt(xOf(a.Start + t), top + (1.0f - a.ValueAt(t)) * h);
                if (k > 0) dl->AddLine(prev, pt, color, 1.5f);
                prev = pt;
            }
            dl->AddText(ImVec2(x0 + 4, y + 1), IM_COL32(220, 240, 238, 255), m_Song.ParamName(a.Target).c_str());
            dl->PopClipRect();
        }
        // Where a click would place the pattern.
        if (hovered && !ImGui::IsItemActive() && Edit::ClipAt(m_Song, mtrack, mt) < 0 && Edit::AutomationAt(m_Song, mtrack, mt) < 0)
        {
            float x0 = xOf(std::max(0, Edit::SnapFloor(mt, snap))), y = yOf(mtrack);
            dl->AddRect(ImVec2(x0, y + 1), ImVec2(x0 + m_Song.PatternTicks(m_Pattern) * tickW, y + trackH - 1), IM_COL32(255, 255, 255, 70), 3.0f);
        }
        // Playhead (Song mode).
        if (m_Player && m_SongMode)
        {
            float x = xOf(m_Player->GetTick());
            dl->AddLine(ImVec2(x, grid.Min.y), ImVec2(x, grid.Max.y), IM_COL32(120, 255, 140, 220), 2.0f);
        }
        dl->PopClipRect();
        if (m_Player && m_SongMode)
        {
            float x = xOf(m_Player->GetTick());
            if (x >= ruler.Min.x && x <= ruler.Max.x)
                dl->AddTriangleFilled(ImVec2(x - 6, ruler.Max.y - 9), ImVec2(x + 6, ruler.Max.y - 9), ImVec2(x, ruler.Max.y), IM_COL32(120, 255, 140, 255));
        }
        if (editorH > 0.0f && m_SelectedAuto >= 0) DrawAutomationEditor(ImVec2(origin.x, grid.Max.y + 6.0f), ImVec2(origin.x + avail.x, origin.y + avail.y));
        ImGui::EndChild();
    }

    void MusicMode::DrawAutomationEditor(const ImVec2& min, const ImVec2& max)
    {
        // The selected automation clip's curve, filling the panel: click adds a point, drag moves one, right-click
        // removes one (a clip keeps at least one).
        AutomationClip& a = m_Song.Automation[m_SelectedAuto];
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImGuiIO& io = ImGui::GetIO();
        dl->AddRectFilled(min, max, IM_COL32(32, 34, 38, 255), 3.0f);
        float lo, hi;
        bool log;
        m_Song.ParamRange(a.Target, lo, hi, log);
        std::string title = m_Song.ParamName(a.Target) + "  (" + std::to_string((int)std::lround(a.Length / (double)m_Song.BarTicks() * 100) / 100.0).substr(0, 4) +
                            " bars)  -  click adds a point, drag moves it, right-click removes it";
        dl->AddText(ImVec2(min.x + 6, min.y + 3), IM_COL32(200, 230, 228, 255), title.c_str());
        ImRect area(ImVec2(min.x + 46, min.y + 22), ImVec2(max.x - 10, max.y - 8));
        if (area.GetWidth() < 20.0f || area.GetHeight() < 20.0f) return;
        auto xOf = [&](double tick) { return area.Min.x + (float)(tick / std::max(1, a.Length)) * area.GetWidth(); };
        auto yOf = [&](float v) { return area.Max.y - v * area.GetHeight(); };
        auto tickAt = [&](float x) { return (x - area.Min.x) / area.GetWidth() * a.Length; };
        auto valueAt = [&](float y) { return std::clamp((area.Max.y - y) / area.GetHeight(), 0.0f, 1.0f); };
        // Grid: bars and beats; the range's ends labeled.
        for (int t = 0; t <= a.Length; t += kTicksPerBeat)
            dl->AddLine(ImVec2(xOf(t), area.Min.y), ImVec2(xOf(t), area.Max.y), t % m_Song.BarTicks() == 0 ? IM_COL32(70, 72, 78, 255) : IM_COL32(48, 50, 55, 255));
        dl->AddRect(area.Min, area.Max, IM_COL32(70, 72, 78, 255));
        char label[32];
        std::snprintf(label, sizeof(label), "%.6g", hi);
        dl->AddText(ImVec2(min.x + 4, area.Min.y - 2), IM_COL32(150, 150, 150, 255), label);
        std::snprintf(label, sizeof(label), "%.6g", lo);
        dl->AddText(ImVec2(min.x + 4, area.Max.y - ImGui::GetTextLineHeight()), IM_COL32(150, 150, 150, 255), label);

        ImGui::SetCursorScreenPos(ImVec2(area.Min.x - 6, area.Min.y - 6));
        ImGui::InvisibleButton("##autoEditor", ImVec2(area.GetWidth() + 12, area.GetHeight() + 12), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
        ImVec2 mouse = io.MousePos;
        auto pointAt = [&]() {
            for (int i = 0; i < (int)a.Points.size(); ++i)
                if (std::fabs(xOf(a.Points[i].Tick) - mouse.x) < 7.0f && std::fabs(yOf(a.Points[i].Value) - mouse.y) < 7.0f) return i;
            return -1;
        };
        int hover = ImGui::IsItemHovered() ? pointAt() : -1;
        const int snap = std::max(1, m_ListSnap == 384 || m_ListSnap == 96 ? m_Song.StepTicks() : m_ListSnap); // (finer than the clips' snap)
        if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            Record();
            m_AutoDragPoint = hover;
            if (m_AutoDragPoint < 0)
            {
                AutomationPoint p{ std::clamp(Edit::SnapRound(tickAt(mouse.x), snap), 0, a.Length), valueAt(mouse.y) };
                a.Points.push_back(p);
                a.SortPoints();
                for (int i = 0; i < (int)a.Points.size(); ++i)
                    if (a.Points[i] == p) m_AutoDragPoint = i;
                Changed();
            }
        }
        if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left) && m_AutoDragPoint >= 0 && m_AutoDragPoint < (int)a.Points.size())
        {
            // Between its neighbours (points keep their order).
            int lo2 = m_AutoDragPoint > 0 ? a.Points[m_AutoDragPoint - 1].Tick : 0;
            int hi2 = m_AutoDragPoint + 1 < (int)a.Points.size() ? a.Points[m_AutoDragPoint + 1].Tick : a.Length;
            AutomationPoint p{ std::clamp(Edit::SnapRound(tickAt(mouse.x), snap), lo2, hi2), valueAt(mouse.y) };
            if (!(p == a.Points[m_AutoDragPoint]))
            {
                a.Points[m_AutoDragPoint] = p;
                Changed();
            }
            float shown = m_Song.FromNormalized(a.Target, p.Value);
            ImGui::SetTooltip("%.3g", shown);
        }
        if (ImGui::IsItemDeactivated()) m_AutoDragPoint = -1;
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && hover >= 0 && a.Points.size() > 1)
        {
            Record();
            a.Points.erase(a.Points.begin() + hover);
            Changed();
        }
        if (hover >= 0 && !ImGui::IsItemActive()) ImGui::SetTooltip("%.3g", m_Song.FromNormalized(a.Target, a.Points[hover].Value));

        // The curve and its points.
        dl->PushClipRect(area.Min, area.Max, true);
        ImVec2 prev(area.Min.x, yOf(a.ValueAt(0)));
        for (int k = 1; k <= 200; ++k)
        {
            double t = a.Length * k / 200.0;
            ImVec2 pt(xOf(t), yOf(a.ValueAt(t)));
            dl->AddLine(prev, pt, IM_COL32(70, 190, 180, 255), 2.0f);
            prev = pt;
        }
        if (m_Player && m_SongMode)
        {
            double t = m_Player->GetTick() - a.Start;
            if (t >= 0 && t <= a.Length) dl->AddLine(ImVec2(xOf(t), area.Min.y), ImVec2(xOf(t), area.Max.y), IM_COL32(120, 255, 140, 200), 1.5f);
        }
        dl->PopClipRect();
        for (int i = 0; i < (int)a.Points.size(); ++i)
        {
            ImVec2 c(xOf(a.Points[i].Tick), yOf(a.Points[i].Value));
            dl->AddCircleFilled(c, i == hover || i == m_AutoDragPoint ? 6.0f : 4.5f, i == m_AutoDragPoint ? IM_COL32(255, 255, 255, 255) : IM_COL32(150, 240, 230, 255));
        }
    }

    void MusicMode::PlaylistKeys()
    {
        if ((ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false)) && m_SelectedAuto >= 0 &&
            m_SelectedAuto < (int)m_Song.Automation.size())
        {
            Record();
            m_Song.Automation.erase(m_Song.Automation.begin() + m_SelectedAuto);
            m_SelectedAuto = -1;
            Changed();
            return;
        }
        if ((ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false)) && m_SelectedClip >= 0 &&
            m_SelectedClip < (int)m_Song.Playlist.size())
        {
            Record();
            m_Song.Playlist.erase(m_Song.Playlist.begin() + m_SelectedClip);
            m_SelectedClip = -1;
            Changed();
        }
    }
}
