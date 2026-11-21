#include "MusicMode.h"
#include "EditorUI.h"
#include "MusicModeUI.h"
#include <Dimension/Music/AudioExport.h>
#include <Dimension/Core/Log.h>
#include <Dimension/Asset/AssetManager.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>
#include <algorithm>
#include <cmath>

namespace Editor
{
    using namespace Dimension;
    using namespace Dimension::Music;
    using namespace Editor::MusicUI;

    namespace
    {
        // The typing keyboard (FL's layout): Z row = one octave from C, Q row = the next.
        struct TypedKey
        {
            ImGuiKey Key;
            int Offset;
        };
        const TypedKey kTypedKeys[] = {
            { ImGuiKey_Z, 0 },  { ImGuiKey_S, 1 },  { ImGuiKey_X, 2 },  { ImGuiKey_D, 3 },  { ImGuiKey_C, 4 },  { ImGuiKey_V, 5 },
            { ImGuiKey_G, 6 },  { ImGuiKey_B, 7 },  { ImGuiKey_H, 8 },  { ImGuiKey_N, 9 },  { ImGuiKey_J, 10 }, { ImGuiKey_M, 11 },
            { ImGuiKey_Comma, 12 }, { ImGuiKey_Q, 12 }, { ImGuiKey_2, 13 }, { ImGuiKey_W, 14 }, { ImGuiKey_3, 15 }, { ImGuiKey_E, 16 },
            { ImGuiKey_R, 17 }, { ImGuiKey_5, 18 }, { ImGuiKey_T, 19 }, { ImGuiKey_6, 20 }, { ImGuiKey_Y, 21 }, { ImGuiKey_7, 22 },
            { ImGuiKey_U, 23 }, { ImGuiKey_I, 24 },
        };

        fs::path UniqueSongFile(const fs::path& folder, const std::string& name)
        {
            fs::path p = folder / (name + ".dsong");
            for (int i = 1; fs::exists(p); ++i) p = folder / (name + " " + std::to_string(i) + ".dsong");
            return p;
        }
    }

    MusicMode::~MusicMode()
    {
        if (m_Active) MusicOutput::Stop();
    }

    AssetHandle MusicMode::CreateSongAsset(const fs::path& folder, const std::string& name)
    {
        std::error_code ec;
        fs::create_directories(folder, ec);
        fs::path path = UniqueSongFile(folder, name);
        Song song = Song::Default();
        song.Name = path.stem().string();
        if (!song.Save(path))
        {
            Log::Error("Could not write the song {}", path.string());
            return AssetHandle::None();
        }
        return AssetManager::Import(path);
    }

    bool MusicMode::Open(AssetHandle song)
    {
        fs::path file = AssetManager::GetAbsolutePath(song);
        Song loaded;
        if (file.empty() || !Song::Load(file, loaded)) return false;
        if (m_Active) Close();
        m_Song = std::move(loaded);
        m_Asset = song;
        m_File = file;
        m_Active = true;
        m_Dirty = m_Unsaved = false;
        m_Undo.clear();
        m_Redo.clear();
        m_Pattern = 0;
        m_SelectedChannel = m_Song.Channels.empty() ? -1 : 0;
        m_RenamingChannel = -1;
        m_Meters.assign(m_Song.Channels.size(), 0.0f);
        m_Player = std::make_shared<SongPlayer>(48000);
        // The song's plug-ins, made from their saved states (they live as long as the song uses them).
        m_Rack = std::make_shared<PluginRack>(48000.0, (int)SongPlayer::kChunk);
        m_Rack->Sync(m_Song);
        for (const auto& e : m_Rack->LastErrors()) m_Status = "Plug-in missing: " + e, m_StatusTime = 8.0f;
        m_Player->SetRack(m_Rack);
        m_Player->SetSong(m_Song);
        if (!MusicOutput::Start(m_Player)) m_Status = "No audio output: songs can still be edited and rendered";
        StartMidi();
        Log::Info("Opened song '{}' (Music Mode)", m_Song.Name);
        return true;
    }

    bool MusicMode::Save()
    {
        if (!m_Active) return false;
        if (m_Rack) m_Rack->StoreStates(m_Song); // (the plug-ins' current settings go into the file)
        if (!m_Song.Save(m_File))
        {
            Log::Error("Could not write the song {}", m_File.string());
            return false;
        }
        m_Unsaved = false;
        return true;
    }

    void MusicMode::Close()
    {
        if (!m_Active) return;
        StopMidi(); // (ends a take in progress; no MIDI reaches the player after this)
        m_RecordArmed = false;
        if (m_Unsaved) Save();
        MusicOutput::Stop();
        m_Player.reset();
        if (m_Rack) m_Rack->Clear(); // (closes their editor windows)
        m_Rack.reset();
        m_ParamCache.clear();
        m_HasEditorCache.clear();
        m_Active = false;
        m_TypedNotes.clear();
        m_PianoNote = -1;
    }

    void MusicMode::Record()
    {
        m_Undo.push_back(m_Song);
        if (m_Undo.size() > 200) m_Undo.pop_front();
        m_Redo.clear();
    }

    void MusicMode::Changed()
    {
        m_Dirty = m_Unsaved = true;
        m_Meters.resize(m_Song.Channels.size(), 0.0f);
        if (m_Rack) m_Rack->Sync(m_Song);
        if (m_Player) m_Player->SetSong(m_Song);
    }

    void MusicMode::Undo()
    {
        if (m_Undo.empty()) return;
        m_Redo.push_back(m_Song);
        m_Song = std::move(m_Undo.back());
        m_Undo.pop_back();
        m_SelectedChannel = std::min(m_SelectedChannel, (int)m_Song.Channels.size() - 1);
        m_Pattern = std::clamp(m_Pattern, 0, (int)m_Song.Patterns.size() - 1);
        m_RollSelection.clear();
        m_SelectedClip = -1;
        m_SelectedAuto = -1;
        m_SelectedAudio = -1;
        m_SelectedMixer = std::min(m_SelectedMixer, (int)m_Song.Mixer.size() - 1);
        m_SelectedEffect = -1;
        Changed();
    }

    void MusicMode::Redo()
    {
        if (m_Redo.empty()) return;
        m_Undo.push_back(m_Song);
        m_Song = std::move(m_Redo.back());
        m_Redo.pop_back();
        m_SelectedChannel = std::min(m_SelectedChannel, (int)m_Song.Channels.size() - 1);
        m_Pattern = std::clamp(m_Pattern, 0, (int)m_Song.Patterns.size() - 1);
        m_RollSelection.clear();
        m_SelectedClip = -1;
        m_SelectedAuto = -1;
        m_SelectedAudio = -1;
        m_SelectedMixer = std::min(m_SelectedMixer, (int)m_Song.Mixer.size() - 1);
        m_SelectedEffect = -1;
        Changed();
    }

    bool MusicMode::Edited(bool changed)
    {
        if (ImGui::IsItemActivated()) Record();
        if (changed) Changed();
        return changed;
    }

    void MusicMode::TogglePlay()
    {
        if (!m_Player) return;
        if (m_Player->IsPlaying()) m_Player->Pause();
        else
        {
            if (!m_SongMode) m_Player->SetPattern(m_Pattern);
            m_Player->Play();
        }
    }

