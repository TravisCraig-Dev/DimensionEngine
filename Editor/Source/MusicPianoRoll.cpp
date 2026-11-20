// Music Mode's Piano Roll: the selected channel's notes in the current pattern, drawn on a grid of keys and time.
#include "MusicMode.h"
#include "MusicModeUI.h"
#include <Dimension/Music/Editing.h>
#include <imgui_internal.h>
#include <algorithm>
#include <cmath>

namespace Editor
{
    using namespace Dimension;
    using namespace Dimension::Music;
    using namespace Editor::MusicUI;

    void MusicMode::DrawPianoRoll(const ImVec2& size)
    {
        ImGui::BeginChild("##PianoRoll", size, ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        if (m_SelectedChannel < 0 || m_SelectedChannel >= (int)m_Song.Channels.size())
        {
            ImGui::TextDisabled("Add a channel to write notes for it");
            ImGui::EndChild();
            return;
        }
        const int ch = m_SelectedChannel;
        Pattern& pat = m_Song.Patterns[m_Pattern];
        const Channel& channel = m_Song.Channels[ch];
        const ImU32 color = ChannelColor(channel);

        // Toolbar: the channel, snap, quantize.
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Piano Roll");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(140.0f);
        if (ImGui::BeginCombo("##rollChannel", channel.Name.c_str()))
        {
            for (int i = 0; i < (int)m_Song.Channels.size(); ++i)
                if (ImGui::Selectable(m_Song.Channels[i].Name.c_str(), i == ch))
                {
                    m_SelectedChannel = i;
                    m_RollSelection.clear();
                }
            ImGui::EndCombo();
        }
        ImGui::SameLine(0.0f, 16.0f);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Snap");
        ImGui::SameLine();
        int snapCount;
        const SnapChoice* snaps = SnapChoices(snapCount);
        const char* snapName = "Step";
        for (int i = 0; i < snapCount; ++i)
            if (snaps[i].Ticks == m_RollSnap) snapName = snaps[i].Name;
        ImGui::SetNextItemWidth(100.0f);
        if (ImGui::BeginCombo("##rollSnap", snapName))
        {
            for (int i = 0; i < snapCount; ++i)
                if (ImGui::Selectable(snaps[i].Name, snaps[i].Ticks == m_RollSnap)) m_RollSnap = snaps[i].Ticks;
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Button("Quantize"))
        {
            std::vector<int> notes = m_RollSelection;
            if (notes.empty())
                for (int i = 0; i < (int)pat.Notes.size(); ++i)
                    if (pat.Notes[i].Channel == ch) notes.push_back(i);
            Record();
            Edit::Quantize(pat, notes, std::max(1, m_RollSnap));
            Changed();
        }
        ImGui::SetItemTooltip("Starts to the snap grid (the selection, or all of the channel's notes) - Ctrl+Q");
        ImGui::SameLine(0.0f, 16.0f);
        ImGui::TextDisabled("Click to add, drag to move, drag the end to resize, right-click deletes, Ctrl+drag selects; wheel scrolls, Ctrl+wheel zooms");

        // Layout: keys on the left, the grid, the velocity lane under it.
        const float keysW = 54.0f, velH = 60.0f, gap = 4.0f;
        ImVec2 origin = ImGui::GetCursorScreenPos();
        ImVec2 avail = ImGui::GetContentRegionAvail();
        ImRect grid(ImVec2(origin.x + keysW, origin.y), ImVec2(origin.x + avail.x, origin.y + avail.y - velH - gap));
        ImRect lane(ImVec2(grid.Min.x, grid.Max.y + gap), ImVec2(grid.Max.x, origin.y + avail.y));
        ImRect keys(origin, ImVec2(grid.Min.x, grid.Max.y));
        if (grid.GetWidth() < 20.0f || grid.GetHeight() < 20.0f)
        {
            ImGui::EndChild();
            return;
        }
        const float keyH = m_RollKeyH, tickW = m_RollTickW;
        const float totalH = 128.0f * keyH;
        // First view: the channel's notes (or its step key) in the middle.
        if (m_RollScrollY < 0.0f)
        {
            int lo = 127, hi = 0;
            for (const Note& n : pat.Notes)
                if (n.Channel == ch) lo = std::min(lo, n.Key), hi = std::max(hi, n.Key);
            int center = lo <= hi ? (lo + hi) / 2 : channel.Key;
            m_RollScrollY = (127 - center) * keyH - grid.GetHeight() * 0.5f;
        }
        m_RollScrollY = std::clamp(m_RollScrollY, 0.0f, std::max(0.0f, totalH - grid.GetHeight()));
        m_RollScrollX = std::max(0.0, m_RollScrollX);
        auto xOf = [&](double tick) { return grid.Min.x + (float)((tick - m_RollScrollX) * tickW); };
        auto yOf = [&](int key) { return grid.Min.y + (127 - key) * keyH - m_RollScrollY; };
        auto tickAt = [&](float x) { return m_RollScrollX + (x - grid.Min.x) / tickW; };
        auto keyAt = [&](float y) { return std::clamp(127 - (int)std::floor((y - grid.Min.y + m_RollScrollY) / keyH), 0, 127); };

        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImGuiIO& io = ImGui::GetIO();
        ImVec2 mouse = io.MousePos;
        const int patternTicks = m_Song.PatternTicks(m_Pattern);

        // ---- The grid's mouse ----
        ImGui::SetCursorScreenPos(grid.Min);
        ImGui::InvisibleButton("##rollGrid", grid.GetSize(), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
        bool hovered = ImGui::IsItemHovered();
        double mt = tickAt(mouse.x);
        int mk = keyAt(mouse.y);
        if (hovered && io.MouseWheel != 0.0f)
        {
            if (io.KeyCtrl)
            {
                // Zoom about the mouse.
                double anchor = mt;
                m_RollTickW = std::clamp(m_RollTickW * std::pow(1.15f, io.MouseWheel), 0.1f, 8.0f);
                m_RollScrollX = std::max(0.0, anchor - (mouse.x - grid.Min.x) / m_RollTickW);
            }
            else if (io.KeyShift) m_RollScrollX = std::max(0.0, m_RollScrollX - io.MouseWheel * 60.0 / tickW);
            else m_RollScrollY -= io.MouseWheel * keyH * 3.0f;
        }
        if (hovered && io.MouseWheelH != 0.0f) m_RollScrollX = std::max(0.0, m_RollScrollX - io.MouseWheelH * 60.0 / tickW);

        const int snap = std::max(1, m_RollSnap);
        auto previewSwap = [&](int key) {
            if (key == m_RollDrag.PreviewKey) return;
            if (m_RollDrag.PreviewKey >= 0) Preview(ch, m_RollDrag.PreviewKey, false);
            m_RollDrag.PreviewKey = key;
            if (key >= 0) Preview(ch, key, true);
        };
        auto inSelection = [&](int i) { return std::find(m_RollSelection.begin(), m_RollSelection.end(), i) != m_RollSelection.end(); };

        if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            m_RollDrag = {};
            m_RollDrag.StartTick = mt;
            m_RollDrag.StartKey = mk;
            m_RollDrag.StartMouse = mouse;
            int hit = Edit::NoteAt(pat, ch, mt, mk);
            if (io.KeyCtrl) m_RollDrag.Kind = RollDrag::Box;
            else
            {
                Record();
                if (hit < 0)
                {
                    // A new note on the grid, ready to be dragged into place.
                    pat.Notes.push_back({ ch, std::max(0, Edit::SnapFloor(mt, snap)), m_RollLength, mk, 0.8f });
                    hit = (int)pat.Notes.size() - 1;
                    m_RollSelection = { hit };
                    m_RollDrag.Kind = RollDrag::Move;
                    m_RollDrag.Moved = true;
                    Changed();
                }
                else
                {
                    if (io.KeyShift) { if (!inSelection(hit)) m_RollSelection.push_back(hit); }
                    else if (!inSelection(hit)) m_RollSelection = { hit };
                    const Note& n = pat.Notes[hit];
                    bool edge = mouse.x > xOf(n.Start + n.Length) - std::min(6.0f, (float)(n.Length * tickW) * 0.3f);
                    m_RollDrag.Kind = edge ? RollDrag::Resize : RollDrag::Move;
                    m_RollLength = n.Length;
                }
                m_RollDrag.Anchor = hit;
                m_RollDrag.Before = pat.Notes;
                previewSwap(pat.Notes[hit].Key);
            }
        }
        if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
        {
            m_RollDrag = {};
            m_RollDrag.Kind = RollDrag::Erase;
            Record();
        }
        if (ImGui::IsItemActive())
        {
            switch (m_RollDrag.Kind)
            {
            case RollDrag::Move:
            case RollDrag::Resize:
            {
                if (m_RollDrag.Anchor < 0 || m_RollDrag.Anchor >= (int)m_RollDrag.Before.size()) break;
                const Note anchor = m_RollDrag.Before[m_RollDrag.Anchor];
                pat.Notes = m_RollDrag.Before;
                if (m_RollDrag.Kind == RollDrag::Move)
                {
                    int start = std::max(0, Edit::SnapRound(anchor.Start + (mt - m_RollDrag.StartTick), snap));
                    int dt = start - anchor.Start, dk = mk - m_RollDrag.StartKey;
                    Edit::Move(pat, m_RollSelection, dt, dk);
                    if (dt != 0 || dk != 0) m_RollDrag.Moved = true;
                    previewSwap(pat.Notes[m_RollDrag.Anchor].Key);
                }
                else
                {
                    int end = Edit::SnapRound(anchor.Start + anchor.Length + (mt - m_RollDrag.StartTick), snap);
                    int dl2 = end - (anchor.Start + anchor.Length);
                    for (int i : m_RollSelection)
                        if (i >= 0 && i < (int)pat.Notes.size()) pat.Notes[i].Length = std::max(snap > 1 ? snap : 4, m_RollDrag.Before[i].Length + dl2);
                    if (dl2 != 0) m_RollDrag.Moved = true;
                    m_RollLength = pat.Notes[m_RollDrag.Anchor].Length;
                }
                if (m_RollDrag.Moved) Changed();
                break;
            }
            case RollDrag::Erase:
            {
                int hit = Edit::NoteAt(pat, ch, mt, mk);
                if (hit >= 0)
                {
                    Edit::Remove(pat, { hit });
                    m_RollSelection.clear();
                    m_RollDrag.Moved = true;
                    Changed();
                }
                break;
            }
            default: break;
            }
        }
        if (ImGui::IsItemDeactivated())
        {
            if (m_RollDrag.Kind == RollDrag::Box)
            {
                int t0 = (int)tickAt(std::min(m_RollDrag.StartMouse.x, mouse.x)), t1 = (int)std::ceil(tickAt(std::max(m_RollDrag.StartMouse.x, mouse.x)));
                auto box = Edit::NotesInBox(pat, ch, t0, t1, keyAt(std::max(m_RollDrag.StartMouse.y, mouse.y)), keyAt(std::min(m_RollDrag.StartMouse.y, mouse.y)));
                if (io.KeyShift) for (int i : box) { if (!inSelection(i)) m_RollSelection.push_back(i); }
                else m_RollSelection = box;
            }
            if ((m_RollDrag.Kind == RollDrag::Move || m_RollDrag.Kind == RollDrag::Resize) && m_RollDrag.Moved)
            {
                m_Song.FitPatternBars(m_Pattern); // (a note past the end makes the pattern longer)
                Changed();
            }
            if ((m_RollDrag.Kind == RollDrag::Move || m_RollDrag.Kind == RollDrag::Resize || m_RollDrag.Kind == RollDrag::Erase) && !m_RollDrag.Moved && !m_Undo.empty())
                m_Undo.pop_back(); // (nothing changed: no undo step)
            previewSwap(-1);
            m_RollDrag.Kind = RollDrag::None;
        }

        // ---- Drawing ----
        dl->PushClipRect(grid.Min, grid.Max, true);
        int topKey = keyAt(grid.Min.y), bottomKey = keyAt(grid.Max.y - 1);
        for (int k = bottomKey; k <= topKey; ++k)
        {
            float y = yOf(k);
            ImU32 row = IsBlackKey(k) ? IM_COL32(40, 42, 46, 255) : IM_COL32(52, 54, 59, 255);
            dl->AddRectFilled(ImVec2(grid.Min.x, y), ImVec2(grid.Max.x, y + keyH), row);
            dl->AddLine(ImVec2(grid.Min.x, y + keyH), ImVec2(grid.Max.x, y + keyH), k % 12 == 0 ? IM_COL32(20, 20, 22, 255) : IM_COL32(34, 35, 38, 255));
        }
        // Time lines: steps, beats, bars; past the pattern's end shaded.
        const int stepT = m_Song.StepTicks(), barT = m_Song.BarTicks();
        double firstTick = std::floor(m_RollScrollX / stepT) * stepT;
        for (double t = firstTick; xOf(t) < grid.Max.x; t += stepT)
        {
            int ti = (int)t;
            ImU32 c = ti % barT == 0 ? IM_COL32(18, 18, 20, 255) : ti % kTicksPerBeat == 0 ? IM_COL32(30, 31, 34, 255) : IM_COL32(44, 46, 50, 255);
            if (stepT * tickW < 5.0f && ti % kTicksPerBeat != 0) continue;
            dl->AddLine(ImVec2(xOf(t), grid.Min.y), ImVec2(xOf(t), grid.Max.y), c, ti % barT == 0 ? 2.0f : 1.0f);
        }
        if (xOf(patternTicks) < grid.Max.x) dl->AddRectFilled(ImVec2(std::max(grid.Min.x, xOf(patternTicks)), grid.Min.y), grid.Max, IM_COL32(0, 0, 0, 70));
        // Other channels' notes (ghosts), then this channel's.
        for (const Note& n : pat.Notes)
        {
            if (n.Channel == ch) continue;
            float x0 = xOf(n.Start), x1 = xOf(n.Start + n.Length), y = yOf(n.Key);
            if (x1 < grid.Min.x || x0 > grid.Max.x || y + keyH < grid.Min.y || y > grid.Max.y) continue;
            dl->AddRectFilled(ImVec2(x0, y + 1), ImVec2(x1, y + keyH - 1), WithAlpha(ChannelColor(m_Song.Channels[n.Channel]), 0.22f), 2.0f);
        }
        for (int i = 0; i < (int)pat.Notes.size(); ++i)
        {
            const Note& n = pat.Notes[i];
            if (n.Channel != ch) continue;
            float x0 = xOf(n.Start), x1 = xOf(n.Start + n.Length), y = yOf(n.Key);
            if (x1 < grid.Min.x || x0 > grid.Max.x || y + keyH < grid.Min.y || y > grid.Max.y) continue;
            bool selected = inSelection(i);
            ImU32 fill = Shade(color, 0.55f + 0.5f * n.Velocity);
            dl->AddRectFilled(ImVec2(x0, y + 1), ImVec2(x1, y + keyH - 1), fill, 2.0f);
            dl->AddRect(ImVec2(x0, y + 1), ImVec2(x1, y + keyH - 1), selected ? IM_COL32(255, 255, 255, 255) : Shade(color, 0.4f), 2.0f, 0, selected ? 2.0f : 1.0f);
            if (x1 - x0 > 26.0f && keyH >= 11.0f) dl->AddText(ImVec2(x0 + 3, y + (keyH - ImGui::GetTextLineHeight()) * 0.5f), IM_COL32(20, 20, 20, 255), NoteName(n.Key).c_str());
        }
        // The hovered cell (where a click puts a note) and the box being drawn.
        if (hovered && !ImGui::IsItemActive() && Edit::NoteAt(pat, ch, mt, mk) < 0)
        {
            float x0 = xOf(std::max(0, Edit::SnapFloor(mt, snap))), y = yOf(mk);
            dl->AddRect(ImVec2(x0, y + 1), ImVec2(x0 + m_RollLength * tickW, y + keyH - 1), WithAlpha(color, 0.6f), 2.0f);
        }
        if (m_RollDrag.Kind == RollDrag::Box && ImGui::IsItemActive())
        {
            dl->AddRectFilled(m_RollDrag.StartMouse, mouse, IM_COL32(120, 170, 255, 40));
            dl->AddRect(m_RollDrag.StartMouse, mouse, IM_COL32(120, 170, 255, 200));
        }
        // Playhead.
        if (m_Player && m_Player->IsPlaying() && m_Player->GetMode() == SongPlayer::Mode::Pattern && m_Player->GetPattern() == m_Pattern)
        {
            float x = xOf(m_Player->GetTick());
            dl->AddLine(ImVec2(x, grid.Min.y), ImVec2(x, grid.Max.y), IM_COL32(120, 255, 140, 220), 2.0f);
        }
        dl->PopClipRect();

        // ---- Keys (click to hear them) ----
        ImGui::SetCursorScreenPos(keys.Min);
        ImGui::InvisibleButton("##rollKeys", keys.GetSize());
        if (ImGui::IsItemHovered() && io.MouseWheel != 0.0f && !io.KeyCtrl) m_RollScrollY -= io.MouseWheel * keyH * 3.0f;
        int keyHeld = ImGui::IsItemActive() ? keyAt(mouse.y) : -1;
        if (keyHeld != m_PianoNote)
        {
            if (m_PianoNote >= 0) Preview(ch, m_PianoNote, false);
            m_PianoNote = keyHeld;
            if (m_PianoNote >= 0) Preview(ch, m_PianoNote, true);
        }
        dl->PushClipRect(keys.Min, keys.Max, true);
        for (int k = bottomKey; k <= topKey; ++k)
        {
            float y = yOf(k);
            bool black = IsBlackKey(k);
            ImU32 fill = k == keyHeld ? IM_COL32(150, 200, 255, 255) : black ? IM_COL32(28, 28, 28, 255) : IM_COL32(236, 236, 236, 255);
            dl->AddRectFilled(ImVec2(keys.Min.x, y), ImVec2(black ? keys.Min.x + keys.GetWidth() * 0.62f : keys.Max.x, y + keyH), fill);
            dl->AddLine(ImVec2(keys.Min.x, y + keyH), ImVec2(keys.Max.x, y + keyH), IM_COL32(60, 60, 60, 255));
            if (k % 12 == 0 && keyH >= 10.0f)
                dl->AddText(ImVec2(keys.Max.x - ImGui::CalcTextSize(NoteName(k).c_str()).x - 3, y + (keyH - ImGui::GetTextLineHeight()) * 0.5f), IM_COL32(60, 60, 60, 255), NoteName(k).c_str());
        }
        dl->PopClipRect();

        // ---- Velocity lane (drag over the notes' stems) ----
        ImGui::SetCursorScreenPos(lane.Min);
        ImGui::InvisibleButton("##rollVelocity", lane.GetSize());
        if (ImGui::IsItemActivated()) Record();
        bool velChanged = false;
        if (ImGui::IsItemActive())
        {
            float v = std::clamp(1.0f - (mouse.y - lane.Min.y) / lane.GetHeight(), 0.0f, 1.0f);
            for (int i = 0; i < (int)pat.Notes.size(); ++i)
            {
                Note& n = pat.Notes[i];
                if (n.Channel != ch || std::fabs(xOf(n.Start) - mouse.x) > std::max(4.0f, stepT * tickW * 0.5f)) continue;
                if (!m_RollSelection.empty() && !inSelection(i) && std::any_of(m_RollSelection.begin(), m_RollSelection.end(), [&](int s) {
                        return s < (int)pat.Notes.size() && std::fabs(xOf(pat.Notes[s].Start) - mouse.x) <= std::max(4.0f, stepT * tickW * 0.5f);
                    }))
                    continue; // (where selected notes are, only they change)
                if (n.Velocity != v) n.Velocity = v, velChanged = true;
            }
        }
        if (velChanged) Changed();
        dl->AddRectFilled(lane.Min, lane.Max, IM_COL32(36, 37, 41, 255));
        dl->PushClipRect(lane.Min, lane.Max, true);
        for (int i = 0; i < (int)pat.Notes.size(); ++i)
        {
            const Note& n = pat.Notes[i];
            if (n.Channel != ch) continue;
            float x = xOf(n.Start);
            if (x < lane.Min.x - 4 || x > lane.Max.x) continue;
            float top = lane.Max.y - n.Velocity * (lane.GetHeight() - 4);
            ImU32 c = inSelection(i) ? IM_COL32(255, 255, 255, 255) : color;
            dl->AddLine(ImVec2(x + 1, lane.Max.y), ImVec2(x + 1, top), c, 2.0f);
            dl->AddCircleFilled(ImVec2(x + 1, top), 3.0f, c);
        }
        dl->PopClipRect();
        dl->AddText(ImVec2(keys.Min.x + 2, lane.Min.y + 2), IM_COL32(150, 150, 150, 255), "Velocity");
        ImGui::EndChild();
    }

    void MusicMode::PianoRollKeys()
    {
        // The Piano Roll's keys (while it is the view): delete, select all, transpose, nudge, quantize.
        if (m_SelectedChannel < 0 || m_SelectedChannel >= (int)m_Song.Channels.size()) return;
        Pattern& pat = m_Song.Patterns[m_Pattern];
        ImGuiIO& io = ImGui::GetIO();
        std::erase_if(m_RollSelection, [&](int i) { return i < 0 || i >= (int)pat.Notes.size() || pat.Notes[i].Channel != m_SelectedChannel; });
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A, false))
        {
            m_RollSelection.clear();
            for (int i = 0; i < (int)pat.Notes.size(); ++i)
                if (pat.Notes[i].Channel == m_SelectedChannel) m_RollSelection.push_back(i);
        }
        if (m_RollSelection.empty()) return;
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Q, false))
        {
            Record();
            Edit::Quantize(pat, m_RollSelection, std::max(1, m_RollSnap));
            Changed();
        }
        if (io.KeyCtrl) return;
        if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false))
        {
            Record();
            Edit::Remove(pat, m_RollSelection);
            m_RollSelection.clear();
            Changed();
            return;
        }
        int keys = (ImGui::IsKeyPressed(ImGuiKey_UpArrow) ? 1 : 0) - (ImGui::IsKeyPressed(ImGuiKey_DownArrow) ? 1 : 0);
        int ticks = (ImGui::IsKeyPressed(ImGuiKey_RightArrow) ? 1 : 0) - (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) ? 1 : 0);
        if (keys || ticks)
        {
            Record();
            Edit::Move(pat, m_RollSelection, ticks * std::max(1, m_RollSnap), keys * (io.KeyShift ? 12 : 1));
            m_Song.FitPatternBars(m_Pattern);
            Changed();
        }
    }
}
