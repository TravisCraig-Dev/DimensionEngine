#pragma once
#include "EditorContext.h"
#include <Dimension/Music/SongPlayer.h>
#include <deque>
#include <imgui.h>
#include <memory>

namespace Editor
{
    // Music Mode: a full-screen music workspace (like FL Studio's) in place of the editor's windows while a song is
    // open. A song is a .dsong asset: channels (synthesized drums, a synth, samplers) in the Channel Rack, their notes
    // in patterns (the step sequencer), played live through the sound card and rendered to WAV files the game plays.
    class MusicMode
    {
    public:
        explicit MusicMode(EditorContext& ctx) : m_Ctx(ctx) {}
        ~MusicMode();

        bool IsActive() const { return m_Active; }
        AssetHandle GetSong() const { return m_Asset; }
        // Opens the song (closing one already open); false when it cannot be read.
        bool Open(AssetHandle song);
        // Saves and goes back to the editor.
        void Close();
        bool Save();
        // A new song file (unique name in `folder`) from the default beat; returns its handle.
        static AssetHandle CreateSongAsset(const fs::path& folder, const std::string& name);

        // The whole window (call each frame while active, instead of the editor's windows).
        void OnImGui();

        // Automation: what is playing.
        Dimension::Music::SongPlayer* GetPlayer() { return m_Player.get(); }
        Dimension::Music::Song& GetSongData() { return m_Song; }

    private:
        void DrawTopBar();
        void DrawChannelRack(const ImVec2& size);
        void DrawChannelSettings(const ImVec2& size);
        void DrawKeyboard(const ImVec2& size);
        void HandleKeys();
        // Undo: Record before a change, Changed after it (the player hears it; the file is written once it is done).
        void Record();
        void Changed();
        void Undo();
        void Redo();
        void TogglePlay();
        void StopPlayback();
        void RenderToFile();
        void Preview(int channel, int key, bool on);
        // A knob (vertical drag; double-click resets). Returns true when the value changed; records undo as it starts.
        bool Knob(const char* id, float* value, float min, float max, float reset, float size, const char* format);
        bool Edited(bool changed); // records undo when the last widget starts being used; true when it changed

        EditorContext& m_Ctx;
        bool m_Active = false;
        AssetHandle m_Asset = AssetHandle::None();
        fs::path m_File;
        Dimension::Music::Song m_Song;
        std::shared_ptr<Dimension::Music::SongPlayer> m_Player;
        bool m_Dirty = false, m_Unsaved = false;
        std::deque<Dimension::Music::Song> m_Undo, m_Redo;
        int m_Pattern = 0;
        int m_SelectedChannel = 0;
        int m_RenamingChannel = -1;
        std::string m_RenameBuffer;
        // Painting steps: dragging over the step buttons sets them all as the first one became.
        int m_PaintChannel = -1;
        bool m_PaintOn = false;
        std::vector<float> m_Meters;
        float m_MasterMeter[2] = { 0, 0 };
        // Notes held by the typing keyboard (ImGuiKey -> key) and the on-screen piano.
        std::vector<std::pair<int, int>> m_TypedNotes;
        int m_PianoNote = -1;
        int m_TypingOctave = 5;
        std::string m_Status;
        float m_StatusTime = 0.0f;
    };
}
