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
}
