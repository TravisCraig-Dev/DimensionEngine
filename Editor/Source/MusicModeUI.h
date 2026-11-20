#pragma once
// Small drawing helpers shared by Music Mode's views (MusicMode.cpp, MusicPianoRoll.cpp, MusicPlaylist.cpp).
#include <Dimension/Music/Song.h>
#include <imgui.h>
#include <algorithm>
#include <string>

namespace Editor::MusicUI
{
    // FL's naming: 60 = C5.
    inline std::string NoteName(int key)
    {
        static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        return std::string(names[((key % 12) + 12) % 12]) + std::to_string(key / 12);
    }

    inline bool IsBlackKey(int key)
    {
        int k = ((key % 12) + 12) % 12;
        return k == 1 || k == 3 || k == 6 || k == 8 || k == 10;
    }

    inline ImU32 Shade(ImU32 c, float f)
    {
        ImVec4 v = ImGui::ColorConvertU32ToFloat4(c);
        return ImGui::ColorConvertFloat4ToU32(ImVec4(std::min(1.0f, v.x * f), std::min(1.0f, v.y * f), std::min(1.0f, v.z * f), v.w));
    }

    inline ImU32 WithAlpha(ImU32 c, float a)
    {
        ImVec4 v = ImGui::ColorConvertU32ToFloat4(c);
        v.w = a;
        return ImGui::ColorConvertFloat4ToU32(v);
    }

    // A pattern's color in the Playlist (picked from its name, like a channel's).
    inline ImU32 PatternColor(const Dimension::Music::Pattern& p)
    {
        Dimension::Music::Channel c;
        c.Name = p.Name + "#pattern";
        return Dimension::Music::ChannelColor(c);
    }

    // Snap choices (ticks; 1 = none) and their names, for a song's steps.
    struct SnapChoice
    {
        const char* Name;
        int Ticks;
    };
    inline const SnapChoice* SnapChoices(int& count)
    {
        static const SnapChoice choices[] = { { "None", 1 }, { "1/4 step", 6 }, { "1/2 step", 12 }, { "Step", 24 }, { "Beat", 96 }, { "Bar", 384 } };
        count = (int)(sizeof(choices) / sizeof(choices[0]));
        return choices;
    }
}
