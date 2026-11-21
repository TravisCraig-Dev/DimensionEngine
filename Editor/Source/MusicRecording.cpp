// Music Mode's recording: a MIDI keyboard (played straight into the song's sound on the MIDI thread), notes recorded
// into the current pattern while it plays with REC on, audio takes from the microphone onto the Playlist, audio clips,
// and the Audio & MIDI window.
#include "MusicMode.h"
#include "MusicModeUI.h"
#include "EditorUI.h"
#include <Dimension/Core/Log.h>
#include <Dimension/Asset/AssetManager.h>
#include <Dimension/Audio/Microphone.h>
#include <Dimension/Project/Project.h>
#include <imgui_internal.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>

namespace Editor
{
    using namespace Dimension;
    using namespace Dimension::Music;

    namespace
    {
        fs::path IOSettingsFile() { return Project::GetDirectory() / ".dimension" / "MusicIO.json"; }
        // Which channel each MIDI key went to when pressed (so its release goes there too). MIDI thread only.
        std::array<int, 128> s_MidiNoteChannel{};
    }

    void MusicMode::LoadIOSettings()
    {
        if (m_IOLoaded) return;
        m_IOLoaded = true;
        std::ifstream file(IOSettingsFile());
        nlohmann::json j = file ? nlohmann::json::parse(file, nullptr, false) : nlohmann::json();
        if (!j.is_object()) return;
        m_MidiDeviceName = j.value("midi", "");
        m_MicDevice = j.value("microphone", -1);
        m_RecordQuantize = j.value("quantize", false);
        m_InputLatencyMs = j.value("inputLatencyMs", 0.0f);
        m_Metronome = j.value("metronome", false);
    }

    void MusicMode::SaveIOSettings()
    {
        std::error_code ec;
        fs::create_directories(IOSettingsFile().parent_path(), ec);
        std::ofstream(IOSettingsFile()) << nlohmann::json{ { "midi", m_MidiDeviceName }, { "microphone", m_MicDevice }, { "quantize", m_RecordQuantize },
                                                           { "metronome", m_Metronome }, { "inputLatencyMs", m_InputLatencyMs } }.dump(1);
    }

    void MusicMode::StartMidi()
    {
        LoadIOSettings();
        SongPlayer* player = m_Player.get();
        // Notes go to the sound at once (on the MIDI thread) and are queued for recording and the piano.
        MidiInput::SetHandler([this, player](const MidiMessage& m) {
            if (m.IsNoteOn())
            {
                int channel = m_LiveChannel.load();
                s_MidiNoteChannel[m.Data1 & 127] = channel;
                player->NoteOn(channel, m.Data1, m.Velocity());
            }
            else if (m.IsNoteOff())
                player->NoteOff(s_MidiNoteChannel[m.Data1 & 127], m.Data1);
            else
                return;
            std::lock_guard lock(m_MidiMutex);
            m_MidiQueue.push_back({ m });
        });
        // The device used last (by name), or the only one there is.
        auto devices = MidiInput::Devices();
        int open = -1;
        for (int i = 0; i < (int)devices.size(); ++i)
            if (!m_MidiDeviceName.empty() && devices[i] == m_MidiDeviceName) open = i;
        if (open < 0 && m_MidiDeviceName.empty() && devices.size() == 1) open = 0;
        if (open >= 0 && MidiInput::OpenDevice() != open)
        {
            if (MidiInput::Open(open)) m_MidiDeviceName = devices[open];
        }
        m_Metronome = m_Metronome && player;
        if (player) player->SetMetronome(m_Metronome);
    }

    void MusicMode::StopMidi()
    {
        MidiInput::SetHandler(nullptr); // (after this no call reaches the player)
        MidiInput::Close();
        if (m_Taking) EndTake();
        m_MidiHeld.clear();
    }

    void MusicMode::RecordLive(int key, float velocity, bool on)
    {
        if (!m_Taking || m_RecordSource != 0 || !m_Player) return;
        double tick = m_Player->GetTick();
        if (m_SongMode) tick = std::fmod(tick, (double)m_Song.PatternTicks(m_Pattern));
        if (on)
        {
            m_NoteRec.NoteOn(key, velocity, tick);
            return;
        }
        Note n;
        if (!m_NoteRec.NoteOff(key, tick, n)) return;
        if (!m_TakeUndone)
        {
            Record(); // (one undo step for the whole take)
            m_TakeUndone = true;
        }
        m_Song.Patterns[m_Pattern].Notes.push_back(n);
        Changed();
    }

