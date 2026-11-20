#pragma once
#include "Dimension/Music/Song.h"
#include <vector>

// Note and clip editing shared by the Piano Roll and the Playlist (kept here so it is tested headlessly).
namespace Dimension::Music::Edit
{
    // A tick on the grid: Floor to the grid line at or before it, Round to the nearest. A grid of 1 or less is free.
    int SnapFloor(double tick, int grid);
    int SnapRound(double tick, int grid);

    // Notes of `channel` in a pattern whose body overlaps ticks [t0, t1) and whose key is in [k0, k1] (indices).
    std::vector<int> NotesInBox(const Pattern& p, int channel, int t0, int t1, int k0, int k1);
    // The note of `channel` under a tick and key (the latest started one), or -1.
    int NoteAt(const Pattern& p, int channel, double tick, int key);

    // Moves notes by `ticks` and `keys` (starts stay at 0 or later, keys in 0..127).
    void Move(Pattern& p, const std::vector<int>& notes, int ticks, int keys);
    // Starts to the nearest grid line.
    void Quantize(Pattern& p, const std::vector<int>& notes, int grid);
    // Removes notes (indices into the pattern); the rest keep their order.
    void Remove(Pattern& p, std::vector<int> notes);

    // The Playlist clip under a tick on a track (the topmost: the last one), or -1.
    int ClipAt(const Song& s, int track, double tick);
}