    void MusicMode::SetSongMode(bool song)
    {
        if (m_SongMode == song) return;
        m_SongMode = song;
        if (!m_Player) return;
        bool playing = m_Player->IsPlaying();
        m_Player->Stop();
        m_Player->SetMode(song ? SongPlayer::Mode::Song : SongPlayer::Mode::Pattern);
        m_Player->SetPattern(m_Pattern);
        if (playing) m_Player->Play();
    }

    void MusicMode::SetPattern(int pattern)
    {
        pattern = std::clamp(pattern, 0, (int)m_Song.Patterns.size() - 1);
        if (pattern != m_Pattern) m_RollSelection.clear();
        m_Pattern = pattern;
        if (m_Player && !m_SongMode) m_Player->SetPattern(pattern);
    }

    void MusicMode::StopPlayback()
    {
        if (m_Player) m_Player->Stop();
    }

    void MusicMode::Preview(int channel, int key, bool on)
    {
        if (!m_Player || channel < 0 || channel >= (int)m_Song.Channels.size()) return;
        if (on) m_Player->NoteOn(channel, key, 0.8f);
        else m_Player->NoteOff(channel, key);
    }

    float MusicMode::SuggestedTail() const
    {
        // Long enough for delays and reverbs to ring out.
        float tail = 1.5f;
        for (const MixerTrack& m : m_Song.Mixer)
            for (const Effect& e : m.Effects)
                if (e.Enabled && (e.Type == EffectType::Delay || e.Type == EffectType::Reverb || e.Type == EffectType::Plugin)) tail = 4.0f;
        return tail;
    }

    void MusicMode::RenderToFile()
    {
        // The Playlist or the current pattern (once), with its tail, next to the song file as an audio asset the game can
        // play (played back through the engine like any other clip).
        std::string name = m_RenderName.find_first_not_of(' ') == std::string::npos ? m_File.stem().string() : m_RenderName;
        for (char& c : name)
            if (std::string_view("<>:\"/\\|?*").find(c) != std::string_view::npos) c = '_';
        fs::path out = m_File.parent_path() / (name + (m_RenderFormat == 1 ? ".opus" : ".wav"));
        if (m_Rack) m_Rack->StoreStates(m_Song); // (the render makes its own plug-ins from these)
        bool song = m_RenderWholeSong && (!m_Song.Playlist.empty() || !m_Song.AudioClips.empty());
        float tail = m_RenderTail >= 0.0f ? m_RenderTail : SuggestedTail();
        auto samples = RenderSong(m_Song, 48000, song ? SongPlayer::Mode::Song : SongPlayer::Mode::Pattern, m_Pattern, 1, tail);
        if (m_RenderNormalize) Normalize(samples, -1.0f);
        bool ok = m_RenderFormat == 1 ? WriteOpus(out, samples, 48000, m_RenderKbps) : WriteWav(out, samples, 48000);
        if (!ok)
        {
            m_Status = "Could not write " + out.filename().string();
            Log::Error("Could not write {}", out.string());
            return;
        }
        AssetHandle h = AssetManager::Import(out);
        if (h.IsValid()) AssetManager::Refresh();
        m_LastRender = out;
        m_Status = "Rendered " + out.filename().string();
        m_StatusTime = 4.0f;
        Log::Info("Rendered '{}' to {} ({:.1f} s)", m_Song.Name, out.filename().string(), samples.size() / 2 / 48000.0);
    }