    void MusicMode::BeginTake()
    {
        m_Taking = true;
        m_TakeUndone = false;
        if (m_RecordSource == 0)
        {
            m_NoteRec.Begin(m_SelectedChannel, m_Song.PatternTicks(m_Pattern), m_RecordQuantize ? m_Song.StepTicks() : 1);
            m_Status = "Recording notes into " + m_Song.Patterns[m_Pattern].Name;
            m_StatusTime = 3.0f;
            return;
        }
        if (!Microphone::Start(m_MicDevice, 20.0f, true, 0))
        {
            m_Taking = false;
            m_RecordArmed = false;
            m_Status = "No microphone to record from";
            m_StatusTime = 5.0f;
            return;
        }
        m_MicCursor = Microphone::GetPosition();
        m_Take.clear();
        m_TakeStartTick = m_Player ? m_Player->GetTick() : 0.0;
        m_Status = "Recording audio";
        m_StatusTime = 3.0f;
    }

    void MusicMode::EndTake()
    {
        if (!m_Taking) return;
        m_Taking = false;
        if (m_RecordSource == 0)
        {
            double tick = m_Player ? m_Player->GetTick() : 0.0;
            if (m_SongMode) tick = std::fmod(tick, (double)m_Song.PatternTicks(m_Pattern));
            auto held = m_NoteRec.End(tick);
            if (!held.empty())
            {
                if (!m_TakeUndone) Record();
                for (const Note& n : held) m_Song.Patterns[m_Pattern].Notes.push_back(n);
                Changed();
            }
            return;
        }
        // The take: the rest of the microphone's samples, written next to the song and placed where it started.
        std::vector<float> rest;
        Microphone::ReadSince(m_MicCursor, rest);
        m_Take.insert(m_Take.end(), rest.begin(), rest.end());
        uint32_t rate = Microphone::GetSampleRate();
        Microphone::Stop();
        if (rate == 0 || m_Take.size() < rate / 10)
        {
            m_Status = "The take was too short to keep";
            m_StatusTime = 4.0f;
            return;
        }
        fs::path folder = m_File.parent_path() / "Recordings";
        std::error_code ec;
        fs::create_directories(folder, ec);
        fs::path path;
        for (int n = 1;; ++n)
            if (path = folder / (m_Song.Name + " Take " + std::to_string(n) + ".wav"); !fs::exists(path)) break;
        std::vector<float> stereo(m_Take.size() * 2);
        for (size_t i = 0; i < m_Take.size(); ++i) stereo[i * 2] = stereo[i * 2 + 1] = m_Take[i];
        if (!WriteWav(path, stereo, rate))
        {
            m_Status = "Could not write " + path.filename().string();
            return;
        }
        AssetHandle h = AssetManager::Import(path);
        Record();
        // Placed earlier by the input latency (the take is late by the time sound takes out to the speakers and back in
        // through the microphone); what would fall before the song's start is trimmed off the clip.
        double latencyTicks = m_InputLatencyMs / 1000.0 * m_Song.TicksPerSecond();
        double start = m_TakeStartTick - latencyTicks;
        int clip = AddAudioClip(h, path, (int)std::lround(std::max(0.0, start)));
        if (start < 0.0) m_Song.AudioClips[clip].Offset = (float)(-start / m_Song.TicksPerSecond());
        m_SelectedAudio = clip;
        Changed();
        m_Status = "Recorded " + path.filename().string();
        m_StatusTime = 4.0f;
        Log::Info("Music: recorded {} ({:.1f} s)", path.filename().string(), m_Take.size() / (double)rate);
        m_Take.clear();
    }

    void MusicMode::UpdateRecording()
    {
        if (!m_Player) return;
        m_LiveChannel = std::clamp(m_SelectedChannel, 0, std::max(0, (int)m_Song.Channels.size() - 1));
        // MIDI notes: the piano shows them, recording takes them.
        std::vector<MidiEvent> events;
        {
            std::lock_guard lock(m_MidiMutex);
            events.swap(m_MidiQueue);
        }
        for (const MidiEvent& e : events)
        {
            int key = e.Message.Data1;
            std::erase(m_MidiHeld, key);
            if (e.Message.IsNoteOn()) m_MidiHeld.push_back(key);
            RecordLive(key, e.Message.Velocity(), e.Message.IsNoteOn());
        }
        // Takes start with playback while REC is on and end when it stops (or REC goes off).
        bool playing = m_Player->IsPlaying();
        if (m_RecordArmed && playing && !m_Taking) BeginTake();
        if (m_Taking && (!playing || !m_RecordArmed)) EndTake();
        if (m_Taking && m_RecordSource == 1)
        {
            std::vector<float> chunk;
            Microphone::ReadSince(m_MicCursor, chunk);
            m_Take.insert(m_Take.end(), chunk.begin(), chunk.end());
        }
        m_WasPlaying = playing;
    }

