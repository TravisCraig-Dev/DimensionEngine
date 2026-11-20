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
        ImGui::TextDisabled("Click to place the pattern, drag to move, drag the end to resize, right-click deletes, double-click edits it; click the ruler to seek");
        if (!m_SongMode)
        {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.35f, 1.0f), "(Pattern mode: switch to SONG to play the Playlist)");
        }

        const float headerW = 140.0f, rulerH = 22.0f;
        ImVec2 origin = ImGui::GetCursorScreenPos();
        ImVec2 avail = ImGui::GetContentRegionAvail();
        ImRect ruler(ImVec2(origin.x + headerW, origin.y), ImVec2(origin.x + avail.x, origin.y + rulerH));
        ImRect grid(ImVec2(ruler.Min.x, ruler.Max.y), ImVec2(ruler.Max.x, origin.y + avail.y));
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

        if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
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
            int hit = Edit::ClipAt(m_Song, mtrack, mt);
            Record();
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
        }
        if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
        {
            m_ListDrag = {};
            m_ListDrag.Kind = ListDrag::Erase;
            Record();
        }
        if (ImGui::IsItemActive())
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
                int hit = Edit::ClipAt(m_Song, mtrack, mt);
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
        // Where a click would place the pattern.
        if (hovered && !ImGui::IsItemActive() && Edit::ClipAt(m_Song, mtrack, mt) < 0)
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
        ImGui::EndChild();
    }

    void MusicMode::PlaylistKeys()
    {
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