    void MusicMode::DrawRenderWindow()
    {
        if (!m_ShowRender) return;
        ImGui::SetNextWindowSize(ImVec2(430.0f, 0.0f), ImGuiCond_Always);
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + 120.0f), ImGuiCond_Appearing, ImVec2(0.5f, 0.0f));
        if (!ImGui::Begin("Render", &m_ShowRender, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse))
        {
            ImGui::End();
            return;
        }
        if (ImGui::IsWindowAppearing())
        {
            if (m_RenderName.empty()) m_RenderName = m_File.stem().string();
            m_RenderWholeSong = m_SongMode && (!m_Song.Playlist.empty() || !m_Song.AudioClips.empty());
        }
        bool hasSong = !m_Song.Playlist.empty() || !m_Song.AudioClips.empty();
        if (UI::BeginProperties("##render"))
        {
            UI::PropertyLabel("Render");
            if (ImGui::RadioButton("Pattern", !m_RenderWholeSong)) m_RenderWholeSong = false;
            ImGui::SetItemTooltip("The current pattern, once");
            ImGui::SameLine();
            ImGui::BeginDisabled(!hasSong);
            if (ImGui::RadioButton("Song", m_RenderWholeSong)) m_RenderWholeSong = true;
            ImGui::EndDisabled();
            ImGui::SetItemTooltip(hasSong ? "The Playlist, start to end" : "The Playlist is empty");
            UI::PropertyLabel("Format", "WAV: 16-bit, large; Opus: compressed (a fraction of the size), what games ship");
            const char* formats[] = { "WAV (16-bit)", "Ogg Opus" };
            ImGui::Combo("##format", &m_RenderFormat, formats, 2);
            if (m_RenderFormat == 1)
            {
                UI::PropertyLabel("Quality");
                ImGui::SliderInt("##kbps", &m_RenderKbps, 64, 320, "%d kbps");
            }
            UI::PropertyLabel("Tail", "Seconds after the end for the sound to ring out (Auto: longer with delays and reverbs)");
            float tail = m_RenderTail >= 0.0f ? m_RenderTail : SuggestedTail();
            bool autoTail = m_RenderTail < 0.0f;
            if (ImGui::Checkbox("Auto##tail", &autoTail)) m_RenderTail = autoTail ? -1.0f : tail;
            ImGui::SameLine();
            ImGui::BeginDisabled(autoTail);
            ImGui::SetNextItemWidth(-1);
            if (ImGui::SliderFloat("##tail", &tail, 0.0f, 10.0f, "%.1f s")) m_RenderTail = tail;
            ImGui::EndDisabled();
            UI::PropertyLabel("Normalize", "Raise or lower it so its loudest moment peaks at -1 dB");
            ImGui::Checkbox("##normalize", &m_RenderNormalize);
            UI::PropertyLabel("File Name");
            ImGui::InputText("##name", &m_RenderName);
            UI::EndProperties();
        }
        double ticks = m_RenderWholeSong ? m_Song.SongTicks() : m_Song.PatternTicks(m_Pattern);
        float seconds = (float)(ticks / m_Song.TicksPerSecond()) + (m_RenderTail >= 0.0f ? m_RenderTail : SuggestedTail());
        ImGui::TextDisabled("%.1f s, next to the song: %s%s", seconds, m_RenderName.c_str(), m_RenderFormat == 1 ? ".opus" : ".wav");
        if (ImGui::Button("Render", ImVec2(120, 0))) RenderToFile();
        if (!m_LastRender.empty())
        {
            ImGui::SameLine();
            ImGui::TextDisabled("Last: %s", m_LastRender.filename().string().c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Show in Project"))
                if (AssetHandle h = AssetManager::GetHandle(m_LastRender); h.IsValid()) m_Ctx.SelectAsset(h), m_Ctx.RequestCloseMusic = true;
        }
        ImGui::End();
    }

    bool MusicMode::Knob(const char* id, float* value, float min, float max, float reset, float size, const char* format)
    {
        ImGui::InvisibleButton(id, ImVec2(size, size));
        bool changed = false;
        if (ImGui::IsItemActivated()) Record();
        if (ImGui::IsItemActive() && ImGui::GetIO().MouseDelta.y != 0.0f)
        {
            float speed = (max - min) / (ImGui::GetIO().KeyShift ? 600.0f : 150.0f);
            float v = std::clamp(*value - ImGui::GetIO().MouseDelta.y * speed, min, max);
            if (v != *value) *value = v, changed = true;
        }
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && *value != reset)
        {
            *value = reset;
            changed = true;
        }
        if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetTooltip(format, *value);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 a = ImGui::GetItemRectMin();
        ImVec2 c(a.x + size * 0.5f, a.y + size * 0.5f);
        float r = size * 0.5f - 1.0f;
        const float start = 0.75f * IM_PI, sweep = 1.5f * IM_PI;
        float t = (*value - min) / std::max(1e-6f, max - min);
        dl->AddCircleFilled(c, r, ImGui::GetColorU32(ImGuiCol_FrameBg));
        dl->PathArcTo(c, r - 1.5f, start, start + sweep, 20);
        dl->PathStroke(ImGui::GetColorU32(ImGuiCol_Border), 0, 2.0f);
        // Bipolar knobs (pan) fill from the middle.
        float from = min < 0.0f && max > 0.0f ? start + sweep * (-min / (max - min)) : start;
        float to = start + sweep * t;
        if (std::fabs(to - from) > 0.01f)
        {
            dl->PathArcTo(c, r - 1.5f, std::min(from, to), std::max(from, to), 20);
            dl->PathStroke(ImGui::GetColorU32(ImGuiCol_SliderGrabActive), 0, 2.5f);
        }
        dl->AddLine(c, ImVec2(c.x + std::cos(to) * (r - 2), c.y + std::sin(to) * (r - 2)), ImGui::GetColorU32(ImGuiCol_Text), 1.5f);
        if (changed) Changed();
        return changed;
    }

    // ---- The window ----

    void MusicMode::OnImGui()
    {
        if (!m_Active) return;
        // Meters fall back between peaks.
        if (m_Player)
        {
            m_Meters.resize(m_Song.Channels.size(), 0.0f);
            for (size_t i = 0; i < m_Meters.size(); ++i) m_Meters[i] = std::max(m_Player->TakeChannelPeak((int)i), m_Meters[i] * 0.88f);
            auto m = m_Player->TakeMasterPeak();
            for (int i = 0; i < 2; ++i) m_MasterMeter[i] = std::max(m[i], m_MasterMeter[i] * 0.88f);
        }
        m_StatusTime = std::max(0.0f, m_StatusTime - ImGui::GetIO().DeltaTime);
        if (m_Rack && m_Rack->TakeEdited()) m_Dirty = m_Unsaved = true; // (a plug-in's own editor changed it)
        UpdateRecording();

        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::SetNextWindowViewport(vp->ID);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                 ImGuiWindowFlags_NoSavedSettings;
        ImGui::Begin("Music Mode", nullptr, flags);
        ImGui::PopStyleVar(2);
        DrawTopBar();
        ImGui::Separator();
        ImVec2 avail = ImGui::GetContentRegionAvail();
        const ImVec2 spacing = ImGui::GetStyle().ItemSpacing;
        float keyboardHeight = m_View == View::Rack ? 92.0f : 0.0f;
        float settingsWidth = m_View == View::Playlist || m_View == View::Mixer ? 0.0f : std::clamp(avail.x * 0.26f, 260.0f, 400.0f);
        ImVec2 main(avail.x - (settingsWidth > 0.0f ? settingsWidth + spacing.x : 0.0f), avail.y - (keyboardHeight > 0.0f ? keyboardHeight + spacing.y : 0.0f));
        switch (m_View)
        {
        case View::Rack: DrawChannelRack(main); break;
        case View::PianoRoll: DrawPianoRoll(main); break;
        case View::Playlist: DrawPlaylist(main); break;
        case View::Mixer: DrawMixer(main); break;
        }
        if (settingsWidth > 0.0f)
        {
            ImGui::SameLine();
            DrawChannelSettings(ImVec2(settingsWidth, main.y));
        }
        if (keyboardHeight > 0.0f) DrawKeyboard(ImVec2(avail.x, keyboardHeight));
        HandleKeys();
        // Status messages: a note at the bottom right, fading out (the missing audio output one stays).
        if (!m_Status.empty() && (m_StatusTime > 0.0f || m_Status.rfind("No audio", 0) == 0))
        {
            float alpha = m_Status.rfind("No audio", 0) == 0 ? 1.0f : std::min(1.0f, m_StatusTime);
            ImVec2 size = ImGui::CalcTextSize(m_Status.c_str());
            ImVec2 max(ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - 14.0f, ImGui::GetWindowPos().y + ImGui::GetWindowHeight() - 14.0f);
            ImVec2 min(max.x - size.x - 16.0f, max.y - size.y - 10.0f);
            ImDrawList* dl = ImGui::GetForegroundDrawList();
            dl->AddRectFilled(min, max, IM_COL32(20, 22, 26, (int)(230 * alpha)), 4.0f);
            dl->AddText(ImVec2(min.x + 8, min.y + 5), IM_COL32(230, 230, 230, (int)(255 * alpha)), m_Status.c_str());
        }
        ImGui::End();
        DrawPluginManager();
        DrawIOWindow();
        DrawRenderWindow();

        // The file follows the song once an edit is finished.
        if (m_Unsaved && m_Dirty && !ImGui::IsAnyItemActive() && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            m_Dirty = false;
            Save();
        }
    }

    void MusicMode::DrawTopBar()
    {
        ImGuiStyle& style = ImGui::GetStyle();
        if (ImGui::Button("< Editor")) m_Ctx.RequestCloseMusic = true; // (closed by the editor layer, outside this window)
        ImGui::SetItemTooltip("Back to the editor (the song is saved)");
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(UI::AccentText(), "%s%s", m_Song.Name.c_str(), m_Unsaved ? "*" : "");
        ImGui::SameLine(0.0f, 24.0f);

        // Views (FL's F5/F6/F7).
        auto toggle = [](const char* label, bool on) {
            if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            bool pressed = ImGui::Button(label);
            if (on) ImGui::PopStyleColor();
            return pressed;
        };
        if (toggle("Channel Rack", m_View == View::Rack)) m_View = View::Rack;
        ImGui::SetItemTooltip("F6");
        ImGui::SameLine(0.0f, 2.0f);
        if (toggle("Piano Roll", m_View == View::PianoRoll)) m_View = View::PianoRoll;
        ImGui::SetItemTooltip("F7: the selected channel's notes");
        ImGui::SameLine(0.0f, 2.0f);
        if (toggle("Playlist", m_View == View::Playlist)) m_View = View::Playlist;
        ImGui::SetItemTooltip("F5: the song's arrangement");
        ImGui::SameLine(0.0f, 2.0f);
        if (toggle("Mixer", m_View == View::Mixer)) m_View = View::Mixer;
        ImGui::SetItemTooltip("F9: tracks, routing and effects");
        ImGui::SameLine(0.0f, 24.0f);
        // Pattern or Song mode: what Play plays.
        if (toggle("PAT", !m_SongMode)) SetSongMode(false);
        ImGui::SetItemTooltip("Pattern mode: Play loops the current pattern");
        ImGui::SameLine(0.0f, 2.0f);
        if (toggle("SONG", m_SongMode)) SetSongMode(true);
        ImGui::SetItemTooltip("Song mode: Play plays the Playlist (Render WAV renders it)");
        ImGui::SameLine(0.0f, 12.0f);

        // Transport.
        bool playing = m_Player && m_Player->IsPlaying();
        if (playing) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button(playing ? "Pause" : "Play", ImVec2(56, 0))) TogglePlay();
        if (playing) ImGui::PopStyleColor();
        ImGui::SetItemTooltip(m_SongMode ? "Play the song (Space)" : "Play the pattern (Space)");
        ImGui::SameLine();
        if (ImGui::Button("Stop", ImVec2(48, 0))) StopPlayback();
        ImGui::SameLine();
        {
            bool armed = m_RecordArmed;
            if (armed) ImGui::PushStyleColor(ImGuiCol_Button, m_Taking ? ImVec4(0.85f, 0.15f, 0.15f, 1.0f) : ImVec4(0.6f, 0.15f, 0.15f, 1.0f));
            if (ImGui::Button("REC"))
            {
                m_RecordArmed = !m_RecordArmed;
                if (m_RecordArmed && m_RecordSource == 1 && !m_SongMode) SetSongMode(true); // (audio takes go on the Playlist)
            }
            if (armed) ImGui::PopStyleColor();
            ImGui::SetItemTooltip(m_RecordSource == 0 ? "Record notes: with REC on, Play and play the selected channel (MIDI keyboard, typing keys, piano)"
                                                      : "Record audio: with REC on, Play (Song mode) records the microphone onto the Playlist");
            ImGui::SameLine(0.0f, 2.0f);
            bool click = m_Metronome;
            if (click) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            if (ImGui::Button("Click"))
            {
                m_Metronome = !m_Metronome;
                if (m_Player) m_Player->SetMetronome(m_Metronome);
                SaveIOSettings();
            }
            if (click) ImGui::PopStyleColor();
            ImGui::SetItemTooltip("Metronome (never in renders)");
            ImGui::SameLine(0.0f, 2.0f);
            if (ImGui::Button("I/O")) m_ShowIO = !m_ShowIO;
            ImGui::SetItemTooltip("Audio & MIDI: the MIDI keyboard, the microphone, what REC records");
        }
        ImGui::SameLine();
        // Position: bar:beat:step.
        {
            double tick = m_Player ? m_Player->GetTick() : 0.0;
            int bar = (int)(tick / m_Song.BarTicks());
            int beat = (int)(std::fmod(tick, m_Song.BarTicks()) / kTicksPerBeat);
            int step = (int)(std::fmod(tick, kTicksPerBeat) / m_Song.StepTicks());
            ImGui::AlignTextToFramePadding();
            ImGui::Text("%d:%d:%d", bar + 1, beat + 1, step + 1);
        }
        ImGui::SameLine(0.0f, 16.0f);
        ImGui::SetNextItemWidth(78.0f);
        Edited(ImGui::DragFloat("##tempo", &m_Song.Tempo, 0.2f, 10.0f, 999.0f, "%.1f BPM"));
        if (ImGui::IsItemDeactivatedAfterEdit()) m_Song.Tempo = std::clamp(m_Song.Tempo, 10.0f, 999.0f);
        ImGui::SetItemTooltip("Tempo: beats per minute (drag, or double-click to type)");
        ImGui::SameLine(0.0f, 16.0f);

        // Patterns.
        ImGui::SetNextItemWidth(110.0f);
        if (ImGui::BeginCombo("##pattern", m_Song.Patterns[m_Pattern].Name.c_str()))
        {
            for (int i = 0; i < (int)m_Song.Patterns.size(); ++i)
                if (ImGui::Selectable(m_Song.Patterns[i].Name.c_str(), i == m_Pattern)) SetPattern(i);
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Button("+##pattern"))
        {
            Record();
            Pattern p;
            p.Name = "Pattern " + std::to_string(m_Song.Patterns.size() + 1);
            p.Bars = m_Song.Patterns[m_Pattern].Bars;
            m_Song.Patterns.push_back(p);
            Changed();
            SetPattern((int)m_Song.Patterns.size() - 1);
        }
        ImGui::SetItemTooltip("A new pattern");
        ImGui::SameLine();
        if (ImGui::Button("...##patternMenu")) ImGui::OpenPopup("PatternMenu");
        if (ImGui::BeginPopup("PatternMenu"))
        {
            ImGui::SetNextItemWidth(160.0f);
            std::string name = m_Song.Patterns[m_Pattern].Name;
            if (ImGui::InputText("Name", &name)) Edited(true), m_Song.Patterns[m_Pattern].Name = name;
            else Edited(false);
            if (ImGui::MenuItem("Clone"))
            {
                Record();
                Pattern p = m_Song.Patterns[m_Pattern];
                p.Name += " copy";
                m_Song.Patterns.insert(m_Song.Patterns.begin() + m_Pattern + 1, p);
                ++m_Pattern;
                Changed();
            }
            if (ImGui::MenuItem("Clear"))
            {
                Record();
                m_Song.Patterns[m_Pattern].Notes.clear();
                Changed();
            }
            if (ImGui::MenuItem("Delete", nullptr, false, m_Song.Patterns.size() > 1))
            {
                Record();
                m_Song.Patterns.erase(m_Song.Patterns.begin() + m_Pattern);
                // Playlist clips of later patterns move down; this one's go.
                std::erase_if(m_Song.Playlist, [&](const PlaylistClip& c) { return c.Pattern == m_Pattern; });
                for (auto& c : m_Song.Playlist)
                    if (c.Pattern > m_Pattern) --c.Pattern;
                m_Pattern = std::max(0, m_Pattern - 1);
                Changed();
            }
            ImGui::EndPopup();
        }
        ImGui::SameLine(0.0f, 24.0f);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Swing");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70.0f);
        float swing = m_Song.Swing * 100.0f;
        if (Edited(ImGui::SliderFloat("##swing", &swing, 0.0f, 100.0f, "%.0f%%"))) m_Song.Swing = swing / 100.0f, Changed();

        // Right: master level and meter, Render, undo.
        // (on a narrow window the rest goes on a second row)
        float right = 330.0f;
        float after = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x + 20.0f; // (just past the swing slider)
        if (after + right > ImGui::GetWindowWidth()) ImGui::SetCursorPosX(ImGui::GetWindowWidth() - right);
        else ImGui::SameLine(ImGui::GetWindowWidth() - right);
        Knob("##master", &m_Song.Master().Volume, 0.0f, 1.25f, 0.8f, ImGui::GetFrameHeight(), "Master %.2f");
        ImGui::SameLine();
        {
            ImVec2 p = ImGui::GetCursorScreenPos();
            float h = ImGui::GetFrameHeight(), w = 64.0f;
            ImGui::Dummy(ImVec2(w, h));
            ImDrawList* dl = ImGui::GetWindowDrawList();
            for (int i = 0; i < 2; ++i)
            {
                ImVec2 a(p.x, p.y + 3 + i * (h - 6) * 0.5f), b(p.x + w, p.y + 3 + (i + 1) * (h - 6) * 0.5f - 1);
                dl->AddRectFilled(a, b, ImGui::GetColorU32(ImGuiCol_FrameBg));
                float level = std::min(1.0f, m_MasterMeter[i]);
                ImU32 col = m_MasterMeter[i] > 1.0f ? IM_COL32(235, 70, 60, 255) : level > 0.7f ? IM_COL32(230, 200, 60, 255) : IM_COL32(90, 200, 110, 255);
                dl->AddRectFilled(a, ImVec2(a.x + w * level, b.y), col);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Render...")) m_ShowRender = !m_ShowRender;
        ImGui::SetItemTooltip("Render the pattern or the song to WAV or Ogg Opus next to the song (an audio clip the game can play)");
        ImGui::SameLine();
        ImGui::BeginDisabled(m_Undo.empty());
        if (ImGui::Button("Undo")) Undo();
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(m_Redo.empty());
        if (ImGui::Button("Redo")) Redo();
        ImGui::EndDisabled();
        (void)style;
    }

    void MusicMode::DrawChannelRack(const ImVec2& size)
    {
        ImGui::BeginChild("##ChannelRack", size, ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
        Pattern& pattern = m_Song.Patterns[m_Pattern];
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Channel Rack");
        ImGui::SameLine(0.0f, 24.0f);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Bars");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80.0f);
        int bars = pattern.Bars;
        if (Edited(ImGui::InputInt("##bars", &bars)))
        {
            pattern.Bars = std::clamp(bars, 1, 16);
            Changed();
        }
        ImGui::SameLine(0.0f, 24.0f);
        ImGui::TextDisabled("Click steps to place hits (drag to paint); the name auditions the channel.");
        ImGui::Spacing();

        const int stepsPerBar = m_Song.BeatsPerBar * m_Song.StepsPerBeat;
        const int steps = pattern.Bars * stepsPerBar;
        const float rowH = 26.0f, stepW = 20.0f, stepH = 24.0f;
        int playStep = -1;
        if (m_Player && m_Player->IsPlaying() && !m_SongMode && m_Player->GetPattern() == m_Pattern) playStep = (int)(m_Player->GetTick() / m_Song.StepTicks());
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) m_PaintChannel = -1;

        int removeChannel = -1, cloneChannel = -1, moveUp = -1;
        for (int ci = 0; ci < (int)m_Song.Channels.size(); ++ci)
        {
            Channel& ch = m_Song.Channels[ci];
            ImGui::PushID(ci);
            ImU32 color = ChannelColor(ch);
            // Mute light (Ctrl+click: solo).
            {
                ImVec2 p = ImGui::GetCursorScreenPos();
                if (ImGui::InvisibleButton("##mute", ImVec2(14, rowH)))
                {
                    Record();
                    if (ImGui::GetIO().KeyCtrl)
                    {
                        bool only = ch.Solo;
                        for (auto& c : m_Song.Channels) c.Solo = false;
                        ch.Solo = !only;
                    }
                    else ch.Mute = !ch.Mute;
                    Changed();
                }
                ImGui::SetItemTooltip(ch.Solo ? "Solo (Ctrl+click: unsolo)" : "Mute (click) / solo (Ctrl+click)");
                bool lit = !ch.Mute;
                ImU32 led = ch.Solo ? IM_COL32(240, 200, 60, 255) : lit ? IM_COL32(110, 220, 110, 255) : IM_COL32(70, 70, 70, 255);
                ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + 7, p.y + rowH * 0.5f), 5.0f, led);
            }
            ImGui::SameLine();
            Knob("##pan", &ch.Pan, -1.0f, 1.0f, 0.0f, 22.0f, "Pan %.2f");
            AutomationMenu({ ParamKind::ChannelPan, ci });
            ImGui::SameLine();
            Knob("##vol", &ch.Volume, 0.0f, 1.25f, 0.78f, 22.0f, "Volume %.2f");
            AutomationMenu({ ParamKind::ChannelVolume, ci });
            ImGui::SameLine();
            // Level meter.
            {
                ImVec2 p = ImGui::GetCursorScreenPos();
                ImGui::Dummy(ImVec2(5, rowH));
                float level = ci < (int)m_Meters.size() ? std::min(1.0f, m_Meters[ci]) : 0.0f;
                ImDrawList* dl = ImGui::GetWindowDrawList();
                dl->AddRectFilled(ImVec2(p.x, p.y + 2), ImVec2(p.x + 5, p.y + rowH - 2), ImGui::GetColorU32(ImGuiCol_FrameBg));
                dl->AddRectFilled(ImVec2(p.x, p.y + rowH - 2 - (rowH - 4) * level), ImVec2(p.x + 5, p.y + rowH - 2), IM_COL32(90, 200, 110, 255));
            }
            ImGui::SameLine();
            // Name: click selects and auditions; double-click renames; right-click for more.
            if (m_RenamingChannel == ci)
            {
                ImGui::SetNextItemWidth(120.0f);
                ImGui::SetKeyboardFocusHere();
                if (ImGui::InputText("##rename", &m_RenameBuffer, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll) ||
                    (!ImGui::IsItemActive() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)))
                {
                    if (!m_RenameBuffer.empty() && m_RenameBuffer != ch.Name)
                    {
                        Record();
                        ch.Name = m_RenameBuffer;
                        Changed();
                    }
                    m_RenamingChannel = -1;
                }
            }
            else
            {
                bool selected = ci == m_SelectedChannel;
                ImGui::PushStyleColor(ImGuiCol_Button, selected ? Shade(color, 0.75f) : Shade(color, 0.4f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Shade(color, 0.85f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, color);
                ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(245, 245, 245, 255));
                ImGui::Button(ch.Name.c_str(), ImVec2(120.0f, rowH));
                ImGui::PopStyleColor(4);
                if (ImGui::IsItemActivated())
                {
                    m_SelectedChannel = ci;
                    Preview(ci, ch.Key, true);
                }
                if (ImGui::IsItemDeactivated()) Preview(ci, ch.Key, false);
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                {
                    m_RenamingChannel = ci;
                    m_RenameBuffer = ch.Name;
                }
                if (ImGui::BeginPopupContextItem("##channelMenu"))
                {
                    m_SelectedChannel = ci;
                    if (ImGui::MenuItem("Rename")) m_RenamingChannel = ci, m_RenameBuffer = ch.Name;
                    if (ImGui::MenuItem("Clone")) cloneChannel = ci;
                    if (ImGui::MenuItem("Move Up", nullptr, false, ci > 0)) moveUp = ci;
                    if (ImGui::MenuItem("Delete", nullptr, false, m_Song.Channels.size() > 1)) removeChannel = ci;
                    ImGui::Separator();
                    if (ImGui::MenuItem("Clear Steps in This Pattern"))
                    {
                        Record();
                        std::erase_if(pattern.Notes, [&](const Note& n) { return n.Channel == ci; });
                        Changed();
                    }
                    if (ImGui::MenuItem("Fill Each 2 Steps")) { Record(); for (int s = 0; s < steps; s += 2) if (!m_Song.HasStep(m_Pattern, ci, s)) m_Song.ToggleStep(m_Pattern, ci, s); Changed(); }
                    if (ImGui::MenuItem("Fill Each 4 Steps")) { Record(); for (int s = 0; s < steps; s += 4) if (!m_Song.HasStep(m_Pattern, ci, s)) m_Song.ToggleStep(m_Pattern, ci, s); Changed(); }
                    ImGui::EndPopup();
                }
            }
            ImGui::SameLine(0.0f, 10.0f);
            // Steps: groups of a beat, alternating shades (FL's red/grey groups).
            ImDrawList* dl = ImGui::GetWindowDrawList();
            for (int s = 0; s < steps; ++s)
            {
                if (s > 0) ImGui::SameLine(0.0f, s % m_Song.StepsPerBeat == 0 ? 6.0f : 2.0f);
                ImGui::PushID(s);
                ImVec2 p = ImGui::GetCursorScreenPos();
                ImGui::InvisibleButton("##step", ImVec2(stepW, stepH));
                bool on = m_Song.HasStep(m_Pattern, ci, s);
                if (ImGui::IsItemActivated())
                {
                    Record();
                    m_PaintChannel = ci;
                    m_PaintOn = !on;
                    m_Song.ToggleStep(m_Pattern, ci, s);
                    on = !on;
                    Changed();
                    if (on && !(m_Player && m_Player->IsPlaying())) Preview(ci, ch.Key, true), Preview(ci, ch.Key, false);
                }
                else if (m_PaintChannel == ci && ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
                         ImGui::IsMouseHoveringRect(p, ImVec2(p.x + stepW, p.y + stepH)) && on != m_PaintOn)
                {
                    m_Song.ToggleStep(m_Pattern, ci, s);
                    on = !on;
                    Changed();
                }
                bool evenBeat = (s / m_Song.StepsPerBeat) % 2 == 0;
                ImU32 off = evenBeat ? IM_COL32(72, 74, 80, 255) : IM_COL32(96, 62, 62, 255);
                ImU32 fill = on ? color : off;
                if (ImGui::IsItemHovered()) fill = Shade(fill, 1.25f);
                dl->AddRectFilled(p, ImVec2(p.x + stepW, p.y + stepH), fill, 3.0f);
                if (on) dl->AddRectFilled(ImVec2(p.x + 3, p.y + 3), ImVec2(p.x + stepW - 3, p.y + 7), Shade(color, 1.35f), 2.0f);
                if (s == playStep) dl->AddRect(p, ImVec2(p.x + stepW, p.y + stepH), IM_COL32(255, 255, 255, 220), 3.0f, 0, 2.0f);
                ImGui::PopID();
            }
            ImGui::PopID();
        }

        if (cloneChannel >= 0)
        {
            Record();
            if (m_Rack) m_Rack->StoreStates(m_Song);
            Channel c = m_Song.Channels[cloneChannel];
            c.Name += " 2";
            if (c.Type == InstrumentType::Plugin) c.Plugin.Id = NewPluginId();
            m_Song.InsertChannel(cloneChannel + 1, c);
            m_SelectedChannel = cloneChannel + 1;
            Changed();
        }
        if (moveUp > 0)
        {
            Record();
            m_Song.SwapChannels(moveUp, moveUp - 1);
            m_SelectedChannel = moveUp - 1;
            Changed();
        }
        if (removeChannel >= 0)
        {
            Record();
            m_Song.RemoveChannel(removeChannel);
            m_SelectedChannel = std::min(m_SelectedChannel, (int)m_Song.Channels.size() - 1);
            Changed();
        }

        // Adding channels.
        ImGui::Spacing();
        if (ImGui::Button("+ Channel")) ImGui::OpenPopup("AddChannel");
        if (ImGui::BeginPopup("AddChannel"))
        {
            auto add = [&](Channel c) {
                Record();
                m_Song.Channels.push_back(std::move(c));
                m_SelectedChannel = (int)m_Song.Channels.size() - 1;
                Changed();
            };
            if (ImGui::BeginMenu("Drum"))
            {
                for (int k = 0; k < (int)DrumKind::Count; ++k)
                    if (ImGui::MenuItem(DrumKindName((DrumKind)k)))
                    {
                        Channel c;
                        c.Name = DrumKindName((DrumKind)k);
                        c.Type = InstrumentType::Drum;
                        c.Drum.Kind = (DrumKind)k;
                        add(c);
                    }
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem("Synth"))
            {
                Channel c;
                c.Name = "Synth";
                c.Type = InstrumentType::Synth;
                c.Volume = 0.6f;
                add(c);
            }
            if (ImGui::BeginMenu("Plug-in"))
            {
                PluginRef picked;
                if (PluginMenu(true, picked))
                {
                    Channel c;
                    c.Name = picked.Name;
                    c.Type = InstrumentType::Plugin;
                    c.Plugin = picked;
                    c.Volume = 0.8f;
                    add(c);
                    if (m_Rack && !m_Rack->Get(picked.Id)) m_Status = "The plug-in would not load", m_StatusTime = 6.0f;
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Sampler"))
            {
                auto clips = AssetManager::GetAssetsOfType(AssetType::Audio);
                for (AssetHandle h : clips)
                    if (ImGui::MenuItem(AssetManager::GetDisplayName(h).c_str()))
                    {
                        Channel c;
                        c.Name = AssetManager::GetDisplayName(h);
                        c.Type = InstrumentType::Sampler;
                        c.Sampler.Sample = (uint64_t)h;
                        if (auto* meta = AssetManager::GetMetadata(h)) c.Sampler.SamplePath = meta->Path.generic_string();
                        add(c);
                    }
                if (clips.empty()) ImGui::TextDisabled("No audio files in the project");
                ImGui::EndMenu();
            }
            ImGui::EndPopup();
        }
        ImGui::EndChild();
    }

    void MusicMode::DrawChannelSettings(const ImVec2& size)
    {
        ImGui::BeginChild("##ChannelSettings", size, ImGuiChildFlags_Borders);
        if (m_SelectedChannel < 0 || m_SelectedChannel >= (int)m_Song.Channels.size())
        {
            ImGui::TextDisabled("Select a channel");
            ImGui::EndChild();
            return;
        }
        Channel& ch = m_Song.Channels[m_SelectedChannel];
        ImGui::PushID(m_SelectedChannel);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ChannelColor(ch)), "%s", ch.Name.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", InstrumentTypeName(ch.Type));
        ImGui::Separator();
        auto slider = [&](const char* label, float* v, float min, float max, const char* fmt, ImGuiSliderFlags f = 0) {
            UI::PropertyLabel(label);
            return Edited(ImGui::SliderFloat((std::string("##") + label).c_str(), v, min, max, fmt, f));
        };
        auto envelope = [&](const char* label, Envelope& e) {
            ImGui::PushID(label);
            ImGui::SeparatorText(label);
            if (UI::BeginProperties("##env"))
            {
                slider("Attack", &e.Attack, 0.0f, 2.0f, "%.3f s", ImGuiSliderFlags_Logarithmic);
                slider("Decay", &e.Decay, 0.0f, 4.0f, "%.3f s", ImGuiSliderFlags_Logarithmic);
                slider("Sustain", &e.Sustain, 0.0f, 1.0f, "%.2f");
                slider("Release", &e.Release, 0.0f, 4.0f, "%.3f s", ImGuiSliderFlags_Logarithmic);
                UI::EndProperties();
            }
            ImGui::PopID();
        };
        if (UI::BeginProperties("##channel"))
        {
            UI::PropertyLabel("Name");
            std::string name = ch.Name;
            if (Edited(ImGui::InputText("##name", &name))) ch.Name = name, Changed();
            UI::PropertyLabel("Instrument");
            int type = (int)ch.Type;
            const char* types[] = { "Drum", "Synth", "Sampler", "Plug-in" };
            int typeCount = ch.Type == InstrumentType::Plugin || ch.Plugin.Valid() ? 4 : 3; // (plug-ins are added from + Channel)
            if (Edited(ImGui::Combo("##type", &type, types, typeCount)))
            {
                ch.Type = (InstrumentType)type;
                Changed();
            }
            slider("Volume", &ch.Volume, 0.0f, 1.25f, "%.2f");
            AutomationMenu({ ParamKind::ChannelVolume, m_SelectedChannel });
            slider("Pan", &ch.Pan, -1.0f, 1.0f, "%.2f");
            AutomationMenu({ ParamKind::ChannelPan, m_SelectedChannel });
            UI::PropertyLabel("Mixer Track", "The mixer track the channel plays through (its effects, level and routing)");
            if (ImGui::BeginCombo("##insert", m_Song.MixerTrackName(ch.Insert).c_str()))
            {
                for (int t = 0; t < (int)m_Song.Mixer.size(); ++t)
                    if (ImGui::Selectable(m_Song.MixerTrackName(t).c_str(), ch.Insert == t))
                    {
                        Record();
                        ch.Insert = t;
                        Changed();
                    }
                if ((int)m_Song.Mixer.size() < SongPlayer::kMaxMixer && ImGui::Selectable("New Insert"))
                {
                    Record();
                    MixerTrack m;
                    m.Name = ch.Name;
                    m_Song.Mixer.push_back(m);
                    ch.Insert = (int)m_Song.Mixer.size() - 1;
                    Changed();
                }
                ImGui::EndCombo();
            }
            UI::PropertyLabel("Step Key", "The note the step sequencer plays");
            int key = ch.Key;
            if (Edited(ImGui::SliderInt("##key", &key, 0, 127, NoteName(key).c_str()))) ch.Key = key, Changed();
            UI::EndProperties();
        }

        switch (ch.Type)
        {
        case InstrumentType::Plugin:
        {
            ImGui::SeparatorText("Plug-in");
            DrawPluginPanel(ch.Plugin, m_SelectedChannel, -1, ImGui::GetContentRegionAvail().y);
            break;
        }
        case InstrumentType::Drum:
        {
            ImGui::SeparatorText("Drum");
            if (UI::BeginProperties("##drum"))
            {
                UI::PropertyLabel("Sound");
                int kind = (int)ch.Drum.Kind;
                if (ImGui::BeginCombo("##kind", DrumKindName(ch.Drum.Kind)))
                {
                    for (int k = 0; k < (int)DrumKind::Count; ++k)
                        if (ImGui::Selectable(DrumKindName((DrumKind)k), k == kind))
                        {
                            Record();
                            ch.Drum.Kind = (DrumKind)k;
                            Changed();
                        }
                    ImGui::EndCombo();
                }
                slider("Tune", &ch.Drum.Tune, -24.0f, 24.0f, "%.1f st");
                slider("Decay", &ch.Drum.Decay, 0.1f, 3.0f, "%.2fx");
                slider("Tone", &ch.Drum.Tone, 0.0f, 1.0f, "%.2f");
                UI::EndProperties();
            }
            break;
        }
        case InstrumentType::Synth:
        {
            SynthParams& s = ch.Synth;
            ImGui::SeparatorText("Oscillators");
            if (UI::BeginProperties("##osc"))
            {
                auto wave = [&](const char* label, Waveform& w) {
                    UI::PropertyLabel(label);
                    if (ImGui::BeginCombo((std::string("##") + label).c_str(), WaveformName(w)))
                    {
                        for (int i = 0; i < (int)Waveform::Count; ++i)
                            if (ImGui::Selectable(WaveformName((Waveform)i), (int)w == i))
                            {
                                Record();
                                w = (Waveform)i;
                                Changed();
                            }
                        ImGui::EndCombo();
                    }
                };
                wave("Osc 1", s.Wave1);
                wave("Osc 2", s.Wave2);
                slider("Mix", &s.Mix, 0.0f, 1.0f, "%.2f");
                UI::PropertyLabel("Osc 2 Octave");
                Edited(ImGui::SliderInt("##oct2", &s.Octave2, -3, 3));
                slider("Osc 2 Detune", &s.Detune, -1.0f, 1.0f, "%.2f st");
                slider("Pulse Width", &s.PulseWidth, 0.05f, 0.95f, "%.2f");
                slider("Glide", &s.Glide, 0.0f, 1.0f, "%.3f s", ImGuiSliderFlags_Logarithmic);
                UI::EndProperties();
            }
            ImGui::SeparatorText("Filter");
            if (UI::BeginProperties("##filter"))
            {
                slider("Cutoff", &s.Cutoff, 20.0f, 20000.0f, "%.0f Hz", ImGuiSliderFlags_Logarithmic);
                AutomationMenu({ ParamKind::SynthCutoff, m_SelectedChannel });
                slider("Resonance", &s.Resonance, 0.0f, 1.0f, "%.2f");
                AutomationMenu({ ParamKind::SynthResonance, m_SelectedChannel });
                slider("Envelope", &s.FilterEnv, 0.0f, 1.0f, "%.2f");
                UI::EndProperties();
            }
            envelope("Amp Envelope", s.Amp);
            envelope("Filter Envelope", s.Filter);
            break;
        }
        case InstrumentType::Sampler:
        {
            SamplerParams& s = ch.Sampler;
            ImGui::SeparatorText("Sampler");
            if (UI::BeginProperties("##sampler"))
            {
                UI::PropertyLabel("Sample");
                std::string current = s.Sample && AssetManager::GetMetadata(AssetHandle(s.Sample)) ? AssetManager::GetDisplayName(AssetHandle(s.Sample))
                                    : s.SamplePath.empty() ? std::string("None") : fs::path(s.SamplePath).stem().string();
                if (ImGui::BeginCombo("##sample", current.c_str()))
                {
                    for (AssetHandle h : AssetManager::GetAssetsOfType(AssetType::Audio))
                        if (ImGui::Selectable(AssetManager::GetDisplayName(h).c_str(), (uint64_t)h == s.Sample))
                        {
                            Record();
                            s.Sample = (uint64_t)h;
                            if (auto* meta = AssetManager::GetMetadata(h)) s.SamplePath = meta->Path.generic_string();
                            Changed();
                        }
                    ImGui::EndCombo();
                }
                if (ImGui::BeginDragDropTarget())
                {
                    if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(UI::kAssetPayload))
                    {
                        AssetHandle h(*(const uint64_t*)p->Data);
                        if (auto* meta = AssetManager::GetMetadata(h); meta && meta->Type == AssetType::Audio)
                        {
                            Record();
                            s.Sample = (uint64_t)h;
                            s.SamplePath = meta->Path.generic_string();
                            Changed();
                        }
                    }
                    ImGui::EndDragDropTarget();
                }
                UI::PropertyLabel("Root Key", "The key that plays the sample at its own pitch");
                int root = s.RootKey;
                if (Edited(ImGui::SliderInt("##root", &root, 0, 127, NoteName(root).c_str()))) s.RootKey = root, Changed();
                UI::PropertyLabel("One Shot", "Play to the end whatever the note's length (drum hits)");
                if (Edited(ImGui::Checkbox("##oneShot", &s.OneShot))) Changed();
                UI::PropertyLabel("Reverse");
                if (Edited(ImGui::Checkbox("##reverse", &s.Reverse))) Changed();
                UI::EndProperties();
            }
            if (!ResolveSamplePath(s).empty() && !LoadSample(ResolveSamplePath(s))) ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "The sample cannot be read");
            envelope("Amp Envelope", s.Amp);
            break;
        }
        }
        ImGui::PopID();
        ImGui::EndChild();
    }

    void MusicMode::DrawKeyboard(const ImVec2& size)
    {
        // A piano across the bottom for the selected channel (the typing keyboard plays it too).
        ImGui::BeginChild("##Keyboard", size, ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
        ImGui::TextDisabled("Keyboard: %s  |  typing keys Z-M and Q-I play octave %d (Page Up/Down change it), Space plays, Ctrl+Z undoes",
                            m_SelectedChannel >= 0 && m_SelectedChannel < (int)m_Song.Channels.size() ? m_Song.Channels[m_SelectedChannel].Name.c_str() : "-", m_TypingOctave);
        ImVec2 origin = ImGui::GetCursorScreenPos();
        ImVec2 avail = ImGui::GetContentRegionAvail();
        const int first = 36, last = 96; // C3..C8 in FL's naming
        int whites = 0;
        for (int k = first; k <= last; ++k) whites += !IsBlackKey(k);
        float whiteW = std::max(10.0f, avail.x / whites), whiteH = avail.y - 2.0f, blackW = whiteW * 0.6f, blackH = whiteH * 0.6f;
        ImGui::InvisibleButton("##piano", ImVec2(whiteW * whites, whiteH));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        auto held = [&](int key) {
            if (key == m_PianoNote || std::find(m_MidiHeld.begin(), m_MidiHeld.end(), key) != m_MidiHeld.end()) return true;
            for (auto& t : m_TypedNotes)
                if (t.second == key) return true;
            return false;
        };
        // Which key the mouse is on (black keys first: they lie over the white ones).
        ImVec2 mouse = ImGui::GetIO().MousePos;
        int hover = -1;
        std::vector<std::pair<int, ImRect>> blacks, whiteRects;
        int w = 0;
        for (int k = first; k <= last; ++k)
        {
            if (IsBlackKey(k))
            {
                float x = origin.x + w * whiteW - blackW * 0.5f;
                blacks.push_back({ k, ImRect(x, origin.y, x + blackW, origin.y + blackH) });
            }
            else
            {
                whiteRects.push_back({ k, ImRect(origin.x + w * whiteW, origin.y, origin.x + (w + 1) * whiteW, origin.y + whiteH) });
                ++w;
            }
        }
        if (ImGui::IsItemHovered() || ImGui::IsItemActive())
        {
            for (auto& [k, r] : blacks)
                if (r.Contains(mouse)) hover = k;
            if (hover < 0)
                for (auto& [k, r] : whiteRects)
                    if (r.Contains(mouse)) hover = k;
        }
        int want = ImGui::IsItemActive() ? hover : -1;
        if (want != m_PianoNote)
        {
            if (m_PianoNote >= 0) Preview(m_SelectedChannel, m_PianoNote, false), RecordLive(m_PianoNote, 0.0f, false);
            m_PianoNote = want;
            if (m_PianoNote >= 0) Preview(m_SelectedChannel, m_PianoNote, true), RecordLive(m_PianoNote, 0.8f, true);
        }
        for (auto& [k, r] : whiteRects)
        {
            dl->AddRectFilled(r.Min, r.Max, held(k) ? IM_COL32(150, 200, 255, 255) : k == hover ? IM_COL32(225, 225, 225, 255) : IM_COL32(245, 245, 245, 255));
            dl->AddRect(r.Min, r.Max, IM_COL32(40, 40, 40, 255));
            if (k % 12 == 0) dl->AddText(ImVec2(r.Min.x + 2, r.Max.y - ImGui::GetTextLineHeight() - 2), IM_COL32(90, 90, 90, 255), NoteName(k).c_str());
        }
        for (auto& [k, r] : blacks)
            dl->AddRectFilled(r.Min, r.Max, held(k) ? IM_COL32(80, 140, 220, 255) : k == hover ? IM_COL32(70, 70, 70, 255) : IM_COL32(25, 25, 25, 255), 2.0f);
        ImGui::EndChild();
    }

    void MusicMode::HandleKeys()
    {
        ImGuiIO& io = ImGui::GetIO();
        if (io.WantTextInput)
        {
            // Typing in a field: notes held by the typing keyboard stop.
            for (auto& [k, note] : m_TypedNotes) Preview(m_SelectedChannel, note, false), RecordLive(note, 0.0f, false);
            m_TypedNotes.clear();
            return;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_F5, false)) m_View = View::Playlist;
        if (ImGui::IsKeyPressed(ImGuiKey_F6, false)) m_View = View::Rack;
        if (ImGui::IsKeyPressed(ImGuiKey_F7, false)) m_View = View::PianoRoll;
        if (ImGui::IsKeyPressed(ImGuiKey_F9, false)) m_View = View::Mixer;
        if (m_View == View::PianoRoll) PianoRollKeys();
        if (m_View == View::Playlist) PlaylistKeys();
        if (io.KeyCtrl)
        {
            if (ImGui::IsKeyPressed(ImGuiKey_Z, false)) io.KeyShift ? Redo() : Undo();
            if (ImGui::IsKeyPressed(ImGuiKey_Y, false)) Redo();
            if (ImGui::IsKeyPressed(ImGuiKey_S, false) && Save()) m_Status = "Saved", m_StatusTime = 2.0f;
            return;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) TogglePlay();
        if (ImGui::IsKeyPressed(ImGuiKey_PageUp, false)) m_TypingOctave = std::min(9, m_TypingOctave + 1);
        if (ImGui::IsKeyPressed(ImGuiKey_PageDown, false)) m_TypingOctave = std::max(0, m_TypingOctave - 1);
        // The typing keyboard.
        for (const TypedKey& t : kTypedKeys)
        {
            int note = std::clamp(m_TypingOctave * 12 + t.Offset, 0, 127);
            if (ImGui::IsKeyPressed(t.Key, false))
            {
                Preview(m_SelectedChannel, note, true);
                RecordLive(note, 0.8f, true);
                m_TypedNotes.push_back({ (int)t.Key, note });
            }
        }
        for (size_t i = 0; i < m_TypedNotes.size();)
            if (!ImGui::IsKeyDown((ImGuiKey)m_TypedNotes[i].first))
            {
                Preview(m_SelectedChannel, m_TypedNotes[i].second, false);
                RecordLive(m_TypedNotes[i].second, 0.0f, false);
                m_TypedNotes.erase(m_TypedNotes.begin() + i);
            }
            else ++i;
    }
}