    int MusicMode::AddAudioClip(AssetHandle asset, const fs::path& file, int start)
    {
        AudioClip clip;
        clip.Name = file.stem().string();
        clip.Sample = (uint64_t)asset;
        const AssetMetadata* meta = asset.IsValid() ? AssetManager::GetMetadata(asset) : nullptr;
        clip.SamplePath = meta ? meta->Path.generic_string() : file.string();
        auto info = AudioEngine::GetClipInfo(file);
        clip.Seconds = info ? info->Seconds : 1.0f;
        clip.Start = std::max(0, start);
        int ticks = m_Song.ClipTicks(clip);
        // The first track free there.
        for (int track = 0;; ++track)
        {
            bool busy = false;
            for (const auto& c : m_Song.Playlist) busy |= c.Track == track && c.Start < clip.Start + ticks && c.Start + m_Song.ClipTicks(c) > clip.Start;
            for (const auto& c : m_Song.Automation) busy |= c.Track == track && c.Start < clip.Start + ticks && c.Start + c.Length > clip.Start;
            for (const auto& c : m_Song.AudioClips) busy |= c.Track == track && c.Start < clip.Start + ticks && c.Start + m_Song.ClipTicks(c) > clip.Start;
            if (!busy)
            {
                clip.Track = track;
                break;
            }
        }
        m_Song.AudioClips.push_back(clip);
        return (int)m_Song.AudioClips.size() - 1;
    }

    const AudioWaveform* MusicMode::ClipWaveform(const AudioClip& clip)
    {
        SamplerParams where;
        where.Sample = clip.Sample;
        where.SamplePath = clip.SamplePath;
        fs::path file = ResolveSamplePath(where);
        std::string key = file.string();
        auto it = m_Waveforms.find(key);
        if (it == m_Waveforms.end())
        {
            auto wave = AudioEngine::GetWaveform(file, 2048);
            it = m_Waveforms.emplace(key, wave ? std::make_shared<AudioWaveform>(*wave) : nullptr).first;
        }
        return it->second.get();
    }

