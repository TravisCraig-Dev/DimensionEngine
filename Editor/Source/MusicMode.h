#pragma once
#include "EditorContext.h"
#include <Dimension/Music/SongPlayer.h>
#include <Dimension/Music/Editing.h>
#include <Dimension/Music/MidiInput.h>
#include <Dimension/Audio/AudioEngine.h>
#include <deque>
#include <imgui.h>
#include <atomic>
#include <map>
#include <mutex>
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
        // Open and drawn (not put aside behind another workspace tab).
        bool IsShown() const { return m_Active && !m_Suspended; }
        // Put aside while another workspace tab is shown: playback stops, MIDI input closes and the song is saved, but it
        // stays open (with its undo history) for Resume.
        void Suspend();
        void Resume();
        // Whether the editor should keep drawing at full rate (playing, recording, or a plug-in's editor window open:
        // their input does not reach the editor's window, so it would look idle).
        bool WantsFrames() const;
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
        // The Piano Roll: the selected channel's notes in the current pattern (MusicPianoRoll.cpp).
        void DrawPianoRoll(const ImVec2& size);
        void PianoRollKeys();
        // The Playlist: patterns placed on tracks along the song (MusicPlaylist.cpp).
        void DrawPlaylist(const ImVec2& size);
        void PlaylistKeys();
        // The Mixer: tracks with faders, pan, routing and effect chains (MusicMixer.cpp).
        void DrawMixer(const ImVec2& size);
        void DrawEffectPanel(const ImVec2& size);
        // Automation: a right-click menu on the last widget offering a clip for `ref`, making one, and the selected
        // clip's curve under the Playlist.
        void AutomationMenu(const Dimension::Music::ParamRef& ref);
        void CreateAutomation(const Dimension::Music::ParamRef& ref);
        void DrawAutomationEditor(const ImVec2& min, const ImVec2& max);
        // VST3 plug-ins (MusicPlugins.cpp): the list of installed ones (cached per project, rescanned on request), the
        // Plug-ins window (folders, scan), and a plug-in's editor button and parameter list (channels and effects).
        void LoadPluginList();
        void ScanPlugins();
        void DrawPluginManager();
        void DrawPluginPanel(Dimension::Music::PluginRef& ref, int index, int slot, float height);
        bool PluginMenu(bool instruments, Dimension::Music::PluginRef& picked); // true when one was picked
        // Recording (MusicRecording.cpp): MIDI input (straight to the sound on its own thread, and queued for recording),
        // notes recorded into the pattern while playing with REC on, audio takes from the microphone onto the Playlist,
        // and the Audio & MIDI window.
        void StartMidi();
        void StopMidi();
        void UpdateRecording();
        void BeginTake();
        void EndTake();
        void RecordLive(int key, float velocity, bool on); // a note played (typing keyboard, piano, MIDI)
        void DrawIOWindow();
        void LoadIOSettings();
        void SaveIOSettings();
        // An audio clip of `asset` (or a file) on the first free Playlist track from `start`; returns its index.
        int AddAudioClip(AssetHandle asset, const fs::path& file, int start);
        void DrawAudioClipBar(const ImVec2& min, const ImVec2& max);
        const Dimension::AudioWaveform* ClipWaveform(const Dimension::Music::AudioClip& clip);
        void SetSongMode(bool song);
        void SetPattern(int pattern);
        void HandleKeys();
        // Undo: Record before a change, Changed after it (the player hears it; the file is written once it is done).
        void Record();
        void Changed();
        void Undo();
        void Redo();
        void TogglePlay();
        void StopPlayback();
        // Render... (the window): what (the pattern or the Playlist), the format (WAV, or Ogg Opus at a bit rate), the
        // tail, peak normalizing and the file's name; the file goes next to the song as an audio asset.
        void RenderToFile();
        void DrawRenderWindow();
        float SuggestedTail() const;
        void Preview(int channel, int key, bool on);
        // A knob (vertical drag; double-click resets). Returns true when the value changed; records undo as it starts.
        bool Knob(const char* id, float* value, float min, float max, float reset, float size, const char* format);
        bool Edited(bool changed); // records undo when the last widget starts being used; true when it changed

        EditorContext& m_Ctx;
        bool m_Active = false;
        bool m_Suspended = false;
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
        // Which workspace view fills the middle (F6 Channel Rack, F7 Piano Roll, F5 Playlist), and Pattern / Song mode.
        enum class View { Rack, PianoRoll, Playlist, Mixer };
        View m_View = View::Rack;
        bool m_SongMode = false;
        // Piano Roll: zoom (pixels per tick, per key), scroll (ticks, pixels from the top key), snap (ticks; 1 = none),
        // the length new notes get, the selection (note indices in the pattern) and what the mouse is doing.
        float m_RollTickW = 1.0f, m_RollKeyH = 14.0f;
        double m_RollScrollX = 0.0;
        float m_RollScrollY = -1.0f; // (-1: put the channel's notes in view first)
        int m_RollSnap = 24, m_RollLength = 24;
        std::vector<int> m_RollSelection;
        struct RollDrag
        {
            enum Kind { None, Move, Resize, Box, Erase, Velocity } Kind = None;
            double StartTick = 0.0;
            int StartKey = 60, Anchor = -1, PreviewKey = -1;
            ImVec2 StartMouse{ 0, 0 };
            std::vector<Dimension::Music::Note> Before;
            bool Moved = false;
        } m_RollDrag;
        // Playlist: zoom (pixels per bar, track height), scroll (ticks, pixels), snap (ticks), the selected clip and drag.
        float m_ListBarW = 72.0f, m_ListTrackH = 42.0f;
        double m_ListScrollX = 0.0;
        float m_ListScrollY = 0.0f;
        int m_ListSnap = 96;
        int m_SelectedClip = -1;
        int m_RenamingTrack = -1;
        struct ListDrag
        {
            enum Kind { None, Move, Resize, Erase, Seek } Kind = None;
            int Clip = -1;
            bool Automation = false; // (Clip indexes the automation clips)
            bool Audio = false;      // (Clip indexes the audio clips)
            double Offset = 0.0;
            bool Moved = false;
        } m_ListDrag;
        // Mixer: the selected track and effect slot, meters; the selected automation clip and the point being dragged.
        int m_SelectedMixer = 0, m_SelectedEffect = -1, m_RenamingMixer = -1;
        std::vector<float> m_MixerMeters;
        int m_SelectedAuto = -1, m_AutoDragPoint = -1;
        std::shared_ptr<Dimension::Music::PluginRack> m_Rack;
        std::vector<Dimension::Music::PluginInfo> m_PluginList;
        std::vector<std::string> m_PluginFolders, m_PluginErrors; // (extra folders, beside the system's)
        bool m_PluginListLoaded = false, m_ShowPluginManager = false;
        std::map<uint64_t, std::vector<Dimension::Music::PluginParameter>> m_ParamCache;
        std::map<uint64_t, bool> m_HasEditorCache;
        std::string m_ParamFilter;
        // Recording: REC armed, what it records (0 notes, 1 audio), snapping notes to steps, the metronome; devices.
        bool m_RecordArmed = false, m_RecordQuantize = false, m_Metronome = false, m_ShowIO = false, m_IOLoaded = false;
        int m_RecordSource = 0;
        int m_MidiDevice = -1, m_MicDevice = -1;
        std::string m_MidiDeviceName;
        bool m_Taking = false, m_WasPlaying = false, m_TakeUndone = false;
        Dimension::Music::Edit::NoteRecorder m_NoteRec;
        struct MidiEvent
        {
            Dimension::Music::MidiMessage Message;
        };
        std::mutex m_MidiMutex;
        std::vector<MidiEvent> m_MidiQueue;
        std::atomic<int> m_LiveChannel{ 0 };
        std::vector<int> m_MidiHeld; // keys held on the MIDI keyboard (shown on the piano)
        uint64_t m_MicCursor = 0;
        std::vector<float> m_Take;
        double m_TakeStartTick = 0.0;
        int m_SelectedAudio = -1;
        std::map<std::string, std::shared_ptr<Dimension::AudioWaveform>> m_Waveforms;
        bool m_ShowRender = false, m_RenderWholeSong = false, m_RenderNormalize = true;
        int m_RenderFormat = 0, m_RenderKbps = 160;
        float m_RenderTail = -1.0f; // (-1: suggested from the effects)
        std::string m_RenderName;
        fs::path m_LastRender;
        float m_InputLatencyMs = 0.0f; // audio takes move this much earlier (the time sound takes out and back in)
        std::string m_Status;
        float m_StatusTime = 0.0f;
    };
}
