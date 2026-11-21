#include "Dimension/Music/Editing.h"
#include <algorithm>
#include <climits>
#include <cmath>

namespace Dimension::Music::Edit
{
    int SnapFloor(double tick, int grid)
    {
        if (grid <= 1) return (int)std::floor(tick);
        return (int)std::floor(tick / grid) * grid;
    }

    int SnapRound(double tick, int grid)
    {
        if (grid <= 1) return (int)std::lround(tick);
        return (int)std::lround(tick / grid) * grid;
    }

    std::vector<int> NotesInBox(const Pattern& p, int channel, int t0, int t1, int k0, int k1)
    {
        if (t0 > t1) std::swap(t0, t1);
        if (k0 > k1) std::swap(k0, k1);
        std::vector<int> out;
        for (int i = 0; i < (int)p.Notes.size(); ++i)
        {
            const Note& n = p.Notes[i];
            if (n.Channel == channel && n.Key >= k0 && n.Key <= k1 && n.Start < t1 && n.Start + n.Length > t0) out.push_back(i);
        }
        return out;
    }

    int NoteAt(const Pattern& p, int channel, double tick, int key)
    {
        int found = -1;
        for (int i = 0; i < (int)p.Notes.size(); ++i)
        {
            const Note& n = p.Notes[i];
            if (n.Channel == channel && n.Key == key && tick >= n.Start && tick < n.Start + n.Length &&
                (found < 0 || n.Start >= p.Notes[found].Start))
                found = i;
        }
        return found;
    }

    void Move(Pattern& p, const std::vector<int>& notes, int ticks, int keys)
    {
        // The whole group stops at the edges (so chords keep their shape).
        int minStart = INT_MAX, minKey = 127, maxKey = 0;
        for (int i : notes)
            if (i >= 0 && i < (int)p.Notes.size())
            {
                minStart = std::min(minStart, p.Notes[i].Start);
                minKey = std::min(minKey, p.Notes[i].Key);
                maxKey = std::max(maxKey, p.Notes[i].Key);
            }
        if (minStart == INT_MAX) return;
        ticks = std::max(ticks, -minStart);
        keys = std::clamp(keys, -minKey, 127 - maxKey);
        for (int i : notes)
            if (i >= 0 && i < (int)p.Notes.size())
            {
                p.Notes[i].Start += ticks;
                p.Notes[i].Key += keys;
            }
    }

    void Quantize(Pattern& p, const std::vector<int>& notes, int grid)
    {
        for (int i : notes)
            if (i >= 0 && i < (int)p.Notes.size()) p.Notes[i].Start = std::max(0, SnapRound(p.Notes[i].Start, grid));
    }

    void Remove(Pattern& p, std::vector<int> notes)
    {
        std::sort(notes.begin(), notes.end());
        notes.erase(std::unique(notes.begin(), notes.end()), notes.end());
        for (auto it = notes.rbegin(); it != notes.rend(); ++it)
            if (*it >= 0 && *it < (int)p.Notes.size()) p.Notes.erase(p.Notes.begin() + *it);
    }

    int ClipAt(const Song& s, int track, double tick)
    {
        for (int i = (int)s.Playlist.size() - 1; i >= 0; --i)
        {
            const PlaylistClip& c = s.Playlist[i];
            if (c.Track == track && tick >= c.Start && tick < c.Start + s.ClipTicks(c)) return i;
        }
        return -1;
    }

    int AutomationAt(const Song& s, int track, double tick)
    {
        for (int i = (int)s.Automation.size() - 1; i >= 0; --i)
        {
            const AutomationClip& a = s.Automation[i];
            if (a.Track == track && tick >= a.Start && tick < a.Start + a.Length) return i;
        }
        return -1;
    }

    int AudioClipAt(const Song& s, int track, double tick)
    {
        for (int i = (int)s.AudioClips.size() - 1; i >= 0; --i)
        {
            const AudioClip& a = s.AudioClips[i];
            if (a.Track == track && tick >= a.Start && tick < a.Start + s.ClipTicks(a)) return i;
        }
        return -1;
    }

    void NoteRecorder::Begin(int channel, int loopTicks, int grid)
    {
        m_Held.clear();
        m_Channel = channel;
        m_Loop = std::max(1, loopTicks);
        m_Grid = std::max(1, grid);
    }

    void NoteRecorder::NoteOn(int key, float velocity, double tick)
    {
        std::erase_if(m_Held, [&](const Held& h) { return h.Key == key; });
        m_Held.push_back({ key, velocity, tick });
    }

    Note NoteRecorder::Make(const Held& h, double end) const
    {
        double length = end - h.Start;
        if (length < 0.0) length += m_Loop; // (it wrapped past the loop's end)
        int start = m_Grid > 1 ? SnapRound(h.Start, m_Grid) : (int)std::floor(h.Start);
        if (start >= m_Loop) start -= m_Loop;
        int len = m_Grid > 1 ? std::max(m_Grid, SnapRound(length, m_Grid)) : std::max(1, (int)std::lround(length));
        return { m_Channel, std::max(0, start), std::min(len, m_Loop), h.Key, std::clamp(h.Velocity, 0.0f, 1.0f) };
    }

    bool NoteRecorder::NoteOff(int key, double tick, Note& out)
    {
        for (size_t i = 0; i < m_Held.size(); ++i)
            if (m_Held[i].Key == key)
            {
                out = Make(m_Held[i], tick);
                m_Held.erase(m_Held.begin() + i);
                return true;
            }
        return false;
    }

    std::vector<Note> NoteRecorder::End(double tick)
    {
        std::vector<Note> out;
        for (const Held& h : m_Held) out.push_back(Make(h, tick));
        m_Held.clear();
        return out;
    }

    bool NoteRecorder::IsHeld(int key) const
    {
        for (const Held& h : m_Held)
            if (h.Key == key) return true;
        return false;
    }
}