    void MusicMode::DrawAudioClipBar(const ImVec2& min, const ImVec2& max)
    {
        // The selected audio clip: its level, mixer track, and where in its file it starts.
        AudioClip& clip = m_Song.AudioClips[m_SelectedAudio];
        ImGui::GetWindowDrawList()->AddRectFilled(min, max, IM_COL32(32, 34, 38, 255), 3.0f);
        ImGui::SetCursorScreenPos(ImVec2(min.x + 6, min.y + 6));
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(UI::AccentText(), "%s", clip.Name.c_str());
        ImGui::SameLine(0.0f, 16.0f);
        ImGui::TextDisabled("Gain");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.0f);
        float db = 20.0f * std::log10(std::max(clip.Gain, 1e-4f));
        if (Edited(ImGui::SliderFloat("##gain", &db, -36.0f, 12.0f, "%.1f dB"))) clip.Gain = std::pow(10.0f, db / 20.0f), Changed();
        ImGui::SameLine(0.0f, 16.0f);
        ImGui::TextDisabled("Mixer Track");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(130.0f);
        if (ImGui::BeginCombo("##clipInsert", m_Song.MixerTrackName(clip.Insert).c_str()))
        {
            for (int t = 0; t < (int)m_Song.Mixer.size(); ++t)
                if (ImGui::Selectable(m_Song.MixerTrackName(t).c_str(), clip.Insert == t))
                {
                    Record();
                    clip.Insert = t;
                    Changed();
                }
            ImGui::EndCombo();
        }
        ImGui::SameLine(0.0f, 16.0f);
        ImGui::TextDisabled("Starts");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110.0f);
        if (Edited(ImGui::DragFloat("##offset", &clip.Offset, 0.01f, 0.0f, std::max(0.0f, clip.Seconds - 0.01f), "%.2f s in"))) Changed();
        ImGui::SetItemTooltip("Where in its file the clip starts (trim its beginning)");
        ImGui::SameLine(0.0f, 16.0f);
        ImGui::TextDisabled("%.2f s file", clip.Seconds);
    }

    void MusicMode::DrawIOWindow()
    {
        if (!m_ShowIO) return;
        ImGui::SetNextWindowSize(ImVec2(440.0f, 0.0f), ImGuiCond_Always);
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + 120.0f), ImGuiCond_Appearing, ImVec2(0.5f, 0.0f));
        if (!ImGui::Begin("Audio & MIDI", &m_ShowIO, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse))
        {
            ImGui::End();
            return;
        }
        bool changed = false;
        if (UI::BeginProperties("##io"))
        {
            UI::PropertyLabel("MIDI Input", "A keyboard or controller: plays the selected channel");
            auto midi = MidiInput::Devices();
            int open = MidiInput::OpenDevice();
            std::string current = open >= 0 && open < (int)midi.size() ? midi[open] : std::string("None");
            if (ImGui::BeginCombo("##midi", current.c_str()))
            {
                if (ImGui::Selectable("None", open < 0))
                {
                    MidiInput::Close();
                    m_MidiDeviceName = "-"; // (none, on purpose: not opened again by itself)
                    changed = true;
                }
                for (int i = 0; i < (int)midi.size(); ++i)
                    if (ImGui::Selectable(midi[i].c_str(), i == open))
                    {
                        if (MidiInput::Open(i)) m_MidiDeviceName = midi[i];
                        else m_Status = "That MIDI device would not open", m_StatusTime = 4.0f;
                        changed = true;
                    }
                if (midi.empty()) ImGui::TextDisabled("No MIDI inputs found");
                ImGui::EndCombo();
            }
            UI::PropertyLabel("Microphone", "What audio recording records from");
            auto mics = Microphone::Devices();
            std::string mic = m_MicDevice >= 0 && m_MicDevice < (int)mics.size() ? mics[m_MicDevice] : std::string("System default");
            if (ImGui::BeginCombo("##mic", mic.c_str()))
            {
                if (ImGui::Selectable("System default", m_MicDevice < 0)) m_MicDevice = -1, changed = true;
                for (int i = 0; i < (int)mics.size(); ++i)
                    if (ImGui::Selectable(mics[i].c_str(), i == m_MicDevice)) m_MicDevice = i, changed = true;
                ImGui::EndCombo();
            }
            UI::PropertyLabel("REC Records");
            if (ImGui::RadioButton("Notes", m_RecordSource == 0)) m_RecordSource = 0;
            ImGui::SameLine();
            if (ImGui::RadioButton("Audio", m_RecordSource == 1))
            {
                m_RecordSource = 1;
                if (!m_SongMode) SetSongMode(true); // (audio takes go on the Playlist)
            }
            UI::PropertyLabel("Input Latency", "Audio takes are moved this much earlier: the delay from the speakers to the microphone and "
                                               "back (try 20-60 ms; adjust until takes sit on the beat)");
            changed |= ImGui::SliderFloat("##latency", &m_InputLatencyMs, 0.0f, 300.0f, "%.0f ms");
            UI::PropertyLabel("Snap Notes to Steps", "Recorded notes start on the step grid (otherwise as played)");
            changed |= ImGui::Checkbox("##quantize", &m_RecordQuantize);
            UI::PropertyLabel("Metronome");
            if (ImGui::Checkbox("##metronome", &m_Metronome))
            {
                if (m_Player) m_Player->SetMetronome(m_Metronome);
                changed = true;
            }
            UI::EndProperties();
        }
        if (m_Taking && m_RecordSource == 1)
        {
            float level = std::min(1.0f, Microphone::GetLevel() * 4.0f);
            ImGui::ProgressBar(level, ImVec2(-1, 0), "Input level");
        }
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled)), ImGui::TextWrapped("%s", "Notes: REC on, then Play: what you play on the selected channel goes into the pattern."), ImGui::PopStyleColor();
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled)), ImGui::TextWrapped("%s", "Audio: REC on, then Play (Song mode): the take goes on the Playlist."), ImGui::PopStyleColor();
        ImGui::End();
        if (changed) SaveIOSettings();
    }
}
